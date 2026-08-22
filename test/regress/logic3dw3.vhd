-- l3dw CERTAINTY ENUM: the 2-bit kind code in byte2 (K1) / byte3 (K0).
--
-- Two jobs, and the second is the one that matters:
--
--  1. SEMANTICS. Hand-checked assertions for kind dominance (certain < W < X <
--     U), for forcing (AND with a certain 0 is certain 0 whatever the other
--     operand; OR with a certain 1 likewise), and for NOT carrying the kind
--     through unchanged.
--
--  2. EQUIVALENCE. A sweep over every (kind, kind) x (value, value) pair for
--     all four ops, reduced to one checksum. lib/sv2vhdl/logic3dw_pkg.vhd and
--     src/jit/jit-intrin.c are two INDEPENDENT implementations of this
--     semantics, and only a differential run proves they agree: run this test
--     with NVC_JIT_INTRINSICS=1 and =0 and the checksum must be identical.
--     Nothing else holds those two files together.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
library sv2vhdl; use sv2vhdl.logic3dw_pkg.all;
use std.env.stop; use std.textio.all;

entity logic3dw3 is end entity;

architecture t of logic3dw3 is
  -- kind codes, as (K1, K0)
  constant KC : integer := 0;   -- certain
  constant KW : integer := 1;   -- weak unknown
  constant KX : integer := 2;   -- contended
  constant KU : integer := 3;   -- uninitialised

  -- Build a one-word vector: every wire gets value v and kind k.
  function w(v, k : integer) return l3dw_vector is
    variable r : l3dw_vector(0 downto 0);
    variable u : unsigned(31 downto 0);
    variable vb, k1b, k0b : unsigned(7 downto 0);
  begin
    if v = 1 then vb := (others => '1'); else vb := (others => '0'); end if;
    if k / 2 = 1 then k1b := (others => '1'); else k1b := (others => '0'); end if;
    if k mod 2 = 1 then k0b := (others => '1'); else k0b := (others => '0'); end if;
    u := k0b & k1b & x"FF" & vb;
    r(0) := l3dw(to_integer(signed(u)));
    return r;
  end function;

  function wbits(x : l3dw_vector) return unsigned is
  begin return unsigned(to_signed(integer(x(x'low)), 32)); end function;

  function vpl(x : l3dw_vector) return integer is
  begin return to_integer(wbits(x)(7 downto 0)); end function;
  -- kind of wire 0
  function kpl(x : l3dw_vector) return integer is
    variable b : unsigned(31 downto 0) := wbits(x);
    variable r : integer := 0;
  begin
    if b(16) = '1' then r := r + 2; end if;   -- K1, wire 0
    if b(24) = '1' then r := r + 1; end if;   -- K0, wire 0
    return r;
  end function;
begin
  process
    variable l : line;
    variable fails : natural := 0;
    variable chk : natural := 0;

    procedure ckk(name : string; got : l3dw_vector; ek : integer) is
    begin
      if kpl(got) /= ek then
        fails := fails + 1;
        write(l, string'("FAIL ") & name & " kind=");
        write(l, kpl(got)); write(l, string'("/")); write(l, ek);
        writeline(output, l);
      end if;
    end procedure;
  begin
    ------------------------------------------------------------------ 1. kinds
    -- dominance: the stronger kind wins, and U beats everything (IEEE 1164)
    ckk("xor W,X", l3dw_xor(w(0, KW), w(0, KX)), KX);
    ckk("xor X,U", l3dw_xor(w(0, KX), w(0, KU)), KU);
    ckk("xor U,W", l3dw_xor(w(0, KU), w(0, KW)), KU);
    ckk("xor C,W", l3dw_xor(w(0, KC), w(0, KW)), KW);
    ckk("xor C,C", l3dw_xor(w(0, KC), w(0, KC)), KC);
    -- symmetric in the operands
    ckk("xor X,W", l3dw_xor(w(0, KX), w(0, KW)), KX);
    ckk("xor U,X", l3dw_xor(w(0, KU), w(0, KX)), KU);

    -- forcing: a certain 0 into AND, a certain 1 into OR, kills uncertainty
    ckk("and 0,U", l3dw_and(w(0, KC), w(1, KU)), KC);
    ckk("and 0,X", l3dw_and(w(0, KC), w(1, KX)), KC);
    ckk("or  1,U", l3dw_or (w(1, KC), w(0, KU)), KC);
    ckk("or  1,X", l3dw_or (w(1, KC), w(0, KX)), KC);
    -- ... but a certain 1 into AND does NOT
    ckk("and 1,U", l3dw_and(w(1, KC), w(1, KU)), KU);
    ckk("or  0,X", l3dw_or (w(0, KC), w(0, KX)), KX);

    -- NOT carries the kind through unchanged
    ckk("not U", l3dw_not(w(0, KU)), KU);
    ckk("not W", l3dw_not(w(1, KW)), KW);
    ckk("not C", l3dw_not(w(1, KC)), KC);

    -- LEGACY: a word with K0 = 0 and K1 set is X, exactly as before this
    -- encoding existed, so pre-existing data keeps its meaning.
    ckk("legacy X", l3dw_xor(w(0, KX), w(0, KC)), KX);

    ------------------------------------------------- 2. equivalence checksum
    -- Every (kind,kind) x (value,value) pair through all four ops. The
    -- checksum must be IDENTICAL with NVC_JIT_INTRINSICS=1 and =0.
    for ka in 0 to 3 loop
      for kb in 0 to 3 loop
        for va in 0 to 1 loop
          for vb in 0 to 1 loop
            chk := (chk * 5 + vpl(l3dw_and(w(va, ka), w(vb, kb)))) mod 1000003;
            chk := (chk * 5 + kpl(l3dw_and(w(va, ka), w(vb, kb)))) mod 1000003;
            chk := (chk * 5 + vpl(l3dw_or (w(va, ka), w(vb, kb)))) mod 1000003;
            chk := (chk * 5 + kpl(l3dw_or (w(va, ka), w(vb, kb)))) mod 1000003;
            chk := (chk * 5 + vpl(l3dw_xor(w(va, ka), w(vb, kb)))) mod 1000003;
            chk := (chk * 5 + kpl(l3dw_xor(w(va, ka), w(vb, kb)))) mod 1000003;
          end loop;
          chk := (chk * 5 + vpl(l3dw_not(w(va, ka)))) mod 1000003;
          chk := (chk * 5 + kpl(l3dw_not(w(va, ka)))) mod 1000003;
        end loop;
      end loop;
    end loop;

    write(l, string'("CHK=")); write(l, chk); writeline(output, l);
    if fails = 0 then
      write(l, string'("PASS")); writeline(output, l);
    else
      write(l, string'("FAILURES=")); write(l, fails); writeline(output, l);
    end if;
    stop;
  end process;
end architecture;
