-- Minimization: TWO chunks, but the SECOND chunk's combinational output depends
-- on its combinational input (forwarding), unlike cchain where cstage_b's output
-- is a pure register. Tests whether a chunk whose comb output depends on a
-- cross-chunk comb input is mis-evaluated. ua exports a comb signal; ub both
-- consumes it AND exports a comb signal derived from it.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;

entity c2a is
  port (clk : in std_logic;
        xin : in std_logic_vector(31 downto 0);
        aout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of c2a is
  signal ra : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then ra <= ra + unsigned(xin) + 1; end if;
  end process;
  aout <= std_logic_vector(ra xor unsigned(xin));   -- COMBINATIONAL out
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity c2b is
  port (clk : in std_logic;
        bin : in std_logic_vector(31 downto 0);
        bout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of c2b is
  signal rb : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then rb <= (rb xor unsigned(bin)) + 7; end if;
  end process;
  bout <= std_logic_vector(rb + unsigned(bin));     -- COMB out depends on comb input bin
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity c2fwd is
  port (clk : in std_logic;
        seed : in std_logic_vector(31 downto 0);
        result : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of c2fwd is
  signal a_to_b : std_logic_vector(31 downto 0);
begin
  ua : entity work.c2a port map (clk => clk, xin => seed,   aout => a_to_b);
  ub : entity work.c2b port map (clk => clk, bin => a_to_b, bout => result);
end architecture;
