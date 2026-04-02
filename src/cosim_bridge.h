//
//  cosim_bridge.h — Shared state between NVC cosim driver and Xyce DPWL callbacks
//
//  D2A: NVC cosim driver updates voltage; Xyce DPWL voltage source reads it.
//  A2D: Xyce DPWL zero-current source detects voltage change on its branch,
//       calls through NVC deposit handle to schedule signal update directly.
//

#ifndef _COSIM_BRIDGE_H
#define _COSIM_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_D2A 0
#define BRIDGE_A2D 1

// Callback for A2D: deposit a voltage into an NVC signal.
// The cosim driver provides this at registration time.
//   ctx      — opaque context (e.g. rt_model_t + rt_signal_t)
//   voltage  — analog voltage to deposit
//   time_s   — Xyce simulation time (seconds)
typedef void (*bridge_deposit_fn)(void *ctx, double voltage, double time_s);

// Register a boundary signal.
// For A2D, deposit_fn/deposit_ctx are the NVC deposit callback.
// Returns an index, or -1 on error.
int cosim_bridge_register(const char *name, int dir,
                          double initial_voltage,
                          bridge_deposit_fn deposit_fn,
                          void *deposit_ctx);

// Update a D2A signal's value (called by NVC cosim driver before Xyce step).
void cosim_bridge_update_d2a(int idx, double voltage,
                             double next_v, double next_time);

// Reset all registrations.
void cosim_bridge_reset(void);

#ifdef __cplusplus
}
#endif

#endif // _COSIM_BRIDGE_H
