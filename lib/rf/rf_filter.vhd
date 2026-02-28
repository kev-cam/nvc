-- RF Filter (Ideal Bandpass)
--
-- Ideal bandpass filter: passes signals within the band,
-- fully attenuates signals outside. Binary pass/reject.
-- Bidirectional.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.rf_signal_pkg.all;

entity rf_filter is
    generic (
        center_freq : real := 2.4e9;    -- center frequency in Hz
        bandwidth   : real := 20.0e6    -- 3dB bandwidth in Hz
    );
    port (
        a : inout std_logic;
        b : inout std_logic
    );
end entity rf_filter;

architecture behavioral of rf_filter is
begin
    process (a'other, b'other)
        variable a_in, b_in : rf_signal;
        variable f_low, f_high : real;
    begin
        f_low  := center_freq - bandwidth / 2.0;
        f_high := center_freq + bandwidth / 2.0;

        a_in := a'other;
        b_in := b'other;

        -- Forward: pass if within band
        if a_in.freq >= f_low and a_in.freq <= f_high then
            b'driver := a_in;
        else
            b'driver := RF_ZERO;
        end if;

        -- Reverse
        if b_in.freq >= f_low and b_in.freq <= f_high then
            a'driver := b_in;
        else
            a'driver := RF_ZERO;
        end if;
    end process;
end architecture behavioral;
