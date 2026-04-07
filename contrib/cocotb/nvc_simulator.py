# nvc_simulator.py -- Drop-in replacement for cocotb.simulator
#
# Copyright (C) 2025-2026  Kevin Cameron
# Licensed under the Apache License, Version 2.0
#
# This module provides the same API as cocotb's simulator C extension
# but bridges directly to NVC's model API via nvc_cocotb_bridge.so.
# It can be used interpreted (via ctypes) or compiled with Nuitka
# (where the ctypes calls get replaced with direct C calls in post-processing).

import ctypes
import os
from typing import Any, Callable

# Load the bridge shared library
_bridge_path = os.environ.get("NVCB_BRIDGE_SO",
    os.path.join(os.path.dirname(__file__), "nvc_cocotb_bridge.so"))
_lib = ctypes.CDLL(_bridge_path)

# Set up function signatures
_lib.nvcb_init.argtypes = [ctypes.c_void_p]
_lib.nvcb_init.restype = None
_lib.nvcb_fini.argtypes = []
_lib.nvcb_fini.restype = None

_lib.nvcb_get_root.argtypes = [ctypes.c_char_p]
_lib.nvcb_get_root.restype = ctypes.c_int64
_lib.nvcb_get_handle_by_name.argtypes = [ctypes.c_int64, ctypes.c_char_p]
_lib.nvcb_get_handle_by_name.restype = ctypes.c_int64
_lib.nvcb_get_handle_by_index.argtypes = [ctypes.c_int64, ctypes.c_int]
_lib.nvcb_get_handle_by_index.restype = ctypes.c_int64

_lib.nvcb_get_name.argtypes = [ctypes.c_int64]
_lib.nvcb_get_name.restype = ctypes.c_char_p
_lib.nvcb_get_type.argtypes = [ctypes.c_int64]
_lib.nvcb_get_type.restype = ctypes.c_int
_lib.nvcb_get_type_string.argtypes = [ctypes.c_int64]
_lib.nvcb_get_type_string.restype = ctypes.c_char_p
_lib.nvcb_get_num_elems.argtypes = [ctypes.c_int64]
_lib.nvcb_get_num_elems.restype = ctypes.c_int
_lib.nvcb_is_const.argtypes = [ctypes.c_int64]
_lib.nvcb_is_const.restype = ctypes.c_bool
_lib.nvcb_is_indexable.argtypes = [ctypes.c_int64]
_lib.nvcb_is_indexable.restype = ctypes.c_bool
_lib.nvcb_get_range.argtypes = [
    ctypes.c_int64, ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
_lib.nvcb_get_range.restype = None
_lib.nvcb_get_definition_name.argtypes = [ctypes.c_int64]
_lib.nvcb_get_definition_name.restype = ctypes.c_char_p
_lib.nvcb_get_definition_file.argtypes = [ctypes.c_int64]
_lib.nvcb_get_definition_file.restype = ctypes.c_char_p

_lib.nvcb_get_signal_val_binstr.argtypes = [ctypes.c_int64]
_lib.nvcb_get_signal_val_binstr.restype = ctypes.c_char_p
_lib.nvcb_get_signal_val_long.argtypes = [ctypes.c_int64]
_lib.nvcb_get_signal_val_long.restype = ctypes.c_int64
_lib.nvcb_get_signal_val_real.argtypes = [ctypes.c_int64]
_lib.nvcb_get_signal_val_real.restype = ctypes.c_double
_lib.nvcb_get_signal_val_str.argtypes = [ctypes.c_int64]
_lib.nvcb_get_signal_val_str.restype = ctypes.c_char_p

_lib.nvcb_set_signal_val_binstr.argtypes = [ctypes.c_int64, ctypes.c_int, ctypes.c_char_p]
_lib.nvcb_set_signal_val_binstr.restype = None
_lib.nvcb_set_signal_val_int.argtypes = [ctypes.c_int64, ctypes.c_int, ctypes.c_int64]
_lib.nvcb_set_signal_val_int.restype = None
_lib.nvcb_set_signal_val_real.argtypes = [ctypes.c_int64, ctypes.c_int, ctypes.c_double]
_lib.nvcb_set_signal_val_real.restype = None

_lib.nvcb_iterate.argtypes = [ctypes.c_int64, ctypes.c_int]
_lib.nvcb_iterate.restype = ctypes.c_int64
_lib.nvcb_next.argtypes = [ctypes.c_int64]
_lib.nvcb_next.restype = ctypes.c_int64

_lib.nvcb_get_sim_time.argtypes = [
    ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32)]
_lib.nvcb_get_sim_time.restype = None
_lib.nvcb_get_precision.argtypes = []
_lib.nvcb_get_precision.restype = ctypes.c_int

_lib.nvcb_register_timed_cb.argtypes = [ctypes.c_uint64]
_lib.nvcb_register_timed_cb.restype = ctypes.c_int64
_lib.nvcb_register_value_change_cb.argtypes = [ctypes.c_int64, ctypes.c_int]
_lib.nvcb_register_value_change_cb.restype = ctypes.c_int64
_lib.nvcb_register_readonly_cb.argtypes = []
_lib.nvcb_register_readonly_cb.restype = ctypes.c_int64
_lib.nvcb_register_readwrite_cb.argtypes = []
_lib.nvcb_register_readwrite_cb.restype = ctypes.c_int64
_lib.nvcb_register_nextstep_cb.argtypes = []
_lib.nvcb_register_nextstep_cb.restype = ctypes.c_int64
_lib.nvcb_deregister_cb.argtypes = [ctypes.c_int64]
_lib.nvcb_deregister_cb.restype = None

_lib.nvcb_run_until_cb.argtypes = []
_lib.nvcb_run_until_cb.restype = ctypes.c_int64
_lib.nvcb_is_running.argtypes = []
_lib.nvcb_is_running.restype = ctypes.c_bool
_lib.nvcb_stop.argtypes = []
_lib.nvcb_stop.restype = None

_lib.nvcb_get_simulator_product.argtypes = []
_lib.nvcb_get_simulator_product.restype = ctypes.c_char_p
_lib.nvcb_get_simulator_version.argtypes = []
_lib.nvcb_get_simulator_version.restype = ctypes.c_char_p

# Dispatcher: C trampolines call this with cb_id to invoke Python callbacks
_DISPATCHER_T = ctypes.CFUNCTYPE(None, ctypes.c_int64)
_lib.nvcb_set_dispatcher.argtypes = [ctypes.c_void_p]
_lib.nvcb_set_dispatcher.restype = None

# Synchronous (blocking) helpers — used by translated tests
_lib.nvcb_wait_time.argtypes = [ctypes.c_uint64]
_lib.nvcb_wait_time.restype = None
_lib.nvcb_wait_edge.argtypes = [ctypes.c_int64, ctypes.c_int]
_lib.nvcb_wait_edge.restype = None
_lib.nvcb_start_clock.argtypes = [ctypes.c_int64, ctypes.c_uint64]
_lib.nvcb_start_clock.restype = ctypes.c_int64
_lib.nvcb_stop_clock.argtypes = [ctypes.c_int64]
_lib.nvcb_stop_clock.restype = None


def nvcb_wait_time(delta_fs):
    """Block until simulation has advanced by delta_fs femtoseconds."""
    _lib.nvcb_wait_time(int(delta_fs))


def nvcb_wait_edge(signal_handle, edge):
    """Block until the given signal has the requested edge."""
    if hasattr(signal_handle, "_hdl"):
        signal_handle = signal_handle._hdl
    _lib.nvcb_wait_edge(int(signal_handle), int(edge))


def nvcb_start_clock(signal_handle, period_fs):
    """Start a free-running clock on the signal (NVC-driven, no Python)."""
    if hasattr(signal_handle, "_hdl"):
        signal_handle = signal_handle._hdl
    return _lib.nvcb_start_clock(int(signal_handle), int(period_fs))


def nvcb_stop_clock(clock_id):
    _lib.nvcb_stop_clock(int(clock_id))

# ---- GPI type constants (match cocotb.simulator) ----
UNKNOWN = 0
MODULE = 1
LOGIC = 2
LOGIC_ARRAY = 3
REAL = 4
INTEGER = 5
ENUM = 6
STRING = 7
GENARRAY = 8
NETARRAY = 9
STRUCTURE = 10
PACKED_STRUCTURE = 11
MEMORY = 12
PACKAGE = 13

OBJECTS = 1
DRIVERS = 2
LOADS = 3

RISING = 1
FALLING = 2
VALUE_CHANGE = 3

RANGE_UP = 1
RANGE_DOWN = 2
RANGE_NO_DIR = 3

# ---- Callback registry (Python side) ----
_callbacks = {}  # cb_id -> (func, args)


# ---- Handle classes ----

class gpi_sim_hdl:
    """Wraps an NVC bridge handle."""

    def __init__(self, hdl_id):
        self._hdl = hdl_id

    def get_signal_val_binstr(self):
        return _lib.nvcb_get_signal_val_binstr(self._hdl).decode()

    def get_signal_val_long(self):
        return _lib.nvcb_get_signal_val_long(self._hdl)

    def get_signal_val_real(self):
        return _lib.nvcb_get_signal_val_real(self._hdl)

    def get_signal_val_str(self):
        return _lib.nvcb_get_signal_val_str(self._hdl)

    def set_signal_val_binstr(self, action, value):
        _lib.nvcb_set_signal_val_binstr(self._hdl, action, value.encode())

    def set_signal_val_int(self, action, value):
        _lib.nvcb_set_signal_val_int(self._hdl, action, value)

    def set_signal_val_real(self, action, value):
        _lib.nvcb_set_signal_val_real(self._hdl, action, value)

    def set_signal_val_str(self, action, value):
        if isinstance(value, str):
            value = value.encode()
        _lib.nvcb_set_signal_val_binstr(self._hdl, action, value)

    def get_handle_by_name(self, name, discovery_method=None):
        hdl = _lib.nvcb_get_handle_by_name(self._hdl, name.encode())
        return gpi_sim_hdl(hdl) if hdl >= 0 else None

    def get_handle_by_index(self, index):
        hdl = _lib.nvcb_get_handle_by_index(self._hdl, index)
        return gpi_sim_hdl(hdl) if hdl >= 0 else None

    def get_name_string(self):
        return _lib.nvcb_get_name(self._hdl).decode()

    def get_type(self):
        return _lib.nvcb_get_type(self._hdl)

    def get_type_string(self):
        return _lib.nvcb_get_type_string(self._hdl).decode()

    def get_num_elems(self):
        return _lib.nvcb_get_num_elems(self._hdl)

    def get_const(self):
        return _lib.nvcb_is_const(self._hdl)

    def get_indexable(self):
        return _lib.nvcb_is_indexable(self._hdl)

    def get_range(self):
        left = ctypes.c_int()
        right = ctypes.c_int()
        direction = ctypes.c_int()
        _lib.nvcb_get_range(self._hdl, ctypes.byref(left),
                            ctypes.byref(right), ctypes.byref(direction))
        return (left.value, right.value, direction.value)

    def get_definition_name(self):
        return _lib.nvcb_get_definition_name(self._hdl).decode()

    def get_definition_file(self):
        return _lib.nvcb_get_definition_file(self._hdl).decode()

    def iterate(self, mode):
        iter_hdl = _lib.nvcb_iterate(self._hdl, mode)
        # -1 means failed, otherwise valid (uses ITER_OFFSET in C)
        return gpi_iterator_hdl(iter_hdl) if iter_hdl >= 0 else None

    def __eq__(self, other):
        if isinstance(other, gpi_sim_hdl):
            return self._hdl == other._hdl
        return NotImplemented

    def __ne__(self, other):
        if isinstance(other, gpi_sim_hdl):
            return self._hdl != other._hdl
        return NotImplemented

    def __hash__(self):
        return hash(self._hdl)

    def __repr__(self):
        return f"gpi_sim_hdl({self._hdl})"


class gpi_cb_hdl:
    """Wraps a callback handle."""

    def __init__(self, cb_id):
        self._cb = cb_id

    def deregister(self):
        _lib.nvcb_deregister_cb(self._cb)
        _callbacks.pop(self._cb, None)

    def __eq__(self, other):
        if isinstance(other, gpi_cb_hdl):
            return self._cb == other._cb
        return NotImplemented

    def __ne__(self, other):
        if isinstance(other, gpi_cb_hdl):
            return self._cb != other._cb
        return NotImplemented

    def __hash__(self):
        return hash(self._cb)


class gpi_iterator_hdl:
    """Wraps an iterator handle."""

    def __init__(self, iter_id):
        self._iter = iter_id

    def __iter__(self):
        return self

    def __next__(self):
        hdl = _lib.nvcb_next(self._iter)
        if hdl < 0:
            raise StopIteration
        return gpi_sim_hdl(hdl)

    def __eq__(self, other):
        if isinstance(other, gpi_iterator_hdl):
            return self._iter == other._iter
        return NotImplemented

    def __ne__(self, other):
        if isinstance(other, gpi_iterator_hdl):
            return self._iter != other._iter
        return NotImplemented

    def __hash__(self):
        return hash(self._iter)


class GpiClock:
    """Clock generator using timed callbacks.

    Schedules itself at half-period intervals to toggle the clock signal.
    """

    def __init__(self, signal):
        self._signal = signal
        self._running = False
        self._period_steps = 0
        self._high_steps = 0
        self._set_action = 0
        self._next_high = True

    def start(self, period_steps, high_steps, start_high, set_action=0):
        self._running = True
        self._period_steps = period_steps
        self._high_steps = high_steps
        self._set_action = set_action

        # Set initial value
        if start_high:
            self._signal.set_signal_val_int(set_action, 1)
            self._next_high = False
            delay = high_steps
        else:
            self._signal.set_signal_val_int(set_action, 0)
            self._next_high = True
            delay = period_steps - high_steps

        self._schedule_toggle(delay)

    def _schedule_toggle(self, delay):
        cb_id = _lib.nvcb_register_timed_cb(delay)
        if cb_id >= 0:
            _callbacks[cb_id] = (self._toggle, ())

    def _toggle(self):
        if not self._running:
            return
        if self._next_high:
            self._signal.set_signal_val_int(self._set_action, 1)
            delay = self._high_steps
        else:
            self._signal.set_signal_val_int(self._set_action, 0)
            delay = self._period_steps - self._high_steps
        self._next_high = not self._next_high
        self._schedule_toggle(delay)

    def stop(self):
        self._running = False


# ---- Module-level functions ----

def get_root_handle(name):
    n = name.encode() if name else None
    hdl = _lib.nvcb_get_root(n)
    return gpi_sim_hdl(hdl) if hdl >= 0 else None


def get_sim_time():
    high = ctypes.c_uint32()
    low = ctypes.c_uint32()
    _lib.nvcb_get_sim_time(ctypes.byref(high), ctypes.byref(low))
    return (high.value, low.value)


def get_precision():
    return _lib.nvcb_get_precision()


def get_simulator_product():
    return _lib.nvcb_get_simulator_product().decode()


def get_simulator_version():
    return _lib.nvcb_get_simulator_version().decode()


def is_running():
    return _lib.nvcb_is_running()


def stop_simulator():
    _lib.nvcb_stop()


def register_timed_callback(time, func, *args):
    cb_id = _lib.nvcb_register_timed_cb(time)
    if cb_id < 0:
        raise RuntimeError("Failed to register timed callback")
    _callbacks[cb_id] = (func, args)
    return gpi_cb_hdl(cb_id)


def register_value_change_callback(signal, func, edge, *args):
    cb_id = _lib.nvcb_register_value_change_cb(signal._hdl, edge)
    if cb_id < 0:
        raise RuntimeError("Failed to register value change callback")
    _callbacks[cb_id] = (func, args)
    return gpi_cb_hdl(cb_id)


def register_readonly_callback(func, *args):
    cb_id = _lib.nvcb_register_readonly_cb()
    if cb_id < 0:
        raise RuntimeError("Failed to register readonly callback")
    _callbacks[cb_id] = (func, args)
    return gpi_cb_hdl(cb_id)


def register_rwsynch_callback(func, *args):
    cb_id = _lib.nvcb_register_readwrite_cb()
    if cb_id < 0:
        raise RuntimeError("Failed to register readwrite callback")
    _callbacks[cb_id] = (func, args)
    return gpi_cb_hdl(cb_id)


def register_nextstep_callback(func, *args):
    cb_id = _lib.nvcb_register_nextstep_cb()
    if cb_id < 0:
        raise RuntimeError("Failed to register nextstep callback")
    _callbacks[cb_id] = (func, args)
    return gpi_cb_hdl(cb_id)


def package_iterate():
    return gpi_iterator_hdl(-1)  # empty iterator


def set_gpi_log_level(level):
    pass  # no-op for now


def initialize_logger(log_func, get_logger):
    pass  # no-op for now


def set_sim_event_callback(sim_event_callback):
    pass  # no-op for now


def clock_create(hdl):
    return GpiClock(hdl)


# ---- Dispatcher (called from C when callback fires) ----

def _dispatch(cb_id):
    """Called from C trampolines to invoke a registered Python callback."""
    entry = _callbacks.pop(cb_id, None)
    if entry is None:
        return
    func, args = entry
    try:
        func(*args)
    except Exception:
        import traceback
        traceback.print_exc()


# Register the dispatcher with the C bridge.
# IMPORTANT: must be a module-level reference so the CFUNCTYPE wrapper
# doesn't get garbage-collected.
_dispatcher_cfunc = _DISPATCHER_T(_dispatch)
# Get the raw C function pointer address from the ctypes wrapper
_dispatcher_addr = ctypes.cast(_dispatcher_cfunc, ctypes.c_void_p).value
_lib.nvcb_set_dispatcher(_dispatcher_addr)
