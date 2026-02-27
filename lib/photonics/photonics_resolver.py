#!/usr/bin/env python3
"""
photonics_resolver.py -- Generate resolver network VHDL for photonic circuits

The resolver is regular VHDL between inputs and outputs:
  - Inputs:  'driver signals from optical components (optical_field)
  - Outputs: 'other signals for each component endpoint (optical_field)

For each optical net with N drivers, each endpoint i gets:
    other(i) = resolve(all drivers except i)

The key difference from digital resolution (sv2vhdl) is that optical
components have transfer matrices: the resolver applies Jones matrix
transformations per-path rather than simple value swaps.

Type selection is pluggable: optical_field -> optical_resolve,
optical_stokes -> stokes_resolve, logic3ds -> l3ds_resolve, etc.

Usage:
    python3 photonics_resolver.py                      # run built-in test
    python3 photonics_resolver.py --from-json FILE     # from NVC JSON export
"""

from dataclasses import dataclass, field
from typing import Optional, Dict, List, Tuple
import math


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

@dataclass
class JonesMatrix:
    """2x2 complex Jones matrix for component transfer function."""
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
        return (abs(self.m00_re) < 1e-12 and abs(self.m00_im) < 1e-12 and
                abs(self.m01_re) < 1e-12 and abs(self.m01_im) < 1e-12 and
                abs(self.m10_re) < 1e-12 and abs(self.m10_im) < 1e-12 and
                abs(self.m11_re) < 1e-12 and abs(self.m11_im) < 1e-12)

    def is_scalar(self) -> bool:
        """True if matrix is a scalar multiple of identity."""
        return (abs(self.m01_re) < 1e-12 and abs(self.m01_im) < 1e-12 and
                abs(self.m10_re) < 1e-12 and abs(self.m10_im) < 1e-12 and
                abs(self.m00_re - self.m11_re) < 1e-12 and
                abs(self.m00_im - self.m11_im) < 1e-12)

    def to_vhdl(self) -> str:
        """VHDL aggregate literal."""
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


def jones_coupler_bar(kappa: float) -> JonesMatrix:
    t = math.sqrt(1.0 - kappa)
    return JonesMatrix(m00_re=t, m11_re=t)


def jones_coupler_cross(kappa: float) -> JonesMatrix:
    k = math.sqrt(kappa)
    return JonesMatrix(m00_re=0, m00_im=k, m01_re=0, m01_im=0,
                       m10_re=0, m10_im=0, m11_re=0, m11_im=k)


def jones_attenuator(factor: float) -> JonesMatrix:
    return JonesMatrix(m00_re=factor, m11_re=factor)


def jones_phase_shift(phi: float) -> JonesMatrix:
    c, s = math.cos(phi), math.sin(phi)
    return JonesMatrix(m00_re=c, m00_im=s, m11_re=c, m11_im=s)


def jones_rotation(theta: float) -> JonesMatrix:
    c, s = math.cos(theta), math.sin(theta)
    return JonesMatrix(m00_re=c, m01_re=-s, m10_re=s, m11_re=c)


def jones_waveguide(length: float, neff: float, wavelength: float,
                    loss_db_per_m: float) -> JonesMatrix:
    phi = 2.0 * math.pi * neff * length / wavelength
    amp = 10.0 ** (-loss_db_per_m * length / 20.0)
    c, s = math.cos(phi), math.sin(phi)
    return JonesMatrix(m00_re=amp*c, m00_im=amp*s, m11_re=amp*c, m11_im=amp*s)


@dataclass
class Endpoint:
    """One endpoint on an optical net."""
    kind: str           # "component_port" or "source"
    instance: str       # e.g. "dc1" for a coupler instance
    entity: str         # e.g. "optical_coupler"
    port: str           # e.g. "a1", "b2"
    transfer: JonesMatrix = field(default_factory=lambda: JonesMatrix())
    comment: str = ""


@dataclass
class Net:
    """A resolved optical net with multiple endpoints."""
    name: str
    sig_type: str = "optical_field"
    endpoints: list = field(default_factory=list)


# ---------------------------------------------------------------------------
# Type info: pluggable resolution strategy
# ---------------------------------------------------------------------------

def _type_info(sig_type: str) -> Tuple[str, str, List[str]]:
    """Select resolution function, vector type, and use clauses by signal type.

    This is the plug point for different physics domains:
      optical_field  -> optical_resolve / optical_field_vector
      optical_stokes -> stokes_resolve  / optical_stokes_vector
      logic3ds       -> l3ds_resolve    / logic3ds_vector
      logic3d        -> l3d_resolve     / logic3d_vector
      std_logic      -> resolved        / std_ulogic_vector
    """
    t = sig_type.lower()

    if t == "optical_field":
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


# ---------------------------------------------------------------------------
# VHDL generation
# ---------------------------------------------------------------------------

def emit_resolver_vhdl(design_name: str, nets: List[Net]) -> str:
    """Generate resolver VHDL with deposit-based processes.

    All writes to implicit signals ('other, 'receiver) use deposit (:=).
    For optical nets, each endpoint's 'other value is computed by applying
    the transfer matrices of all OTHER endpoints' paths.
    """
    lines = []
    wrapper = f"resolved_{design_name}"

    # Classify nets
    active_nets = [n for n in nets if len(n.endpoints) > 1]
    leaf_nets = [n for n in nets if len(n.endpoints) == 1]

    lines.append(f"-- Photonic resolver networks for {design_name}")
    lines.append(f"-- Auto-generated by photonics_resolver.py")
    lines.append(f"--")
    lines.append(f"-- All writes use deposit (:=) inside processes.")
    lines.append(f"-- {len(active_nets)} nets needing resolution")
    lines.append(f"-- {len(leaf_nets)} leaf nets (no resolution)")
    lines.append(f"")

    # --- Connectivity detail as comments ---
    lines.append(f"-- ====================================================================")
    lines.append(f"-- Net connectivity")
    lines.append(f"-- ====================================================================")
    for net in active_nets:
        parts = []
        for ep in net.endpoints:
            if ep.instance:
                parts.append(f"{ep.instance}.{ep.port}")
            else:
                parts.append(f"source({ep.port})")
        lines.append(f"-- {net.name}: {' <-> '.join(parts)}")
    lines.append(f"")

    # --- Determine libraries needed ---
    all_types = set(n.sig_type for n in nets)
    all_uses = set()
    for t in all_types:
        _, _, uses = _type_info(t)
        for u in uses:
            all_uses.add(u)

    # Always need ieee for math_real
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

    # --- External name aliases ---
    alias_map = {}  # (net_name, ep_index) -> (drv_alias, oth_alias)
    alias_idx = 0

    lines.append(f"    -- Implicit signals inside component instances")
    lines.append(f"    -- drv_N = 'driver (what component drives onto net)")
    lines.append(f"    -- oth_N = 'other (what component sees from all other drivers)")

    for net in nets:
        resolve_func, vec_type, _ = _type_info(net.sig_type)
        ep_type = net.sig_type.lower()

        for i, ep in enumerate(net.endpoints):
            drv = f"drv_{alias_idx}"
            oth = f"oth_{alias_idx}"
            if ep.instance:
                inst_path = ep.instance
                port_lower = ep.port
                lines.append(f"    -- {net.name}: {ep.instance}.{ep.port}")
                lines.append(f"    alias {drv} is << signal"
                             f" .{wrapper}.dut.{inst_path}"
                             f".{port_lower}.driver : {ep_type} >>;")
                lines.append(f"    alias {oth} is << signal"
                             f" .{wrapper}.dut.{inst_path}"
                             f".{port_lower}.other : {ep_type} >>;")
            else:
                # Source endpoint (top-level port or signal)
                lines.append(f"    -- {net.name}: source {ep.port}")
                lines.append(f"    alias {drv} is << signal"
                             f" .{wrapper}.dut.{ep.port}.driver : {ep_type} >>;")
                lines.append(f"    alias {oth} is << signal"
                             f" .{wrapper}.dut.{ep.port}.other : {ep_type} >>;")
            alias_map[(net.name, i)] = (drv, oth)
            alias_idx += 1

    lines.append(f"")

    # --- Transfer matrix constants ---
    mat_constants = {}  # (net_name, ep_idx) -> constant_name
    mat_idx = 0
    has_matrices = False

    for net in active_nets:
        if net.sig_type.lower() != "optical_field":
            continue
        for i, ep in enumerate(net.endpoints):
            if not ep.transfer.is_identity():
                cname = f"M_{mat_idx}"
                mat_constants[(net.name, i)] = cname
                lines.append(f"    constant {cname} : jones_matrix := "
                             f"{ep.transfer.to_vhdl()};")
                mat_idx += 1
                has_matrices = True

    if has_matrices:
        lines.append(f"")

    lines.append(f"begin")
    lines.append(f"")

    proc_idx = 0

    # --- Leaf nets: set 'other to zero (no other drivers) ---
    leaf_others = []
    for net in leaf_nets:
        for i, ep in enumerate(net.endpoints):
            if (net.name, i) in alias_map:
                _, oth = alias_map[(net.name, i)]
                zero_const = _zero_constant(net.sig_type)
                leaf_others.append((net.name, ep, oth, zero_const))

    if leaf_others:
        lines.append(f"    ---------------------------------------------------------------")
        lines.append(f"    -- Leaf nets: single endpoint, no other drivers")
        lines.append(f"    ---------------------------------------------------------------")
        lines.append(f"    p_leaf: process")
        lines.append(f"    begin")
        for net_name, ep, oth, zero in leaf_others:
            desc = f"{ep.instance}.{ep.port}" if ep.instance else f"source {ep.port}"
            lines.append(f"        -- {net_name}: {desc}")
            lines.append(f"        {oth} := {zero};")
        lines.append(f"        wait;")
        lines.append(f"    end process;")
        lines.append(f"")
        proc_idx += 1

    # --- Active nets: per-receiver resolution ---
    for net in active_nets:
        n_ep = len(net.endpoints)
        resolve_func, vec_type, _ = _type_info(net.sig_type)
        is_optical = net.sig_type.lower() == "optical_field"

        # Sensitivity list: all drivers on this net
        all_drvs = [alias_map[(net.name, i)][0] for i in range(n_ep)]
        sens = ", ".join(all_drvs)

        lines.append(f"    -- {net.name}: {n_ep} endpoints")

        if n_ep == 2 and is_optical:
            # Special case: 2-endpoint optical net
            drv0, oth0 = alias_map[(net.name, 0)]
            drv1, oth1 = alias_map[(net.name, 1)]
            ep0 = net.endpoints[0]
            ep1 = net.endpoints[1]

            lines.append(f"    p_{proc_idx}: process({sens})")
            lines.append(f"    begin")

            # Endpoint 0 sees driver 1 (possibly through transfer matrix)
            expr1 = _apply_transfer(drv1, net.name, 1, mat_constants)
            lines.append(f"        {oth0} := {expr1};")

            # Endpoint 1 sees driver 0
            expr0 = _apply_transfer(drv0, net.name, 0, mat_constants)
            lines.append(f"        {oth1} := {expr0};")

            lines.append(f"    end process;")
            lines.append(f"")

        elif n_ep == 2 and not is_optical:
            # Non-optical 2-port: simple swap
            drv0, oth0 = alias_map[(net.name, 0)]
            drv1, oth1 = alias_map[(net.name, 1)]
            lines.append(f"    p_{proc_idx}: process({sens})")
            lines.append(f"    begin")
            lines.append(f"        {oth0} := {drv1};")
            lines.append(f"        {oth1} := {drv0};")
            lines.append(f"    end process;")
            lines.append(f"")

        else:
            # N>2: each receiver gets resolution of all others
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
                    expr = _apply_transfer(drv_j, net.name, j, mat_constants)
                    lines.append(f"        {oth_self} := {expr};")
                else:
                    for k, j in enumerate(others):
                        drv_j = alias_map[(net.name, j)][0]
                        expr = _apply_transfer(drv_j, net.name, j, mat_constants)
                        lines.append(f"        v({k}) := {expr};")
                    lines.append(f"        {oth_self} := {resolve_func}(v);")

            lines.append(f"    end process;")
            lines.append(f"")

        proc_idx += 1

    lines.append(f"end architecture;")
    lines.append(f"")

    # --- Wrapper ---
    lines.append(f"-- Wrapper: instantiates DUT + resolver for standalone simulation")
    lines.append(f"library ieee;")
    lines.append(f"use ieee.std_logic_1164.all;")
    lines.append(f"")
    lines.append(f"entity {wrapper} is end;")
    lines.append(f"architecture wrapper of {wrapper} is")
    lines.append(f"begin")
    lines.append(f"    dut: entity work.{design_name};")
    lines.append(f"    resolver: entity work.resolver_{design_name};")
    lines.append(f"end architecture;")
    lines.append(f"")

    return "\n".join(lines)


def _zero_constant(sig_type: str) -> str:
    """Return the 'zero' constant name for a signal type."""
    t = sig_type.lower()
    if t == "optical_field":
        return "OPTICAL_ZERO"
    elif t == "optical_stokes":
        return "STOKES_ZERO"
    elif t == "logic3ds":
        return "L3DS_Z"
    elif t == "logic3d":
        return "L3D_Z"
    else:
        return "'Z'"


def _apply_transfer(drv_alias: str, net_name: str, ep_idx: int,
                    mat_constants: dict) -> str:
    """VHDL expression for driver value, possibly through a transfer matrix."""
    key = (net_name, ep_idx)
    if key in mat_constants:
        return f"jones_apply({mat_constants[key]}, {drv_alias})"
    return drv_alias


# ---------------------------------------------------------------------------
# JSON import (from NVC --export-resolvers)
# ---------------------------------------------------------------------------

def load_from_json(json_path: str) -> Tuple[str, List[Net]]:
    """Load net connectivity from NVC --export-resolvers JSON."""
    import json
    with open(json_path) as f:
        data = json.load(f)

    design = data["design"]
    nets = []
    for jnet in data["nets"]:
        sig_type = jnet.get("type", "optical_field")
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

    return design, nets


# ---------------------------------------------------------------------------
# Built-in test: source -> coupler -> two detectors
# ---------------------------------------------------------------------------

def build_test_coupler():
    """Build connectivity for a simple coupler test circuit.

    Circuit:
        laser (1 mW, 1550 nm) -> coupler (kappa=0.5) -> det1 (bar port)
                                                       -> det2 (cross port)

    Nets:
        n_input:  laser.output <-> coupler.a1
        n_bar:    coupler.b1   <-> det1.input
        n_cross:  coupler.b2   <-> det2.input
        n_a2:     coupler.a2   (leaf - unused second input)
    """
    nets = []

    kappa = 0.5

    # Net: laser output to coupler input a1
    n_input = Net("n_input", "optical_field")
    n_input.endpoints.append(Endpoint(
        "source", "laser1", "optical_source", "output",
        JONES_IDENTITY, "laser 1 mW"))
    n_input.endpoints.append(Endpoint(
        "component_port", "dc1", "optical_coupler", "a1",
        JONES_IDENTITY))
    nets.append(n_input)

    # Net: coupler bar output to detector 1
    n_bar = Net("n_bar", "optical_field")
    n_bar.endpoints.append(Endpoint(
        "component_port", "dc1", "optical_coupler", "b1",
        jones_coupler_bar(kappa), "bar path"))
    n_bar.endpoints.append(Endpoint(
        "component_port", "det1", "optical_detector", "input",
        JONES_IDENTITY))
    nets.append(n_bar)

    # Net: coupler cross output to detector 2
    n_cross = Net("n_cross", "optical_field")
    n_cross.endpoints.append(Endpoint(
        "component_port", "dc1", "optical_coupler", "b2",
        jones_coupler_cross(kappa), "cross path"))
    n_cross.endpoints.append(Endpoint(
        "component_port", "det2", "optical_detector", "input",
        JONES_IDENTITY))
    nets.append(n_cross)

    # Net: coupler second input (unused, leaf)
    n_a2 = Net("n_a2", "optical_field")
    n_a2.endpoints.append(Endpoint(
        "component_port", "dc1", "optical_coupler", "a2",
        JONES_IDENTITY, "unused input"))
    nets.append(n_a2)

    return "test_coupler", nets


def echo_connectivity(design: str, nets: List[Net]) -> str:
    """Print connectivity summary for verification."""
    lines = []
    lines.append(f"# Resolver connectivity for {design}")
    lines.append(f"# {len(nets)} nets total")
    lines.append(f"#")

    for net in nets:
        n_ep = len(net.endpoints)
        if n_ep <= 1:
            tag = "leaf"
        elif n_ep == 2:
            tag = "pair"
        else:
            tag = f"N={n_ep}"

        parts = []
        for ep in net.endpoints:
            if ep.instance:
                label = f"{ep.instance}.{ep.port}"
            else:
                label = f"source({ep.port})"
            if ep.comment:
                label += f" ({ep.comment})"
            if not ep.transfer.is_identity():
                label += " [matrix]"
            parts.append(label)

        lines.append(f"# [{tag:6s}] {net.name} ({net.sig_type}): "
                     f"{' <-> '.join(parts)}")

    active = sum(1 for n in nets if len(n.endpoints) > 1)
    leaf = sum(1 for n in nets if len(n.endpoints) == 1)
    lines.append(f"#")
    lines.append(f"# Summary: {active} active, {leaf} leaf")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    import sys
    import argparse

    parser = argparse.ArgumentParser(
        description="Generate photonic resolver VHDL")
    parser.add_argument("--from-json", metavar="FILE",
                        help="Read connectivity from NVC JSON export")
    parser.add_argument("--echo", action="store_true",
                        help="Print connectivity summary only")
    args = parser.parse_args()

    if args.from_json:
        design, nets = load_from_json(args.from_json)
        if args.echo:
            print(echo_connectivity(design, nets))
        else:
            vhdl = emit_resolver_vhdl(design, nets)
            outfile = f"resolver_{design}.vhd"
            with open(outfile, "w") as f:
                f.write(vhdl)
            print(vhdl)
            print(f"\n-- Written to {outfile}", file=sys.stderr)
    else:
        design_name, nets = build_test_coupler()
        print(echo_connectivity(design_name, nets))
        print()
        vhdl = emit_resolver_vhdl(design_name, nets)
        outfile = f"resolver_{design_name}.vhd"
        with open(outfile, "w") as f:
            f.write(vhdl)
        print(vhdl)
        print(f"\n-- Written to {outfile}", file=sys.stderr)


if __name__ == "__main__":
    main()
