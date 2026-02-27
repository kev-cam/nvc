-- Optical Attenuator
--
-- Variable optical attenuator with loss specified in dB.
-- Bidirectional: same attenuation in both directions.
-- Positive attenuation_db means loss (e.g. 3.0 = 3 dB loss).

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.optical_field_pkg.all;

entity optical_attenuator is
    generic (
        attenuation_db : real := 3.0    -- loss in dB (positive = loss)
    );
    port (
        a : inout std_logic;
        b : inout std_logic
    );
end entity optical_attenuator;

architecture behavioral of optical_attenuator is
begin
    process (a'other, b'other)
        variable amp_factor : real;
        variable a_in, b_in : optical_field;
    begin
        amp_factor := db_to_amplitude(-attenuation_db);
        a_in := a'other;
        b_in := b'other;

        b'driver := scale_field(a_in, amp_factor);
        a'driver := scale_field(b_in, amp_factor);
    end process;
end architecture behavioral;
