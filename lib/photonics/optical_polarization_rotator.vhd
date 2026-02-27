-- Optical Polarization Rotator
--
-- Rotates the polarization state by a given angle.
-- Uses Jones rotation matrix: [[cos(t), -sin(t)], [sin(t), cos(t)]]
-- Bidirectional: forward rotates by +angle, reverse by -angle.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.optical_field_pkg.all;
use work.optical_matrix_pkg.all;

entity optical_polarization_rotator is
    generic (
        angle : real := 0.0    -- rotation angle in radians
    );
    port (
        a : inout std_logic;
        b : inout std_logic
    );
end entity optical_polarization_rotator;

architecture behavioral of optical_polarization_rotator is
begin
    process (a'other, b'other)
        variable m_fwd : jones_matrix;
        variable m_rev : jones_matrix;
        variable a_in, b_in : optical_field;
    begin
        m_fwd := jones_rotation(angle);
        m_rev := jones_rotation(-angle);
        a_in := a'other;
        b_in := b'other;

        b'driver := jones_apply(m_fwd, a_in);
        a'driver := jones_apply(m_rev, b_in);
    end process;
end architecture behavioral;
