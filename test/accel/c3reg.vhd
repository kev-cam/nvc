-- Minimization: 3 chunks A->B->C, but each stage's output is a PURE REGISTER
-- (no combinational forwarding through middle). The only combinational
-- cross-chunk dependency is A's output (like cchain). B and C sample their
-- inputs at the edge and output their own register. Tests whether 3 chained
-- chunks per se (vs 2) trigger the bug, even without comb forwarding.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;

entity c3ra is
  port (clk : in std_logic; xin : in std_logic_vector(31 downto 0);
        aout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of c3ra is
  signal ra : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then ra <= ra + unsigned(xin) + 1; end if;
  end process;
  aout <= std_logic_vector(ra xor unsigned(xin));   -- COMBINATIONAL out (only one)
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity c3rb is
  port (clk : in std_logic; bin : in std_logic_vector(31 downto 0);
        bout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of c3rb is
  signal rb : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then rb <= (rb xor unsigned(bin)) + 7; end if;
  end process;
  bout <= std_logic_vector(rb);                     -- PURE register out
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity c3rc is
  port (clk : in std_logic; cin : in std_logic_vector(31 downto 0);
        cout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of c3rc is
  signal rc : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then rc <= (rc + unsigned(cin)) xor x"5A5A5A5A"; end if;
  end process;
  cout <= std_logic_vector(rc);                     -- PURE register out
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity c3reg is
  port (clk : in std_logic; seed : in std_logic_vector(31 downto 0);
        result : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of c3reg is
  signal a_to_b, b_to_c : std_logic_vector(31 downto 0);
begin
  ua : entity work.c3ra port map (clk => clk, xin => seed,   aout => a_to_b);
  ub : entity work.c3rb port map (clk => clk, bin => a_to_b, bout => b_to_c);
  uc : entity work.c3rc port map (clk => clk, cin => b_to_c, cout => result);
end architecture;
