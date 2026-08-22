-- Repro (a) CLEAN: 3-chunk combinational chain A->B->C, every stage body uses
-- ONLY patterns proven to compile correctly standalone (add / xor-with-input),
-- NO xor-with-constant (which is a separate single-chunk codegen bug). Each
-- stage holds a register and exports a COMBINATIONAL signal derived from its
-- register + its (combinational) input, so a_to_b and b_to_c are combinational
-- cross-chunk boundaries that must settle native->native->native in one delta.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;

entity m3a is
  port (clk : in std_logic; xin : in std_logic_vector(31 downto 0);
        aout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of m3a is
  signal ra : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then ra <= ra + unsigned(xin) + 1; end if;
  end process;
  aout <= std_logic_vector(ra xor unsigned(xin));      -- COMB out
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity m3b is
  port (clk : in std_logic; bin : in std_logic_vector(31 downto 0);
        bout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of m3b is
  signal rb : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then rb <= (rb xor unsigned(bin)) + 7; end if;
  end process;
  bout <= std_logic_vector(rb + unsigned(bin));        -- COMB out depends on comb input
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity m3c is
  port (clk : in std_logic; cin : in std_logic_vector(31 downto 0);
        cout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of m3c is
  signal rc : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then rc <= (rc + unsigned(cin)) xor unsigned(cin); end if;  -- xor INPUT (safe)
  end process;
  cout <= std_logic_vector(rc xor unsigned(cin));      -- COMB out depends on comb input
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity m3chain is
  port (clk : in std_logic; seed : in std_logic_vector(31 downto 0);
        result : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of m3chain is
  signal a_to_b, b_to_c : std_logic_vector(31 downto 0);
begin
  ua : entity work.m3a port map (clk => clk, xin => seed,   aout => a_to_b);
  ub : entity work.m3b port map (clk => clk, bin => a_to_b, bout => b_to_c);
  uc : entity work.m3c port map (clk => clk, cin => b_to_c, cout => result);
end architecture;
