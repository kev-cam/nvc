#!/usr/bin/env python3
"""
gen_resolver.py -- Generate resolver network VHDL from design connectivity

The resolver is regular VHDL between inputs and outputs:
  - Inputs:  'driver signals from tran entities (logic3ds)
             + regular assign values (std_logic, converted to logic3ds)
  - Outputs: 'other signals for each tran endpoint (logic3ds)

For each net with N drivers, each endpoint i gets:
    other(i) = resolve(all drivers except i)
For N=2 this is a simple swap.

Usage:
    python3 gen_resolver.py
"""

from dataclasses import dataclass, field
from typing import Optional


@dataclass
class Endpoint:
    """One driver on a net."""
    kind: str           # "tran_port" or "assign"
    instance: str       # e.g. "gen_chain(1).tc" for tran; "" for assign
    entity: str         # e.g. "sv_tran"
    arch: str           # e.g. "strength"
    port: str           # e.g. "a", "b" for tran; "" for assign
    source_expr: str    # for assigns: source signal name (e.g. "a")
    str_one: int = 8    # strength when driving 1 (l3ds_strength)
    str_zero: int = 8   # strength when driving 0 (l3ds_strength)
    comment: str = ""   # e.g. "(supply1, strong0)"

    @property
    def has_implicit(self):
        """Whether this endpoint uses 'driver/'other."""
        return self.kind == "tran_port"


@dataclass
class Net:
    """A resolved net with multiple endpoints."""
    name: str
    sig_type: str = "std_logic"
    endpoints: list = field(default_factory=list)
    # For vector signal elements: parent signal name and index
    # e.g. name="ac(2)" → parent_signal="ac", element_index=2
    parent_signal: Optional[str] = None
    element_index: Optional[int] = None
    parent_type: Optional[str] = None  # e.g. "std_logic_vector(1 to 7)"


# Strength constants (must match logic3ds_pkg)
ST_HIGHZ  = 0
ST_WEAK   = 2
ST_PULL   = 4
ST_STRONG = 8
ST_SUPPLY = 16

STR_MAP = {
    "supply": ST_SUPPLY, "strong": ST_STRONG,
    "pull": ST_PULL, "weak": ST_WEAK, "highz": ST_HIGHZ,
}
STR_VHDL = {
    ST_SUPPLY: "ST_SUPPLY", ST_STRONG: "ST_STRONG",
    ST_PULL: "ST_PULL", ST_WEAK: "ST_WEAK", ST_HIGHZ: "ST_HIGHZ",
}


def build_test_tran_str():
    """Build connectivity model for test_tran_str (translation of tran.v)."""
    nets = []
    str_names = ["supply", "strong", "pull", "weak", "highz"]

    AC_TYPE = "std_logic_vector(1 to 7)"
    AG_TYPE = "std_logic_vector(1 to 25)"

    # --- Chain: ac(1) through ac(7) ---
    ac1 = Net("ac(1)", parent_signal="ac", element_index=1,
              parent_type=AC_TYPE)
    ac1.endpoints.append(Endpoint(
        "assign", "", "", "", "", "a",
        ST_SUPPLY, ST_SUPPLY, "(supply1, supply0)"))
    ac1.endpoints.append(Endpoint(
        "tran_port", "gen_chain(1).tc", "sv_tran", "strength", "a", ""))
    nets.append(ac1)

    for i in range(2, 7):
        net = Net(f"ac({i})", parent_signal="ac", element_index=i,
                  parent_type=AC_TYPE)
        net.endpoints.append(Endpoint(
            "tran_port", f"gen_chain({i-1}).tc", "sv_tran", "strength", "b", ""))
        net.endpoints.append(Endpoint(
            "tran_port", f"gen_chain({i}).tc", "sv_tran", "strength", "a", ""))
        nets.append(net)

    ac7 = Net("ac(7)", parent_signal="ac", element_index=7,
              parent_type=AC_TYPE)
    ac7.endpoints.append(Endpoint(
        "tran_port", "gen_chain(6).tc", "sv_tran", "strength", "b", ""))
    nets.append(ac7)

    # --- Grid: ag(k), bg(k) for k=1..25 ---
    for i in range(1, 6):
        for j in range(1, 6):
            k = (i - 1) * 5 + j
            inst = f"gen_row({i}).gen_col({j}).t_grid"
            s1 = STR_MAP[str_names[i - 1]]
            s0 = STR_MAP[str_names[j - 1]]
            comment = f"({str_names[i-1]}1, {str_names[j-1]}0)"

            ag = Net(f"ag({k})", parent_signal="ag", element_index=k,
                     parent_type=AG_TYPE)
            if not (i == 5 and j == 5):
                ag.endpoints.append(Endpoint(
                    "assign", "", "", "", "", "a", s1, s0, comment))
            ag.endpoints.append(Endpoint(
                "tran_port", inst, "sv_tran", "strength", "a", ""))
            nets.append(ag)

            bg = Net(f"bg({k})", parent_signal="bg", element_index=k,
                     parent_type=AG_TYPE)
            if not (i == 5 and j == 5):
                bg.endpoints.append(Endpoint(
                    "assign", "", "", "", "", "b", s1, s0, comment))
            bg.endpoints.append(Endpoint(
                "tran_port", inst, "sv_tran", "strength", "b", ""))
            nets.append(bg)

    return "test_tran_str", nets


def emit_resolver_vhdl(design_name, nets):
    """Generate resolver VHDL with deposit-based processes.

    All writes to implicit signals ('other, 'receiver) use deposit (:=)
    inside processes.  This is the sv2ghdl model: signal values come from
    resolution, not from VHDL signal assignment.
    """
    lines = []
    wrapper = f"resolved_{design_name}"

    # Classify nets
    active_nets = [n for n in nets if len(n.endpoints) > 1]
    tran_tran = [n for n in active_nets
                 if all(e.has_implicit for e in n.endpoints)]
    assign_tran = [n for n in active_nets
                   if any(not e.has_implicit for e in n.endpoints)]
    leaf_nets = [n for n in nets if len(n.endpoints) == 1]

    # Nets that need receiver deposits: ALL nets with at least one tran
    # endpoint.  The resolver must deposit the resolved tran contribution
    # to signal.receiver for the parent signal to reflect it.
    #
    # For tran-only nets (tran<->tran, leaf): receiver = resolve(all tran drivers)
    # For assign+tran nets: receiver = resolve(tran drivers only)
    #   The assign's contribution comes from the direct VHDL concurrent
    #   assignment (SOURCE_DRIVER), so we only include tran cross-drive
    #   in the receiver to avoid double-counting.
    rcv_nets = [n for n in nets
                if any(e.has_implicit for e in n.endpoints)]

    lines.append(f"-- Resolver networks for {design_name}")
    lines.append(f"-- Auto-generated by gen_resolver.py")
    lines.append(f"--")
    lines.append(f"-- All writes use deposit (:=) inside processes.")
    lines.append(f"-- {len(active_nets)} nets needing resolution:")
    lines.append(f"--   {len(tran_tran)} tran<->tran  (swap 'driver/'other)")
    lines.append(f"--   {len(assign_tran)} assign+tran (regular driver + implicit)")
    lines.append(f"--   {len(leaf_nets)} leaf nets (no resolution)")
    lines.append(f"--   {len(rcv_nets)} nets needing receiver deposit")
    lines.append(f"")

    # --- Connectivity detail as comments ---
    lines.append(f"-- ====================================================================")
    lines.append(f"-- Net connectivity")
    lines.append(f"-- ====================================================================")
    for net in active_nets:
        parts = []
        for ep in net.endpoints:
            if ep.kind == "assign":
                parts.append(f"assign({ep.source_expr}) {ep.comment}")
            else:
                parts.append(f"{ep.instance}.{ep.port}")
        lines.append(f"-- {net.name}: {' <-> '.join(parts)}")
    lines.append(f"")

    # --- Entity ---
    lines.append(f"library ieee;")
    lines.append(f"use ieee.std_logic_1164.all;")
    lines.append(f"use work.logic3ds_pkg.all;")
    lines.append(f"")
    lines.append(f"entity resolver_{design_name} is")
    lines.append(f"end entity;")
    lines.append(f"")
    lines.append(f"architecture generated of resolver_{design_name} is")
    lines.append(f"")

    # --- External names: source signals for regular assigns ---
    source_sigs = set()
    for net in active_nets:
        for ep in net.endpoints:
            if ep.kind == "assign":
                source_sigs.add(ep.source_expr)

    if source_sigs:
        lines.append(f"    -- Source signals for regular assigns (read from DUT)")
        for sig in sorted(source_sigs):
            lines.append(f"    alias src_{sig} is"
                         f" << signal .{wrapper}.dut.{sig} : std_logic >>;")
        lines.append(f"")

    # --- External names: 'driver/'other for tran endpoints ---
    lines.append(f"    -- Implicit signals inside tran instances")
    lines.append(f"    -- drv_N = 'driver (what tran drives onto net)")
    lines.append(f"    -- oth_N = 'other (what tran sees from all other drivers)")

    alias_map = {}  # (net_name, ep_index) -> (drv_alias, oth_alias)
    alias_idx = 0

    for net in nets:
        for i, ep in enumerate(net.endpoints):
            if not ep.has_implicit:
                continue
            drv = f"drv_{alias_idx}"
            oth = f"oth_{alias_idx}"
            inst_path = ep.instance
            port_lower = ep.port
            lines.append(f"    -- {net.name}: {ep.instance}.{ep.port}")
            lines.append(f"    alias {drv} is << signal"
                         f" .{wrapper}.dut.{inst_path}"
                         f".{port_lower}.driver : logic3ds >>;")
            lines.append(f"    alias {oth} is << signal"
                         f" .{wrapper}.dut.{inst_path}"
                         f".{port_lower}.other : logic3ds >>;")
            alias_map[(net.name, i)] = (drv, oth)
            alias_idx += 1

    lines.append(f"")

    # --- External names: signal.receiver for tran-only nets ---
    # Group by parent signal for vector types
    rcv_map = {}       # net_name -> (rcv_alias, element_index or None)
    rcv_aliases = {}   # parent_signal -> alias_name (dedup for vectors)
    if rcv_nets:
        lines.append(f"    -- Receiver signals for tran-only nets")
        lines.append(f"    -- Deposit resolved value here for testbench observability")
        for net in rcv_nets:
            if net.parent_signal:
                # Vector element: one alias per parent signal
                parent = net.parent_signal
                if parent not in rcv_aliases:
                    rcv_alias = f"rcv_{parent}"
                    rcv_aliases[parent] = rcv_alias
                    lines.append(
                        f"    alias {rcv_alias} is << signal"
                        f" .{wrapper}.dut.{parent}.receiver"
                        f" : {net.parent_type} >>;")
                rcv_map[net.name] = (rcv_aliases[parent], net.element_index)
            else:
                # Scalar signal
                rcv_alias = f"rcv_{net.name.replace('(', '_').replace(')', '')}"
                lines.append(
                    f"    alias {rcv_alias} is << signal"
                    f" .{wrapper}.dut.{net.name}.receiver : std_logic >>;")
                rcv_map[net.name] = (rcv_alias, None)
        lines.append(f"")

    # --- Helper function for asymmetric strength conversion ---
    lines.append(f"    -- Convert std_logic to logic3ds with asymmetric strengths")
    lines.append(f"    -- Models Verilog: assign (str1, str0) y = d;")
    lines.append(f"    function to_logic3ds_asym(")
    lines.append(f"        val : std_logic; str1, str0 : l3ds_strength")
    lines.append(f"    ) return logic3ds is")
    lines.append(f"    begin")
    lines.append(f"        case val is")
    lines.append(f"            when '1' | 'H' => return l3ds_drive(true, str1);")
    lines.append(f"            when '0' | 'L' => return l3ds_drive(false, str0);")
    lines.append(f"            when 'Z'       => return L3DS_Z;")
    lines.append(f"            when others     =>")
    lines.append(f"                -- IEEE 1364: if one side is highz (no drive),")
    lines.append(f"                -- the other side wins even with X input.")
    lines.append(f"                -- Both sides non-highz: stays X at max strength.")
    lines.append(f"                if str1 = ST_HIGHZ and str0 = ST_HIGHZ then")
    lines.append(f"                    return L3DS_Z;")
    lines.append(f"                elsif str1 = ST_HIGHZ then")
    lines.append(f"                    return l3ds_drive(false, str0);")
    lines.append(f"                elsif str0 = ST_HIGHZ then")
    lines.append(f"                    return l3ds_drive(true, str1);")
    lines.append(f"                elsif str_gt(str1, str0) then")
    lines.append(f"                    return make_logic3ds(0, str1, FL_UNKNOWN);")
    lines.append(f"                else")
    lines.append(f"                    return make_logic3ds(0, str0, FL_UNKNOWN);")
    lines.append(f"                end if;")
    lines.append(f"        end case;")
    lines.append(f"    end function;")
    lines.append(f"")

    lines.append(f"begin")
    lines.append(f"")

    # --- Resolution processes ---
    proc_idx = 0

    # Leaf nets: set 'other to L3DS_Z (no other drivers)
    leaf_tran_others = []
    for net in leaf_nets:
        for i, ep in enumerate(net.endpoints):
            if ep.has_implicit and (net.name, i) in alias_map:
                _, oth = alias_map[(net.name, i)]
                leaf_tran_others.append((net.name, ep, oth))

    if leaf_tran_others:
        lines.append(f"    ---------------------------------------------------------------")
        lines.append(f"    -- Leaf nets: single tran endpoint, no other drivers")
        lines.append(f"    -- Set 'other to L3DS_Z (undriven)")
        lines.append(f"    ---------------------------------------------------------------")
        lines.append(f"    p_leaf: process")
        lines.append(f"    begin")
        for net_name, ep, oth in leaf_tran_others:
            lines.append(f"        -- {net_name}: {ep.instance}.{ep.port}")
            lines.append(f"        {oth} := L3DS_Z;")
        lines.append(f"        wait;")
        lines.append(f"    end process;")
        lines.append(f"")
        proc_idx += 1

    # tran<->tran N=2: swap deposits
    tt_n2 = [n for n in tran_tran if len(n.endpoints) == 2]
    if tt_n2:
        lines.append(f"    ---------------------------------------------------------------")
        lines.append(f"    -- tran <-> tran (N=2): swap 'driver/'other via deposit")
        lines.append(f"    ---------------------------------------------------------------")

        # Group all N=2 swaps into a single process for efficiency
        all_drvs = []
        swap_stmts = []
        for net in tt_n2:
            drv0, oth0 = alias_map[(net.name, 0)]
            drv1, oth1 = alias_map[(net.name, 1)]
            all_drvs.extend([drv0, drv1])
            ep0 = net.endpoints[0]
            ep1 = net.endpoints[1]
            swap_stmts.append(
                f"        -- {net.name}: "
                f"{ep0.instance}.{ep0.port} <-> "
                f"{ep1.instance}.{ep1.port}")
            swap_stmts.append(f"        {oth0} := {drv1};")
            swap_stmts.append(f"        {oth1} := {drv0};")

        lines.append(f"    p_swap: process({', '.join(all_drvs)})")
        lines.append(f"    begin")
        lines.extend(swap_stmts)
        lines.append(f"    end process;")
        lines.append(f"")
        proc_idx += 1

    # tran<->tran N>2: resolve and deposit
    tt_multi = [n for n in tran_tran if len(n.endpoints) > 2]
    for net in tt_multi:
        tran_eps = [(i, ep) for i, ep in enumerate(net.endpoints)
                    if ep.has_implicit]
        drvs = [alias_map[(net.name, i)][0] for i, _ in tran_eps]
        lines.append(f"    -- {net.name}: {len(tran_eps)} tran endpoints")
        lines.append(f"    p_resolve_{proc_idx}: process({', '.join(drvs)})")
        lines.append(f"        variable others_vec : logic3ds_vector"
                     f"(0 to {len(tran_eps)-2});")
        lines.append(f"    begin")
        for idx, (ep_i, ep) in enumerate(tran_eps):
            drv_self, oth_self = alias_map[(net.name, ep_i)]
            others = [alias_map[(net.name, j)][0]
                      for j, _ in tran_eps if j != ep_i]
            for k, o in enumerate(others):
                lines.append(f"        others_vec({k}) := {o};")
            lines.append(f"        {oth_self} := l3ds_resolve(others_vec);")
        lines.append(f"    end process;")
        lines.append(f"")
        proc_idx += 1

    # assign+tran: deposit assign value to tran's 'other
    if assign_tran:
        lines.append(f"    ---------------------------------------------------------------")
        lines.append(f"    -- assign + tran: deposit source value to tran 'other")
        lines.append(f"    ---------------------------------------------------------------")

        # Group by source signal for efficiency
        src_groups = {}  # source_expr -> [(net, tran_idx, s1, s0)]
        for net in assign_tran:
            assign_ep = [e for e in net.endpoints if e.kind == "assign"][0]
            tran_idx = next(i for i, e in enumerate(net.endpoints)
                           if e.kind == "tran_port")
            src_expr = assign_ep.source_expr
            if src_expr not in src_groups:
                src_groups[src_expr] = []
            src_groups[src_expr].append(
                (net, tran_idx, assign_ep.str_one, assign_ep.str_zero,
                 assign_ep.comment))

        for src_expr, group in sorted(src_groups.items()):
            src_alias = f"src_{src_expr}"
            lines.append(f"    p_assign_{proc_idx}: process({src_alias})")
            lines.append(f"    begin")
            for net, tran_idx, s1, s0, comment in group:
                tran_ep = net.endpoints[tran_idx]
                _, oth_t = alias_map[(net.name, tran_idx)]
                s1v = STR_VHDL[s1]
                s0v = STR_VHDL[s0]
                lines.append(f"        -- {net.name}: assign({src_expr})"
                             f" {comment} -> "
                             f"{tran_ep.instance}.{tran_ep.port}")
                lines.append(f"        {oth_t} := to_logic3ds_asym("
                             f"{src_alias}, {s1v}, {s0v});")
            lines.append(f"    end process;")
            lines.append(f"")
            proc_idx += 1

    # Receiver deposits for all nets with tran endpoints
    #
    # The DUT has NO concurrent assignments — all signal values come from
    # the resolver via deposit to signal.receiver.  This is critical because
    # plain VHDL assignments drive at full strength and would overpower
    # strength-attenuated values from the resolver.
    #
    # For each net, we resolve ALL contributions:
    #   assign: to_logic3ds_asym(source, str1, str0)
    #   tran:   drv_N ('driver cross-drive value)
    #
    # NVC limitation: element-wise deposits to vector receiver aliases
    # don't work.  We must build complete vector values and deposit
    # the whole vector at once.
    if rcv_nets:
        lines.append(f"    ---------------------------------------------------------------")
        lines.append(f"    -- Resolve ALL drivers (assign + tran) and deposit to receiver")
        lines.append(f"    -- No concurrent assignments in DUT; this is the sole source")
        lines.append(f"    -- of signal values, preserving strength semantics.")
        lines.append(f"    ---------------------------------------------------------------")

        # Collect all tran drivers AND source signals for the sensitivity list
        all_rcv_sens = []
        for net in rcv_nets:
            tran_eps = [(i, ep) for i, ep in enumerate(net.endpoints)
                        if ep.has_implicit]
            drvs = [alias_map[(net.name, i)][0] for i, _ in tran_eps]
            all_rcv_sens.extend(drvs)
        # Add source signals for assigns
        for net in rcv_nets:
            for ep in net.endpoints:
                if ep.kind == "assign":
                    all_rcv_sens.append(f"src_{ep.source_expr}")

        # Deduplicate sensitivity list
        seen = set()
        unique_sens = []
        for d in all_rcv_sens:
            if d not in seen:
                seen.add(d)
                unique_sens.append(d)

        lines.append(f"    p_receivers: process({', '.join(unique_sens)})")

        # Variable for resolve with many drivers
        max_all_eps = max(len(n.endpoints) for n in rcv_nets) \
            if rcv_nets else 0
        if max_all_eps > 2:
            lines.append(f"        variable rcv_vec : logic3ds_vector"
                         f"(0 to {max_all_eps - 1});")

        # Group nets by parent signal for vector types
        vec_groups = {}   # parent_signal -> [(element_index, net)]
        scalar_nets = []
        for net in rcv_nets:
            if net.parent_signal:
                if net.parent_signal not in vec_groups:
                    vec_groups[net.parent_signal] = []
                vec_groups[net.parent_signal].append(
                    (net.element_index, net))
            else:
                scalar_nets.append(net)

        # Declare vector variables for whole-vector deposits
        for parent, elems in sorted(vec_groups.items()):
            ptype = elems[0][1].parent_type
            var_name = f"v_{parent}"
            lines.append(f"        variable {var_name} : {ptype}"
                         f" := (others => 'U');")

        lines.append(f"    begin")

        def emit_net_resolve(net, target_expr, indent="        "):
            """Emit resolution code for a net, including assign contributions."""
            # Collect ALL contributions (assign + tran)
            contribs = []  # list of (expr_str, is_tran)
            for i, ep in enumerate(net.endpoints):
                if ep.has_implicit:
                    drv, _ = alias_map[(net.name, i)]
                    contribs.append(drv)
                elif ep.kind == "assign":
                    s1v = STR_VHDL[ep.str_one]
                    s0v = STR_VHDL[ep.str_zero]
                    contribs.append(
                        f"to_logic3ds_asym(src_{ep.source_expr},"
                        f" {s1v}, {s0v})")

            if len(contribs) == 1:
                lines.append(
                    f"{indent}{target_expr}"
                    f" := to_std_logic({contribs[0]});")
            elif len(contribs) == 2:
                lines.append(
                    f"{indent}{target_expr}"
                    f" := to_std_logic(l3ds_resolve("
                    f"logic3ds_vector'({contribs[0]}, {contribs[1]})));")
            else:
                for k, c in enumerate(contribs):
                    lines.append(f"{indent}rcv_vec({k}) := {c};")
                lines.append(
                    f"{indent}{target_expr}"
                    f" := to_std_logic("
                    f"l3ds_resolve(rcv_vec(0 to {len(contribs)-1})));")

        # Scalar nets
        for net in scalar_nets:
            rcv_alias, rcv_idx = rcv_map[net.name]
            lines.append(f"        -- {net.name}")
            emit_net_resolve(net, rcv_alias)

        # Vector nets: build each vector, deposit whole
        for parent, elems in sorted(vec_groups.items()):
            var_name = f"v_{parent}"
            rcv_alias = rcv_aliases[parent]
            lines.append(f"        -- {parent}: compute all elements")

            for elem_idx, net in sorted(elems):
                emit_net_resolve(net, f"{var_name}({elem_idx})")

            lines.append(f"        {rcv_alias} := {var_name};")
            lines.append(f"")

        lines.append(f"    end process;")
        lines.append(f"")

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


def load_from_json(json_path):
    """Load net connectivity from NVC --export-resolvers JSON."""
    import json
    with open(json_path) as f:
        data = json.load(f)

    design = data["design"]
    nets = []
    for jnet in data["nets"]:
        net = Net(jnet["net"])
        for ep in jnet["endpoints"]:
            kind = ep["kind"]
            if kind == "driver":
                net.endpoints.append(Endpoint(
                    kind="assign", instance="", entity="", arch="",
                    port="", source_expr=""))
            elif kind == "tran":
                net.endpoints.append(Endpoint(
                    kind="tran_port", instance=ep["instance"],
                    entity="", arch="", port=ep["port"], source_expr=""))
            elif kind == "gate":
                net.endpoints.append(Endpoint(
                    kind="gate_port", instance=ep["instance"],
                    entity="", arch="", port=ep["port"], source_expr=""))
        nets.append(net)

    return design, nets


def echo_connectivity(design, nets):
    """Print what we understood from the JSON, for verification."""
    lines = []
    lines.append(f"# Resolver connectivity for {design}")
    lines.append(f"# {len(nets)} nets total")
    lines.append(f"#")

    tran_tran = 0
    driver_tran = 0
    leaf = 0
    for net in nets:
        kinds = [e.kind for e in net.endpoints]
        n_tran = kinds.count("tran_port")
        n_assign = kinds.count("assign")
        n_gate = kinds.count("gate_port")

        if len(net.endpoints) <= 1:
            leaf += 1
            tag = "leaf"
        elif n_assign > 0 and n_tran > 0:
            driver_tran += 1
            tag = "driver+tran"
        elif n_tran >= 2:
            tran_tran += 1
            tag = "tran<->tran"
        else:
            tag = "other"

        parts = []
        for ep in net.endpoints:
            if ep.kind == "assign":
                parts.append("driver")
            elif ep.kind == "tran_port":
                parts.append(f"{ep.instance}.{ep.port}(tran)")
            elif ep.kind == "gate_port":
                parts.append(f"{ep.instance}.{ep.port}(gate)")
        lines.append(f"# [{tag:13s}] {net.name}: {' <-> '.join(parts)}")

    lines.append(f"#")
    lines.append(f"# Summary: {tran_tran} tran<->tran, "
                 f"{driver_tran} driver+tran, {leaf} leaf")
    return "\n".join(lines)


def main():
    import sys
    import argparse

    parser = argparse.ArgumentParser(description="Generate resolver VHDL")
    parser.add_argument("--from-json", metavar="FILE",
                        help="Read connectivity from NVC JSON export")
    args = parser.parse_args()

    if args.from_json:
        design, nets = load_from_json(args.from_json)
        print(echo_connectivity(design, nets))
    else:
        design_name, nets = build_test_tran_str()
        vhdl = emit_resolver_vhdl(design_name, nets)
        outfile = f"resolver_{design_name}.vhd"
        with open(outfile, "w") as f:
            f.write(vhdl)
        print(vhdl)
        print(f"\n-- Written to {outfile}", file=sys.stderr)


if __name__ == "__main__":
    main()
