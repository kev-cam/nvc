-- Optical Detector (Photodetector)
--
-- Converts optical power to an electrical signal (real-valued).
-- This is a bridge component between the optical and electrical domains.
--
-- The detector reads the resolved optical field at its input and
-- outputs the photocurrent: I = responsivity * optical_power.
--
-- The optical port uses 'driver/'other; the electrical output
-- is a plain real-valued signal (no implicit signals needed).

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.optical_field_pkg.all;

entity optical_detector is
    generic (
        responsivity : real := 1.0    -- A/W (typical InGaAs ~ 0.9-1.1)
    );
    port (
        input  : inout std_logic;
        output : out   real
    );
end entity optical_detector;

architecture behavioral of optical_detector is
begin
    process (input'other)
        variable f : optical_field;
        variable p : real;
    begin
        f := input'other;

        if is_unknown(f) then
            output <= -1.0;  -- sentinel for unknown
        else
            p := optical_power(f);
            output <= responsivity * p;
        end if;

        -- Detector absorbs light: drives zero back onto the net
        input'driver := OPTICAL_ZERO;
    end process;
end architecture behavioral;
