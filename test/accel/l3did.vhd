-- A ONE-PARAMETER l3dk==2 IDENTITY used as a CONCATENATION ELEMENT.
--
-- Companion to l3dcat.vhd, which covers the same hazard reached through a
-- DIFFERENT door.  l3dcat exercises l3d_bit_read, whose self-determined width
-- was fixed in f251009c0 by wrapping the read in a relational.  This file
-- exercises the identity emissions, which that fix does NOT cover.
--
-- vhdl2vlog classifies a set of sv2vhdl helpers as "l3dk==2 identities": they
-- are emitted as their FIRST ARGUMENT VERBATIM, on the reasoning that Verilog's
-- assignment context will resize the result.  Inside {} nothing resizes, so
-- emit_expr guards them with
--
--     ident_bad = (celem && nw > 0 && emitted_width(a0, 0) != nw)
--
-- where nw is read from PARAMETER 1 -- the explicit width argument.  A
-- one-parameter identity has no such argument, so nw stays -1, `nw > 0` is
-- false, and the guard CANNOT FIRE.  Five of the sixteen identities return a
-- SCALAR while taking a vector argument:
--
--     unsigned_to_l3d_bit(a : unsigned) return logic3d   -- a(a'right)
--     l3d_to_bit, to_bit, is_one, boolean_to_logic
--
-- so each occupies width(a) concat slots instead of one, exactly as
-- l3d_bit_read used to.  Here u is 4 bits and the mask has 8 elements, so the
-- intended 8'hFF mask is emitted as 32 bits whose low 8 are 8'h11 -- the same
-- comb shape, and the same class of silent wrong answer: yosys reads the
-- shredded Verilog without complaint and the chunk installs.
--
-- Deliberately tiny so it synthesises in about a second.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
library sv2vhdl; use sv2vhdl.logic3d_types_pkg.all;

entity l3did is
  port (clk : in  std_logic;
        sel : in  logic3d_vector(3 downto 0);
        d   : in  logic3d_vector(7 downto 0);
        q   : out logic3d_vector(7 downto 0));
end entity;

architecture rtl of l3did is
  signal r : logic3d_vector(7 downto 0) := (others => L3D_0);
begin
  p : process (clk) is
    variable u : unsigned(3 downto 0);
    variable b : logic3d;
    variable m : logic3d_vector(7 downto 0);
  begin
    if rising_edge(clk) then
      -- u is 4 bits wide; unsigned_to_l3d_bit returns ONE bit (u's rightmost).
      -- The CALL ITSELF must be the concatenation element: assigning it to a
      -- 1-bit variable first would make the element an ordinary T_REF, which is
      -- correctly 1 bit and does not exercise the identity emission at all.
      u := l3d_to_unsigned(sel);
      b := unsigned_to_l3d_bit(u);   -- kept so the variable is not unused
      -- eight concat elements, each of which must be exactly 1 bit wide
      m := unsigned_to_l3d_bit(u) & unsigned_to_l3d_bit(u)
         & unsigned_to_l3d_bit(u) & unsigned_to_l3d_bit(u)
         & unsigned_to_l3d_bit(u) & unsigned_to_l3d_bit(u)
         & unsigned_to_l3d_bit(u) & unsigned_to_l3d_bit(u);
      r <= l3d_and(m, d);
    end if;
  end process;

  q <= r;
end architecture;
