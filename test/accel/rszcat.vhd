-- A width-wrong expression NESTED UNDER `resize` inside a CONCATENATION -- the
-- shape that ESCAPED the concat-element guard when that guard was one node deep.
--
-- mulcat.vhd puts the bare `a * b` in the concat, so the guard sees it directly.
-- Here the very same product is wrapped in `resize(..., 16)`, which vhdl2vlog
-- lowers as an l3dk==2 IDENTITY: the width argument is DROPPED and param 0 is
-- printed verbatim, exactly like a T_TYPE_CONV.  The guard originally consumed
-- the concat-element flag at emit_expr entry and only T_TYPE_CONV / T_QUALIFIED
-- / T_INERTIAL handed it back on, so everything under resize / to_l3d /
-- unsigned_to_l3d / l3d_to_unsigned / to_unsigned / to_signed / is_one was
-- invisible to it -- and that family is the DOMINANT sv2vhdl wrapper (2539
-- `resize` and 370 `unsigned_to_l3d_bit` calls in VeeR's design.vhd alone).
--
-- VHDL: `resize(a * b, 16)` is SIXTEEN bits, so w is 8 + 16 = 24.
-- Verilog: the emission is the bare `(a * b)`, self-determined max(8,8) = EIGHT
-- bits, and a concatenation does not resize its parts -- so `{st[23:16], (a*b)}`
-- is 16 bits, not 24.  The product's high byte is dropped and the slice above it
-- shifts down eight places.
--
-- Expected: DECLINE (vhdl2vlog cannot express the VHDL width here), so the
-- module stays interpreted and Y matches the reference.  Two independent guards
-- now fire on it: `resize`'s own emitted-width check (the emitted width of a
-- vlog_op call is not statically known, and unknown never equals the requested
-- 16), and the `*` guard, which now SEES the flag because the identity hands it
-- down.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity rszcat is
  port (clk, rst_l : in std_logic; y : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of rszcat is
  signal st  : unsigned(31 downto 0) := to_unsigned(1, 32);
  signal cnt : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is
    variable a, b : unsigned(7 downto 0);
    variable w    : std_logic_vector(23 downto 0);
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        st  <= to_unsigned(1, 32);
        cnt <= (others => '0');
      else
        a := st(7 downto 0);
        b := st(15 downto 8);
        -- 8-bit slice & resize(16-bit product, 16) = 24 bits in VHDL
        w   := std_logic_vector(st(23 downto 16))
               & std_logic_vector(resize(a * b, 16));
        cnt <= cnt + unsigned(w);
        st  <= resize(st * 1103515245 + 12345, 32);
      end if;
    end if;
  end process;
  y <= std_logic_vector(cnt);
end architecture;
