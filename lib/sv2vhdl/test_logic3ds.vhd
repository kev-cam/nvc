-- Testbench for logic3ds_pkg
-- Exercises: promotion/demotion round-trip, strength resolution,
-- contention -> X, all-highz -> Z, power-missing flag propagation

library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;
use work.logic3ds_pkg.all;

entity test_logic3ds is
end entity;

architecture test of test_logic3ds is

    procedure check(cond : boolean; msg : string) is
    begin
        assert cond report "FAIL: " & msg severity failure;
    end procedure;

    -- Helper: compare logic3ds (ignoring reserved)
    function l3ds_eq(a, b : logic3ds) return boolean is
    begin
        return a.value = b.value and
               a.strength = b.strength and
               a.flags = b.flags;
    end function;

begin
    process
        variable s : logic3ds;
        variable d : logic3d;
        variable r : logic3ds;
    begin

        -----------------------------------------------------------------------
        -- 1. Promotion round-trip: logic3d -> logic3ds -> logic3d
        -----------------------------------------------------------------------
        report "=== Test 1: Promotion/demotion round-trip ===" severity note;

        -- L3D_0 at strong -> L3DS strong 0 -> L3D_0
        s := to_logic3ds(L3D_0, ST_STRONG);
        check(s.value = 0, "L3D_0 promote: value");
        check(s.strength = ST_STRONG, "L3D_0 promote: strength");
        check(s.flags = FL_KNOWN, "L3D_0 promote: flags");
        d := to_logic3d(s);
        check(d = L3D_0, "L3D_0 round-trip");

        -- L3D_1 at strong -> L3DS strong 1 -> L3D_1
        s := to_logic3ds(L3D_1, ST_STRONG);
        check(s.value = 255, "L3D_1 promote: value");
        check(s.strength = ST_STRONG, "L3D_1 promote: strength");
        d := to_logic3d(s);
        check(d = L3D_1, "L3D_1 round-trip");

        -- L3D_L (undriven 0) -> weak 0 -> L3D_L
        s := to_logic3ds(L3D_L, ST_STRONG);  -- str ignored, uses ST_WEAK
        check(s.strength = ST_WEAK, "L3D_L promote: strength forced to weak");
        check(s.value = 0, "L3D_L promote: value");
        d := to_logic3d(s);
        check(d = L3D_L, "L3D_L round-trip");

        -- L3D_H (undriven 1) -> weak 1 -> L3D_H
        s := to_logic3ds(L3D_H, ST_STRONG);
        check(s.strength = ST_WEAK, "L3D_H promote: strength forced to weak");
        check(s.value = 255, "L3D_H promote: value");
        d := to_logic3d(s);
        check(d = L3D_H, "L3D_H round-trip");

        -- L3D_Z -> highz undriven -> L3D_Z
        s := to_logic3ds(L3D_Z, ST_STRONG);
        check(s.strength = ST_HIGHZ, "L3D_Z promote: strength forced to highz");
        check(s.flags = FL_UNDRIVEN, "L3D_Z promote: flags");
        d := to_logic3d(s);
        check(d = L3D_Z, "L3D_Z round-trip");

        -- L3D_X at strong -> strong unknown -> L3D_X
        s := to_logic3ds(L3D_X, ST_STRONG);
        check(s.strength = ST_STRONG, "L3D_X promote: strength");
        check(s.flags = FL_UNKNOWN, "L3D_X promote: flags");
        d := to_logic3d(s);
        check(d = L3D_X, "L3D_X round-trip");

        -- L3D_W -> weak unknown -> L3D_W
        s := to_logic3ds(L3D_W, ST_STRONG);  -- str ignored, uses ST_WEAK
        check(s.strength = ST_WEAK, "L3D_W promote: strength forced to weak");
        check(s.flags = FL_UNKNOWN, "L3D_W promote: flags");
        d := to_logic3d(s);
        check(d = L3D_W, "L3D_W round-trip");

        -- L3D_U -> highz unknown -> (demotes to Z since highz)
        s := to_logic3ds(L3D_U, ST_STRONG);
        check(s.strength = ST_HIGHZ, "L3D_U promote: strength forced to highz");
        check(s.flags = FL_UNKNOWN, "L3D_U promote: flags");

        report "  Round-trip tests PASSED" severity note;

        -----------------------------------------------------------------------
        -- 2. Demote at different strength levels
        -----------------------------------------------------------------------
        report "=== Test 2: Demotion at various strengths ===" severity note;

        -- Pull-driven 1 -> L3D_1 (pull > weak)
        s := make_logic3ds(255, ST_PULL, FL_KNOWN);
        d := to_logic3d(s);
        check(d = L3D_1, "pull 1 -> L3D_1");

        -- Pull-driven 0 -> L3D_0
        s := make_logic3ds(0, ST_PULL, FL_KNOWN);
        d := to_logic3d(s);
        check(d = L3D_0, "pull 0 -> L3D_0");

        -- Weak-driven 1 -> L3D_H (weak <= ST_WEAK)
        s := make_logic3ds(255, ST_WEAK, FL_KNOWN);
        d := to_logic3d(s);
        check(d = L3D_H, "weak 1 -> L3D_H");

        -- Weak-driven 0 -> L3D_L
        s := make_logic3ds(0, ST_WEAK, FL_KNOWN);
        d := to_logic3d(s);
        check(d = L3D_L, "weak 0 -> L3D_L");

        -- Small capacitance 1 -> L3D_H (small <= weak)
        s := make_logic3ds(255, ST_SMALL, FL_KNOWN);
        d := to_logic3d(s);
        check(d = L3D_H, "small 1 -> L3D_H");

        -- Supply 0 -> L3D_0
        s := make_logic3ds(0, ST_SUPPLY, FL_KNOWN);
        d := to_logic3d(s);
        check(d = L3D_0, "supply 0 -> L3D_0");

        -- Unknown at pull strength -> L3D_X (pull > weak)
        s := make_logic3ds(0, ST_PULL, FL_UNKNOWN);
        d := to_logic3d(s);
        check(d = L3D_X, "pull unknown -> L3D_X");

        -- Unknown at weak strength -> L3D_W
        s := make_logic3ds(0, ST_WEAK, FL_UNKNOWN);
        d := to_logic3d(s);
        check(d = L3D_W, "weak unknown -> L3D_W");

        -- FL_NOPOWER -> L3D_X regardless of value/strength
        s := make_logic3ds(255, ST_SUPPLY, FL_NOPOWER);
        d := to_logic3d(s);
        check(d = L3D_X, "nopower -> L3D_X");

        report "  Demotion tests PASSED" severity note;

        -----------------------------------------------------------------------
        -- 3. Value threshold
        -----------------------------------------------------------------------
        report "=== Test 3: Value threshold ===" severity note;

        -- Threshold at 127
        s := make_logic3ds(127, ST_STRONG, FL_KNOWN);
        check(is_logic_one(s), "value 127 is logic one");
        d := to_logic3d(s);
        check(d = L3D_1, "value 127 -> L3D_1");

        s := make_logic3ds(126, ST_STRONG, FL_KNOWN);
        check(is_logic_zero(s), "value 126 is logic zero");
        d := to_logic3d(s);
        check(d = L3D_0, "value 126 -> L3D_0");

        -- Edge: value 0
        s := make_logic3ds(0, ST_STRONG, FL_KNOWN);
        check(is_logic_zero(s), "value 0 is logic zero");

        -- Edge: value 255
        s := make_logic3ds(255, ST_STRONG, FL_KNOWN);
        check(is_logic_one(s), "value 255 is logic one");

        report "  Threshold tests PASSED" severity note;

        -----------------------------------------------------------------------
        -- 4. Resolution: strong vs weak
        -----------------------------------------------------------------------
        report "=== Test 4: Resolution - strong vs weak ===" severity note;

        -- Strong 1 vs weak 0: strong wins
        r := l3ds_resolve((L3DS_1, L3DS_WEAK0));
        check(l3ds_eq(r, L3DS_1), "strong 1 vs weak 0: strong 1 wins");

        -- Weak 1 vs strong 0: strong wins
        r := l3ds_resolve((L3DS_WEAK1, L3DS_0));
        check(l3ds_eq(r, L3DS_0), "weak 1 vs strong 0: strong 0 wins");

        -- Strong 1 vs pull 0: strong wins
        r := l3ds_resolve((L3DS_1, L3DS_PULL0));
        check(l3ds_eq(r, L3DS_1), "strong 1 vs pull 0: strong 1 wins");

        -- Pull 1 vs weak 0: pull wins
        r := l3ds_resolve((L3DS_PULL1, L3DS_WEAK0));
        check(r.value = 255, "pull 1 vs weak 0: pull value");
        check(r.strength = ST_PULL, "pull 1 vs weak 0: pull strength");
        check(r.flags = FL_KNOWN, "pull 1 vs weak 0: known");

        report "  Strong vs weak tests PASSED" severity note;

        -----------------------------------------------------------------------
        -- 5. Resolution: supply dominance
        -----------------------------------------------------------------------
        report "=== Test 5: Resolution - supply dominance ===" severity note;

        -- Supply 1 vs strong 0: supply wins
        r := l3ds_resolve((L3DS_SU1, L3DS_0));
        check(r.value = 255, "supply 1 vs strong 0: supply value");
        check(r.strength = ST_SUPPLY, "supply 1 vs strong 0: supply strength");

        -- Supply 0 vs supply 0: same value, no contention
        r := l3ds_resolve((L3DS_SU0, L3DS_SU0));
        check(r.value = 0, "supply 0 vs supply 0: value 0");
        check(r.strength = ST_SUPPLY, "supply 0 vs supply 0: supply");
        check(r.flags = FL_KNOWN, "supply 0 vs supply 0: known");

        report "  Supply dominance tests PASSED" severity note;

        -----------------------------------------------------------------------
        -- 6. Resolution: equal-strength contention -> X
        -----------------------------------------------------------------------
        report "=== Test 6: Resolution - contention ===" severity note;

        -- Strong 1 vs strong 0: contention -> X at strong
        r := l3ds_resolve((L3DS_1, L3DS_0));
        check(r.flags = FL_UNKNOWN, "strong 1 vs strong 0: unknown (X)");
        check(r.strength = ST_STRONG, "strong 1 vs strong 0: strong strength");

        -- Supply 1 vs supply 0: contention -> X at supply
        r := l3ds_resolve((L3DS_SU1, L3DS_SU0));
        check(r.flags = FL_UNKNOWN, "supply 1 vs supply 0: unknown (X)");
        check(r.strength = ST_SUPPLY, "supply 1 vs supply 0: supply strength");

        -- Weak 1 vs weak 0: contention -> X at weak
        r := l3ds_resolve((L3DS_WEAK1, L3DS_WEAK0));
        check(r.flags = FL_UNKNOWN, "weak 1 vs weak 0: unknown (X)");
        check(r.strength = ST_WEAK, "weak 1 vs weak 0: weak strength");

        report "  Contention tests PASSED" severity note;

        -----------------------------------------------------------------------
        -- 7. Resolution: all highz -> Z
        -----------------------------------------------------------------------
        report "=== Test 7: Resolution - all highz ===" severity note;

        r := l3ds_resolve((L3DS_Z, L3DS_Z));
        check(l3ds_eq(r, L3DS_Z), "two Z drivers -> Z");

        -- Single Z driver
        r := l3ds_resolve((0 => L3DS_Z));
        check(l3ds_eq(r, L3DS_Z), "single Z driver -> Z");

        report "  All-highz tests PASSED" severity note;

        -----------------------------------------------------------------------
        -- 8. Resolution: same value at same strength
        -----------------------------------------------------------------------
        report "=== Test 8: Resolution - same value agreement ===" severity note;

        -- Two strong 1 drivers: agree -> strong 1
        r := l3ds_resolve((L3DS_1, L3DS_1));
        check(l3ds_eq(r, L3DS_1), "strong 1 + strong 1 -> strong 1");

        -- Three drivers: strong 1, weak 0, strong 1 -> strong 1 wins
        r := l3ds_resolve((L3DS_1, L3DS_WEAK0, L3DS_1));
        check(l3ds_eq(r, L3DS_1), "2x strong 1 + weak 0 -> strong 1");

        report "  Agreement tests PASSED" severity note;

        -----------------------------------------------------------------------
        -- 9. Resolution: X driver propagation
        -----------------------------------------------------------------------
        report "=== Test 9: Resolution - X driver ===" severity note;

        -- Strong X vs weak 1: strong X wins
        r := l3ds_resolve((L3DS_X, L3DS_WEAK1));
        check(r.flags = FL_UNKNOWN, "strong X vs weak 1: unknown");
        check(r.strength = ST_STRONG, "strong X vs weak 1: strong");

        -- Strong X vs strong 1: same strength, one is X -> X
        r := l3ds_resolve((L3DS_X, L3DS_1));
        check(r.flags = FL_UNKNOWN, "strong X vs strong 1: unknown");

        report "  X propagation tests PASSED" severity note;

        -----------------------------------------------------------------------
        -- 10. Resolution: mixed with highz (should be ignored)
        -----------------------------------------------------------------------
        report "=== Test 10: Resolution - highz ignored ===" severity note;

        -- Strong 1, Z, Z: only strong 1 active -> strong 1
        r := l3ds_resolve((L3DS_1, L3DS_Z, L3DS_Z));
        check(l3ds_eq(r, L3DS_1), "strong 1 + 2x Z -> strong 1");

        -- Weak 0, Z: only weak 0 active -> weak 0
        r := l3ds_resolve((L3DS_WEAK0, L3DS_Z));
        check(r.value = 0, "weak 0 + Z: value");
        check(r.strength = ST_WEAK, "weak 0 + Z: weak");
        check(r.flags = FL_KNOWN, "weak 0 + Z: known");

        report "  Highz-ignored tests PASSED" severity note;

        -----------------------------------------------------------------------
        -- 11. Power-missing flag propagation
        -----------------------------------------------------------------------
        report "=== Test 11: Power-missing flag ===" severity note;

        -- Supply driver with nopower flag
        r := l3ds_resolve((
            make_logic3ds(255, ST_SUPPLY, FL_NOPOWER),
            L3DS_0
        ));
        -- Supply is strongest, but has power issue -> FL_NOPOWER
        check(r.strength = ST_SUPPLY, "nopower supply: strength");
        check(r.flags = FL_NOPOWER or r.flags = FL_UNK_NOPOWER,
              "nopower supply: power flag propagated");

        report "  Power-missing tests PASSED" severity note;

        -----------------------------------------------------------------------
        -- 12. Query and builder functions
        -----------------------------------------------------------------------
        report "=== Test 12: Query and builder functions ===" severity note;

        s := l3ds_drive(true, ST_PULL);
        check(s.value = 255, "l3ds_drive true: value");
        check(s.strength = ST_PULL, "l3ds_drive true: strength");
        check(s.flags = FL_KNOWN, "l3ds_drive true: flags");
        check(is_driven(s), "l3ds_drive true: is_driven");
        check(not is_supply(s), "l3ds_drive true: not is_supply");

        s := l3ds_drive(false, ST_SUPPLY);
        check(s.value = 0, "l3ds_drive false supply: value");
        check(is_supply(s), "l3ds_drive false supply: is_supply");

        check(not is_driven(L3DS_Z), "Z is not driven");
        check(not is_driven(L3DS_X), "X is not driven (flags /= FL_KNOWN)");
        check(is_supply(L3DS_SU1), "SU1 is supply");

        -- to_std_logic sanity
        check(to_std_logic(L3DS_1) = '1', "to_std_logic strong 1");
        check(to_std_logic(L3DS_0) = '0', "to_std_logic strong 0");
        check(to_std_logic(L3DS_Z) = 'Z', "to_std_logic Z");
        check(to_std_logic(L3DS_X) = 'X', "to_std_logic X");

        report "  Query/builder tests PASSED" severity note;

        -----------------------------------------------------------------------
        -- 13. Conversion: to_std_logic for all constant forms
        -----------------------------------------------------------------------
        report "=== Test 13: to_std_logic conversions ===" severity note;

        check(to_std_logic(L3DS_WEAK0) = 'L', "to_std_logic weak 0 = L");
        check(to_std_logic(L3DS_WEAK1) = 'H', "to_std_logic weak 1 = H");
        check(to_std_logic(L3DS_PULL0) = '0', "to_std_logic pull 0 = 0");
        check(to_std_logic(L3DS_PULL1) = '1', "to_std_logic pull 1 = 1");
        check(to_std_logic(L3DS_SU0)   = '0', "to_std_logic supply 0 = 0");
        check(to_std_logic(L3DS_SU1)   = '1', "to_std_logic supply 1 = 1");

        report "  to_std_logic conversions PASSED" severity note;

        report "ALL TESTS PASSED" severity note;
        std.env.finish;
        wait;
    end process;
end architecture;
