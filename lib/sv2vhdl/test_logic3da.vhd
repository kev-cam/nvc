-- Testbench for logic3da_pkg
-- Exercises: constants, conversions (round-trip), Thevenin resolution,
-- ideal source handling, contention, PWL flag propagation, query functions

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
library sv2vhdl;
use sv2vhdl.logic3d_types_pkg.all;
use sv2vhdl.logic3ds_pkg.all;
use sv2vhdl.logic3da_pkg.all;

entity test_logic3da is
end entity;

architecture test of test_logic3da is

    procedure check(cond : boolean; msg : string) is
    begin
        assert cond report "FAIL: " & msg severity failure;
    end procedure;

    -- Approximate real comparison (relative tolerance)
    function approx_eq(a, b : real; tol : real := 1.0e-6) return boolean is
    begin
        if abs(a) + abs(b) < tol then
            return true;  -- both near zero
        end if;
        return abs(a - b) <= tol * (1.0 + abs(a) + abs(b));
    end function;

    -- Check logic3da equality (approximate voltage/resistance, exact flags)
    function l3da_eq(a, b : logic3da; tol : real := 1.0e-6) return boolean is
    begin
        return approx_eq(a.voltage, b.voltage, tol) and
               approx_eq(a.resistance, b.resistance, tol) and
               a.flags = b.flags;
    end function;

begin
    process
        variable a   : logic3da;
        variable r   : logic3da;
        variable d   : logic3d;
        variable ds  : logic3ds;
    begin

        -----------------------------------------------------------------------
        -- 1. Constants validation
        -----------------------------------------------------------------------
        report "=== Test 1: Constants ===" severity note;

        check(L3DA_Z.voltage = 0.0, "L3DA_Z voltage");
        check(L3DA_Z.resistance = R_HIGHZ, "L3DA_Z resistance");
        check(L3DA_Z.flags = AFL_UNDRIVEN, "L3DA_Z flags");

        check(L3DA_0.voltage = 0.0, "L3DA_0 voltage");
        check(L3DA_0.resistance = R_STRONG, "L3DA_0 resistance");
        check(L3DA_0.flags = AFL_KNOWN, "L3DA_0 flags");

        check(L3DA_1.voltage = VDD, "L3DA_1 voltage");
        check(L3DA_1.resistance = R_STRONG, "L3DA_1 resistance");
        check(L3DA_1.flags = AFL_KNOWN, "L3DA_1 flags");

        check(L3DA_X.voltage = 0.0, "L3DA_X voltage");
        check(L3DA_X.resistance = R_STRONG, "L3DA_X resistance");
        check(L3DA_X.flags = AFL_UNKNOWN, "L3DA_X flags");

        check(L3DA_SU0.resistance = R_SUPPLY, "L3DA_SU0 resistance");
        check(L3DA_SU1.voltage = VDD, "L3DA_SU1 voltage");
        check(L3DA_SU1.resistance = R_SUPPLY, "L3DA_SU1 resistance");

        report "  Constants PASSED" severity note;

        -----------------------------------------------------------------------
        -- 2. Conversion round-trip: logic3d -> logic3da -> logic3d
        -----------------------------------------------------------------------
        report "=== Test 2: logic3d round-trip ===" severity note;

        d := to_logic3d(to_logic3da(L3D_0));
        check(d = L3D_0, "L3D_0 round-trip");

        d := to_logic3d(to_logic3da(L3D_1));
        check(d = L3D_1, "L3D_1 round-trip");

        d := to_logic3d(to_logic3da(L3D_L));
        check(d = L3D_L, "L3D_L round-trip");

        d := to_logic3d(to_logic3da(L3D_H));
        check(d = L3D_H, "L3D_H round-trip");

        d := to_logic3d(to_logic3da(L3D_Z));
        check(d = L3D_Z, "L3D_Z round-trip");

        d := to_logic3d(to_logic3da(L3D_X));
        check(d = L3D_X, "L3D_X round-trip");

        d := to_logic3d(to_logic3da(L3D_W));
        check(d = L3D_W, "L3D_W round-trip");

        -- L3D_U -> unknown at high-Z -> maps back to Z (lossy)
        a := to_logic3da(L3D_U);
        check(a.flags = AFL_UNKNOWN, "L3D_U -> unknown flags");
        check(a.resistance = R_HIGHZ, "L3D_U -> high-Z resistance");

        report "  logic3d round-trip PASSED" severity note;

        -----------------------------------------------------------------------
        -- 3. Conversion from logic3ds
        -----------------------------------------------------------------------
        report "=== Test 3: logic3ds -> logic3da ===" severity note;

        -- Strong 0
        a := to_logic3da(L3DS_0);
        check(approx_eq(a.voltage, 0.0), "L3DS_0 -> voltage 0");
        check(approx_eq(a.resistance, R_STRONG), "L3DS_0 -> R_STRONG");
        check(a.flags = AFL_KNOWN, "L3DS_0 -> known");

        -- Strong 1
        a := to_logic3da(L3DS_1);
        check(approx_eq(a.voltage, VDD), "L3DS_1 -> voltage VDD");
        check(approx_eq(a.resistance, R_STRONG), "L3DS_1 -> R_STRONG");

        -- Weak 0
        a := to_logic3da(L3DS_WEAK0);
        check(approx_eq(a.voltage, 0.0), "L3DS_WEAK0 -> voltage 0");
        check(approx_eq(a.resistance, R_WEAK), "L3DS_WEAK0 -> R_WEAK");

        -- Pull 1
        a := to_logic3da(L3DS_PULL1);
        check(approx_eq(a.voltage, VDD), "L3DS_PULL1 -> voltage VDD");
        check(approx_eq(a.resistance, R_PULL), "L3DS_PULL1 -> R_PULL");

        -- Z
        a := to_logic3da(L3DS_Z);
        check(a.flags = AFL_UNDRIVEN, "L3DS_Z -> undriven");

        -- X
        a := to_logic3da(L3DS_X);
        check(a.flags = AFL_UNKNOWN, "L3DS_X -> unknown");

        report "  logic3ds conversion PASSED" severity note;

        -----------------------------------------------------------------------
        -- 4. Resolution: single driver identity
        -----------------------------------------------------------------------
        report "=== Test 4: Single driver ===" severity note;

        r := l3da_resolve((0 => L3DA_1));
        check(l3da_eq(r, L3DA_1), "single L3DA_1 -> identity");

        r := l3da_resolve((0 => L3DA_Z));
        check(l3da_eq(r, L3DA_Z), "single L3DA_Z -> identity");

        report "  Single driver PASSED" severity note;

        -----------------------------------------------------------------------
        -- 5. Resolution: strong vs weak (lower R dominates)
        -----------------------------------------------------------------------
        report "=== Test 5: Strong vs weak ===" severity note;

        -- Strong 1V vs weak 0V: Thevenin combination
        -- V = (1.0/100 + 0.0/10000) / (1/100 + 1/10000) = 0.01 / 0.0101 ≈ 0.9901
        -- R = 1 / 0.0101 ≈ 99.01
        r := l3da_resolve((L3DA_1, L3DA_WEAK0));
        check(r.voltage > 0.98, "strong 1 vs weak 0: voltage > 0.98");
        check(r.resistance < 100.0, "strong 1 vs weak 0: R < 100");
        check(r.flags = AFL_KNOWN, "strong 1 vs weak 0: known");

        -- Weak 1V vs strong 0V
        r := l3da_resolve((L3DA_WEAK1, L3DA_0));
        check(r.voltage < 0.02, "weak 1 vs strong 0: voltage < 0.02");

        report "  Strong vs weak PASSED" severity note;

        -----------------------------------------------------------------------
        -- 6. Resolution: equal-R opposing voltages (midpoint)
        -----------------------------------------------------------------------
        report "=== Test 6: Equal resistance midpoint ===" severity note;

        -- Two 1k drivers: 1V and 0V -> V = 0.5V, R = 500
        r := l3da_resolve((L3DA_PULL1, L3DA_PULL0));
        check(approx_eq(r.voltage, 0.5), "1k+1k midpoint: V = 0.5");
        check(approx_eq(r.resistance, 500.0), "1k+1k midpoint: R = 500");
        check(r.flags = AFL_KNOWN, "1k+1k midpoint: known");

        -- Two equal strong drivers: 1V and 0V -> V = 0.5V, R = 50
        r := l3da_resolve((L3DA_1, L3DA_0));
        check(approx_eq(r.voltage, 0.5), "100+100 midpoint: V = 0.5");
        check(approx_eq(r.resistance, 50.0), "100+100 midpoint: R = 50");

        report "  Equal resistance midpoint PASSED" severity note;

        -----------------------------------------------------------------------
        -- 7. Resolution: all undriven -> Z
        -----------------------------------------------------------------------
        report "=== Test 7: All undriven ===" severity note;

        r := l3da_resolve((L3DA_Z, L3DA_Z));
        check(r.flags = AFL_UNDRIVEN, "two Z -> undriven");

        r := l3da_resolve((L3DA_Z, L3DA_Z, L3DA_Z));
        check(r.flags = AFL_UNDRIVEN, "three Z -> undriven");

        report "  All undriven PASSED" severity note;

        -----------------------------------------------------------------------
        -- 8. Resolution: ideal source dominance
        -----------------------------------------------------------------------
        report "=== Test 8: Ideal source ===" severity note;

        -- Supply 1V vs strong 0V: ideal source wins
        r := l3da_resolve((L3DA_SU1, L3DA_0));
        check(approx_eq(r.voltage, VDD), "supply 1 vs strong 0: V = VDD");
        check(r.resistance <= R_SHORT, "supply 1 vs strong 0: R = 0");
        check(r.flags = AFL_KNOWN, "supply 1 vs strong 0: known");

        -- Supply 0V vs pull 1V: ideal source wins
        r := l3da_resolve((L3DA_SU0, L3DA_PULL1));
        check(approx_eq(r.voltage, 0.0), "supply 0 vs pull 1: V = 0");

        -- Two matching supply sources
        r := l3da_resolve((L3DA_SU1, L3DA_SU1));
        check(approx_eq(r.voltage, VDD), "two supply 1: V = VDD");
        check(r.flags = AFL_KNOWN, "two supply 1: known");

        report "  Ideal source PASSED" severity note;

        -----------------------------------------------------------------------
        -- 9. Resolution: ideal source contention
        -----------------------------------------------------------------------
        report "=== Test 9: Ideal source contention ===" severity note;

        -- Supply 1V vs supply 0V: contention -> unknown
        r := l3da_resolve((L3DA_SU1, L3DA_SU0));
        check(r.flags = AFL_UNKNOWN, "supply 1 vs supply 0: unknown");

        report "  Ideal source contention PASSED" severity note;

        -----------------------------------------------------------------------
        -- 10. Resolution: PWL flag propagation
        -----------------------------------------------------------------------
        report "=== Test 10: PWL flag ===" severity note;

        -- PWL driver + normal driver: result has PWL flag
        a := l3da_pwl(0.5, R_STRONG);
        check(is_pwl(a), "l3da_pwl creates PWL");

        r := l3da_resolve((a, L3DA_0));
        check(r.flags = AFL_PWL, "PWL + normal: PWL flag propagated");

        -- PWL + PWL
        r := l3da_resolve((a, l3da_pwl(0.3, R_PULL)));
        check(r.flags = AFL_PWL, "PWL + PWL: PWL flag");

        report "  PWL flag PASSED" severity note;

        -----------------------------------------------------------------------
        -- 11. Thevenin arithmetic verification
        -----------------------------------------------------------------------
        report "=== Test 11: Thevenin math ===" severity note;

        -- Three resistive drivers: 1V/1k, 0V/1k, 0.5V/2k
        -- sum_g = 1/1000 + 1/1000 + 1/2000 = 0.0025
        -- sum_vg = 1.0/1000 + 0.0/1000 + 0.5/2000 = 0.00125
        -- V = 0.00125 / 0.0025 = 0.5V
        -- R = 1/0.0025 = 400 ohm
        r := l3da_resolve((
            l3da_drive(1.0, 1000.0),
            l3da_drive(0.0, 1000.0),
            l3da_drive(0.5, 2000.0)
        ));
        check(approx_eq(r.voltage, 0.5), "3-driver Thevenin: V = 0.5");
        check(approx_eq(r.resistance, 400.0), "3-driver Thevenin: R = 400");

        -- Undriven driver should be ignored in the sum
        r := l3da_resolve((
            l3da_drive(1.0, 1000.0),
            L3DA_Z,
            l3da_drive(0.0, 1000.0)
        ));
        check(approx_eq(r.voltage, 0.5), "with Z: V = 0.5");
        check(approx_eq(r.resistance, 500.0), "with Z: R = 500");

        report "  Thevenin math PASSED" severity note;

        -----------------------------------------------------------------------
        -- 12. Unknown propagation
        -----------------------------------------------------------------------
        report "=== Test 12: Unknown propagation ===" severity note;

        -- Unknown + known -> unknown
        r := l3da_resolve((L3DA_X, L3DA_1));
        check(r.flags = AFL_UNKNOWN, "X + known: unknown");

        -- Unknown + Z (Z ignored, only X active)
        r := l3da_resolve((L3DA_X, L3DA_Z));
        check(r.flags = AFL_UNKNOWN, "X + Z: unknown");

        report "  Unknown propagation PASSED" severity note;

        -----------------------------------------------------------------------
        -- 13. Query functions
        -----------------------------------------------------------------------
        report "=== Test 13: Query functions ===" severity note;

        check(is_driven(L3DA_1), "L3DA_1 is driven");
        check(not is_driven(L3DA_Z), "L3DA_Z not driven");
        check(not is_driven(L3DA_X), "L3DA_X not driven (unknown)");

        check(is_unknown(L3DA_X), "L3DA_X is unknown");
        check(not is_unknown(L3DA_1), "L3DA_1 not unknown");

        check(is_ideal_source(L3DA_SU0), "SU0 is ideal source");
        check(is_ideal_source(L3DA_SU1), "SU1 is ideal source");
        check(not is_ideal_source(L3DA_1), "L3DA_1 not ideal source");

        check(approx_eq(get_voltage(L3DA_1), VDD), "get_voltage L3DA_1");
        check(approx_eq(get_resistance(L3DA_1), R_STRONG), "get_resistance L3DA_1");

        report "  Query functions PASSED" severity note;

        -----------------------------------------------------------------------
        -- 14. Builder functions
        -----------------------------------------------------------------------
        report "=== Test 14: Builder functions ===" severity note;

        a := make_logic3da(3.3, 50.0, AFL_KNOWN);
        check(approx_eq(a.voltage, 3.3), "make_logic3da voltage");
        check(approx_eq(a.resistance, 50.0), "make_logic3da resistance");
        check(a.flags = AFL_KNOWN, "make_logic3da flags");

        a := l3da_drive(2.5, 200.0);
        check(a.flags = AFL_KNOWN, "l3da_drive flags");
        check(approx_eq(a.voltage, 2.5), "l3da_drive voltage");

        a := l3da_pwl(1.8, 75.0);
        check(a.flags = AFL_PWL, "l3da_pwl flags");
        check(approx_eq(a.voltage, 1.8), "l3da_pwl voltage");

        report "  Builder functions PASSED" severity note;

        -----------------------------------------------------------------------
        -- 15. Strength-to-resistance mapping
        -----------------------------------------------------------------------
        report "=== Test 15: Strength-resistance bridge ===" severity note;

        check(approx_eq(strength_to_resistance(ST_SUPPLY), R_SUPPLY),
              "ST_SUPPLY -> R_SUPPLY");
        check(approx_eq(strength_to_resistance(ST_STRONG), R_STRONG),
              "ST_STRONG -> R_STRONG");
        check(approx_eq(strength_to_resistance(ST_PULL), R_PULL),
              "ST_PULL -> R_PULL");
        check(approx_eq(strength_to_resistance(ST_WEAK), R_WEAK),
              "ST_WEAK -> R_WEAK");
        check(approx_eq(strength_to_resistance(ST_HIGHZ), R_HIGHZ),
              "ST_HIGHZ -> R_HIGHZ");

        check(resistance_to_strength(R_SUPPLY) = ST_SUPPLY,
              "R_SUPPLY -> ST_SUPPLY");
        check(resistance_to_strength(R_STRONG) = ST_STRONG,
              "R_STRONG -> ST_STRONG");
        check(resistance_to_strength(R_PULL) = ST_PULL,
              "R_PULL -> ST_PULL");
        check(resistance_to_strength(R_WEAK) = ST_WEAK,
              "R_WEAK -> ST_WEAK");
        check(resistance_to_strength(R_HIGHZ) = ST_HIGHZ,
              "R_HIGHZ -> ST_HIGHZ");

        report "  Strength-resistance bridge PASSED" severity note;

        -----------------------------------------------------------------------
        -- 16. to_std_logic via logic3d
        -----------------------------------------------------------------------
        report "=== Test 16: to_std_logic ===" severity note;

        check(to_std_logic(L3DA_0) = '0', "to_std_logic L3DA_0 = '0'");
        check(to_std_logic(L3DA_1) = '1', "to_std_logic L3DA_1 = '1'");
        check(to_std_logic(L3DA_Z) = 'Z', "to_std_logic L3DA_Z = 'Z'");
        check(to_std_logic(L3DA_X) = 'X', "to_std_logic L3DA_X = 'X'");
        check(to_std_logic(L3DA_WEAK0) = 'L', "to_std_logic WEAK0 = 'L'");
        check(to_std_logic(L3DA_WEAK1) = 'H', "to_std_logic WEAK1 = 'H'");
        check(to_std_logic(L3DA_PULL0) = '0', "to_std_logic PULL0 = '0'");
        check(to_std_logic(L3DA_PULL1) = '1', "to_std_logic PULL1 = '1'");
        check(to_std_logic(L3DA_SU0) = '0', "to_std_logic SU0 = '0'");
        check(to_std_logic(L3DA_SU1) = '1', "to_std_logic SU1 = '1'");

        report "  to_std_logic PASSED" severity note;

        report "ALL TESTS PASSED" severity note;
        std.env.finish;
        wait;
    end process;
end architecture;
