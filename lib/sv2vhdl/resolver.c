/*
 * resolver.c -- VHPI plugin for sv2vhdl resolution network generation
 *
 * Walks the elaborated design hierarchy, detects _driver/_others vector
 * signal pairs, and calls Python to generate resolver VHDL using
 * VHDL-2008 external names.
 *
 * Two-pass workflow:
 *   1. Discovery run with this plugin: generates + compiles resolver VHDL
 *   2. Standalone run: wrapper entity instantiates DUT + resolver
 *
 * Build:  make
 * Usage:  nvc -r --load=./libresolver.so <top_entity>
 *
 * Copyright (C) 2025  sv2ghdl contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "vhpi_user.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <errno.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>

/* ---------- Configuration ---------- */

#define MAX_NAME  512
#define MAX_TYPE  128

#define SUFFIX_DRIVER  "_driver"
#define SUFFIX_OTHERS  "_others"

#define RESOLVER_MODULE  "sv2vhdl_resolver"
#define RESOLVER_FUNC    "resolve_net"

#define CACHE_DIR        "_sv2vhdl_cache"

/* ---------- Data structures ---------- */

/*
 * Simplified net_info: one driver vector signal + one others vector signal.
 * Vector length indicates number of switch endpoints on the net.
 */
typedef struct net_info {
    char net_name[MAX_NAME];      /* base name (VHPI full name minus suffix) */
    char type_name[MAX_TYPE];     /* signal type: "STD_LOGIC_VECTOR" or "STD_LOGIC" */
    char elem_type[MAX_TYPE];     /* element type: "STD_ULOGIC" or same as type_name */
    char driver_ename[MAX_NAME];  /* external name path: ".top.sig_driver" */
    char others_ename[MAX_NAME];  /* external name path: ".top.sig_others" */
    int  length;                  /* vector size (N endpoints) */
    int  has_driver;
    int  has_others;
    int  needs_resolution;
    struct net_info *next;
} net_info_t;

/* ---------- Global state ---------- */

static net_info_t *g_nets = NULL;
static int g_total_signals = 0;
static int g_total_instances = 0;
static int g_depth = 0;
static char g_design_name[MAX_NAME] = {0};

/* Python state */
static int g_python_ok = 0;
static PyObject *g_py_module = NULL;
static PyObject *g_py_func = NULL;

/* ---------- Utility functions ---------- */

static int streqi(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++; b++;
    }
    return *a == *b;
}

static int ends_with(const char *str, const char *suffix)
{
    size_t slen = strlen(str);
    size_t xlen = strlen(suffix);
    if (xlen > slen) return 0;
    return streqi(str + slen - xlen, suffix);
}

static void safe_copy(char *dst, const char *src, size_t dstsz)
{
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if (len >= dstsz) len = dstsz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/*
 * Strip suffix from str, write base into buf. Returns buf on success, NULL
 * if str doesn't end with suffix.
 */
static const char *strip_suffix(const char *str, const char *suffix,
                                char *buf, size_t bufsz)
{
    size_t slen = strlen(str);
    size_t xlen = strlen(suffix);
    if (xlen > slen) return NULL;
    if (!streqi(str + slen - xlen, suffix)) return NULL;
    size_t baselen = slen - xlen;
    if (baselen >= bufsz) baselen = bufsz - 1;
    memcpy(buf, str, baselen);
    buf[baselen] = '\0';
    return buf;
}

/*
 * Convert a VHPI full name to an external name path.
 * VHPI format: ":ENTITY:SIGNAL" or ":ENTITY:INSTANCE:SIGNAL"
 * External name: ".entity.signal" or ".entity.instance.signal"
 * Simply: strip leading :/@ , replace : with . , lowercase.
 */
static void vhpi_to_ename(const char *vhpi_full, char *ename, size_t esz)
{
    const char *src = vhpi_full;

    /* Strip leading : or @ */
    while (*src == ':' || *src == '@') src++;

    /* Copy, replacing : with . and lowercasing */
    char *dst = ename;
    char *end = ename + esz - 1;

    /* Leading dot for absolute external name */
    if (dst < end) *dst++ = '.';

    while (*src && dst < end) {
        if (*src == ':')
            *dst++ = '.';
        else
            *dst++ = tolower((unsigned char)*src);
        src++;
    }
    *dst = '\0';
}

static net_info_t *find_or_create_net(const char *name)
{
    for (net_info_t *n = g_nets; n; n = n->next) {
        if (streqi(n->net_name, name))
            return n;
    }
    net_info_t *n = calloc(1, sizeof(*n));
    if (!n) { vhpi_printf("resolver: out of memory"); return NULL; }
    safe_copy(n->net_name, name, sizeof(n->net_name));
    n->next = g_nets;
    g_nets = n;
    return n;
}

/* ---------- Hierarchy walker ---------- */

static const char *get_name(vhpiHandleT h)
{
    return (const char *)vhpi_get_str(vhpiNameP, h);
}

static const char *get_full_name(vhpiHandleT h)
{
    return (const char *)vhpi_get_str(vhpiFullNameP, h);
}

static void indent(void)
{
    for (int i = 0; i < g_depth; i++)
        vhpi_printf("  ");
}

/*
 * Get type info for a signal.
 * sig_type_buf: full type name ("STD_LOGIC_VECTOR" or "STD_LOGIC")
 * elem_type_buf: element type for arrays ("STD_ULOGIC"), same as sig for scalars
 */
static void get_type_info(vhpiHandleT sig, char *sig_type_buf, size_t stbufsz,
                          char *elem_type_buf, size_t etbufsz)
{
    sig_type_buf[0] = '\0';
    elem_type_buf[0] = '\0';
    vhpiHandleT type = vhpi_handle(vhpiType, sig);
    if (!type) {
        safe_copy(sig_type_buf, "?", stbufsz);
        safe_copy(elem_type_buf, "?", etbufsz);
        return;
    }

    /* Get the signal's own type name */
    const char *tn = (const char *)vhpi_get_str(vhpiNameP, type);
    safe_copy(sig_type_buf, tn ? tn : "?", stbufsz);

    /* Try to get element type (for arrays) */
    vhpiHandleT etype = vhpi_handle(vhpiElemType, type);
    if (etype) {
        const char *etn = (const char *)vhpi_get_str(vhpiNameP, etype);
        safe_copy(elem_type_buf, etn ? etn : "?", etbufsz);
        vhpi_release_handle(etype);
    } else {
        /* Scalar: element type = signal type */
        safe_copy(elem_type_buf, tn ? tn : "?", etbufsz);
    }
    vhpi_release_handle(type);
}

static void scan_signals(vhpiHandleT region)
{
    vhpiHandleT iter = vhpi_iterator(vhpiSigDecls, region);
    if (!iter) return;

    for (vhpiHandleT sig = vhpi_scan(iter); sig; sig = vhpi_scan(iter)) {
        const char *name = get_name(sig);
        const char *full = get_full_name(sig);
        g_total_signals++;

        if (!name || !full) {
            vhpi_release_handle(sig);
            continue;
        }

        char base[MAX_NAME];
        char ename[MAX_NAME];
        int is_driver = ends_with(name, SUFFIX_DRIVER);
        int is_others = ends_with(name, SUFFIX_OTHERS);

        if (!is_driver && !is_others) {
            vhpi_release_handle(sig);
            continue;
        }

        const char *suffix = is_driver ? SUFFIX_DRIVER : SUFFIX_OTHERS;

        /* Strip suffix from signal name (not full path) to get net base name */
        char name_base[MAX_NAME];
        if (!strip_suffix(name, suffix, name_base, sizeof(name_base))) {
            vhpi_release_handle(sig);
            continue;
        }

        /* Build the net name from full path: strip suffix from full */
        if (!strip_suffix(full, suffix, base, sizeof(base))) {
            vhpi_release_handle(sig);
            continue;
        }

        net_info_t *net = find_or_create_net(base);
        if (!net) {
            vhpi_release_handle(sig);
            continue;
        }

        /* Get vector size */
        int size = vhpi_get(vhpiSizeP, sig);
        if (size <= 0) size = 1;  /* scalar */

        /* Get type info */
        char sig_type[MAX_TYPE], elem_type[MAX_TYPE];
        get_type_info(sig, sig_type, sizeof(sig_type),
                      elem_type, sizeof(elem_type));
        if (net->type_name[0] == '\0')
            safe_copy(net->type_name, sig_type, sizeof(net->type_name));
        if (net->elem_type[0] == '\0')
            safe_copy(net->elem_type, elem_type, sizeof(net->elem_type));

        /* Convert VHPI full path to external name */
        vhpi_to_ename(full, ename, sizeof(ename));

        if (is_driver) {
            safe_copy(net->driver_ename, ename, sizeof(net->driver_ename));
            net->has_driver = 1;
            if (net->length == 0 || size > net->length)
                net->length = size;
        } else {
            safe_copy(net->others_ename, ename, sizeof(net->others_ename));
            net->has_others = 1;
            if (net->length == 0 || size > net->length)
                net->length = size;
        }

        indent();
        vhpi_printf("  signal: %s  size: %d  type: %s  ename: %s",
                     name, size, elem_type, ename);

        vhpi_release_handle(sig);
    }
    vhpi_release_handle(iter);
}

static void walk_hierarchy(vhpiHandleT region);

static void scan_instances(vhpiHandleT region)
{
    vhpiHandleT iter = vhpi_iterator(vhpiCompInstStmts, region);
    if (!iter) return;

    for (vhpiHandleT inst = vhpi_scan(iter); inst; inst = vhpi_scan(iter)) {
        const char *inst_name = get_name(inst);
        const char *inst_full = get_full_name(inst);
        g_total_instances++;

        const char *entity_name = "?";
        vhpiHandleT du = vhpi_handle(vhpiDesignUnit, inst);
        if (du) {
            vhpiHandleT entity = vhpi_handle(vhpiPrimaryUnit, du);
            if (entity) {
                entity_name = get_name(entity);
                vhpi_release_handle(entity);
            }
            vhpi_release_handle(du);
        }

        indent();
        vhpi_printf("  instance: %s  entity: %s",
                     inst_full ? inst_full : (inst_name ? inst_name : "?"),
                     entity_name ? entity_name : "?");

        vhpi_release_handle(inst);
    }
    vhpi_release_handle(iter);
}

static void walk_hierarchy(vhpiHandleT region)
{
    const char *rname = get_full_name(region);
    indent();
    vhpi_printf("region: %s", rname ? rname : "(root)");

    scan_signals(region);
    scan_instances(region);

    vhpiHandleT riter = vhpi_iterator(vhpiInternalRegions, region);
    if (riter) {
        for (vhpiHandleT sub = vhpi_scan(riter); sub;
             sub = vhpi_scan(riter)) {
            g_depth++;
            walk_hierarchy(sub);
            g_depth--;
            vhpi_release_handle(sub);
        }
        vhpi_release_handle(riter);
    }
}

/* ---------- Analysis ---------- */

static void analyze_nets(void)
{
    for (net_info_t *n = g_nets; n; n = n->next) {
        n->needs_resolution = (n->has_driver && n->has_others && n->length > 1);
    }
}

/* ---------- Topology hash ---------- */

static unsigned long djb2_hash(const char *str, unsigned long hash)
{
    while (*str)
        hash = hash * 33 + (unsigned char)*str++;
    return hash;
}

static unsigned long compute_topology_hash(void)
{
    /* Collect net names that need resolution, sort them */
    int count = 0;
    for (net_info_t *n = g_nets; n; n = n->next)
        if (n->needs_resolution) count++;

    if (count == 0) return 0;

    /* Simple array of pointers for sorting */
    net_info_t **sorted = malloc(count * sizeof(*sorted));
    if (!sorted) return 0;

    int idx = 0;
    for (net_info_t *n = g_nets; n; n = n->next)
        if (n->needs_resolution)
            sorted[idx++] = n;

    /* Sort by net_name */
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (strcmp(sorted[i]->net_name, sorted[j]->net_name) > 0) {
                net_info_t *tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }

    /* Hash: name:type:length for each net */
    unsigned long hash = 5381;
    for (int i = 0; i < count; i++) {
        char buf[MAX_NAME + MAX_TYPE + 32];
        snprintf(buf, sizeof(buf), "%s:%s:%d",
                 sorted[i]->net_name, sorted[i]->type_name, sorted[i]->length);
        hash = djb2_hash(buf, hash);
    }

    free(sorted);
    return hash;
}

/* ---------- Cache ---------- */

static int check_cache(const char *cache_path, unsigned long expected_hash)
{
    FILE *f = fopen(cache_path, "r");
    if (!f) return 0;

    char line[256];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);

    unsigned long stored_hash = 0;
    if (sscanf(line, "-- Topology hash: %lx", &stored_hash) == 1) {
        return stored_hash == expected_hash;
    }
    return 0;
}

static int write_and_compile(const char *vhdl, const char *cache_path,
                             const char *work_dir)
{
    /* Create cache directory */
    mkdir(CACHE_DIR, 0755);

    /* Write VHDL file */
    FILE *f = fopen(cache_path, "w");
    if (!f) {
        vhpi_printf("resolver: ERROR - cannot write %s: %s",
                     cache_path, strerror(errno));
        return -1;
    }
    fputs(vhdl, f);
    fclose(f);
    vhpi_printf("resolver: wrote %s", cache_path);

    /* Compile with nvc */
    char cmd[MAX_NAME * 2];
    if (work_dir && work_dir[0])
        snprintf(cmd, sizeof(cmd),
                 "nvc --std=2008 --work=%s -a %s 2>&1", work_dir, cache_path);
    else
        snprintf(cmd, sizeof(cmd),
                 "nvc --std=2008 -a %s 2>&1", cache_path);

    vhpi_printf("resolver: compiling: %s", cmd);
    FILE *proc = popen(cmd, "r");
    if (!proc) {
        vhpi_printf("resolver: ERROR - cannot spawn nvc: %s", strerror(errno));
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), proc)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        vhpi_printf("  nvc: %s", line);
    }

    int status = pclose(proc);
    if (status != 0) {
        vhpi_printf("resolver: ERROR - nvc compilation failed (status %d)", status);
        return -1;
    }

    vhpi_printf("resolver: compilation successful");
    return 0;
}

/* ---------- Python interface ---------- */

static const char *get_plugin_dir(void)
{
    static char dir[MAX_NAME] = {0};
    if (dir[0]) return dir;

    Dl_info info;
    if (dladdr((void *)get_plugin_dir, &info) && info.dli_fname) {
        safe_copy(dir, info.dli_fname, sizeof(dir));
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0';
        else safe_copy(dir, ".", sizeof(dir));
    }
    else {
        safe_copy(dir, ".", sizeof(dir));
    }
    return dir;
}

static int python_init(void)
{
    Py_Initialize();
    if (!Py_IsInitialized()) {
        vhpi_printf("resolver: ERROR - failed to initialize Python");
        return 0;
    }

    const char *plugin_dir = get_plugin_dir();
    PyObject *sys_path = PySys_GetObject("path");
    if (sys_path) {
        PyObject *dir_str = PyUnicode_FromString(plugin_dir);
        if (dir_str) {
            PyList_Insert(sys_path, 0, dir_str);
            Py_DECREF(dir_str);
        }
    }

    vhpi_printf("resolver: Python %s initialized, module path: %s",
                Py_GetVersion(), plugin_dir);

    g_py_module = PyImport_ImportModule(RESOLVER_MODULE);
    if (!g_py_module) {
        vhpi_printf("resolver: ERROR - cannot import %s", RESOLVER_MODULE);
        if (PyErr_Occurred()) {
            PyErr_Print();
            PyErr_Clear();
        }
        return 0;
    }

    g_py_func = PyObject_GetAttrString(g_py_module, RESOLVER_FUNC);
    if (!g_py_func || !PyCallable_Check(g_py_func)) {
        vhpi_printf("resolver: ERROR - %s.%s not callable",
                     RESOLVER_MODULE, RESOLVER_FUNC);
        Py_XDECREF(g_py_func);
        g_py_func = NULL;
        return 0;
    }

    vhpi_printf("resolver: loaded %s.%s", RESOLVER_MODULE, RESOLVER_FUNC);
    return 1;
}

static void python_fini(void)
{
    Py_XDECREF(g_py_func);
    Py_XDECREF(g_py_module);
    g_py_func = NULL;
    g_py_module = NULL;
    if (Py_IsInitialized())
        Py_Finalize();
}

static PyObject *build_net_dict(const net_info_t *net)
{
    PyObject *d = PyDict_New();
    if (!d) return NULL;

    PyDict_SetItemString(d, "net_name",
                         PyUnicode_FromString(net->net_name));
    PyDict_SetItemString(d, "type",
                         PyUnicode_FromString(net->type_name));
    PyDict_SetItemString(d, "elem_type",
                         PyUnicode_FromString(net->elem_type));
    PyDict_SetItemString(d, "length",
                         PyLong_FromLong(net->length));
    PyDict_SetItemString(d, "driver_ename",
                         PyUnicode_FromString(net->driver_ename));
    PyDict_SetItemString(d, "others_ename",
                         PyUnicode_FromString(net->others_ename));

    return d;
}

/*
 * Call Python resolve_net() with list of nets needing resolution.
 * Returns the VHDL string (caller must free), or NULL on error/no-op.
 */
static char *call_python_resolver(void)
{
    if (!g_python_ok || !g_py_func) {
        vhpi_printf("resolver: Python not available, skipping resolver calls");
        return NULL;
    }

    /* Build list of nets needing resolution */
    PyObject *net_list = PyList_New(0);
    if (!net_list) {
        vhpi_printf("resolver: ERROR - cannot create Python list");
        return NULL;
    }

    for (net_info_t *n = g_nets; n; n = n->next) {
        if (!n->needs_resolution) continue;
        PyObject *nd = build_net_dict(n);
        if (nd) {
            PyList_Append(net_list, nd);
            Py_DECREF(nd);
        }
    }

    Py_ssize_t list_len = PyList_Size(net_list);
    if (list_len == 0) {
        Py_DECREF(net_list);
        vhpi_printf("resolver: no nets need resolution");
        return NULL;
    }

    vhpi_printf("resolver: calling %s.%s with %zd net(s), design=%s",
                RESOLVER_MODULE, RESOLVER_FUNC, list_len, g_design_name);

    /* Call: resolve_net(net_list, design_name) */
    PyObject *py_design = PyUnicode_FromString(g_design_name);
    PyObject *args = PyTuple_Pack(2, net_list, py_design);
    PyObject *result = PyObject_CallObject(g_py_func, args);
    Py_DECREF(args);
    Py_DECREF(py_design);
    Py_DECREF(net_list);

    if (!result) {
        vhpi_printf("resolver: ERROR - Python exception in %s", RESOLVER_FUNC);
        if (PyErr_Occurred()) {
            PyErr_Print();
            PyErr_Clear();
        }
        return NULL;
    }

    if (result == Py_None) {
        Py_DECREF(result);
        vhpi_printf("resolver: Python returned None (no resolver generated)");
        return NULL;
    }

    char *vhdl = NULL;
    if (PyUnicode_Check(result)) {
        const char *s = PyUnicode_AsUTF8(result);
        if (s) vhdl = strdup(s);
    } else {
        vhpi_printf("resolver: WARNING - unexpected return type from Python");
    }

    Py_DECREF(result);
    return vhdl;
}

/* ---------- Report ---------- */

static void print_report(void)
{
    int nets_needing = 0;
    int total_nets = 0;

    vhpi_printf("");
    vhpi_printf("=== SV2VHDL Resolution Network Analysis ===");
    vhpi_printf("");

    for (net_info_t *n = g_nets; n; n = n->next) {
        total_nets++;
        if (!n->needs_resolution) continue;
        nets_needing++;

        vhpi_printf("--- Net: %s ---", n->net_name);
        vhpi_printf("  Type: %s  Length: %d", n->type_name, n->length);
        vhpi_printf("  Driver ename: %s", n->driver_ename);
        vhpi_printf("  Others ename: %s", n->others_ename);
        vhpi_printf("");
    }

    vhpi_printf("=== Summary ===");
    vhpi_printf("Total signals scanned: %d", g_total_signals);
    vhpi_printf("Total instances scanned: %d", g_total_instances);
    vhpi_printf("Total nets discovered: %d", total_nets);
    vhpi_printf("Nets requiring resolution: %d", nets_needing);
    vhpi_printf("");
}

/* ---------- Cleanup ---------- */

static void cleanup(void)
{
    net_info_t *n = g_nets;
    while (n) {
        net_info_t *nn = n->next;
        free(n);
        n = nn;
    }
    g_nets = NULL;
    g_total_signals = 0;
    g_total_instances = 0;
}

/* ---------- VHPI callbacks ---------- */

static void start_of_sim(const vhpiCbDataT *cb_data)
{
    (void)cb_data;

    vhpi_printf("");
    vhpi_printf("=== SV2VHDL Resolver Plugin ===");
    vhpi_printf("");

    vhpiHandleT root = vhpi_handle(vhpiRootInst, NULL);
    if (!root) {
        vhpi_printf("resolver: ERROR - cannot get root instance");
        return;
    }

    /* Get design name for wrapper entity generation */
    const char *root_name = get_name(root);
    if (root_name)
        safe_copy(g_design_name, root_name, sizeof(g_design_name));
    /* Lowercase it */
    for (char *c = g_design_name; *c; c++)
        *c = tolower((unsigned char)*c);

    const char *root_full = get_full_name(root);
    vhpi_printf("Root: %s (design: %s)", root_full ? root_full : "(unnamed)",
                g_design_name);
    vhpi_printf("");

    /* Phase 1: Discover resolution networks */
    vhpi_printf("--- Hierarchy Trace ---");
    g_depth = 0;
    walk_hierarchy(root);

    vhpi_printf("");
    vhpi_printf("--- Analysis ---");
    analyze_nets();

    /* Report */
    print_report();

    /* Phase 2: Check cache */
    unsigned long hash = compute_topology_hash();

    int nets_needing = 0;
    for (net_info_t *n = g_nets; n; n = n->next)
        if (n->needs_resolution) nets_needing++;

    if (nets_needing == 0) {
        vhpi_printf("resolver: no nets need resolution, nothing to generate");
        cleanup();
        vhpi_release_handle(root);
        return;
    }

    char cache_path[MAX_NAME];
    snprintf(cache_path, sizeof(cache_path), "%s/%s_resolver.vhd",
             CACHE_DIR, g_design_name);

    if (check_cache(cache_path, hash)) {
        vhpi_printf("resolver: cache hit (%s, hash %08lx)", cache_path, hash);
        vhpi_printf("resolver: for standalone run: nvc --std=2008 -e resolved_%s"
                     " && nvc --std=2008 -r resolved_%s",
                     g_design_name, g_design_name);
        cleanup();
        vhpi_release_handle(root);
        return;
    }

    vhpi_printf("resolver: cache miss (hash %08lx), generating resolver...", hash);

    /* Phase 3: Call Python to generate VHDL */
    char *vhdl = call_python_resolver();
    if (!vhdl) {
        vhpi_printf("resolver: ERROR - no VHDL generated");
        for (net_info_t *n = g_nets; n; n = n->next) {
            if (n->needs_resolution) {
                vhpi_printf("resolver: UNRESOLVED net %s (%d endpoints, type %s)",
                             n->net_name, n->length, n->type_name);
            }
        }
        cleanup();
        vhpi_release_handle(root);
        return;
    }

    vhpi_printf("resolver: received VHDL (%zu bytes)", strlen(vhdl));

    /* Phase 4: Write and compile */
    /* Try to find work directory (same dir as where the design was compiled) */
    const char *work_dir = getenv("NVC_WORK");
    if (!work_dir) work_dir = "work";

    if (write_and_compile(vhdl, cache_path, work_dir) == 0) {
        vhpi_printf("");
        vhpi_printf("resolver: resolver generated successfully!");
        vhpi_printf("resolver: for standalone simulation:");
        vhpi_printf("  nvc --std=2008 --work=%s -e resolved_%s",
                     work_dir, g_design_name);
        vhpi_printf("  nvc --std=2008 --work=%s -r resolved_%s",
                     work_dir, g_design_name);
    }

    free(vhdl);
    cleanup();
    vhpi_release_handle(root);
}

static void resolver_startup(void)
{
    vhpi_printf("resolver: plugin loaded");
    g_python_ok = python_init();

    vhpiCbDataT cb = {
        .reason = vhpiCbStartOfSimulation,
        .cb_rtn = start_of_sim,
    };
    vhpi_register_cb(&cb, vhpiReturnCb);
}

static void end_of_sim(const vhpiCbDataT *cb_data)
{
    (void)cb_data;
    python_fini();
    vhpi_printf("resolver: Python finalized");
}

static void resolver_register_cleanup(void)
{
    vhpiCbDataT cb = {
        .reason = vhpiCbEndOfSimulation,
        .cb_rtn = end_of_sim,
    };
    vhpi_register_cb(&cb, vhpiReturnCb);
}

/* ---------- VHPI entry point ---------- */

void (*vhpi_startup_routines[])() = {
    resolver_startup,
    resolver_register_cleanup,
    NULL
};
