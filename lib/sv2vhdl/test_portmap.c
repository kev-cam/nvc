/*
 * test_portmap.c -- Minimal VHPI plugin to test nvc_vhpi_get_port_map()
 *                   and nvc_vhpi_get_driver_type()
 *
 * Build:  gcc -shared -fPIC -Wall -I/usr/local/include -o libtest_portmap.so test_portmap.c
 * Usage:  nvc --std=2040 -r --load=./libtest_portmap.so test_tran_str
 */
#include "vhpi_user.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* NVC extension functions */
extern const vhpiCharT *nvc_vhpi_get_port_map(vhpiHandleT inst_handle);
extern const vhpiCharT *nvc_vhpi_get_driver_type(vhpiHandleT inst_handle,
                                                  const vhpiCharT *port_name);

static void walk(vhpiHandleT region, int depth)
{
    vhpiHandleT iter = vhpi_iterator(vhpiCompInstStmts, region);
    if (iter) {
        for (vhpiHandleT inst = vhpi_scan(iter); inst; inst = vhpi_scan(iter)) {
            const char *iname = (const char *)vhpi_get_str(vhpiNameP, inst);

            /* Get entity name */
            const char *entity_name = "";
            vhpiHandleT du = vhpi_handle(vhpiDesignUnit, inst);
            if (du) {
                vhpiHandleT entity = vhpi_handle(vhpiPrimaryUnit, du);
                if (entity) {
                    entity_name = (const char *)vhpi_get_str(vhpiNameP, entity);
                    vhpi_release_handle(entity);
                }
                vhpi_release_handle(du);
            }

            /* Port map */
            const vhpiCharT *pm = nvc_vhpi_get_port_map(inst);

            /* Implicit signal types */
            const vhpiCharT *drv_a = nvc_vhpi_get_driver_type(inst,
                                         (const vhpiCharT *)"A");
            const vhpiCharT *drv_b = nvc_vhpi_get_driver_type(inst,
                                         (const vhpiCharT *)"B");

            vhpi_printf("  [%d] %s (%s) portmap={%s}",
                        depth, iname ? iname : "?",
                        entity_name ? entity_name : "?",
                        pm ? (const char *)pm : "NULL");
            vhpi_printf("      A'driver type=%s  B'driver type=%s",
                        drv_a ? (const char *)drv_a : "NULL",
                        drv_b ? (const char *)drv_b : "NULL");

            /* Only show first instance at each depth for brevity */
            if (depth >= 1 && strstr(entity_name, "SV_TRAN")) {
                vhpi_release_handle(inst);
                while ((inst = vhpi_scan(iter)) != NULL)
                    vhpi_release_handle(inst);
                break;
            }

            vhpi_release_handle(inst);
        }
        vhpi_release_handle(iter);
    }

    vhpiHandleT riter = vhpi_iterator(vhpiInternalRegions, region);
    if (riter) {
        for (vhpiHandleT sub = vhpi_scan(riter); sub; sub = vhpi_scan(riter)) {
            walk(sub, depth + 1);
            vhpi_release_handle(sub);
        }
        vhpi_release_handle(riter);
    }
}

static void start_of_sim(const vhpiCbDataT *cb_data)
{
    (void)cb_data;
    vhpi_printf("=== test_portmap: testing port map + driver types ===");

    vhpiHandleT root = vhpi_handle(vhpiRootInst, NULL);
    if (!root) {
        vhpi_printf("ERROR: no root instance");
        return;
    }

    walk(root, 0);
    vhpi_release_handle(root);

    vhpi_printf("=== done ===");
    vhpi_control(vhpiFinish, 0);
}

static void startup(void)
{
    vhpiCbDataT cb = {
        .reason = vhpiCbStartOfSimulation,
        .cb_rtn = start_of_sim,
    };
    vhpi_register_cb(&cb, vhpiReturnCb);
}

void (*vhpi_startup_routines[])() = { startup, NULL };
