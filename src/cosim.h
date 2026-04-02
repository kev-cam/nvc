//
//  NVC↔Xyce mixed-signal co-simulation
//

#ifndef _COSIM_H
#define _COSIM_H

#include "prim.h"
#include "rt/rt.h"

int cosim_run(rt_model_t *m, const char *xyce_netlist,
              const char *xyce_config, uint64_t stop_time);

#endif  // _COSIM_H
