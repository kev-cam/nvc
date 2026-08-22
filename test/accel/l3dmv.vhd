-- std_logic METAVALUES reaching emit_expr's T_REF fallthrough.
--
-- emit_expr special-cases exactly two of the nine std_logic enum literals --
-- '0' and '1' -- and falls through to `fputs(nm, f)` for the rest.  vid()
-- sanitises `'U'` to `_u_`, `'X'` to `_x_`, `'-'` to `_-_` and so on, none of
-- which is DECLARED anywhere in the emitted module.  yosys reads an undeclared
-- identifier as a fresh undriven wire, reports nothing, and the chunk installs
-- ACTIVE with a silently wrong value.
--
-- Measured in the artifact corpus before the fix (aj_mvvu_dut_subtree.v):
--
--     r <= {8{_u_}};          -- _u_ declared 0 times in the whole module
--
-- Two designs, because they must decline differently:
--
--   l3dmv_meta  drives 'U' -- no value-plane representation at all, so the
--               only correct answer is to DECLINE.  Expect DECLINED-SAFE.
--   l3dmv_weak  drives 'L' and 'H' -- weak drives that DO carry a definite
--               value on the 2-state value plane (0 and 1), so they must keep
--               working.  Expect ACCEL-MATCH; a decline here would be a
--               needless coverage loss.
library ieee; use ieee.std_logic_1164.all;

entity l3dmv_meta is
  port (clk    : in  std_logic;
        sel    : in  std_logic_vector(7 downto 0);
        result : out std_logic_vector(7 downto 0));
end entity;

architecture rtl of l3dmv_meta is
  signal r : std_logic_vector(7 downto 0) := (others => '0');
begin
  process (clk) is
  begin
    if rising_edge(clk) then
      if sel(0) = '1' then
        r <= (others => 'U');       -- no value-plane representation
      else
        r <= sel;
      end if;
    end if;
  end process;
  result <= r;
end architecture;

library ieee; use ieee.std_logic_1164.all;

entity l3dmv_weak is
  port (clk    : in  std_logic;
        sel    : in  std_logic_vector(7 downto 0);
        result : out std_logic_vector(7 downto 0));
end entity;

architecture rtl of l3dmv_weak is
  signal r : std_logic_vector(7 downto 0) := (others => '0');
begin
  process (clk) is
  begin
    if rising_edge(clk) then
      if sel(0) = '1' then
        r <= (others => 'H');       -- weak 1 -> value plane 1
      elsif sel(1) = '1' then
        r <= (others => 'L');       -- weak 0 -> value plane 0
      else
        r <= sel;
      end if;
    end if;
  end process;
  result <= r;
end architecture;
