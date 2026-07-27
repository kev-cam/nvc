-- Repro (c): native + INTERPRETED MIX. Three chained registered stages.
--   ua (mmix_a): native-installable (add/xor-input only)
--   um (mmix_m): MIDDLE stage uses REAL (floating-point) arithmetic -> accel
--                DECLINES ("not fully translatable") -> stays INTERPRETED
--   uc (mmix_c): native-installable (add/xor-input only)
-- The cross-chunk signals a_to_m (native->interpreted) and m_to_c
-- (interpreted->native) are COMBINATIONAL boundaries. This tests whether the
-- native<->interpreted handoff in BOTH directions stays correct, with two
-- native chunks separated by an interpreted island.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;

entity mmix_a is
  port (clk : in std_logic; xin : in std_logic_vector(31 downto 0);
        aout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of mmix_a is
  signal ra : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then ra <= ra + unsigned(xin) + 1; end if;
  end process;
  aout <= std_logic_vector(ra xor unsigned(xin));      -- COMB out (native)
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity mmix_m is   -- INTERPRETED island (real arithmetic -> accel declines)
  port (clk : in std_logic; min : in std_logic_vector(31 downto 0);
        mout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of mmix_m is
  signal rm : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is
    variable r : real;
  begin
    if rising_edge(clk) then
      r := real(to_integer(unsigned(min) mod 100000)) * 1.5 + 2.0;
      rm <= to_unsigned(integer(r) mod 1000000, 32);
    end if;
  end process;
  mout <= std_logic_vector(rm + unsigned(min));        -- COMB out depends on comb input
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity mmix_c is
  port (clk : in std_logic; cin : in std_logic_vector(31 downto 0);
        cout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of mmix_c is
  signal rc : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then rc <= (rc + unsigned(cin)) xor unsigned(cin); end if;  -- xor INPUT (safe)
  end process;
  cout <= std_logic_vector(rc xor unsigned(cin));      -- COMB out depends on comb input
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity mmix is
  port (clk : in std_logic; seed : in std_logic_vector(31 downto 0);
        result : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of mmix is
  signal a_to_m, m_to_c : std_logic_vector(31 downto 0);
begin
  ua : entity work.mmix_a port map (clk => clk, xin => seed,   aout => a_to_m);
  um : entity work.mmix_m port map (clk => clk, min => a_to_m, mout => m_to_c);
  uc : entity work.mmix_c port map (clk => clk, cin => m_to_c, cout => result);
end architecture;
