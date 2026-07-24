-- Packed 3D-logic word (l3dw) intrinsic correctness. Constructs words with
-- scalar integer math (byte0 value, byte1 driven=0xFF, byte2 uncertain) and
-- checks and/or/xor/not against hand-computed results, covering 2-state
-- per-wire patterns and X-propagation on the uncertain plane. Scalar-only so
-- it runs clean with the JIT intrinsics enabled.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
library sv2vhdl; use sv2vhdl.logic3dw_pkg.all;
use std.env.stop; use std.textio.all;
entity logic3dw1 is end entity;
architecture t of logic3dw1 is
  constant DRV : integer := 255*256;
  -- build a one-word vector from value/uncertain bytes
  function w(v, u : integer) return l3dw_vector is
    variable r : l3dw_vector(0 downto 0);
  begin r(0) := l3dw(v + DRV + u*65536); return r; end function;
  function vpl(x : l3dw_vector) return integer is begin return integer(x(x'low)) mod 256; end;
  function upl(x : l3dw_vector) return integer is begin return (integer(x(x'low))/65536) mod 256; end;
begin
  process
    variable l : line; variable fails : natural := 0;
    procedure ck(name : string; got : l3dw_vector; ev, eu : integer) is begin
      if vpl(got) /= ev or upl(got) /= eu then
        fails := fails + 1;
        write(l, string'("FAIL ") & name); write(l, string'(" v=")); write(l, vpl(got));
        write(l, string'("/")); write(l, ev); write(l, string'(" u=")); write(l, upl(got));
        write(l, string'("/")); write(l, eu); writeline(output, l);
      end if;
    end procedure;
  begin
    -- 2-state value plane: AND/OR/XOR/NOT over all 8 wires at once
    ck("and2", l3dw_and(w(16#A5#,0), w(16#3C#,0)), 16#24#, 0);  -- A5 & 3C
    ck("or2",  l3dw_or (w(16#A5#,0), w(16#3C#,0)), 16#BD#, 0);  -- A5 | 3C
    ck("xor2", l3dw_xor(w(16#A5#,0), w(16#3C#,0)), 16#99#, 0);  -- A5 ^ 3C
    ck("not2", l3dw_not(w(16#A5#,0)),              16#5A#, 0);

    -- X propagation. wire0 uncertain in a (u=1), b certain.
    -- AND with certain 0 (b value bit0=0) -> certain 0; AND with certain 1 -> X
    ck("andX0", l3dw_and(w(0,1), w(0,0)), 0, 0);         -- X & 0 = 0
    ck("andX1", l3dw_and(w(0,1), w(1,0)), 0, 1);         -- X & 1 = X
    -- OR with certain 1 -> 1; OR with certain 0 -> X
    ck("orX1",  l3dw_or (w(0,1), w(1,0)), 1, 0);         -- X | 1 = 1
    ck("orX0",  l3dw_or (w(0,1), w(0,0)), 0, 1);         -- X | 0 = X
    -- XOR keeps uncertainty (union)
    ck("xorX",  l3dw_xor(w(0,1), w(1,0)), 1, 1);         -- X ^ 1 = X (value 1)
    -- NOT flips the whole value byte (all 8 wires 0 -> 0xFF), keeps uncertainty
    ck("notX",  l3dw_not(w(0,1)), 16#FF#, 1);

    assert fails = 0 report "logic3dw intrinsic mismatch" severity failure;
    report "logic3dw1 PASS";
    stop;
  end process;
end architecture;
