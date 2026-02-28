-- RF Mixer
--
-- Frequency converter: shifts input frequency by LO frequency.
-- Downconvert: f_out = |f_rf - f_lo|
-- Conversion loss applied as amplitude scaling.
--
-- The LO port reads its 'other to get the LO frequency.
-- The mixer multiplies the RF signal by the LO signal (complex multiply),
-- which shifts the frequency.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.rf_signal_pkg.all;

entity rf_mixer is
    generic (
        conversion_loss_db : real := 6.0   -- conversion loss in dB
    );
    port (
        rf_in  : inout std_logic;
        rf_out : inout std_logic;
        lo     : inout std_logic
    );
end entity rf_mixer;

architecture behavioral of rf_mixer is
begin
    process (rf_in'other, rf_out'other, lo'other)
        variable sig_in, sig_out, lo_sig : rf_signal;
        variable result : rf_signal;
        variable loss_amp : real;
    begin
        loss_amp := db_to_amplitude(-conversion_loss_db);

        sig_in  := rf_in'other;
        sig_out := rf_out'other;
        lo_sig  := lo'other;

        -- Forward: downconvert rf_in by lo -> rf_out
        result := scale_rf(sig_in, loss_amp);
        result.freq := abs(sig_in.freq - lo_sig.freq);
        rf_out'driver := result;

        -- Reverse: upconvert rf_out by lo -> rf_in
        result := scale_rf(sig_out, loss_amp);
        result.freq := sig_out.freq + lo_sig.freq;
        rf_in'driver := result;

        -- LO port: absorb (no signal driven back)
        lo'driver := RF_ZERO;
    end process;
end architecture behavioral;
