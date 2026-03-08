/*
 * sv_analog.c — VHPIDIRECT implementation for sv_analog_pkg
 *
 * Phase 1: Collects analog block strings and prints them to stderr.
 * Phase 2 (future): Write .va file, invoke OpenVAF/ngSpice.
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

static char *analog_blocks[MAX_ANALOG_BLOCKS];
static int analog_block_count = 0;

/*
 * sv_analog_eval — called for each sv_analog("...") concurrent call.
 * Stores the block text and prints it to stderr.
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

    fprintf(stderr, "[sv_analog] Block %d: %s\n", analog_block_count, copy);
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
