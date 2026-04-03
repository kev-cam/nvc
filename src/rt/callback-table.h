// callback-table.h — Fixed callback table for high fan-out signal events
//
// Replaces the dynamic pending list for signals with many sensitive
// processes (clocks, resets, enables). Each slot is a function pointer
// that's either a NOP or the real process wakeup. Slots are flipped
// by input-change detection: when a process's data inputs change,
// its slot goes from NOP to eval. After eval, it resets to NOP.
//
// The table can be swapped in/out with the list-based mechanism
// via the nexus vtable.

#ifndef _CALLBACK_TABLE_H
#define _CALLBACK_TABLE_H

#include "rt/structs.h"

typedef void (*cb_slot_fn)(rt_model_t *m, void *arg);

typedef struct {
   cb_slot_fn  fn;       // NOP or real callback
   cb_slot_fn  real_fn;  // saved real callback (for restore after NOP)
   void       *arg;      // process or wakeable pointer
} cb_slot_t;

typedef struct {
   int        n_slots;
   int        n_active;  // count of non-NOP slots (for stats)
   cb_slot_t  slots[];
} cb_table_t;

// Create a callback table from an existing pending list
cb_table_t *cb_table_from_pending(rt_model_t *m, void *pending);

// Fire all callbacks (NOP slots are one indirect call)
static inline void cb_table_fire(rt_model_t *m, cb_table_t *t)
{
   for (int i = 0; i < t->n_slots; i++)
      t->slots[i].fn(m, t->slots[i].arg);
}

// Set a slot to NOP
void cb_slot_set_nop(cb_table_t *t, int slot);

// Set a slot to its real callback
void cb_slot_set_active(cb_table_t *t, int slot);

// NOP function (does nothing)
void cb_nop(rt_model_t *m, void *arg);

#endif // _CALLBACK_TABLE_H
