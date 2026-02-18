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

/* NVC extension: port map and implicit type discovery */
extern const vhpiCharT *nvc_vhpi_get_port_map(vhpiHandleT inst_handle);
extern const vhpiCharT *nvc_vhpi_get_driver_type(vhpiHandleT inst_handle,
                                                  const vhpiCharT *port_name);

/* ---------- Configuration ---------- */

#define MAX_NAME  512
#define MAX_TYPE  128
#define MAX_VAL   1024

/* Tran-like entity names (bidirectional switches using 'driver/'other) */
static const char *tran_entities[] = {
    "SV_TRAN", "SV_TRANIF0", "SV_TRANIF1",
    "SV_RTRAN", "SV_RTRANIF0", "SV_RTRANIF1",
    NULL
};

#define RESOLVER_MODULE  "sv2vhdl_resolver"
#define RESOLVER_FUNC    "resolve_net"

#define CACHE_DIR        "_sv2vhdl_cache"
#define VHPI_MODULE      "_sv2vhdl_vhpi"

/* ---------- Data structures ---------- */

#define MAX_ENDPOINTS 64

/*
 * One endpoint on a net: a 'driver signal paired with a 'receiver (or 'other).
 */
typedef struct endpoint {
    char driver_ename[MAX_NAME];   /* external name path: ".top.inst.port.driver" */
    char receiver_ename[MAX_NAME]; /* external name path: ".top.inst.port.other" */
    char type_name[MAX_TYPE];      /* signal type */
} endpoint_t;

/*
 * A net requiring resolution: has a list of driver signals and
 * a corresponding list of receiver signals.
 */
typedef struct net_info {
    char net_name[MAX_NAME];
    endpoint_t endpoints[MAX_ENDPOINTS];
    int  n_endpoints;
    int  needs_resolution;
    struct net_info *next;
} net_info_t;

/* ---------- Global state ---------- */

static net_info_t *g_nets = NULL;
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

static void safe_copy(char *dst, const char *src, size_t dstsz)
{
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if (len >= dstsz) len = dstsz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
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

static int net_add_endpoint(net_info_t *net,
                            const char *driver_ename,
                            const char *receiver_ename,
                            const char *type_name)
{
    if (net->n_endpoints >= MAX_ENDPOINTS) {
        vhpi_printf("resolver: too many endpoints on net %s", net->net_name);
        return -1;
    }
    endpoint_t *ep = &net->endpoints[net->n_endpoints++];
    safe_copy(ep->driver_ename, driver_ename, sizeof(ep->driver_ename));
    safe_copy(ep->receiver_ename, receiver_ename, sizeof(ep->receiver_ename));
    safe_copy(ep->type_name, type_name, sizeof(ep->type_name));
    return 0;
}

static int is_tran_entity(const char *entity_name)
{
    for (const char **p = tran_entities; *p; p++)
        if (streqi(entity_name, *p))
            return 1;
    return 0;
}

/*
 * Parse a port map string from nvc_vhpi_get_port_map().
 * Format: "FORMAL1=ACTUAL1;FORMAL2=ACTUAL2;..."
 * Looks up the given formal_name and copies the actual name to dst.
 * Returns 1 on success, 0 if not found.
 */
static int portmap_lookup(const char *portmap, const char *formal_name,
                          char *dst, size_t dstsz)
{
    if (!portmap || !formal_name) return 0;

    const char *p = portmap;
    size_t flen = strlen(formal_name);

    while (*p) {
        /* Match formal name (case-insensitive) */
        const char *eq = strchr(p, '=');
        if (!eq) break;

        size_t name_len = (size_t)(eq - p);
        int match = (name_len == flen);
        if (match) {
            for (size_t i = 0; i < flen; i++) {
                if (tolower((unsigned char)p[i]) !=
                    tolower((unsigned char)formal_name[i])) {
                    match = 0;
                    break;
                }
            }
        }

        /* Find end of actual (next ';' or end of string) */
        const char *actual_start = eq + 1;
        const char *semi = strchr(actual_start, ';');
        size_t actual_len = semi ? (size_t)(semi - actual_start)
                                 : strlen(actual_start);

        if (match) {
            if (actual_len >= dstsz) actual_len = dstsz - 1;
            memcpy(dst, actual_start, actual_len);
            dst[actual_len] = '\0';
            return 1;
        }

        p = semi ? semi + 1 : actual_start + actual_len;
    }
    return 0;
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

/*
 * Scan instances in a region.  For each instance whose entity is a
 * tran-like primitive, use nvc_vhpi_get_port_map() to discover the
 * actual signal each inout port connects to, and group endpoints
 * by actual signal (the "net").
 *
 * Example: tran instances with port map(a => ac(2), b => ac(3))
 *   - Signal AC(2) gets an endpoint: {tc.a.driver, tc.a.other}
 *   - Signal AC(3) gets an endpoint: {tc.b.driver, tc.b.other}
 *
 * path_prefix is the accumulated hierarchical ename path, e.g.
 * ".test_tran_str.gen_chain(1)"
 *
 * sig_prefix is the ename path of the scope where the actual signals
 * are declared (the enclosing architecture), e.g. ".test_tran_str"
 */
static void scan_instances(vhpiHandleT region, const char *path_prefix,
                           const char *sig_prefix)
{
    vhpiHandleT iter = vhpi_iterator(vhpiCompInstStmts, region);
    if (!iter) return;

    for (vhpiHandleT inst = vhpi_scan(iter); inst; inst = vhpi_scan(iter)) {
        const char *inst_name = get_name(inst);
        g_total_instances++;

        /* Get entity name */
        const char *entity_name = NULL;
        vhpiHandleT du = vhpi_handle(vhpiDesignUnit, inst);
        if (du) {
            vhpiHandleT entity = vhpi_handle(vhpiPrimaryUnit, du);
            if (entity) {
                entity_name = get_name(entity);
                vhpi_release_handle(entity);
            }
            vhpi_release_handle(du);
        }

        /* Build full instance ename path */
        char inst_ename[MAX_NAME];
        char inst_lower[MAX_NAME];
        safe_copy(inst_lower, inst_name ? inst_name : "?", sizeof(inst_lower));
        for (char *c = inst_lower; *c; c++)
            *c = tolower((unsigned char)*c);
        snprintf(inst_ename, sizeof(inst_ename), "%s.%s",
                 path_prefix, inst_lower);

        indent();
        vhpi_printf("  instance: %s  entity: %s  ename: %s",
                     inst_name ? inst_name : "?",
                     entity_name ? entity_name : "?",
                     inst_ename);

        if (!entity_name || !is_tran_entity(entity_name)) {
            vhpi_release_handle(inst);
            continue;
        }

        /* Get port map from NVC extension */
        const vhpiCharT *portmap =
            (const vhpiCharT *)nvc_vhpi_get_port_map(inst);
        if (!portmap) {
            indent();
            vhpi_printf("    WARNING: no port map for tran instance");
            vhpi_release_handle(inst);
            continue;
        }

        indent();
        vhpi_printf("    portmap: %s", (const char *)portmap);

        /* Scan inout ports, using port map to identify actual signals */
        vhpiHandleT piter = vhpi_iterator(vhpiPortDecls, inst);
        if (!piter) {
            vhpi_release_handle(inst);
            continue;
        }

        for (vhpiHandleT port = vhpi_scan(piter); port;
             port = vhpi_scan(piter)) {
            vhpiModeT mode = (vhpiModeT)vhpi_get(vhpiModeP, port);
            if (mode != vhpiInoutMode) {
                vhpi_release_handle(port);
                continue;
            }

            const char *port_name = get_name(port);
            if (!port_name) {
                vhpi_release_handle(port);
                continue;
            }

            /* Get implicit signal type (actual 'driver type, not port type).
             * Falls back to port element type if no implicit signal found. */
            char port_type[MAX_TYPE], port_etype[MAX_TYPE];
            get_type_info(port, port_type, sizeof(port_type),
                          port_etype, sizeof(port_etype));

            const vhpiCharT *drv_type =
                nvc_vhpi_get_driver_type(inst, (const vhpiCharT *)port_name);
            if (drv_type)
                safe_copy(port_etype, (const char *)drv_type,
                          sizeof(port_etype));

            char port_lower[MAX_NAME];
            safe_copy(port_lower, port_name, sizeof(port_lower));
            for (char *c = port_lower; *c; c++)
                *c = tolower((unsigned char)*c);

            /* Look up actual signal from port map */
            char actual[MAX_NAME];
            if (!portmap_lookup((const char *)portmap, port_name,
                                actual, sizeof(actual))) {
                indent();
                vhpi_printf("    WARNING: port %s not in port map", port_name);
                vhpi_release_handle(port);
                continue;
            }

            /* Lowercase the actual signal name */
            for (char *c = actual; *c; c++)
                *c = tolower((unsigned char)*c);

            /* Build net name: sig_prefix + "." + actual_signal
             * e.g. ".test_tran_str.ac(2)" */
            char net_name[MAX_NAME];
            snprintf(net_name, sizeof(net_name), "%s.%s",
                     sig_prefix, actual);

            /* External name paths for implicit signals:
             * .top.inst.port.driver and .top.inst.port.other */
            char drv_ename[MAX_NAME], rcv_ename[MAX_NAME];
            snprintf(drv_ename, sizeof(drv_ename), "%s.%s.driver",
                     inst_ename, port_lower);
            snprintf(rcv_ename, sizeof(rcv_ename), "%s.%s.other",
                     inst_ename, port_lower);

            /* Group by actual signal: all tran ports connecting
             * to the same signal form one resolution group */
            net_info_t *net = find_or_create_net(net_name);
            if (net) {
                net_add_endpoint(net, drv_ename, rcv_ename, port_etype);
                indent();
                vhpi_printf("    port %s -> actual=%s net=%s",
                             port_name, actual, net_name);
                vhpi_printf("      drv=%s rcv=%s", drv_ename, rcv_ename);
            }

            vhpi_release_handle(port);
        }
        vhpi_release_handle(piter);
        vhpi_release_handle(inst);
    }
    vhpi_release_handle(iter);
}

/*
 * Walk the hierarchy recursively.
 * path_prefix: accumulated ename of this region (e.g. ".test.gen(1)")
 * sig_prefix:  ename of the scope where actual port-map signals live
 *              (typically the enclosing architecture, e.g. ".test")
 */
static void walk_hierarchy(vhpiHandleT region, const char *path_prefix,
                           const char *sig_prefix)
{
    const char *rname = get_name(region);
    indent();
    vhpi_printf("region: %s  path: %s  sig_prefix: %s",
                rname ? rname : "(root)", path_prefix, sig_prefix);

    scan_instances(region, path_prefix, sig_prefix);

    vhpiHandleT riter = vhpi_iterator(vhpiInternalRegions, region);
    if (riter) {
        for (vhpiHandleT sub = vhpi_scan(riter); sub;
             sub = vhpi_scan(riter)) {
            const char *sub_name = get_name(sub);
            char sub_path[MAX_NAME];
            char sub_lower[MAX_NAME];
            safe_copy(sub_lower, sub_name ? sub_name : "?",
                      sizeof(sub_lower));
            for (char *c = sub_lower; *c; c++)
                *c = tolower((unsigned char)*c);
            snprintf(sub_path, sizeof(sub_path), "%s.%s",
                     path_prefix, sub_lower);

            /* Determine sig_prefix for the sub-region.
             * For generate blocks and block statements, actual signals
             * in port maps still reference the enclosing architecture,
             * so sig_prefix stays the same.
             * For component instances (entity architectures), the
             * sig_prefix becomes the instance path. */
            vhpiIntT kind = vhpi_get(vhpiKindP, sub);
            const char *sub_sig_prefix;
            if (kind == vhpiCompInstStmtK || kind == vhpiRootInstK)
                sub_sig_prefix = sub_path;  /* entering a new entity */
            else
                sub_sig_prefix = sig_prefix;  /* generate/block: same scope */

            g_depth++;
            walk_hierarchy(sub, sub_path, sub_sig_prefix);
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
        n->needs_resolution = (n->n_endpoints >= 2);
    }
}


/* ---------- VHPI bridge for Python ---------- */

/*
 * Python module: _sv2vhdl_vhpi
 *
 * Gives Python code direct VHPI access to explore the design hierarchy.
 * Uses vhpi_handle_by_name() for navigation (NVC accepts both ":" and "."
 * delimiters, so enames work directly).
 *
 * Functions:
 *   get_signals(region_path)   -> list of signal dicts
 *   get_instances(region_path) -> list of instance dicts
 *   get_generics(path)         -> dict of generic name -> value
 *   get_value(signal_path)     -> string value
 *   get_signal_info(path)      -> dict with signal properties
 */

/*
 * Resolve a path to a VHPI handle.
 * Accepts ename format (.foo.bar) or VHPI format (:FOO:BAR).
 * Returns NULL with Python exception set on failure.
 */
static vhpiHandleT resolve_path(const char *path)
{
    if (!path || !*path) {
        PyErr_SetString(PyExc_ValueError, "empty path");
        return NULL;
    }

    /* Skip leading dot for enames — vhpi_handle_by_name handles both */
    const char *p = path;
    if (*p == '.') p++;

    vhpiHandleT h = vhpi_handle_by_name(p, NULL);
    if (!h) {
        PyErr_Format(PyExc_KeyError, "VHPI path not found: %s", path);
        return NULL;
    }
    return h;
}

/*
 * get_signals(region_path) -> list of dicts
 *
 * Each dict: {name, full_name, ename, type, elem_type, size}
 */
static PyObject *py_vhpi_get_signals(PyObject *self, PyObject *args)
{
    (void)self;
    const char *path;
    if (!PyArg_ParseTuple(args, "s", &path))
        return NULL;

    vhpiHandleT region = resolve_path(path);
    if (!region) return NULL;

    PyObject *result = PyList_New(0);
    if (!result) { vhpi_release_handle(region); return NULL; }

    vhpiHandleT iter = vhpi_iterator(vhpiSigDecls, region);
    if (!iter) {
        vhpi_release_handle(region);
        return result;  /* empty list — no signals */
    }

    for (vhpiHandleT sig = vhpi_scan(iter); sig; sig = vhpi_scan(iter)) {
        const char *name = get_name(sig);
        const char *full = get_full_name(sig);
        int size = vhpi_get(vhpiSizeP, sig);
        if (size <= 0) size = 1;

        char sig_type[MAX_TYPE], elem_type[MAX_TYPE], ename[MAX_NAME];
        get_type_info(sig, sig_type, sizeof(sig_type),
                      elem_type, sizeof(elem_type));
        if (full)
            vhpi_to_ename(full, ename, sizeof(ename));
        else
            ename[0] = '\0';

        PyObject *d = PyDict_New();
        if (d) {
            PyDict_SetItemString(d, "name",
                PyUnicode_FromString(name ? name : ""));
            PyDict_SetItemString(d, "full_name",
                PyUnicode_FromString(full ? full : ""));
            PyDict_SetItemString(d, "ename",
                PyUnicode_FromString(ename));
            PyDict_SetItemString(d, "type",
                PyUnicode_FromString(sig_type));
            PyDict_SetItemString(d, "elem_type",
                PyUnicode_FromString(elem_type));
            PyDict_SetItemString(d, "size",
                PyLong_FromLong(size));
            PyList_Append(result, d);
            Py_DECREF(d);
        }
        vhpi_release_handle(sig);
    }
    vhpi_release_handle(iter);
    vhpi_release_handle(region);
    return result;
}

/*
 * get_instances(region_path) -> list of dicts
 *
 * Each dict: {name, full_name, ename, entity}
 */
static PyObject *py_vhpi_get_instances(PyObject *self, PyObject *args)
{
    (void)self;
    const char *path;
    if (!PyArg_ParseTuple(args, "s", &path))
        return NULL;

    vhpiHandleT region = resolve_path(path);
    if (!region) return NULL;

    PyObject *result = PyList_New(0);
    if (!result) { vhpi_release_handle(region); return NULL; }

    vhpiHandleT iter = vhpi_iterator(vhpiCompInstStmts, region);
    if (!iter) {
        vhpi_release_handle(region);
        return result;
    }

    for (vhpiHandleT inst = vhpi_scan(iter); inst; inst = vhpi_scan(iter)) {
        const char *name = get_name(inst);
        const char *full = get_full_name(inst);

        /* Get entity name */
        const char *entity_name = "";
        vhpiHandleT du = vhpi_handle(vhpiDesignUnit, inst);
        if (du) {
            vhpiHandleT entity = vhpi_handle(vhpiPrimaryUnit, du);
            if (entity) {
                const char *en = get_name(entity);
                if (en) entity_name = en;
                vhpi_release_handle(entity);
            }
            vhpi_release_handle(du);
        }

        char ename[MAX_NAME];
        if (full)
            vhpi_to_ename(full, ename, sizeof(ename));
        else
            ename[0] = '\0';

        PyObject *d = PyDict_New();
        if (d) {
            PyDict_SetItemString(d, "name",
                PyUnicode_FromString(name ? name : ""));
            PyDict_SetItemString(d, "full_name",
                PyUnicode_FromString(full ? full : ""));
            PyDict_SetItemString(d, "ename",
                PyUnicode_FromString(ename));
            PyDict_SetItemString(d, "entity",
                PyUnicode_FromString(entity_name));
            PyList_Append(result, d);
            Py_DECREF(d);
        }
        vhpi_release_handle(inst);
    }
    vhpi_release_handle(iter);
    vhpi_release_handle(region);
    return result;
}

/*
 * get_generics(path) -> dict of {name: value}
 *
 * Reads generic constants from an instance or entity.
 * Values returned as Python int, float, or string depending on VHPI type.
 */
static PyObject *py_vhpi_get_generics(PyObject *self, PyObject *args)
{
    (void)self;
    const char *path;
    if (!PyArg_ParseTuple(args, "s", &path))
        return NULL;

    vhpiHandleT obj = resolve_path(path);
    if (!obj) return NULL;

    PyObject *result = PyDict_New();
    if (!result) { vhpi_release_handle(obj); return NULL; }

    vhpiHandleT iter = vhpi_iterator(vhpiGenericDecls, obj);
    if (!iter) {
        vhpi_release_handle(obj);
        return result;  /* empty dict */
    }

    for (vhpiHandleT gen = vhpi_scan(iter); gen; gen = vhpi_scan(iter)) {
        const char *gname = get_name(gen);
        if (!gname) {
            vhpi_release_handle(gen);
            continue;
        }

        /* Try reading as integer first */
        vhpiValueT val;
        memset(&val, 0, sizeof(val));
        val.format = vhpiIntVal;
        if (vhpi_get_value(gen, &val) == 0) {
            PyDict_SetItemString(result, gname,
                PyLong_FromLong(val.value.intg));
            vhpi_release_handle(gen);
            continue;
        }

        /* Try as real */
        memset(&val, 0, sizeof(val));
        val.format = vhpiRealVal;
        if (vhpi_get_value(gen, &val) == 0) {
            PyDict_SetItemString(result, gname,
                PyFloat_FromDouble(val.value.real));
            vhpi_release_handle(gen);
            continue;
        }

        /* Fall back to string */
        char strbuf[MAX_VAL];
        memset(&val, 0, sizeof(val));
        val.format = vhpiStrVal;
        val.bufSize = sizeof(strbuf);
        val.value.str = (vhpiCharT *)strbuf;
        if (vhpi_get_value(gen, &val) == 0) {
            PyDict_SetItemString(result, gname,
                PyUnicode_FromString(strbuf));
        } else {
            /* Can't read — store None */
            PyDict_SetItemString(result, gname, Py_None);
        }

        vhpi_release_handle(gen);
    }
    vhpi_release_handle(iter);
    vhpi_release_handle(obj);
    return result;
}

/*
 * get_value(signal_path) -> string
 *
 * Reads the current value of a signal as a string.
 * For std_logic: "1", "0", "X", "Z", "U", etc.
 * For vectors: "10XZ" etc.
 */
static PyObject *py_vhpi_get_value(PyObject *self, PyObject *args)
{
    (void)self;
    const char *path;
    if (!PyArg_ParseTuple(args, "s", &path))
        return NULL;

    vhpiHandleT sig = resolve_path(path);
    if (!sig) return NULL;

    char strbuf[MAX_VAL];
    vhpiValueT val;
    memset(&val, 0, sizeof(val));
    val.format = vhpiStrVal;
    val.bufSize = sizeof(strbuf);
    val.value.str = (vhpiCharT *)strbuf;

    if (vhpi_get_value(sig, &val) != 0) {
        vhpi_release_handle(sig);
        PyErr_Format(PyExc_RuntimeError,
                     "vhpi_get_value failed for: %s", path);
        return NULL;
    }

    vhpi_release_handle(sig);
    return PyUnicode_FromString(strbuf);
}

/*
 * get_signal_info(signal_path) -> dict
 *
 * Returns detailed info about a single signal:
 * {name, full_name, ename, type, elem_type, size, value}
 */
static PyObject *py_vhpi_get_signal_info(PyObject *self, PyObject *args)
{
    (void)self;
    const char *path;
    if (!PyArg_ParseTuple(args, "s", &path))
        return NULL;

    vhpiHandleT sig = resolve_path(path);
    if (!sig) return NULL;

    const char *name = get_name(sig);
    const char *full = get_full_name(sig);
    int size = vhpi_get(vhpiSizeP, sig);
    if (size <= 0) size = 1;

    char sig_type[MAX_TYPE], elem_type[MAX_TYPE], ename[MAX_NAME];
    get_type_info(sig, sig_type, sizeof(sig_type),
                  elem_type, sizeof(elem_type));
    if (full)
        vhpi_to_ename(full, ename, sizeof(ename));
    else
        ename[0] = '\0';

    /* Read value */
    char valbuf[MAX_VAL];
    vhpiValueT val;
    memset(&val, 0, sizeof(val));
    val.format = vhpiStrVal;
    val.bufSize = sizeof(valbuf);
    val.value.str = (vhpiCharT *)valbuf;
    int got_val = (vhpi_get_value(sig, &val) == 0);

    PyObject *d = PyDict_New();
    if (d) {
        PyDict_SetItemString(d, "name",
            PyUnicode_FromString(name ? name : ""));
        PyDict_SetItemString(d, "full_name",
            PyUnicode_FromString(full ? full : ""));
        PyDict_SetItemString(d, "ename",
            PyUnicode_FromString(ename));
        PyDict_SetItemString(d, "type",
            PyUnicode_FromString(sig_type));
        PyDict_SetItemString(d, "elem_type",
            PyUnicode_FromString(elem_type));
        PyDict_SetItemString(d, "size",
            PyLong_FromLong(size));
        PyDict_SetItemString(d, "value",
            got_val ? PyUnicode_FromString(valbuf)
                    : PyUnicode_FromString("?"));
    }

    vhpi_release_handle(sig);
    return d;
}

/* Module method table */
static PyMethodDef vhpi_methods[] = {
    {"get_signals",     py_vhpi_get_signals,     METH_VARARGS,
     "get_signals(region_path) -> list of signal dicts in a region"},
    {"get_instances",   py_vhpi_get_instances,   METH_VARARGS,
     "get_instances(region_path) -> list of instance dicts in a region"},
    {"get_generics",    py_vhpi_get_generics,    METH_VARARGS,
     "get_generics(path) -> dict of generic name -> value"},
    {"get_value",       py_vhpi_get_value,       METH_VARARGS,
     "get_value(signal_path) -> string value of a signal"},
    {"get_signal_info", py_vhpi_get_signal_info, METH_VARARGS,
     "get_signal_info(signal_path) -> dict with full signal properties"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef vhpi_module_def = {
    PyModuleDef_HEAD_INIT,
    VHPI_MODULE,                          /* m_name */
    "VHPI bridge for Python — explore "
    "design hierarchy from sv2vhdl",      /* m_doc */
    -1,                                   /* m_size */
    vhpi_methods,                         /* m_methods */
    NULL, NULL, NULL, NULL                /* m_slots, traverse, clear, free */
};

static PyObject *PyInit_sv2vhdl_vhpi(void)
{
    return PyModule_Create(&vhpi_module_def);
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
    /* Register VHPI bridge module BEFORE Py_Initialize */
    PyImport_AppendInittab(VHPI_MODULE, PyInit_sv2vhdl_vhpi);

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

    /* Build lists of driver and receiver external name paths + types */
    PyObject *drivers = PyList_New(0);
    PyObject *receivers = PyList_New(0);

    for (int i = 0; i < net->n_endpoints; i++) {
        const endpoint_t *ep = &net->endpoints[i];

        PyObject *drv = PyDict_New();
        PyDict_SetItemString(drv, "ename",
                             PyUnicode_FromString(ep->driver_ename));
        PyDict_SetItemString(drv, "type",
                             PyUnicode_FromString(ep->type_name));
        PyList_Append(drivers, drv);
        Py_DECREF(drv);

        PyObject *rcv = PyDict_New();
        PyDict_SetItemString(rcv, "ename",
                             PyUnicode_FromString(ep->receiver_ename));
        PyDict_SetItemString(rcv, "type",
                             PyUnicode_FromString(ep->type_name));
        PyList_Append(receivers, rcv);
        Py_DECREF(rcv);
    }

    PyDict_SetItemString(d, "drivers", drivers);
    PyDict_SetItemString(d, "receivers", receivers);
    Py_DECREF(drivers);
    Py_DECREF(receivers);

    return d;
}

/*
 * Call Python resolve_net() with list of nets needing resolution.
 * Returns a PyDict {filename: vhdl_string} (new reference), or NULL.
 * Caller must Py_DECREF the result.
 */
static PyObject *call_python_resolver(void)
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

    if (!PyDict_Check(result)) {
        vhpi_printf("resolver: WARNING - expected dict from Python, got %s",
                     Py_TYPE(result)->tp_name);
        Py_DECREF(result);
        return NULL;
    }

    return result;  /* caller must Py_DECREF */
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

        vhpi_printf("--- Net: %s  (%d endpoints) ---",
                     n->net_name, n->n_endpoints);
        for (int i = 0; i < n->n_endpoints; i++) {
            vhpi_printf("  [%d] driver:   %s  type: %s",
                         i, n->endpoints[i].driver_ename,
                         n->endpoints[i].type_name);
            vhpi_printf("      receiver: %s",
                         n->endpoints[i].receiver_ename);
        }
        vhpi_printf("");
    }

    vhpi_printf("=== Summary ===");
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
    char root_ename[MAX_NAME];
    snprintf(root_ename, sizeof(root_ename), ".%s", g_design_name);
    walk_hierarchy(root, root_ename, root_ename);

    vhpi_printf("");
    vhpi_printf("--- Analysis ---");
    analyze_nets();

    /* Report */
    print_report();

    /* Phase 2: Count nets needing resolution */
    int nets_needing = 0;
    for (net_info_t *n = g_nets; n; n = n->next)
        if (n->needs_resolution) nets_needing++;

    if (nets_needing == 0) {
        vhpi_printf("resolver: no nets need resolution, nothing to generate");
        cleanup();
        vhpi_release_handle(root);
        return;
    }

    /* Phase 3: Call Python to generate per-net VHDL files */
    PyObject *file_dict = call_python_resolver();
    if (!file_dict) {
        vhpi_printf("resolver: ERROR - no VHDL generated");
        for (net_info_t *n = g_nets; n; n = n->next) {
            if (n->needs_resolution) {
                vhpi_printf("resolver: UNRESOLVED net %s (%d endpoints)",
                             n->net_name, n->n_endpoints);
            }
        }
        cleanup();
        vhpi_release_handle(root);
        return;
    }

    Py_ssize_t n_files = PyDict_Size(file_dict);
    vhpi_printf("resolver: received %zd VHDL file(s)", n_files);

    /* Phase 4: Write files and optionally compile (per-file caching) */
    const char *resolver_dir = getenv("NVC_RESOLVER_DIR");
    if (!resolver_dir) resolver_dir = CACHE_DIR;
    mkdir(resolver_dir, 0755);
    const char *rcmode = getenv("NVC_RCMODE");
    const int skip_compile = (rcmode && strcmp(rcmode, "none") == 0);
    const char *work_dir = getenv("NVC_WORK");
    if (!work_dir) work_dir = "work";

    int written = 0, cached = 0, compiled = 0, errors = 0;

    PyObject *py_fname, *py_vhdl;
    Py_ssize_t pos = 0;
    while (PyDict_Next(file_dict, &pos, &py_fname, &py_vhdl)) {
        const char *fname = PyUnicode_AsUTF8(py_fname);
        const char *vhdl = PyUnicode_AsUTF8(py_vhdl);
        if (!fname || !vhdl) continue;

        char cache_path[MAX_NAME];
        snprintf(cache_path, sizeof(cache_path), "%s/%s",
                 resolver_dir, fname);

        /* Per-file cache: compare first line (net hash comment) */
        {
            FILE *existing = fopen(cache_path, "r");
            if (existing) {
                char old_line[256], new_line[256];
                if (fgets(old_line, sizeof(old_line), existing)) {
                    /* Extract first line from new VHDL */
                    const char *nl = strchr(vhdl, '\n');
                    size_t len = nl ? (size_t)(nl - vhdl) : strlen(vhdl);
                    if (len >= sizeof(new_line)) len = sizeof(new_line) - 1;
                    memcpy(new_line, vhdl, len);
                    new_line[len] = '\0';

                    /* Strip newlines for comparison */
                    size_t olen = strlen(old_line);
                    if (olen > 0 && old_line[olen-1] == '\n')
                        old_line[olen-1] = '\0';

                    if (strcmp(old_line, new_line) == 0) {
                        fclose(existing);
                        cached++;
                        continue;  /* file unchanged */
                    }
                }
                fclose(existing);
            }
        }

        /* Write the file */
        FILE *f = fopen(cache_path, "w");
        if (!f) {
            vhpi_printf("resolver: ERROR - cannot write %s: %s",
                         cache_path, strerror(errno));
            errors++;
            continue;
        }
        fputs(vhdl, f);
        fclose(f);
        written++;

        /* Compile if not in rcmode=none */
        if (!skip_compile) {
            char cmd[MAX_NAME * 2];
            snprintf(cmd, sizeof(cmd),
                     "nvc --std=2008 --work=%s -a %s 2>&1",
                     work_dir, cache_path);

            FILE *proc = popen(cmd, "r");
            if (!proc) {
                vhpi_printf("resolver: ERROR - cannot spawn nvc for %s",
                             fname);
                errors++;
                continue;
            }

            char line[512];
            while (fgets(line, sizeof(line), proc)) {
                size_t len = strlen(line);
                if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
                vhpi_printf("  nvc: %s", line);
            }

            int status = pclose(proc);
            if (status != 0) {
                vhpi_printf("resolver: ERROR - compilation failed for %s",
                             fname);
                errors++;
            } else {
                compiled++;
            }
        }
    }

    vhpi_printf("");
    if (skip_compile) {
        vhpi_printf("resolver: --rcmode=none: wrote %d file(s), "
                     "%d cached, %d error(s)", written, cached, errors);
        vhpi_printf("resolver: output directory: %s", resolver_dir);
        vhpi_printf("resolver: to compile and run manually:");
        vhpi_printf("  nvc --std=2008 -a %s/%s_rn_*.vhd %s/%s_wrapper.vhd",
                     resolver_dir, g_design_name, resolver_dir, g_design_name);
        vhpi_printf("  nvc --std=2008 -e resolved_%s", g_design_name);
        vhpi_printf("  nvc --std=2008 -r resolved_%s", g_design_name);
    } else {
        vhpi_printf("resolver: wrote %d, cached %d, compiled %d, errors %d",
                     written, cached, compiled, errors);
        if (errors == 0) {
            vhpi_printf("resolver: for standalone simulation:");
            vhpi_printf("  nvc --std=2008 --work=%s -e resolved_%s",
                         work_dir, g_design_name);
            vhpi_printf("  nvc --std=2008 --work=%s -r resolved_%s",
                         work_dir, g_design_name);
        }
    }

    Py_DECREF(file_dict);
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
