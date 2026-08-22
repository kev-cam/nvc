-- Generic module instantiated at multiple widths.
-- gadder is elaborated TWICE -- once at W=8 and once at W=4 -- and both
-- variants feed a registered accumulator, so the design is sequential and
-- both width-instances are exercised every clock. This is the case the
-- whole-subtree accel path must handle: one generic entity, two distinct
-- width-specialised T_BLOCKs.
--   acc_next = (acc + a8) + zero_extend(acc(3..0) + a4)  ; y = acc
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity gen_top is
  port (clk, rst_l : in  std_logic;
        a8         : in  std_logic_vector(7 downto 0);
        a4         : in  std_logic_vector(3 downto 0);
        y          : out std_logic_vector(7 downto 0));
end entity;

architecture rtl of gen_top is
  signal acc       : std_logic_vector(7 downto 0);
  signal sum8      : std_logic_vector(7 downto 0);
  signal sum4      : std_logic_vector(3 downto 0);
  signal acc_next  : std_logic_vector(7 downto 0);
begin
  -- wide variant: 8-bit add of input a8 with the full accumulator
  u_w8 : entity work.gadder
    generic map (W => 8)
    port map (a => a8, b => acc, s => sum8);

  -- narrow variant: 4-bit add of input a4 with the low nibble of the accumulator
  u_w4 : entity work.gadder
    generic map (W => 4)
    port map (a => a4, b => acc(3 downto 0), s => sum4);

  -- both width-variants contribute to the next accumulator value
  acc_next <= std_logic_vector(unsigned(sum8) + resize(unsigned(sum4), 8));

  reg : process (clk) is
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        acc <= (others => '0');
      else
        acc <= acc_next;
      end if;
    end if;
  end process;

  y <= acc;
end architecture;
