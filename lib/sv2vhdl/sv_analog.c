/*
 * sv_analog.c — VHPIDIRECT implementation for sv_analog_pkg
 *
 * Collects analog block strings with metadata and writes reconstructed
 * .va files when the shared library is unloaded.
 *
 * Metadata format (prepended by iverilog):
 *   MODULE:<name>|PORT:<name>:<dir>:<discipline>|...||<body>
 *
 * If no "||" separator is found, the entire string is treated as body
 * (backward-compatible with Phase 1 usage).
 *
 * VHPIDIRECT calling convention for `in string`:
 *   const uint8_t *text  — pointer to string data (not null-terminated)
 *   int32_t len           — length of the string
 *
 * Loaded by NVC via --load=libsv_analog.so
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ANALOG_BLOCKS 256
#define MAX_MODULES       64
#define MAX_PORTS         64
#define MAX_BODIES        64

/* Legacy flat storage for query API */
static char *analog_blocks[MAX_ANALOG_BLOCKS];
static int analog_block_count = 0;

/* Per-port metadata */
typedef struct {
    char *name;
    char *dir;         /* "input", "output", "inout" */
    char *discipline;  /* "electrical", etc. */
} analog_port_t;

/* Per-module collection */
typedef struct {
    char *module_name;
    analog_port_t ports[MAX_PORTS];
    int nports;
    char *bodies[MAX_BODIES];
    int nbodies;
} analog_module_t;

static analog_module_t modules[MAX_MODULES];
static int module_count = 0;

/*
 * Find or create a module entry by name.
 */
static analog_module_t *find_or_create_module(const char *name)
{
    for (int i = 0; i < module_count; i++) {
        if (strcmp(modules[i].module_name, name) == 0)
            return &modules[i];
    }
    if (module_count >= MAX_MODULES) {
        fprintf(stderr, "[sv_analog] ERROR: too many modules (max %d)\n",
                MAX_MODULES);
        return NULL;
    }
    analog_module_t *m = &modules[module_count++];
    m->module_name = strdup(name);
    m->nports = 0;
    m->nbodies = 0;
    return m;
}

/*
 * Parse metadata prefix and body from a block string.
 * Format: MODULE:<name>|PORT:<n>:<dir>:<disc>|...||<body>
 */
static void parse_and_store(const char *str)
{
    const char *sep = strstr(str, "||");
    if (!sep) {
        /* No metadata — legacy format, store body only */
        fprintf(stderr, "[sv_analog] Block (no metadata): %s\n", str);
        return;
    }

    /* Extract metadata portion */
    size_t meta_len = sep - str;
    char *meta = (char *)malloc(meta_len + 1);
    memcpy(meta, str, meta_len);
    meta[meta_len] = '\0';

    /* Body is after "||" */
    const char *body = sep + 2;

    /* Parse metadata fields separated by '|' */
    analog_module_t *mod = NULL;
    char *saveptr = NULL;
    char *token = strtok_r(meta, "|", &saveptr);
    while (token) {
        if (strncmp(token, "MODULE:", 7) == 0) {
            mod = find_or_create_module(token + 7);
            if (!mod) { free(meta); return; }
        } else if (strncmp(token, "PORT:", 5) == 0 && mod) {
            /* PORT:<name>:<dir>:<discipline> */
            char *p = token + 5;
            char *name_end = strchr(p, ':');
            if (!name_end) { token = strtok_r(NULL, "|", &saveptr); continue; }

            char *dir_start = name_end + 1;
            char *dir_end = strchr(dir_start, ':');
            if (!dir_end) { token = strtok_r(NULL, "|", &saveptr); continue; }

            char *disc_start = dir_end + 1;

            /* Only add ports on first encounter (avoid duplicates) */
            int already_have = 0;
            size_t name_len = name_end - p;
            for (int i = 0; i < mod->nports; i++) {
                if (strlen(mod->ports[i].name) == name_len &&
                    strncmp(mod->ports[i].name, p, name_len) == 0) {
                    already_have = 1;
                    break;
                }
            }
            if (!already_have && mod->nports < MAX_PORTS) {
                analog_port_t *port = &mod->ports[mod->nports++];
                port->name = strndup(p, name_len);
                port->dir = strndup(dir_start, dir_end - dir_start);
                port->discipline = strdup(disc_start);
            }
        }
        token = strtok_r(NULL, "|", &saveptr);
    }

    /* Append body to module */
    if (mod && mod->nbodies < MAX_BODIES) {
        mod->bodies[mod->nbodies++] = strdup(body);
    }

    fprintf(stderr, "[sv_analog] Block for module '%s': %s\n",
            mod ? mod->module_name : "?", body);

    free(meta);
}

/*
 * sv_analog_eval — called for each sv_analog("...") concurrent call.
 * Stores the block text, parses metadata, and prints to stderr.
 */
void sv_analog_eval(const uint8_t *text, int32_t len)
{
    if (analog_block_count >= MAX_ANALOG_BLOCKS) {
        fprintf(stderr, "[sv_analog] ERROR: too many analog blocks (max %d)\n",
                MAX_ANALOG_BLOCKS);
        return;
    }

    /* Store a null-terminated copy */
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        fprintf(stderr, "[sv_analog] ERROR: malloc failed\n");
        return;
    }
    memcpy(copy, text, len);
    copy[len] = '\0';

    analog_blocks[analog_block_count] = copy;
    analog_block_count++;

    /* Parse metadata and collect per-module */
    parse_and_store(copy);
}

/*
 * Query API for downstream tools (resolver, etc.)
 */
int sv_analog_count(void)
{
    return analog_block_count;
}

const char *sv_analog_get(int idx)
{
    if (idx < 0 || idx >= analog_block_count)
        return NULL;
    return analog_blocks[idx];
}

/*
 * Write reconstructed .va files for all collected modules.
 * Called automatically when the shared library is unloaded.
 */
static void write_va_files(void)
{
    for (int m = 0; m < module_count; m++) {
        analog_module_t *mod = &modules[m];
        if (mod->nbodies == 0) continue;

        char filename[256];
        snprintf(filename, sizeof(filename), "%s.va", mod->module_name);

        FILE *f = fopen(filename, "w");
        if (!f) {
            fprintf(stderr, "[sv_analog] ERROR: cannot write %s\n", filename);
            continue;
        }

        fprintf(f, "`include \"disciplines.vams\"\n");
        fprintf(f, "module %s(", mod->module_name);
        for (int i = 0; i < mod->nports; i++) {
            if (i > 0) fprintf(f, ", ");
            fprintf(f, "%s", mod->ports[i].name);
        }
        fprintf(f, ");\n");

        /* Port direction declarations */
        for (int i = 0; i < mod->nports; i++) {
            fprintf(f, "  %s %s;\n", mod->ports[i].dir, mod->ports[i].name);
        }

        /* Dummy parameter — OpenVAF crashes on modules with no parameters */
        fprintf(f, "  parameter real __dummy = 0.0;\n");

        /* Discipline declarations — group ports by discipline */
        for (int i = 0; i < mod->nports; i++) {
            if (!mod->ports[i].discipline[0]) continue;

            /* Check if we already emitted this discipline */
            int already = 0;
            for (int j = 0; j < i; j++) {
                if (strcmp(mod->ports[i].discipline,
                           mod->ports[j].discipline) == 0) {
                    already = 1;
                    break;
                }
            }
            if (already) continue;

            /* Collect all ports with this discipline */
            fprintf(f, "  %s ", mod->ports[i].discipline);
            int first = 1;
            for (int j = i; j < mod->nports; j++) {
                if (strcmp(mod->ports[i].discipline,
                           mod->ports[j].discipline) == 0) {
                    if (!first) fprintf(f, ", ");
                    fprintf(f, "%s", mod->ports[j].name);
                    first = 0;
                }
            }
            fprintf(f, ";\n");
        }

        /* Analog block */
        fprintf(f, "  analog begin\n");
        for (int i = 0; i < mod->nbodies; i++) {
            fprintf(f, "    %s\n", mod->bodies[i]);
        }
        fprintf(f, "  end\n");
        fprintf(f, "endmodule\n");

        fclose(f);
        fprintf(stderr, "[sv_analog] Wrote %s (%d ports, %d body blocks)\n",
                filename, mod->nports, mod->nbodies);
    }
}

/*
 * Cleanup: write .va files and free memory on library unload.
 */
__attribute__((destructor))
static void sv_analog_fini(void)
{
    write_va_files();

    /* Free legacy storage */
    for (int i = 0; i < analog_block_count; i++)
        free(analog_blocks[i]);
    analog_block_count = 0;

    /* Free module storage */
    for (int i = 0; i < module_count; i++) {
        analog_module_t *mod = &modules[i];
        free(mod->module_name);
        for (int j = 0; j < mod->nports; j++) {
            free(mod->ports[j].name);
            free(mod->ports[j].dir);
            free(mod->ports[j].discipline);
        }
        for (int j = 0; j < mod->nbodies; j++)
            free(mod->bodies[j]);
    }
    module_count = 0;
}
