-- RF Antenna
--
-- Bidirectional antenna with gain and polarization.
-- Two ports: feed (connection to RF front-end) and air (free-space coupling).
--
-- The air port participates in arena resolution for inter-antenna coupling.
-- The resolver applies Friis path-loss and polarization rotation between
-- antenna pairs on the shared air net.
--
-- Gain is applied as amplitude scaling: sqrt(gain_linear).
-- The antenna rotates polarization by pol_angle when transmitting/receiving.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.rf_signal_pkg.all;

entity rf_antenna is
    generic (
        gain_dbi  : real := 0.0;    -- antenna gain in dBi
        pol_angle : real := 0.0     -- polarization angle (0 = H, pi/2 = V)
    );
    port (
        feed : inout std_logic;
        air  : inout std_logic
    );
end entity rf_antenna;

architecture behavioral of rf_antenna is
begin
    process (feed'other, air'other)
        variable feed_in, air_in : rf_signal;
        variable gain_amp : real;
        variable cos_pol, sin_pol : real;
        variable rotated : rf_signal;
    begin
        gain_amp := sqrt(db_to_linear(gain_dbi));
        cos_pol := cos(pol_angle);
        sin_pol := sin(pol_angle);

        feed_in := feed'other;
        air_in  := air'other;

        -- TX path: feed -> air (apply gain + polarization rotation)
        rotated := (
            eh_re => feed_in.eh_re * cos_pol - feed_in.ev_re * sin_pol,
            eh_im => feed_in.eh_im * cos_pol - feed_in.ev_im * sin_pol,
            ev_re => feed_in.eh_re * sin_pol + feed_in.ev_re * cos_pol,
            ev_im => feed_in.eh_im * sin_pol + feed_in.ev_im * cos_pol,
            freq  => feed_in.freq
        );
        air'driver := scale_rf(rotated, gain_amp);

        -- RX path: air -> feed (reverse rotation + gain)
        rotated := (
            eh_re => air_in.eh_re * cos_pol + air_in.ev_re * sin_pol,
            eh_im => air_in.eh_im * cos_pol + air_in.ev_im * sin_pol,
            ev_re => -air_in.eh_re * sin_pol + air_in.ev_re * cos_pol,
            ev_im => -air_in.eh_im * sin_pol + air_in.ev_im * cos_pol,
            freq  => air_in.freq
        );
        feed'driver := scale_rf(rotated, gain_amp);
    end process;
end architecture behavioral;
