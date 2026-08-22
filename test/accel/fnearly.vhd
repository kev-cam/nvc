-- EARLY RETURN in a design-declared VHDL function.  Same shape as fnret.vhd but
-- the saturating branch returns before the end of the body.
--
-- A Verilog function has no early exit: emit_seq lowers `return x` to
-- `<name> = x` and falls through, so the body emits as
--     if (s[8]) begin sat = 8'hff; end
--     sat = s[7:0];
-- and the SECOND assignment always wins -- saturation silently never happens,
-- while the module still passes every other translatability check and INSTALLS.
-- vhdl2vlog must therefore DECLINE this function (has_nontail_return), leaving
-- the module interpreted.  Y then matches the reference.
--
-- Sizing note: the addends are the two low bytes of a churning LCG state, so the
-- carry-out fires on roughly a quarter of the cycles -- the mistranslation is
-- not a rare corner, it moves Y on nearly every run.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity fnearly is
  port (clk, rst_l : in std_logic; y : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of fnearly is
  function sat (a, b : unsigned(7 downto 0)) return std_logic_vector is
    variable s : unsigned(8 downto 0);
    variable r : std_logic_vector(7 downto 0);
  begin
    s := resize(a, 9) + resize(b, 9);
    if s(8) = '1' then
      r := (others => '1');
      return r;                                -- EARLY return (saturate)
    end if;
    r := std_logic_vector(s(7 downto 0));
    return r;
  end function;
  signal st  : unsigned(31 downto 0) := to_unsigned(1, 32);
  signal cnt : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is
    variable m : std_logic_vector(7 downto 0);
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        st  <= to_unsigned(1, 32);
        cnt <= (others => '0');
      else
        m   := sat(st(7 downto 0), st(15 downto 8));
        cnt <= cnt + unsigned(m);
        st  <= resize(st * 1103515245 + 12345, 32);
      end if;
    end if;
  end process;
  y <= std_logic_vector(cnt);
end architecture;
