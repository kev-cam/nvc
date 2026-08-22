-- The LOGIC3D form of rszcat.vhd: the sv2vhdl wrapper chain
-- `unsigned_to_l3d(Resize(l3d_to_unsigned(X), N))` / `to_l3d(..., N)` used as a
-- CONCATENATION ELEMENT.  vhdl2vlog lowers every link of that chain as an
-- l3dk==2 IDENTITY -- it prints its operand verbatim and DROPS the width
-- argument -- so a concat-element guard that stops at the first node sees only
-- the outermost wrapper and never looks at what is underneath.  That chain is
-- the single most common construct sv2vhdl emits (2539 `resize` and 370
-- `unsigned_to_l3d_bit` calls in VeeR's design.vhd).
--
-- TWO entities, so the good and the bad can be judged independently: a decline
-- takes out the whole module, and lumping them together would prove nothing
-- about the good one.
--
-- l3dwrap  (must TRANSLATE and INSTALL).  Every element is an identity whose
--   EMITTED width really is its VHDL width -- but establishing that requires
--   DESCENDING the chain: `unsigned_to_l3d` and `l3d_to_unsigned` return
--   unconstrained types, so at `to_l3d(unsigned_to_l3d(l3d_to_unsigned(hi)), 8)`
--   the operand's own type width is unknown and only reaching `hi` recovers the
--   8 that will really be printed.  A guard that declined on ignorance rather
--   than descending would kill this -- i.e. would kill the dominant idiom.
--
-- l3dwrapx (must DECLINE).  `to_l3d(l3d_index(sel,false) + 1, 8)` asks for 8
--   bits but emits an INTEGER expression -- `(sel + 1)` against an unsized
--   decimal, so 32 bits.  It is the LOW element of the concatenation on purpose:
--   a too-wide element at the top would merely be truncated by the assignment
--   and the result would come out right by accident, hiding the defect.  Here
--   the eight bits above it are displaced clean out of the 16-bit target.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
library sv2vhdl; use sv2vhdl.logic3d_types_pkg.all;

entity l3dwrap is
  port (clk : in  std_logic;
        din : in  logic3d_vector(15 downto 0);
        q   : out logic3d_vector(15 downto 0));
end entity;

architecture rtl of l3dwrap is
  signal r : logic3d_vector(15 downto 0) := (others => L3D_0);
begin
  process (clk) is
    variable lo, hi : logic3d_vector(7 downto 0);
  begin
    if rising_edge(clk) then
      lo := l3d_part_read(din, 0, 8);
      hi := l3d_part_read(din, 8, 8);
      -- identity chain, three wrappers deep, widths all genuinely 8
      r <= to_l3d(unsigned_to_l3d(l3d_to_unsigned(lo)), 8) & to_l3d(hi, 8);
    end if;
  end process;
  q <= r;
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
library sv2vhdl; use sv2vhdl.logic3d_types_pkg.all;

entity l3dwrapx is
  port (clk : in  std_logic;
        sel : in  logic3d_vector(1 downto 0);
        din : in  logic3d_vector(15 downto 0);
        q   : out logic3d_vector(15 downto 0));
end entity;

architecture rtl of l3dwrapx is
  signal r : logic3d_vector(15 downto 0) := (others => L3D_0);
begin
  process (clk) is
    variable lo : logic3d_vector(7 downto 0);
  begin
    if rising_edge(clk) then
      lo := l3d_part_read(din, 0, 8);
      -- LOW element asks for 8 bits and emits a 32-bit integer expression
      r <= to_l3d(lo, 8) & to_l3d(l3d_index(sel, false) + 1, 8);
    end if;
  end process;
  q <= r;
end architecture;
