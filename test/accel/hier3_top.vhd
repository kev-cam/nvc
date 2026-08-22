-- 3-level hierarchy (top -> mid -> {leaf_add, leaf_dff}) + a 2nd mid instance,
-- to exercise NESTED T_BLOCK and multiple instances of the same child module.
library ieee;
use ieee.std_logic_1164.all;

entity acc_mid is
  port (clk, rst_l : in  std_logic;
        x          : in  std_logic_vector(7 downto 0);
        y          : out std_logic_vector(7 downto 0));
end entity;

architecture rtl of acc_mid is
  signal sum, reg_out : std_logic_vector(7 downto 0);
begin
  u_add : entity work.leaf_add port map (a => x, b => reg_out, s => sum);
  u_dff : entity work.leaf_dff port map (clk => clk, rst_l => rst_l, d => sum, q => reg_out);
  y <= reg_out;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity hier3_top is
  port (clk, rst_l : in  std_logic;
        x          : in  std_logic_vector(7 downto 0);
        y          : out std_logic_vector(7 downto 0));
end entity;

architecture rtl of hier3_top is
  signal mid0, mid1 : std_logic_vector(7 downto 0);
begin
  -- two accumulators in series; second accumulates the first's output
  m0 : entity work.acc_mid port map (clk => clk, rst_l => rst_l, x => x,    y => mid0);
  m1 : entity work.acc_mid port map (clk => clk, rst_l => rst_l, x => mid0, y => mid1);
  y <= mid1;
end architecture;
