//
//  cosim_bridge.h — Shared state between the NVC cosim driver and the analog
//  engine's bridged sources (Xyce DPWL callbacks / VACASK external sources).
//
//  The analog solver is the master scheduler.  At every converged candidate
//  step t_n -> t_{n+1} it hands the bridge the node voltages at both ends
//  (A2D probes) and the bridge advances NVC to t_{n+1} through the stepper
//  the driver registered here.  If NVC changes an analog input (a D2A source)
//  before t_{n+1}, the analog step is redone to end at that time.
//
//  D2A: NVC signal value -> bridge registry -> analog source ramp.
//  A2D: analog node voltage -> bridge -> NVC deposit (inside the model, at
//       the digital time the analog has reached).
//

#ifndef _COSIM_BRIDGE_H
#define _COSIM_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_D2A 0
#define BRIDGE_A2D 1

// A2D: deposit a voltage into an NVC signal at the model's current time.
// Only ever called from inside the stepper's apply callback (see below), so
// the driver runs it inside the model.
//   ctx      — opaque context (e.g. rt_model_t + rt_signal_t)
//   voltage  — analog voltage to deposit
typedef void (*bridge_deposit_fn)(void *ctx, double voltage);

// Register a boundary signal.
// For A2D, deposit_fn/deposit_ctx are the NVC deposit callback.
// Returns an index, or -1 on error.
int cosim_bridge_register(const char *name, int dir,
                          double initial_voltage,
                          bridge_deposit_fn deposit_fn,
                          void *deposit_ctx);

// Give a D2A signal its NVC value, observed at digital time now_s, together
// with the time of the next scheduled digital event (-1 if none).  Returns 1
// if the value changed (an analog input changed at now_s), else 0.
int cosim_bridge_update_d2a(int idx, double voltage, double next_time_s,
                            double now_s);

// Reset all registrations.
void cosim_bridge_reset(void);

// Analog-master stepping.  The driver registers a stepper that advances the
// digital through an analog candidate step t_prev_s -> t_s:
//   - it first calls apply(ctx, -1) at the current digital time (the bridge
//     deposits any step-start values the digital has not seen yet; apply
//     returns the number of deposits), then walks the digital time points
//     before t_s one at a time, checking the D2A signals after each;
//   - on the way it deposits the probes' trajectory at nsamples-1 evenly
//     spaced times inside the step (apply(ctx, frac), 0 < frac < 1: the bridge
//     deposits the values interpolated at that fraction of the step), so the
//     digital resolves threshold crossings finer than the analog step;
//   - if a D2A signal changed at a time t_evt < t_s (or at the start), it
//     stops there and returns 1 with *t_evt_s set: the analog must redo the
//     step so that it ends at t_evt;
//   - otherwise it reaches t_s, calls apply(ctx, 1) there (the candidate
//     values), runs the digital at t_s and returns 0.
// A D2A change exactly at t_s is not a veto: the source ramps from t_s.
// Every apply() runs inside the model, so deposits are made at the digital
// time it is called at.
typedef int (*bridge_apply_fn)(void *ctx, double frac);
typedef int (*bridge_stepper_fn)(void *ctx, double t_prev_s, double t_s,
                                 int nsamples, bridge_apply_fn apply,
                                 void *apply_ctx, double *t_evt_s);
void cosim_bridge_set_stepper(bridge_stepper_fn fn, void *ctx);

// Engine-neutral entry used by the engine glue (xyce_bridge_step,
// vacask_extsource_step): all probes have staged their values for the
// candidate time t; advance the digital.  Returns 1 and *t_evt if the step
// must be redone to end at *t_evt.
int cosim_bridge_step(double t, double *t_evt);

#ifdef __cplusplus
}
#endif

#endif // _COSIM_BRIDGE_H
