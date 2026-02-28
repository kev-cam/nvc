-- RF Splitter / Power Divider (Wilkinson)
--
-- 1x2 power divider with configurable split ratio.
-- Forward: input power splits to output1 (ratio) and output2 (1-ratio).
-- Reverse: acts as combiner (coherent sum, scaled).

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.rf_signal_pkg.all;

entity rf_splitter is
    generic (
        ratio : real := 0.5    -- power split ratio to output1
    );
    port (
        input   : inout std_logic;
        output1 : inout std_logic;
        output2 : inout std_logic
    );
end entity rf_splitter;

architecture behavioral of rf_splitter is
begin
    process (input'other, output1'other, output2'other)
        variable f_in, f_out1, f_out2 : rf_signal;
        variable amp1, amp2 : real;
    begin
        amp1 := sqrt(ratio);
        amp2 := sqrt(1.0 - ratio);

        f_in   := input'other;
        f_out1 := output1'other;
        f_out2 := output2'other;

        -- Forward: split input to outputs
        output1'driver := scale_rf(f_in, amp1);
        output2'driver := scale_rf(f_in, amp2);

        -- Reverse: combine outputs to input
        input'driver := add_rf(
            scale_rf(f_out1, amp1),
            scale_rf(f_out2, amp2)
        );
    end process;
end architecture behavioral;
