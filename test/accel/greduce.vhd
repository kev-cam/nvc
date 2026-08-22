-- Generic AND-reduction: y = a(W-1) and ... and a(0).
-- Width-SENSITIVE (unlike an adder): if a wrong-width variant of this module is
-- instantiated, the reduction runs over the wrong number of bits and silently
-- produces a wrong result. Used to prove the generic-dedup variant naming.
library ieee;
use ieee.std_logic_1164.all;

entity greduce is
  generic (W : positive := 8);
  port (a : in  std_logic_vector(W-1 downto 0);
        y : out std_logic);
end entity;

architecture rtl of greduce is
begin
  process (a) is
    variable r : std_logic;
  begin
    r := '1';
    for i in a'range loop
      r := r and a(i);
    end loop;
    y <= r;
  end process;
end architecture;
