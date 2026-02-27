-- test_splitter.vhd
-- Laser -> Y-splitter (70:30) -> two terminators
--
-- Expected:
--   Input: 1 mW
--   Output 1: 0.7 mW (ratio = 0.7)
--   Output 2: 0.3 mW (1 - ratio)
--
-- Assertions are in the resolver entity, which has access to
-- optical_field typed 'driver/'other signals.

-- DUT: only std_logic nets (no real signals)
library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
library photonics;

entity test_splitter is end entity;

architecture test of test_splitter is
    signal n_input : std_logic;
    signal n_out1  : std_logic;
    signal n_out2  : std_logic;
begin
    laser1: entity photonics.optical_source(behavioral)
        generic map (
            power_w    => 1.0e-3,
            wavelength => 1.55e-6
        )
        port map (output => n_input);

    split1: entity photonics.optical_splitter(behavioral)
        generic map (ratio => 0.7)
        port map (
            input   => n_input,
            output1 => n_out1,
            output2 => n_out2
        );
end architecture;

-- Hand-written resolver + test assertions
library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
library photonics;
use photonics.optical_field_pkg.all;

entity rn_test_splitter is end entity;

architecture hand_written of rn_test_splitter is
    -- Net n_input: laser1.output <-> split1.input
    alias drv_laser    is << signal .resolved_test_splitter.dut.laser1.output.driver : optical_field >>;
    alias oth_laser    is << signal .resolved_test_splitter.dut.laser1.output.other : optical_field >>;
    alias drv_split_in is << signal .resolved_test_splitter.dut.split1.input.driver : optical_field >>;
    alias oth_split_in is << signal .resolved_test_splitter.dut.split1.input.other : optical_field >>;

    -- Net n_out1: split1.output1 (leaf)
    alias drv_split_o1 is << signal .resolved_test_splitter.dut.split1.output1.driver : optical_field >>;
    alias oth_split_o1 is << signal .resolved_test_splitter.dut.split1.output1.other : optical_field >>;

    -- Net n_out2: split1.output2 (leaf)
    alias drv_split_o2 is << signal .resolved_test_splitter.dut.split1.output2.driver : optical_field >>;
    alias oth_split_o2 is << signal .resolved_test_splitter.dut.split1.output2.other : optical_field >>;

begin
    -- Net n_input: laser <-> splitter input (swap)
    p_input: process(drv_laser, drv_split_in)
    begin
        oth_laser    := drv_split_in;
        oth_split_in := drv_laser;
    end process;

    -- Leaf nets: no other drivers
    p_leaf: process
    begin
        oth_split_o1 := OPTICAL_ZERO;
        oth_split_o2 := OPTICAL_ZERO;
        wait;
    end process;

    -- Test process: check splitter output drivers
    p_check: process
        variable p_out1  : real;
        variable p_out2  : real;
        variable p_total : real;
        variable tol     : real := 1.0e-9;
    begin
        wait for 10 ns;

        p_out1  := optical_power(drv_split_o1);
        p_out2  := optical_power(drv_split_o2);
        p_total := p_out1 + p_out2;

        report "=== Y-Splitter 70:30 Test ===" severity note;
        report "Output 1 power: " & real'image(p_out1) & " W" severity note;
        report "Output 2 power: " & real'image(p_out2) & " W" severity note;
        report "Total power:    " & real'image(p_total) & " W" severity note;

        assert abs(p_out1 - 0.7e-3) < tol
            report "FAIL: Output 1 = " & real'image(p_out1) &
                   " expected 0.7e-3"
            severity error;

        assert abs(p_out2 - 0.3e-3) < tol
            report "FAIL: Output 2 = " & real'image(p_out2) &
                   " expected 0.3e-3"
            severity error;

        assert abs(p_total - 1.0e-3) < tol
            report "FAIL: Power not conserved"
            severity error;

        report "DONE" severity note;
        wait;
    end process;

end architecture;

-- Wrapper
entity resolved_test_splitter is end entity;

architecture wrap of resolved_test_splitter is
begin
    dut:      entity work.test_splitter;
    resolver: entity work.rn_test_splitter;
end architecture;
