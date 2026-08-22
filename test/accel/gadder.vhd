-- Generic combinational adder: s = a + b, width W.
-- Instantiated at multiple widths to exercise generic-module elaboration
-- (each width-variant elaborates to its own T_BLOCK on the accel path).
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity gadder is
  generic (W : positive := 8);
  port (a, b : in  std_logic_vector(W-1 downto 0);
        s    : out std_logic_vector(W-1 downto 0));
end entity;

architecture rtl of gadder is
begin
  s <= std_logic_vector(unsigned(a) + unsigned(b));
end architecture;
