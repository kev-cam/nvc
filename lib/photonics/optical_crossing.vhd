-- Optical Waveguide Crossing
--
-- Two waveguides cross each other. Ideally zero crosstalk,
-- but a configurable crosstalk level is supported.
--
-- Path 1: a1 <-> b1 (through)
-- Path 2: a2 <-> b2 (through)
-- Crosstalk: a1 <-> b2 and a2 <-> b1 at crosstalk_db level.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.optical_field_pkg.all;

entity optical_crossing is
    generic (
        crosstalk_db : real := -40.0    -- crosstalk level in dB (negative)
    );
    port (
        a1 : inout std_logic;
        b1 : inout std_logic;
        a2 : inout std_logic;
        b2 : inout std_logic
    );
end entity optical_crossing;

architecture behavioral of optical_crossing is
begin
    process (a1'other, a2'other, b1'other, b2'other)
        variable thru_amp : real;
        variable xtalk_amp : real;
        variable a1_in, a2_in, b1_in, b2_in : optical_field;
    begin
        -- Through path: nearly lossless
        -- Crosstalk: very small coupling
        xtalk_amp := db_to_amplitude(crosstalk_db);
        thru_amp  := sqrt(1.0 - xtalk_amp * xtalk_amp);

        a1_in := a1'other;
        a2_in := a2'other;
        b1_in := b1'other;
        b2_in := b2'other;

        -- Forward
        b1'driver := add_fields(
            scale_field(a1_in, thru_amp),
            scale_field(a2_in, xtalk_amp)
        );
        b2'driver := add_fields(
            scale_field(a2_in, thru_amp),
            scale_field(a1_in, xtalk_amp)
        );

        -- Reverse
        a1'driver := add_fields(
            scale_field(b1_in, thru_amp),
            scale_field(b2_in, xtalk_amp)
        );
        a2'driver := add_fields(
            scale_field(b2_in, thru_amp),
            scale_field(b1_in, xtalk_amp)
        );
    end process;
end architecture behavioral;
