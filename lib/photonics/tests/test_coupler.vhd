-- test_coupler.vhd
-- Laser -> 50:50 directional coupler -> two terminators
--
-- Expected behavior:
--   1 mW laser (X-polarized) enters coupler port a1
--   Coupler a2 is unused (no light)
--   Bar output b1: 0.5 mW (sqrt(0.5) amplitude, no phase shift)
--   Cross output b2: 0.5 mW (sqrt(0.5) amplitude, 90 deg phase shift)
--
-- Assertions are in the resolver entity, which has access to
-- optical_field typed 'driver/'other signals.

-- DUT: test circuit instantiating photonic components
-- Only std_logic nets (no real signals — auto-receivers can't handle them yet)
library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
library photonics;

entity test_coupler is end entity;

architecture test of test_coupler is
    signal n_input : std_logic;
    signal n_bar   : std_logic;
    signal n_cross : std_logic;
    signal n_a2    : std_logic;
begin
    -- Laser source: 1 mW, 1550 nm, X-polarized
    laser1: entity photonics.optical_source(behavioral)
        generic map (
            power_w    => 1.0e-3,
            wavelength => 1.55e-6,
            phase      => 0.0,
            pol_angle  => 0.0
        )
        port map (output => n_input);

    -- Directional coupler: 50:50 split
    dc1: entity photonics.optical_coupler(behavioral)
        generic map (kappa => 0.5)
        port map (
            a1 => n_input,
            a2 => n_a2,
            b1 => n_bar,
            b2 => n_cross
        );
end architecture;

-- Hand-written resolver + test assertions
-- The resolver wires 'driver/'other and checks optical power.
library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
library photonics;
use photonics.optical_field_pkg.all;
use photonics.optical_matrix_pkg.all;

entity rn_test_coupler is end entity;

architecture hand_written of rn_test_coupler is
    -- Net n_input: laser1.output <-> dc1.a1
    alias drv_laser is << signal .resolved_test_coupler.dut.laser1.output.driver : optical_field >>;
    alias oth_laser is << signal .resolved_test_coupler.dut.laser1.output.other : optical_field >>;
    alias drv_a1    is << signal .resolved_test_coupler.dut.dc1.a1.driver : optical_field >>;
    alias oth_a1    is << signal .resolved_test_coupler.dut.dc1.a1.other : optical_field >>;

    -- Net n_bar: dc1.b1 (leaf — no other component on this net)
    alias drv_b1    is << signal .resolved_test_coupler.dut.dc1.b1.driver : optical_field >>;
    alias oth_b1    is << signal .resolved_test_coupler.dut.dc1.b1.other : optical_field >>;

    -- Net n_cross: dc1.b2 (leaf)
    alias drv_b2    is << signal .resolved_test_coupler.dut.dc1.b2.driver : optical_field >>;
    alias oth_b2    is << signal .resolved_test_coupler.dut.dc1.b2.other : optical_field >>;

    -- Net n_a2: dc1.a2 (leaf — unused second input)
    alias drv_a2    is << signal .resolved_test_coupler.dut.dc1.a2.driver : optical_field >>;
    alias oth_a2    is << signal .resolved_test_coupler.dut.dc1.a2.other : optical_field >>;

begin
    -- Leaf nets: no other drivers
    p_leaf: process
    begin
        oth_a2 := OPTICAL_ZERO;
        oth_b1 := OPTICAL_ZERO;
        oth_b2 := OPTICAL_ZERO;
        wait;
    end process;

    -- Net n_input: laser output <-> coupler a1 (simple swap)
    p_input: process(drv_laser, drv_a1)
    begin
        oth_laser := drv_a1;
        oth_a1    := drv_laser;
    end process;

    -- Test process: check coupler output drivers
    p_check: process
        variable p_bar   : real;
        variable p_cross : real;
        variable p_total : real;
        variable tol     : real := 1.0e-9;
    begin
        wait for 10 ns;

        -- The coupler's b1 'driver should contain the bar output field
        -- The coupler's b2 'driver should contain the cross output field
        p_bar   := optical_power(drv_b1);
        p_cross := optical_power(drv_b2);
        p_total := p_bar + p_cross;

        report "=== Photonic Coupler Test ===" severity note;
        report "Bar output power:   " & real'image(p_bar) & " W" severity note;
        report "Cross output power: " & real'image(p_cross) & " W" severity note;
        report "Total power:        " & real'image(p_total) & " W" severity note;

        assert abs(p_bar - 0.5e-3) < tol
            report "FAIL: Bar output " & real'image(p_bar) &
                   " expected 0.5e-3"
            severity error;

        assert abs(p_cross - 0.5e-3) < tol
            report "FAIL: Cross output " & real'image(p_cross) &
                   " expected 0.5e-3"
            severity error;

        assert abs(p_total - 1.0e-3) < tol
            report "FAIL: Power not conserved: " &
                   real'image(p_total) & " /= 1.0e-3"
            severity error;

        report "DONE" severity note;
        wait;
    end process;

end architecture;

-- Wrapper: DUT + resolver side by side
entity resolved_test_coupler is end entity;

architecture wrap of resolved_test_coupler is
begin
    dut:      entity work.test_coupler;
    resolver: entity work.rn_test_coupler;
end architecture;
