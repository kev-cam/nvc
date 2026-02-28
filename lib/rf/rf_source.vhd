-- RF Source (CW Signal Generator)
--
-- Generates a CW RF signal with specified power, frequency,
-- phase, and polarization.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.rf_signal_pkg.all;

entity rf_source is
    generic (
        power_w   : real := 1.0e-3;   -- output power in watts
        freq      : real := 2.4e9;    -- frequency in Hz
        phase     : real := 0.0;      -- initial phase in radians
        pol_angle : real := 0.0       -- polarization angle (0 = H)
    );
    port (
        output : inout std_logic
    );
end entity rf_source;

architecture behavioral of rf_source is
begin
    process (output'other)
        variable other_field : rf_signal;
    begin
        other_field := output'other;
        output'driver := make_rf_signal(power_w, phase, pol_angle, freq);
    end process;
end architecture behavioral;
