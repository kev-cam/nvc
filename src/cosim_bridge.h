//
//  cosim_bridge.h — Shared state between the NVC cosim driver and the analog
//  engine's bridged sources (Xyce DPWL callbacks / VACASK external sources).
//
//  The analog solver is the master scheduler.  At every converged candidate
//  step t_n -> t_{n+1} it hands the bridge the node voltages at both ends
//  (A2D probes) and the bridge advances NVC to t_{n+1} through the stepper
//  the driver registered here.  If NVC changes an analog input (a D2A source)
//  before t_{n+1}, the analog step is redone to end at that time.  If the
//  digital has stopped ($finish, a fatal error), the stepper says so and the
//  engine accepts the point and ends its transient (the finish protocol).
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

// ABI version of this library, returned by cosim_bridge_abi().  2 = the
// finish protocol (stepper result COSIM_STEP_FINISH), per-boundary ramps
// (cosim_bridge_set_ramp) and the COSIM_BRIDGE_MAX_SIGNALS registry.  The
// engines' C interfaces export the same version as vacask_cosim_abi() /
// xyce_cosim_abi(); NVC refuses to co-simulate when any of them is missing
// or older (an old engine reads a finish as "accept" and never ends).
#define COSIM_BRIDGE_ABI 2
int cosim_bridge_abi(void);

#define BRIDGE_D2A 0
#define BRIDGE_A2D 1

// Registry limits.  Names are matched exactly (case-sensitive) against the
// <name> of the engine's code: URI.
#define COSIM_BRIDGE_MAX_SIGNALS 8192
#define COSIM_BRIDGE_NAME_MAX    255

// cosim_bridge_register() failures
#define COSIM_BRIDGE_ERR_FULL  (-1)   // registry full (COSIM_BRIDGE_MAX_SIGNALS)
#define COSIM_BRIDGE_ERR_DUP   (-2)   // the name is already registered
#define COSIM_BRIDGE_ERR_NAME  (-3)   // empty or over-long name, bad direction

// D2A ramps: the default rise/fall time, and the shortest one (shorter
// values are clamped to it with a warning).
#define COSIM_BRIDGE_DEFAULT_RAMP 1e-9
#define COSIM_BRIDGE_MIN_RAMP     1e-15

// A2D: deposit a voltage into an NVC signal at the model's current time.
// Only ever called from inside the stepper's apply callback (see below), so
// the driver runs it inside the model.
//   ctx      — opaque context (e.g. rt_model_t + rt_signal_t)
//   voltage  — analog voltage to deposit
typedef void (*bridge_deposit_fn)(void *ctx, double voltage);

// Register a boundary signal.
// For A2D, deposit_fn/deposit_ctx are the NVC deposit callback.
// Returns an index >= 0, or one of the COSIM_BRIDGE_ERR_* codes.
int cosim_bridge_register(const char *name, int dir,
                          double initial_voltage,
                          bridge_deposit_fn deposit_fn,
                          void *deposit_ctx);

// Set the rise and fall times (seconds) of a D2A signal's ramps.  Every new
// ramp -- a mid-ramp reversal or a step to a mid level included -- takes the
// full rise time (if it goes up from the voltage the source drives at that
// moment) or fall time (if it goes down): these are durations, not slew
// rates.  Values below COSIM_BRIDGE_MIN_RAMP are clamped to it with a
// warning; NaN, infinite or negative values are rejected.  Without a call
// both are COSIM_BRIDGE_DEFAULT_RAMP.  A ramp shorter than the analog engine
// resolves at the time it starts (1e-13 x t: 1 fs from 10 ms on, 10 ps from
// 100 s on) takes that long instead, with a warning naming the signal (once
// per signal).  Returns 1 on success, 0 on a bad index or value.
int cosim_bridge_set_ramp(int idx, double rise, double fall);

// Give a D2A signal its NVC value, observed at digital time now_s, together
// with the time of the next scheduled digital event (-1 if none).  Returns 1
// if the value changed (an analog input changed at now_s), else 0.
int cosim_bridge_update_d2a(int idx, double voltage, double next_time_s,
                            double now_s);

// Reset all registrations.
void cosim_bridge_reset(void);

// Results of a candidate step (the stepper's and cosim_bridge_step's return)
#define COSIM_STEP_ACCEPT 0   // accept the candidate point
#define COSIM_STEP_VETO   1   // redo the step so that it ends at *t_evt
#define COSIM_STEP_FINISH 2   // accept the point, then end the transient

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
//     stops there and returns COSIM_STEP_VETO with *t_evt_s set: the analog
//     must redo the step so that it ends at t_evt;
//   - if the digital has stopped (or stops on the way) at t_f, it returns
//     COSIM_STEP_VETO with *t_evt_s = t_f once when the step ends after t_f
//     and the analog has not yet accepted a point at or after t_f, else
//     COSIM_STEP_FINISH: the engine accepts this point and ends its
//     transient (it never offers another step);
//   - otherwise it reaches t_s, calls apply(ctx, 1) there (the candidate
//     values), runs the digital at t_s and returns COSIM_STEP_ACCEPT.
// A D2A change exactly at t_s is not a veto: the source ramps from t_s.
// *t_evt_s is -1 unless the result is a veto.  Every apply() runs inside the
// model, so deposits are made at the digital time it is called at.
typedef int (*bridge_apply_fn)(void *ctx, double frac);
typedef int (*bridge_stepper_fn)(void *ctx, double t_prev_s, double t_s,
                                 int nsamples, bridge_apply_fn apply,
                                 void *apply_ctx, double *t_evt_s);
void cosim_bridge_set_stepper(bridge_stepper_fn fn, void *ctx);

// Engine-neutral entry used by the engine glue (xyce_bridge_step,
// vacask_extsource_step): all probes have staged their values for the
// candidate time t; advance the digital.  Returns the stepper's result
// (COSIM_STEP_*), COSIM_STEP_ACCEPT when no stepper is registered.
int cosim_bridge_step(double t, double *t_evt);

#ifdef __cplusplus
}
#endif

#endif // _COSIM_BRIDGE_H
