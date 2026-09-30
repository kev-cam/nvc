//
//  NVC↔Xyce mixed-signal co-simulation
//

#ifndef _COSIM_H
#define _COSIM_H

#include "prim.h"
#include "rt/rt.h"

// Analog engine driven through its C interface library
typedef enum {
   COSIM_XYCE,     // libxycecinterface.so, xyce_* entry points
   COSIM_VACASK,   // libvacaskcinterface.so, vacask_* entry points
} cosim_engine_t;

int cosim_run(rt_model_t *m, cosim_engine_t engine, const char *xyce_netlist,
              const char *xyce_config, uint64_t stop_time);

#endif  // _COSIM_H
