-- Combinational leaf: s = a + b
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity leaf_add is
  port (a, b : in  std_logic_vector(7 downto 0);
        s    : out std_logic_vector(7 downto 0));
end entity;

architecture rtl of leaf_add is
begin
  s <= std_logic_vector(unsigned(a) + unsigned(b));
end architecture;
