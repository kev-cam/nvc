-- RF Attenuator
--
-- Variable attenuator with loss specified in dB.
-- Bidirectional: same attenuation in both directions.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.rf_signal_pkg.all;

entity rf_attenuator is
    generic (
        attenuation_db : real := 3.0    -- loss in dB (positive = loss)
    );
    port (
        a : inout std_logic;
        b : inout std_logic
    );
end entity rf_attenuator;

architecture behavioral of rf_attenuator is
begin
    process (a'other, b'other)
        variable a_in, b_in : rf_signal;
        variable amp : real;
    begin
        amp := db_to_amplitude(-attenuation_db);
        a_in := a'other;
        b_in := b'other;

        b'driver := scale_rf(a_in, amp);
        a'driver := scale_rf(b_in, amp);
    end process;
end architecture behavioral;
