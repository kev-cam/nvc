-- Repro (b): CYCLIC cross-coupling between two native chunks. Each stage is
-- registered. Stage A exports a COMBINATIONAL signal a2b derived from its
-- register ra + the back-signal b2a; stage B exports a COMBINATIONAL signal b2a
-- derived from its register rb + a2b. There is NO pure combinational loop:
--   a2b = f(ra, b2a-reg-derived) ; b2a = g(rb, a2b) -- but b2a depends on a2b...
-- To stay loop-free, A's comb output uses ONLY its own register (ra) and the
-- testbench seed; B's comb output uses ONLY its own register (rb) plus a2b. So
-- the cross-coupling is: A->B via a2b (comb), B->A via b2a (comb but derived
-- from B's REGISTER rb, so reading it in A is register-clean, no comb cycle).
-- Both chunks sample the OTHER's comb signal at the clock edge -> the bidi
-- native<->native bridge must present each chunk a consistent settled view.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;

entity mcyc_a is
  port (clk : in std_logic;
        xin : in std_logic_vector(31 downto 0);
        b2a : in std_logic_vector(31 downto 0);    -- back-signal from B (comb, B-reg-derived)
        a2b : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of mcyc_a is
  signal ra : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then
      ra <= ra + unsigned(xin) + unsigned(b2a);     -- samples B's back-signal at edge
    end if;
  end process;
  a2b <= std_logic_vector(ra xor unsigned(xin));    -- COMB out: own reg + seed only (no b2a)
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity mcyc_b is
  port (clk : in std_logic;
        a2b : in std_logic_vector(31 downto 0);     -- forward-signal from A (comb, A-reg-derived)
        b2a : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of mcyc_b is
  signal rb : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then
      rb <= (rb xor unsigned(a2b)) + 1;             -- samples A's forward-signal at edge
    end if;
  end process;
  b2a <= std_logic_vector(rb);                       -- COMB out: own REGISTER only (loop-free)
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity mcycle is
  port (clk : in std_logic; seed : in std_logic_vector(31 downto 0);
        result : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of mcycle is
  signal a2b, b2a : std_logic_vector(31 downto 0);
begin
  ua : entity work.mcyc_a port map (clk => clk, xin => seed, b2a => b2a, a2b => a2b);
  ub : entity work.mcyc_b port map (clk => clk, a2b => a2b, b2a => b2a);
  result <= std_logic_vector(unsigned(a2b) + unsigned(b2a));
end architecture;
