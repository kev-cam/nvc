//
//  Copyright (C) 2025-2026  Kevin Cameron
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
//  nvc_cocotb_bridge.h -- Direct bridge between CocoTB and NVC model API.
//  Replaces VPI/VHPI with direct signal access for performance and debug.
//

#ifndef NVC_COCOTB_BRIDGE_H
#define NVC_COCOTB_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Opaque handle type (index into handle table)
typedef int64_t nvcb_hdl_t;

// GPI type codes (match CocoTB's cocotb.simulator constants)
enum {
   NVCB_UNKNOWN         = 0,
   NVCB_MODULE          = 1,
   NVCB_LOGIC           = 2,
   NVCB_LOGIC_ARRAY     = 3,
   NVCB_REAL            = 4,
   NVCB_INTEGER         = 5,
   NVCB_ENUM            = 6,
   NVCB_STRING          = 7,
   NVCB_GENARRAY        = 8,
   NVCB_NETARRAY        = 9,
   NVCB_STRUCTURE       = 10,
   NVCB_PACKED_STRUCTURE = 11,
   NVCB_MEMORY          = 12,
   NVCB_PACKAGE         = 13,
};

// Iteration modes
enum {
   NVCB_OBJECTS = 1,
   NVCB_DRIVERS = 2,
   NVCB_LOADS   = 3,
};

// Edge types for value change callbacks
enum {
   NVCB_RISING       = 1,
   NVCB_FALLING      = 2,
   NVCB_VALUE_CHANGE = 3,
};

// Signal deposit actions
enum {
   NVCB_DEPOSIT = 0,
   NVCB_FORCE   = 1,
   NVCB_RELEASE = 2,
   NVCB_FREEZE  = 3,
};

// Range direction
enum {
   NVCB_RANGE_UP     = 1,
   NVCB_RANGE_DOWN   = 2,
   NVCB_RANGE_NO_DIR = 3,
};

// Callback ID
typedef int64_t nvcb_cb_t;

// Callback kind
typedef enum {
   NVCB_CB_TIMED,
   NVCB_CB_VALUE_CHANGE,
   NVCB_CB_READONLY,
   NVCB_CB_READWRITE,
   NVCB_CB_NEXTSTEP,
} nvcb_cb_kind_t;

// ---- Initialization ----
void nvcb_init(void *model);   // Pass rt_model_t*
void nvcb_fini(void);

// Set Python dispatcher (called from C trampolines to invoke Python callbacks).
// Forward-declared as void* to avoid pulling in Python.h here.
void nvcb_set_dispatcher(void *py_callable);

// ---- Handle management ----
nvcb_hdl_t nvcb_get_root(const char *name);
nvcb_hdl_t nvcb_get_handle_by_name(nvcb_hdl_t parent, const char *name);
nvcb_hdl_t nvcb_get_handle_by_index(nvcb_hdl_t parent, int index);

// ---- Handle metadata ----
const char *nvcb_get_name(nvcb_hdl_t hdl);
const char *nvcb_get_fullname(nvcb_hdl_t hdl);
int         nvcb_get_type(nvcb_hdl_t hdl);
const char *nvcb_get_type_string(nvcb_hdl_t hdl);
int         nvcb_get_num_elems(nvcb_hdl_t hdl);
bool        nvcb_is_const(nvcb_hdl_t hdl);
bool        nvcb_is_indexable(nvcb_hdl_t hdl);
void        nvcb_get_range(nvcb_hdl_t hdl, int *left, int *right, int *dir);
const char *nvcb_get_definition_name(nvcb_hdl_t hdl);
const char *nvcb_get_definition_file(nvcb_hdl_t hdl);

// ---- Signal value access ----
const char *nvcb_get_signal_val_binstr(nvcb_hdl_t hdl);
int64_t     nvcb_get_signal_val_long(nvcb_hdl_t hdl);
double      nvcb_get_signal_val_real(nvcb_hdl_t hdl);
const char *nvcb_get_signal_val_str(nvcb_hdl_t hdl);

void nvcb_set_signal_val_binstr(nvcb_hdl_t hdl, int action, const char *val);
void nvcb_set_signal_val_int(nvcb_hdl_t hdl, int action, int64_t val);
void nvcb_set_signal_val_real(nvcb_hdl_t hdl, int action, double val);
void nvcb_set_signal_val_str(nvcb_hdl_t hdl, int action, const char *val);

// ---- Iteration ----
nvcb_hdl_t nvcb_iterate(nvcb_hdl_t hdl, int mode);
nvcb_hdl_t nvcb_next(nvcb_hdl_t iter);

// ---- Time ----
void nvcb_get_sim_time(uint32_t *high, uint32_t *low);
int  nvcb_get_precision(void);

// ---- Callbacks ----
nvcb_cb_t nvcb_register_timed_cb(uint64_t time_steps);
nvcb_cb_t nvcb_register_value_change_cb(nvcb_hdl_t signal, int edge);
nvcb_cb_t nvcb_register_readonly_cb(void);
nvcb_cb_t nvcb_register_readwrite_cb(void);
nvcb_cb_t nvcb_register_nextstep_cb(void);
void      nvcb_deregister_cb(nvcb_cb_t cb);

// ---- Event loop ----
// Returns the callback ID that fired, or -1 if simulation ended.
// Advances simulation until the next registered callback fires.
nvcb_cb_t nvcb_run_until_cb(void);

// ---- Simulation control ----
bool nvcb_is_running(void);
void nvcb_stop(void);

// ---- Info ----
const char *nvcb_get_simulator_product(void);
const char *nvcb_get_simulator_version(void);

#endif // NVC_COCOTB_BRIDGE_H
