//
//  Copyright (C) 2022-2024  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#ifndef _JIT_EXITS_H
#define _JIT_EXITS_H

#include "prim.h"
#include "jit/jit.h"
#include "jit/jit-ffi.h"
#include "rt/rt.h"

#define WEAK __attribute__((weak))

void x_sched_process(int64_t delay);
void x_sched_inactive(void);
void x_drive_signal(sig_shared_t *ss, uint32_t offset, int32_t count);
sig_shared_t *x_init_signal(int64_t count, uint32_t size, jit_scalar_t value,
                            bool scalar, uint32_t flags, tree_t where,
                            int32_t offset);
void x_sched_waveform(sig_shared_t *ss, uint32_t offset, void *values,
                      int32_t count, int64_t after, int64_t reject);
void x_transfer_signal(sig_shared_t *target_ss, uint32_t toffset,
                       sig_shared_t *source_ss, uint32_t soffset,
                       int32_t count, int64_t after, int64_t reject);
int32_t x_test_net_event(sig_shared_t *ss, uint32_t offset, int32_t count);
int32_t x_test_net_active(sig_shared_t *ss, uint32_t offset,
                          int32_t count);
void x_sched_event(sig_shared_t *ss, uint32_t offset, int32_t count);
void x_enable_last_value(sig_shared_t *ss, uint32_t offset, int32_t count);
void x_alias_signal(sig_shared_t *ss, tree_t where);
void x_sched_waveform_s(sig_shared_t *ss, uint32_t offset, uint64_t scalar,
                        int64_t after, int64_t reject);
void x_file_open(int8_t *status, void **_fp, const uint8_t *name_bytes,
                 int32_t name_len, int8_t mode);
void x_file_write(void **_fp, void *data, int64_t size, int64_t count);
int64_t x_file_read(void **_fp, void *data, int64_t size, int64_t count);
void x_index_fail(int64_t value, int64_t left, int64_t right, int8_t dir,
                  tree_t where, tree_t hint);
void x_length_fail(int64_t left, int64_t right, int32_t dim, tree_t where);
void x_range_fail(int64_t value, int64_t left, int64_t right, int8_t dir,
                  tree_t where, tree_t hint);
void x_exponent_fail(int64_t value, tree_t where);
void x_overflow(int64_t lhs, int64_t rhs, tree_t where);
void x_null_deref(tree_t where);
void x_div_zero(tree_t where);
void x_assert_fail(const uint8_t *msg, int32_t msg_len, int8_t severity,
                   int64_t hint_left, int64_t hint_right, int8_t hint_valid,
                   object_t *where);
void x_report(const uint8_t *msg, int32_t msg_len, int8_t severity,
              object_t *where);
int64_t x_last_event(sig_shared_t *ss, uint32_t offset, int32_t count);
int64_t x_last_active(sig_shared_t *ss, uint32_t offset, int32_t count);
void x_map_signal(sig_shared_t *src_ss, uint32_t src_offset,
                  sig_shared_t *dst_ss, uint32_t dst_offset, uint32_t count);
void x_map_const(sig_shared_t *ss, uint32_t offset,
                 const uint8_t *values, uint32_t count);
void x_map_implicit(sig_shared_t *src_ss, uint32_t src_offset,
                    sig_shared_t *dst_ss, uint32_t dst_offset,
                    uint32_t count);
void x_push_scope(tree_t where, int32_t size, rt_scope_kind_t kind);
void x_pop_scope(void);
bool x_driving(sig_shared_t *ss, uint32_t offset, int32_t count);
void *x_driving_value(sig_shared_t *ss, uint32_t offset, int32_t count);
sig_shared_t *x_implicit_signal(uint32_t count, uint32_t size, tree_t where,
                                implicit_kind_t kind, ffi_closure_t *closure,
                                int64_t delay);
void x_disconnect(sig_shared_t *ss, uint32_t offset, int32_t count,
                  int64_t after, int64_t reject);
void x_force(sig_shared_t *ss, uint32_t offset, int32_t count, void *values);
void x_release(sig_shared_t *ss, uint32_t offset, int32_t count);
void x_deposit_signal(sig_shared_t *ss, uint32_t offset, int32_t count,
                      void *values);
void x_sched_deposit(sig_shared_t *ss, uint32_t offset, int32_t count,
                     void *values, int64_t after);
void x_put_driver(sig_shared_t *ss, uint32_t offset, int32_t count,
                  void *values);
void x_put_conversion(rt_conv_func_t *cf, sig_shared_t *ss, uint32_t offset,
                      int32_t count, void *values);
void x_resolve_signal(sig_shared_t *ss, jit_handle_t handle, void *context,
                      int32_t nlits, int32_t flags);
void x_unreachable(tree_t where);
void x_cover_setup_toggle_cb(sig_shared_t *ss, int32_t tag);
void x_cover_setup_state_cb(sig_shared_t *ss, int64_t low, int32_t tag);
void x_process_init(jit_handle_t handle, tree_t where);
void x_clear_event(sig_shared_t *ss, uint32_t offset, int32_t count);
void x_enter_state(int32_t state, bool strong);
void *x_reflect_value(void *context, jit_scalar_t value, tree_t where,
                      const jit_scalar_t *bounds);
void *x_reflect_subtype(void *context, tree_t where,
                        const jit_scalar_t *bounds);
void *x_function_trigger(jit_handle_t handle, unsigned nargs,
                         const jit_scalar_t *args);
rt_trigger_t *x_or_trigger(rt_trigger_t *left, rt_trigger_t *right);
void *x_cmp_trigger(sig_shared_t *ss, uint32_t offset, int64_t right);
void *x_level_trigger(sig_shared_t *ss, uint32_t offset, int32_t count);
void x_add_trigger(void *ptr);
void *x_port_conversion(const ffi_closure_t *driving,
                        const ffi_closure_t *effective);
void x_convert_in(void *ptr, sig_shared_t *ss, uint32_t offset, int32_t count);
void x_convert_out(void *ptr, sig_shared_t *ss, uint32_t offset, int32_t count);
void x_bind_external(tree_t where, jit_handle_t scope, jit_scalar_t *result);
void x_instance_name(attr_kind_t kind, text_buf_t *tb);
void x_enable_trigger(rt_trigger_t *trigger);
void x_disable_trigger(rt_trigger_t *trigger);

// Pipe operations
void x_init_pipe(sig_shared_t *ss, int32_t depth);
void x_pipe_write(sig_shared_t *ss, uint32_t offset, int32_t count,
                  void *values);
void x_pipe_read(sig_shared_t *ss, uint32_t offset, int32_t count,
                 void *result);
bool x_pipe_full(sig_shared_t *ss, uint32_t offset, int32_t count);
bool x_pipe_empty(sig_shared_t *ss, uint32_t offset, int32_t count);

// ---------------------------------------------------------------------------
// Native-projection pilot (NVC_INLINE_DRIVE): the runtime publishes the
// frozen layout facts of its driver-update fast path so a backend can
// specialize signal-assignment sites against them at JIT compile time
// (first Futamura projection done natively: the spec's structure is data
// for the compiler, the spec itself remains the fallback).  All offsets are
// bytes; bitfield positions are probed at startup so no assumption is made
// about the C compiler's bitfield allocation.  `valid` is false when any
// probe failed or the runtime configuration (e.g. RT_MULTITHREADED) makes
// the single-threaded inline path unsound -- backends must then keep the
// plain call.  Implemented in rt/model.c which owns all these types.

typedef struct {
   bool     valid;
   // Addresses stable for the process lifetime
   void    *model_var;               // rt_model_t ** -- current model
   void    *par_active_var;          // int * -- worker-eval window active
   void    *trace_var;               // bool * -- --trace enabled
   void    *fast_driver_fn;          // void (*)(rt_model_t *, void *)
   // rt_signal_t / sig_shared_t / rt_nexus_t
   int32_t  signal_shared;           // offsetof(rt_signal_t, shared)
   int32_t  signal_nexus;            // offsetof(rt_signal_t, nexus)
   int32_t  shared_flags;            // offsetof(sig_shared_t, flags)
   int32_t  nexus_flags;             // u8
   int32_t  nexus_size;              // u8
   int32_t  nexus_n_sources;         // u8
   int32_t  nexus_width;             // u32
   int32_t  nexus_active_delta;      // u16 (delta_cycle_t)
   int32_t  nexus_sources;           // embedded rt_source_t
   // rt_source_t (single fast driver)
   int32_t  source_bits;             // byte containing the flag bitfields
   uint8_t  source_fastqueued_mask;
   uint8_t  source_was_active_mask;
   int32_t  source_when;             // u.driver.waveforms.when (u64)
   int32_t  source_value;            // u.driver.waveforms.value (rt_value_t)
   // rt_model_t
   int32_t  model_now;               // u64
   int32_t  model_iteration;         // i32
   int32_t  model_next_is_delta;     // bool (u8)
   int32_t  model_probe_member;      // i32 (fast-clk probation)
   int32_t  model_thread0;           // threads[0] (model_thread_t *)
   int32_t  thread_active_obj;       // rt_wakeable_t *
   int32_t  wakeable_bits;           // byte containing postponed bit
   uint8_t  wakeable_postponed_mask;
   // deferq (driverq) push
   int32_t  driverq_tasks;           // defer_task_t *
   int32_t  driverq_count;           // u32
   int32_t  driverq_max;             // u32
   int32_t  task_size;               // sizeof(defer_task_t)
   int32_t  task_fn;                 // offsetof(defer_task_t, fn)
   int32_t  task_arg;                // offsetof(defer_task_t, arg)
   // Flag constants
   uint32_t net_f_fast_driver;       // NET_F_FAST_DRIVER
} jit_drive_layout_t;

const jit_drive_layout_t *jit_drive_layout(void);

#endif  // _JIT_EXITS_H
