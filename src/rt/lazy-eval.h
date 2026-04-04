// lazy-eval.h — Process-centric lazy evaluation
//
// Each process has input slots. Each slot has:
//   - A pack method (primes the cached value from signal storage)
//   - Cached packed value (abits/bbits)
//   - Pointer to the signal's byte-per-bit storage
//
// When a signal changes, its notify handler calls the pack method
// on every process that reads it. The pack updates the cache and
// arms the process. When the clock fires, armed processes eval
// using cached values. Unarmed processes are NOPs.
//
// The pack methods are on the process vtable, so the signal's
// notify just does: proc->pack[slot_index](proc)
// which is a single indirect call.

#ifndef _LAZY_EVAL_H
#define _LAZY_EVAL_H

#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct _rt_model rt_model_t;
typedef struct _rt_proc rt_proc_t;

// Cached packed value for one input signal
typedef struct {
   uint64_t abits;
   uint64_t bbits;
   int      width;       // signal width in bits
   uint8_t *sig_data;    // -> NVC signal byte-per-bit storage
} lazy_input_t;

// A process-to-input binding: which process slot to prime
// when this signal changes. Chained per-signal.
typedef struct _lazy_binding {
   rt_proc_t              *proc;
   int                     slot;      // index into proc's input array
   struct _lazy_binding   *next;      // next binding on same signal
} lazy_binding_t;

// Per-process lazy eval state (extends rt_proc_t via vtable)
typedef struct {
   lazy_input_t *inputs;
   int           n_inputs;
   bool          armed;     // true = eval needed on next clock
} lazy_proc_state_t;

// Pack a single input slot (called by signal notify)
static inline void lazy_pack_input(lazy_input_t *inp)
{
   uint64_t a = 0, b = 0;
   const int w = inp->width < 64 ? inp->width : 64;
   for (int i = 0; i < w; i++) {
      a |= (uint64_t)(inp->sig_data[i] & 1) << i;
      b |= (uint64_t)((inp->sig_data[i] >> 1) & 1) << i;
   }
   inp->abits = a;
   inp->bbits = b;
}

#endif // _LAZY_EVAL_H
