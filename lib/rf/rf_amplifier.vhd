-- RF Amplifier (LNA / PA)
--
-- Linear amplifier with configurable gain.
-- Bidirectional: same gain in both directions.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.rf_signal_pkg.all;

entity rf_amplifier is
    generic (
        gain_db : real := 20.0    -- gain in dB
    );
    port (
        a : inout std_logic;
        b : inout std_logic
    );
end entity rf_amplifier;

architecture behavioral of rf_amplifier is
begin
    process (a'other, b'other)
        variable a_in, b_in : rf_signal;
        variable amp : real;
    begin
        amp := db_to_amplitude(gain_db);
        a_in := a'other;
        b_in := b'other;

        b'driver := scale_rf(a_in, amp);
        a'driver := scale_rf(b_in, amp);
    end process;
end architecture behavioral;
