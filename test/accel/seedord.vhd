-- t=0 SEED-ORDER probe. Three MEALY stages (at reset every register is 0, so
-- each stage's output is a pure function of its input) chained a->b->c, but
-- instantiated in REVERSE (uc, ub, ua). Under per-instance accel these are three
-- native chunks; the t=0 comb-seed pass gives each chunk exactly ONE eval in
-- install order with no fixpoint iteration across the cross-chunk bridges, so a
-- downstream chunk can be seeded while its cross-chunk input is still 'U'.
-- The tb observes `result` BEFORE the first clock edge, where the interpreter
-- gives result = seed.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity so_a is port (clk:in std_logic; xin:in std_logic_vector(31 downto 0);
                     aout:out std_logic_vector(31 downto 0)); end entity;
architecture rtl of so_a is signal r:unsigned(31 downto 0):=(others=>'0'); begin
  process(clk) is begin if rising_edge(clk) then r <= r + unsigned(xin) + 1; end if; end process;
  aout <= std_logic_vector(unsigned(xin) xor r);
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity so_b is port (clk:in std_logic; bin:in std_logic_vector(31 downto 0);
                     bout:out std_logic_vector(31 downto 0)); end entity;
architecture rtl of so_b is signal r:unsigned(31 downto 0):=(others=>'0'); begin
  process(clk) is begin if rising_edge(clk) then r <= (r xor unsigned(bin)) + 7; end if; end process;
  bout <= std_logic_vector(unsigned(bin) + r);
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity so_c is port (clk:in std_logic; cin:in std_logic_vector(31 downto 0);
                     cout:out std_logic_vector(31 downto 0)); end entity;
architecture rtl of so_c is signal r:unsigned(31 downto 0):=(others=>'0'); begin
  process(clk) is begin if rising_edge(clk) then r <= (r + unsigned(cin)) xor x"5A5A5A5A"; end if; end process;
  cout <= std_logic_vector(unsigned(cin) xor r);
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity seedord is port (clk:in std_logic; seed:in std_logic_vector(31 downto 0);
                        result:out std_logic_vector(31 downto 0)); end entity;
architecture rtl of seedord is signal a2b,b2c:std_logic_vector(31 downto 0); begin
  uc : entity work.so_c port map (clk=>clk, cin=>b2c,  cout=>result);  -- LAST stage instantiated FIRST
  ub : entity work.so_b port map (clk=>clk, bin=>a2b,  bout=>b2c);
  ua : entity work.so_a port map (clk=>clk, xin=>seed, aout=>a2b);     -- FIRST stage instantiated LAST
end architecture;
