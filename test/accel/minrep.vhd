-- Minimal repro: ONE 96-bit signed comparison where signed vs unsigned differ.
-- a = -1 (all ones, top bit set)  ; b = +1.  Signed: a < b is TRUE.
-- Unsigned: a (=2^96-1) < b (=1) is FALSE.  So a correct signed compare -> 1,
-- an (incorrect) unsigned compare -> 0.  We drive that bit into Y.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity minrep is
  port (clk, rst_l : in std_logic; y : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of minrep is
  signal a   : signed(95 downto 0) := to_signed(-1, 96);
  signal b   : signed(95 downto 0) := to_signed(1, 96);
  signal acc : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is
    variable lt : std_logic;
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        acc <= (others => '0');
      else
        lt := '0';
        if a < b then lt := '1'; end if;   -- signed: TRUE ; unsigned: FALSE
        acc <= acc + 1 + unsigned'("0" & lt);  -- +2 if signed-correct, +1 if unsigned-buggy
      end if;
    end if;
  end process;
  y <= std_logic_vector(acc);
end architecture;
