-- Repro (a): 3-chunk COMBINATIONAL chain A->B->C.
-- Each stage holds a register but exports a COMBINATIONAL signal derived from
-- that register + its (combinational) input. So the cross-chunk boundaries
-- a_to_b and b_to_c are both combinational and must settle native->native->native
-- within a single delta before stage C's (and the chk's) sample at the clock edge.
-- Under per-instance accel these are THREE separate native chunks chained by
-- combinational bridges. Gold (interpreted) is the bit-exact oracle.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;

entity c3stage_a is
  port (clk : in std_logic;
        xin : in std_logic_vector(31 downto 0);
        aout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of c3stage_a is
  signal ra : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then ra <= ra + unsigned(xin) + 1; end if;
  end process;
  aout <= std_logic_vector(ra xor unsigned(xin));   -- COMBINATIONAL out
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity c3stage_b is
  port (clk : in std_logic;
        bin : in std_logic_vector(31 downto 0);
        bout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of c3stage_b is
  signal rb : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then rb <= (rb xor unsigned(bin)) + 7; end if;
  end process;
  bout <= std_logic_vector(rb + unsigned(bin));     -- COMBINATIONAL out (depends on bin!)
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity c3stage_c is
  port (clk : in std_logic;
        cin : in std_logic_vector(31 downto 0);
        cout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of c3stage_c is
  signal rc : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then rc <= (rc + unsigned(cin)) xor x"5A5A5A5A"; end if;
  end process;
  cout <= std_logic_vector(rc xor unsigned(cin));   -- COMBINATIONAL out (depends on cin!)
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity c3chain is
  port (clk : in std_logic;
        seed : in std_logic_vector(31 downto 0);
        result : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of c3chain is
  signal a_to_b, b_to_c : std_logic_vector(31 downto 0);
begin
  ua : entity work.c3stage_a port map (clk => clk, xin => seed,   aout => a_to_b);
  ub : entity work.c3stage_b port map (clk => clk, bin => a_to_b, bout => b_to_c);
  uc : entity work.c3stage_c port map (clk => clk, cin => b_to_c, cout => result);
end architecture;
