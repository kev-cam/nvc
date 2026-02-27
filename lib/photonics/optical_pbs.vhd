-- Optical Polarization Beam Splitter (PBS)
--
-- Separates an input field into horizontal (X) and vertical (Y)
-- polarization components at two output ports.
--
-- Forward: input -> output_h (X polarization) + output_v (Y polarization)
-- Reverse: output_h + output_v -> input (recombine)
--
-- Uses Jones matrices: H-polarizer for output_h, V-polarizer for output_v.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.optical_field_pkg.all;
use work.optical_matrix_pkg.all;

entity optical_pbs is
    port (
        input    : inout std_logic;
        output_h : inout std_logic;
        output_v : inout std_logic
    );
end entity optical_pbs;

architecture behavioral of optical_pbs is
begin
    process (input'other, output_h'other, output_v'other)
        variable f_in   : optical_field;
        variable f_h    : optical_field;
        variable f_v    : optical_field;
    begin
        f_in := input'other;
        f_h  := output_h'other;
        f_v  := output_v'other;

        -- Forward: split by polarization
        -- output_h gets X component only
        output_h'driver := jones_apply(JONES_H_POLARIZER, f_in);
        -- output_v gets Y component only
        output_v'driver := jones_apply(JONES_V_POLARIZER, f_in);

        -- Reverse: recombine polarizations
        input'driver := add_fields(
            jones_apply(JONES_H_POLARIZER, f_h),
            jones_apply(JONES_V_POLARIZER, f_v)
        );
    end process;
end architecture behavioral;
