-- RF Channel (Free-Space Propagation)
--
-- Models free-space path loss and phase delay using Friis equation.
-- Bidirectional: same loss in both directions.
--
-- Free-space path loss (amplitude):
--   FSPL_amp = wavelength / (4 * pi * distance)
--            = c / (4 * pi * distance * freq)
--
-- Phase delay:
--   phi = 2 * pi * distance / wavelength
--       = 2 * pi * distance * freq / c

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.rf_signal_pkg.all;

entity rf_channel is
    generic (
        distance      : real := 10.0;   -- propagation distance in meters
        extra_loss_db : real := 0.0     -- additional loss (obstacles, etc.)
    );
    port (
        a : inout std_logic;
        b : inout std_logic
    );
end entity rf_channel;

architecture behavioral of rf_channel is
begin
    process (a'other, b'other)
        variable a_in, b_in : rf_signal;
        variable wavelength : real;
        variable fspl_amp   : real;
        variable extra_amp  : real;
        variable total_amp  : real;
        variable phase_delay: real;
    begin
        a_in := a'other;
        b_in := b'other;

        -- Forward: a -> b
        if a_in.freq > 0.0 then
            wavelength   := SPEED_OF_LIGHT / a_in.freq;
            fspl_amp     := wavelength / (4.0 * MATH_PI * distance);
            extra_amp    := db_to_amplitude(-extra_loss_db);
            total_amp    := fspl_amp * extra_amp;
            phase_delay  := 2.0 * MATH_PI * distance / wavelength;
            b'driver := scale_rf(shift_rf_phase(a_in, -phase_delay), total_amp);
        else
            b'driver := RF_ZERO;
        end if;

        -- Reverse: b -> a (same path loss, opposite phase)
        if b_in.freq > 0.0 then
            wavelength   := SPEED_OF_LIGHT / b_in.freq;
            fspl_amp     := wavelength / (4.0 * MATH_PI * distance);
            extra_amp    := db_to_amplitude(-extra_loss_db);
            total_amp    := fspl_amp * extra_amp;
            phase_delay  := 2.0 * MATH_PI * distance / wavelength;
            a'driver := scale_rf(shift_rf_phase(b_in, -phase_delay), total_amp);
        else
            a'driver := RF_ZERO;
        end if;
    end process;
end architecture behavioral;
