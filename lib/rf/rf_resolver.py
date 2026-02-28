#!/usr/bin/env python3
"""
rf_resolver.py -- Generate resolver network VHDL for RF circuits

Extends the photonics resolver pattern for RF signals with antenna
arena resolution. Uses the same 'driver/'other deposit-based approach
but adds Friis free-space coupling for antenna networks.

Type selection is pluggable: rf_signal -> rf_resolve, plus all types
from photonics_resolver.py (optical_field, logic3ds, etc.).

Usage:
    python3 rf_resolver.py                      # run built-in test
    python3 rf_resolver.py --from-json FILE     # from NVC JSON export
"""

from dataclasses import dataclass, field
from typing import Optional, Dict, List, Tuple
import math


# ---------------------------------------------------------------------------
# Data model (reuses JonesMatrix from photonics for polarization coupling)
# ---------------------------------------------------------------------------

@dataclass
class JonesMatrix:
    """2x2 complex matrix for polarization coupling."""
    m00_re: float = 1.0;  m00_im: float = 0.0
    m01_re: float = 0.0;  m01_im: float = 0.0
    m10_re: float = 0.0;  m10_im: float = 0.0
    m11_re: float = 1.0;  m11_im: float = 0.0

    def is_identity(self) -> bool:
        return (abs(self.m00_re - 1.0) < 1e-12 and abs(self.m00_im) < 1e-12 and
                abs(self.m01_re) < 1e-12 and abs(self.m01_im) < 1e-12 and
                abs(self.m10_re) < 1e-12 and abs(self.m10_im) < 1e-12 and
                abs(self.m11_re - 1.0) < 1e-12 and abs(self.m11_im) < 1e-12)

    def is_zero(self) -> bool:
        return all(abs(v) < 1e-12 for v in [
            self.m00_re, self.m00_im, self.m01_re, self.m01_im,
            self.m10_re, self.m10_im, self.m11_re, self.m11_im])

    def is_scalar(self) -> bool:
        return (abs(self.m01_re) < 1e-12 and abs(self.m01_im) < 1e-12 and
                abs(self.m10_re) < 1e-12 and abs(self.m10_im) < 1e-12 and
                abs(self.m00_re - self.m11_re) < 1e-12 and
                abs(self.m00_im - self.m11_im) < 1e-12)

    def to_vhdl(self) -> str:
        def _f(v):
            if abs(v) < 1e-15:
                return "0.0"
            return f"{v:.15e}"
        return (f"("
                f"m00_re => {_f(self.m00_re)}, m00_im => {_f(self.m00_im)}, "
                f"m01_re => {_f(self.m01_re)}, m01_im => {_f(self.m01_im)}, "
                f"m10_re => {_f(self.m10_re)}, m10_im => {_f(self.m10_im)}, "
                f"m11_re => {_f(self.m11_re)}, m11_im => {_f(self.m11_im)})")


JONES_IDENTITY = JonesMatrix()
JONES_ZERO = JonesMatrix(0, 0, 0, 0, 0, 0, 0, 0)


# ---------------------------------------------------------------------------
# Friis free-space coupling
# ---------------------------------------------------------------------------

SPEED_OF_LIGHT = 299792458.0


@dataclass
class FriisCoupling:
    """Compute antenna-to-antenna coupling as a Jones matrix.

    Free-space path loss (amplitude): lambda / (4 * pi * distance)
    Gain: sqrt(gain_tx_linear * gain_rx_linear)
    Polarization mismatch: rotation matrix by mismatch angle
    Phase delay: 2 * pi * distance / lambda
    """
    distance: float       # meters
    freq: float           # Hz
    gain_tx_dbi: float = 0.0   # dBi
    gain_rx_dbi: float = 0.0   # dBi
    pol_mismatch: float = 0.0  # radians (0 = matched)

    def to_jones_matrix(self) -> JonesMatrix:
        wavelength = SPEED_OF_LIGHT / self.freq
        # Free-space path loss (amplitude)
        fspl_amp = wavelength / (4.0 * math.pi * self.distance)
        # Antenna gains (amplitude)
        gain_tx = math.sqrt(10.0 ** (self.gain_tx_dbi / 10.0))
        gain_rx = math.sqrt(10.0 ** (self.gain_rx_dbi / 10.0))
        total_amp = fspl_amp * gain_tx * gain_rx
        # Phase delay
        phase = -2.0 * math.pi * self.distance / wavelength
        # Amplitude with phase as complex scalar
        amp_re = total_amp * math.cos(phase)
        amp_im = total_amp * math.sin(phase)
        # Polarization rotation
        c = math.cos(self.pol_mismatch)
        s = math.sin(self.pol_mismatch)
        # M = amplitude * rotation = (amp_re + j*amp_im) * [[c,-s],[s,c]]
        m00_re = amp_re * c;  m00_im = amp_im * c
        m01_re = -amp_re * s; m01_im = -amp_im * s
        m10_re = amp_re * s;  m10_im = amp_im * s
        m11_re = amp_re * c;  m11_im = amp_im * c
        return JonesMatrix(m00_re, m00_im, m01_re, m01_im,
                           m10_re, m10_im, m11_re, m11_im)


def friis_scalar(distance: float, freq: float,
                 gain_tx_dbi: float = 0.0,
                 gain_rx_dbi: float = 0.0) -> JonesMatrix:
    """Simple Friis coupling: scalar (no polarization mismatch, no phase)."""
    wavelength = SPEED_OF_LIGHT / freq
    fspl_amp = wavelength / (4.0 * math.pi * distance)
    gain_tx = math.sqrt(10.0 ** (gain_tx_dbi / 10.0))
    gain_rx = math.sqrt(10.0 ** (gain_rx_dbi / 10.0))
    total_amp = fspl_amp * gain_tx * gain_rx
    return JonesMatrix(m00_re=total_amp, m11_re=total_amp)


# ---------------------------------------------------------------------------
# Endpoint / Net dataclasses
# ---------------------------------------------------------------------------

@dataclass
class Endpoint:
    """One endpoint on an RF net."""
    kind: str           # "component_port" or "source"
    instance: str       # e.g. "ant1"
    entity: str         # e.g. "rf_antenna"
    port: str           # e.g. "air", "feed"
    transfer: JonesMatrix = field(default_factory=lambda: JonesMatrix())
    comment: str = ""


@dataclass
class Net:
    """A resolved RF net with multiple endpoints."""
    name: str
    sig_type: str = "rf_signal"
    endpoints: list = field(default_factory=list)


# ---------------------------------------------------------------------------
# Type info: pluggable resolution strategy
# ---------------------------------------------------------------------------

def _type_info(sig_type: str) -> Tuple[str, str, List[str]]:
    t = sig_type.lower()

    if t == "rf_signal":
        return ("rf_resolve", "rf_signal_vector",
                ["library rf;",
                 "use rf.rf_signal_pkg.all;"])
    elif t == "optical_field":
        return ("optical_resolve", "optical_field_vector",
                ["library photonics;",
                 "use photonics.optical_field_pkg.all;",
                 "use photonics.optical_matrix_pkg.all;"])
    elif t == "optical_stokes":
        return ("stokes_resolve", "optical_stokes_vector",
                ["library photonics;",
                 "use photonics.optical_stokes_pkg.all;"])
    elif t == "logic3ds":
        return ("l3ds_resolve", "logic3ds_vector",
                ["library sv2vhdl;",
                 "use sv2vhdl.logic3ds_pkg.all;"])
    elif t == "logic3d":
        return ("l3d_resolve", "logic3d_vector",
                ["library sv2vhdl;",
                 "use sv2vhdl.logic3d_types_pkg.all;"])
    else:
        return ("resolved", "std_ulogic_vector",
                ["library ieee;",
                 "use ieee.std_logic_1164.all;"])


def _zero_constant(sig_type: str) -> str:
    t = sig_type.lower()
    if t == "rf_signal":
        return "RF_ZERO"
    elif t == "optical_field":
        return "OPTICAL_ZERO"
    elif t == "optical_stokes":
        return "STOKES_ZERO"
    elif t == "logic3ds":
        return "L3DS_Z"
    elif t == "logic3d":
        return "L3D_Z"
    else:
        return "'Z'"


def _needs_jones(sig_type: str) -> bool:
    """Does this type use Jones matrix transfer functions?"""
    return sig_type.lower() in ("rf_signal", "optical_field")


def _jones_apply_func(sig_type: str) -> str:
    """Return the jones_apply function name for this type."""
    # Both rf_signal and optical_field use the same jones_apply from their
    # respective matrix packages. For rf_signal, we define rf_jones_apply
    # inline since rf doesn't have a separate matrix package.
    t = sig_type.lower()
    if t == "rf_signal":
        return "rf_jones_apply"
    return "jones_apply"


# ---------------------------------------------------------------------------
# VHDL generation
# ---------------------------------------------------------------------------

def emit_resolver_vhdl(design_name: str, nets: List[Net]) -> str:
    lines = []
    wrapper = f"resolved_{design_name}"

    active_nets = [n for n in nets if len(n.endpoints) > 1]
    leaf_nets = [n for n in nets if len(n.endpoints) == 1]

    lines.append(f"-- RF resolver networks for {design_name}")
    lines.append(f"-- Auto-generated by rf_resolver.py")
    lines.append(f"--")
    lines.append(f"-- {len(active_nets)} nets needing resolution")
    lines.append(f"-- {len(leaf_nets)} leaf nets")
    lines.append(f"")

    # Connectivity comments
    for net in active_nets:
        parts = []
        for ep in net.endpoints:
            if ep.instance:
                parts.append(f"{ep.instance}.{ep.port}")
            else:
                parts.append(f"source({ep.port})")
        lines.append(f"-- {net.name}: {' <-> '.join(parts)}")
    lines.append(f"")

    # Libraries
    all_types = set(n.sig_type for n in nets)
    all_uses = set()
    for t in all_types:
        _, _, uses = _type_info(t)
        for u in uses:
            all_uses.add(u)

    lines.append(f"library ieee;")
    lines.append(f"use ieee.std_logic_1164.all;")
    lines.append(f"use ieee.math_real.all;")
    for u in sorted(all_uses):
        if not u.startswith("library ieee") and not u.startswith("use ieee"):
            lines.append(u)
    lines.append(f"")

    lines.append(f"entity resolver_{design_name} is")
    lines.append(f"end entity;")
    lines.append(f"")
    lines.append(f"architecture generated of resolver_{design_name} is")
    lines.append(f"")

    # External name aliases
    alias_map = {}
    alias_idx = 0

    for net in nets:
        ep_type = net.sig_type.lower()
        for i, ep in enumerate(net.endpoints):
            drv = f"drv_{alias_idx}"
            oth = f"oth_{alias_idx}"
            if ep.instance:
                lines.append(f"    alias {drv} is << signal"
                             f" .{wrapper}.dut.{ep.instance}"
                             f".{ep.port}.driver : {ep_type} >>;")
                lines.append(f"    alias {oth} is << signal"
                             f" .{wrapper}.dut.{ep.instance}"
                             f".{ep.port}.other : {ep_type} >>;")
            else:
                lines.append(f"    alias {drv} is << signal"
                             f" .{wrapper}.dut.{ep.port}.driver : {ep_type} >>;")
                lines.append(f"    alias {oth} is << signal"
                             f" .{wrapper}.dut.{ep.port}.other : {ep_type} >>;")
            alias_map[(net.name, i)] = (drv, oth)
            alias_idx += 1

    lines.append(f"")

    # Transfer matrix constants
    mat_constants = {}
    mat_idx = 0
    has_matrices = False

    for net in active_nets:
        if not _needs_jones(net.sig_type):
            continue
        for i, ep in enumerate(net.endpoints):
            if not ep.transfer.is_identity():
                cname = f"M_{mat_idx}"
                mat_constants[(net.name, i)] = cname
                lines.append(f"    constant {cname} : jones_matrix := "
                             f"{ep.transfer.to_vhdl()};")
                mat_idx += 1
                has_matrices = True

    # If we use Jones matrices with rf_signal, we need jones_matrix type
    # and rf_jones_apply function. Define inline since rf library doesn't
    # have a separate matrix package.
    need_rf_jones = any(
        _needs_jones(n.sig_type) and n.sig_type.lower() == "rf_signal"
        for n in active_nets
        if any(not ep.transfer.is_identity() for ep in n.endpoints)
    )

    if need_rf_jones:
        lines.append(f"")
        lines.append(f"    -- Jones matrix type for RF polarization coupling")
        lines.append(f"    type jones_matrix is record")
        lines.append(f"        m00_re, m00_im : real;")
        lines.append(f"        m01_re, m01_im : real;")
        lines.append(f"        m10_re, m10_im : real;")
        lines.append(f"        m11_re, m11_im : real;")
        lines.append(f"    end record;")
        lines.append(f"")
        lines.append(f"    function rf_jones_apply(m : jones_matrix; f : rf_signal) return rf_signal is")
        lines.append(f"        variable r : rf_signal;")
        lines.append(f"    begin")
        lines.append(f"        r.eh_re := m.m00_re*f.eh_re - m.m00_im*f.eh_im + m.m01_re*f.ev_re - m.m01_im*f.ev_im;")
        lines.append(f"        r.eh_im := m.m00_re*f.eh_im + m.m00_im*f.eh_re + m.m01_re*f.ev_im + m.m01_im*f.ev_re;")
        lines.append(f"        r.ev_re := m.m10_re*f.eh_re - m.m10_im*f.eh_im + m.m11_re*f.ev_re - m.m11_im*f.ev_im;")
        lines.append(f"        r.ev_im := m.m10_re*f.eh_im + m.m10_im*f.eh_re + m.m11_re*f.ev_im + m.m11_im*f.ev_re;")
        lines.append(f"        r.freq := f.freq;")
        lines.append(f"        return r;")
        lines.append(f"    end function;")

    if has_matrices:
        lines.append(f"")

    lines.append(f"begin")
    lines.append(f"")

    proc_idx = 0

    # Leaf nets
    leaf_others = []
    for net in leaf_nets:
        for i, ep in enumerate(net.endpoints):
            if (net.name, i) in alias_map:
                _, oth = alias_map[(net.name, i)]
                zero = _zero_constant(net.sig_type)
                leaf_others.append((oth, zero))

    if leaf_others:
        lines.append(f"    p_leaf: process")
        lines.append(f"    begin")
        for oth, zero in leaf_others:
            lines.append(f"        {oth} := {zero};")
        lines.append(f"        wait;")
        lines.append(f"    end process;")
        lines.append(f"")
        proc_idx += 1

    # Active nets
    for net in active_nets:
        n_ep = len(net.endpoints)
        resolve_func, vec_type, _ = _type_info(net.sig_type)
        has_jones = _needs_jones(net.sig_type)
        apply_func = _jones_apply_func(net.sig_type)

        all_drvs = [alias_map[(net.name, i)][0] for i in range(n_ep)]
        sens = ", ".join(all_drvs)

        if n_ep == 2 and has_jones:
            drv0, oth0 = alias_map[(net.name, 0)]
            drv1, oth1 = alias_map[(net.name, 1)]

            lines.append(f"    p_{proc_idx}: process({sens})")
            lines.append(f"    begin")

            expr1 = _apply_transfer(drv1, net.name, 1, mat_constants, apply_func)
            lines.append(f"        {oth0} := {expr1};")
            expr0 = _apply_transfer(drv0, net.name, 0, mat_constants, apply_func)
            lines.append(f"        {oth1} := {expr0};")

            lines.append(f"    end process;")
            lines.append(f"")

        elif n_ep == 2:
            drv0, oth0 = alias_map[(net.name, 0)]
            drv1, oth1 = alias_map[(net.name, 1)]
            lines.append(f"    p_{proc_idx}: process({sens})")
            lines.append(f"    begin")
            lines.append(f"        {oth0} := {drv1};")
            lines.append(f"        {oth1} := {drv0};")
            lines.append(f"    end process;")
            lines.append(f"")

        else:
            lines.append(f"    p_{proc_idx}: process({sens})")
            if n_ep > 2:
                lines.append(f"        variable v : {vec_type}"
                             f"(0 to {n_ep - 2});")
            lines.append(f"    begin")

            for i in range(n_ep):
                _, oth_self = alias_map[(net.name, i)]
                others = [j for j in range(n_ep) if j != i]

                if len(others) == 1:
                    j = others[0]
                    drv_j = alias_map[(net.name, j)][0]
                    expr = _apply_transfer(drv_j, net.name, j, mat_constants, apply_func)
                    lines.append(f"        {oth_self} := {expr};")
                else:
                    for k, j in enumerate(others):
                        drv_j = alias_map[(net.name, j)][0]
                        expr = _apply_transfer(drv_j, net.name, j, mat_constants, apply_func)
                        lines.append(f"        v({k}) := {expr};")
                    lines.append(f"        {oth_self} := {resolve_func}(v);")

            lines.append(f"    end process;")
            lines.append(f"")

        proc_idx += 1

    lines.append(f"end architecture;")
    lines.append(f"")

    # Wrapper
    lines.append(f"entity {wrapper} is end;")
    lines.append(f"architecture wrapper of {wrapper} is")
    lines.append(f"begin")
    lines.append(f"    dut: entity work.{design_name};")
    lines.append(f"    resolver: entity work.resolver_{design_name};")
    lines.append(f"end architecture;")
    lines.append(f"")

    return "\n".join(lines)


def _apply_transfer(drv_alias: str, net_name: str, ep_idx: int,
                    mat_constants: dict, apply_func: str) -> str:
    key = (net_name, ep_idx)
    if key in mat_constants:
        return f"{apply_func}({mat_constants[key]}, {drv_alias})"
    return drv_alias


# ---------------------------------------------------------------------------
# Built-in test: antenna link
# ---------------------------------------------------------------------------

def build_test_antenna_link():
    """Build connectivity for TX antenna -> air -> RX antenna test.

    Circuit:
        rf_source (1 mW, 2.4 GHz) -> antenna_tx.feed
        antenna_tx.air <-[air net]-> antenna_rx.air
        antenna_rx.feed (leaf, checked by resolver)
        source feed net: rf_source.output <-> antenna_tx.feed

    The air net has Friis coupling between the two antennas.
    """
    nets = []
    freq = 2.4e9
    distance = 10.0

    coupling = FriisCoupling(distance, freq).to_jones_matrix()

    # Net: source -> TX antenna feed
    n_feed_tx = Net("n_feed_tx", "rf_signal")
    n_feed_tx.endpoints.append(Endpoint(
        "source", "src1", "rf_source", "output",
        JONES_IDENTITY, "1 mW CW"))
    n_feed_tx.endpoints.append(Endpoint(
        "component_port", "ant_tx", "rf_antenna", "feed",
        JONES_IDENTITY))
    nets.append(n_feed_tx)

    # Net: air (TX antenna <-> RX antenna with Friis coupling)
    n_air = Net("n_air", "rf_signal")
    n_air.endpoints.append(Endpoint(
        "component_port", "ant_tx", "rf_antenna", "air",
        coupling, "Friis FSPL"))
    n_air.endpoints.append(Endpoint(
        "component_port", "ant_rx", "rf_antenna", "air",
        coupling, "Friis FSPL"))
    nets.append(n_air)

    # Net: RX antenna feed (leaf)
    n_feed_rx = Net("n_feed_rx", "rf_signal")
    n_feed_rx.endpoints.append(Endpoint(
        "component_port", "ant_rx", "rf_antenna", "feed",
        JONES_IDENTITY))
    nets.append(n_feed_rx)

    return "test_antenna_link", nets


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    import sys
    import argparse

    parser = argparse.ArgumentParser(
        description="Generate RF resolver VHDL")
    parser.add_argument("--from-json", metavar="FILE",
                        help="Read connectivity from NVC JSON export")
    args = parser.parse_args()

    if args.from_json:
        import json
        with open(args.from_json) as f:
            data = json.load(f)
        design = data["design"]
        nets = []
        for jnet in data["nets"]:
            sig_type = jnet.get("type", "rf_signal")
            net = Net(jnet["net"], sig_type=sig_type)
            for ep in jnet["endpoints"]:
                transfer = JONES_IDENTITY
                if "transfer" in ep:
                    t = ep["transfer"]
                    transfer = JonesMatrix(
                        t.get("m00_re", 1), t.get("m00_im", 0),
                        t.get("m01_re", 0), t.get("m01_im", 0),
                        t.get("m10_re", 0), t.get("m10_im", 0),
                        t.get("m11_re", 1), t.get("m11_im", 0))
                net.endpoints.append(Endpoint(
                    kind=ep.get("kind", "component_port"),
                    instance=ep.get("instance", ""),
                    entity=ep.get("entity", ""),
                    port=ep.get("port", ""),
                    transfer=transfer,
                    comment=ep.get("comment", "")))
            nets.append(net)
        vhdl = emit_resolver_vhdl(design, nets)
        print(vhdl)
    else:
        design, nets = build_test_antenna_link()
        vhdl = emit_resolver_vhdl(design, nets)
        print(vhdl)
        outfile = f"resolver_{design}.vhd"
        with open(outfile, "w") as f:
            f.write(vhdl)
        print(f"\n-- Written to {outfile}", file=sys.stderr)


if __name__ == "__main__":
    main()
