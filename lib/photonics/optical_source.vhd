-- Optical Source (Laser / LED)
--
-- Generates a coherent optical field with specified power, wavelength,
-- and polarization. Output-only: drives 'driver with the source field.
-- The 'other input is ignored (source does not respond to back-reflections
-- in this simple model).

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.optical_field_pkg.all;

entity optical_source is
    generic (
        power_w    : real := 1.0e-3;    -- output power in watts (default 1 mW)
        wavelength : real := 1.55e-6;   -- wavelength in meters
        phase      : real := 0.0;       -- initial phase in radians
        pol_angle  : real := 0.0        -- polarization angle (0 = X-polarized)
    );
    port (
        output : inout std_logic
    );
end entity optical_source;

architecture behavioral of optical_source is
begin
    process (output'other)
        variable other_field : optical_field;
    begin
        -- Read 'other to ensure the implicit signal is created
        -- (needed for external name access by resolver)
        other_field := output'other;
        output'driver := make_optical_field(power_w, phase, pol_angle, wavelength);
    end process;
end architecture behavioral;
