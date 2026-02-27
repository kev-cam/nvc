-- test_polarization.vhd
-- Tests polarization rotator and PBS:
--   Laser (X-polarized) -> 45-deg rotator -> PBS -> two terminators
--
-- Expected: 45-degree rotation creates equal H and V components,
-- so PBS splits evenly: 0.5 mW each.
--
-- Assertions are in the resolver entity, which has access to
-- optical_field typed 'driver/'other signals.

-- DUT: only std_logic nets (no real signals)
library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
library photonics;

entity test_polarization is end entity;

architecture test of test_polarization is
    signal n_laser_rot : std_logic;
    signal n_rot_pbs   : std_logic;
    signal n_pbs_h     : std_logic;
    signal n_pbs_v     : std_logic;
begin
    -- Laser: 1 mW, X-polarized
    laser1: entity photonics.optical_source(behavioral)
        generic map (
            power_w    => 1.0e-3,
            wavelength => 1.55e-6,
            pol_angle  => 0.0
        )
        port map (output => n_laser_rot);

    -- 45-degree polarization rotation
    rot1: entity photonics.optical_polarization_rotator(behavioral)
        generic map (angle => MATH_PI / 4.0)
        port map (a => n_laser_rot, b => n_rot_pbs);

    -- Polarization beam splitter
    pbs1: entity photonics.optical_pbs(behavioral)
        port map (
            input    => n_rot_pbs,
            output_h => n_pbs_h,
            output_v => n_pbs_v
        );
end architecture;

-- Hand-written resolver + test assertions
library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
library photonics;
use photonics.optical_field_pkg.all;
use photonics.optical_matrix_pkg.all;

entity rn_test_polarization is end entity;

architecture hand_written of rn_test_polarization is
    -- Net n_laser_rot: laser1.output <-> rot1.a
    alias drv_laser is << signal .resolved_test_polarization.dut.laser1.output.driver : optical_field >>;
    alias oth_laser is << signal .resolved_test_polarization.dut.laser1.output.other : optical_field >>;
    alias drv_rot_a is << signal .resolved_test_polarization.dut.rot1.a.driver : optical_field >>;
    alias oth_rot_a is << signal .resolved_test_polarization.dut.rot1.a.other : optical_field >>;

    -- Net n_rot_pbs: rot1.b <-> pbs1.input
    alias drv_rot_b  is << signal .resolved_test_polarization.dut.rot1.b.driver : optical_field >>;
    alias oth_rot_b  is << signal .resolved_test_polarization.dut.rot1.b.other : optical_field >>;
    alias drv_pbs_in is << signal .resolved_test_polarization.dut.pbs1.input.driver : optical_field >>;
    alias oth_pbs_in is << signal .resolved_test_polarization.dut.pbs1.input.other : optical_field >>;

    -- Net n_pbs_h: pbs1.output_h (leaf)
    alias drv_pbs_h  is << signal .resolved_test_polarization.dut.pbs1.output_h.driver : optical_field >>;
    alias oth_pbs_h  is << signal .resolved_test_polarization.dut.pbs1.output_h.other : optical_field >>;

    -- Net n_pbs_v: pbs1.output_v (leaf)
    alias drv_pbs_v  is << signal .resolved_test_polarization.dut.pbs1.output_v.driver : optical_field >>;
    alias oth_pbs_v  is << signal .resolved_test_polarization.dut.pbs1.output_v.other : optical_field >>;

begin
    -- Net n_laser_rot: laser <-> rotator a (swap)
    p_laser: process(drv_laser, drv_rot_a)
    begin
        oth_laser := drv_rot_a;
        oth_rot_a := drv_laser;
    end process;

    -- Net n_rot_pbs: rotator b <-> PBS input (swap)
    p_rot_pbs: process(drv_rot_b, drv_pbs_in)
    begin
        oth_rot_b  := drv_pbs_in;
        oth_pbs_in := drv_rot_b;
    end process;

    -- Leaf nets: no other drivers
    p_leaf: process
    begin
        oth_pbs_h := OPTICAL_ZERO;
        oth_pbs_v := OPTICAL_ZERO;
        wait;
    end process;

    -- Test process: check PBS output drivers
    p_check: process
        variable p_h     : real;
        variable p_v     : real;
        variable p_total : real;
        variable expected : real := 0.5e-3;
        variable tol     : real := 1.0e-9;
    begin
        wait for 10 ns;

        p_h     := optical_power(drv_pbs_h);
        p_v     := optical_power(drv_pbs_v);
        p_total := p_h + p_v;

        report "=== Polarization Rotator + PBS Test ===" severity note;
        report "H output power: " & real'image(p_h) & " W" severity note;
        report "V output power: " & real'image(p_v) & " W" severity note;
        report "Total power:    " & real'image(p_total) & " W" severity note;

        assert abs(p_h - expected) < tol
            report "FAIL: H power " & real'image(p_h) &
                   " expected " & real'image(expected)
            severity error;

        assert abs(p_v - expected) < tol
            report "FAIL: V power " & real'image(p_v) &
                   " expected " & real'image(expected)
            severity error;

        assert abs(p_total - 1.0e-3) < tol
            report "FAIL: Power not conserved"
            severity error;

        report "DONE" severity note;
        wait;
    end process;

end architecture;

-- Wrapper
entity resolved_test_polarization is end entity;

architecture wrap of resolved_test_polarization is
begin
    dut:      entity work.test_polarization;
    resolver: entity work.rn_test_polarization;
end architecture;
