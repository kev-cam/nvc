-- Optical Phase Shifter
--
-- Applies a phase shift to both polarization components.
-- Bidirectional: same phase shift in both directions.
--
-- For dynamic (electrically controlled) phase shifting,
-- the phi generic sets the static bias and an electrical
-- control port can be added in a future architecture variant.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.optical_field_pkg.all;

entity optical_phase_shifter is
    generic (
        phi : real := 0.0    -- phase shift in radians
    );
    port (
        a : inout std_logic;
        b : inout std_logic
    );
end entity optical_phase_shifter;

architecture behavioral of optical_phase_shifter is
begin
    process (a'other, b'other)
        variable a_in, b_in : optical_field;
    begin
        a_in := a'other;
        b_in := b'other;
        -- Forward: a -> b with phase shift
        b'driver := shift_phase(a_in, phi);
        -- Reverse: b -> a with same phase shift
        a'driver := shift_phase(b_in, phi);
    end process;
end architecture behavioral;
