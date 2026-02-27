-- Optical Y-Splitter / Combiner
--
-- 1x2 splitter with configurable power split ratio.
-- Forward: input power splits to output1 (ratio) and output2 (1-ratio).
-- Reverse: acts as combiner (coherent superposition of outputs).
--
-- Amplitude splitting factors: sqrt(ratio) and sqrt(1-ratio).

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.optical_field_pkg.all;

entity optical_splitter is
    generic (
        ratio : real := 0.5    -- power split ratio to output1 (0.0 to 1.0)
    );
    port (
        input   : inout std_logic;
        output1 : inout std_logic;
        output2 : inout std_logic
    );
end entity optical_splitter;

architecture behavioral of optical_splitter is
begin
    process (input'other, output1'other, output2'other)
        variable f_in   : optical_field;
        variable f_out1 : optical_field;
        variable f_out2 : optical_field;
        variable amp1   : real;
        variable amp2   : real;
    begin
        amp1 := sqrt(ratio);
        amp2 := sqrt(1.0 - ratio);

        f_in   := input'other;
        f_out1 := output1'other;
        f_out2 := output2'other;

        -- Forward: split input to outputs
        output1'driver := scale_field(f_in, amp1);
        output2'driver := scale_field(f_in, amp2);

        -- Reverse: combine outputs to input (coherent sum, scaled)
        input'driver := add_fields(
            scale_field(f_out1, amp1),
            scale_field(f_out2, amp2)
        );
    end process;
end architecture behavioral;
