-- End-to-end proof of the l3dw emission template that tgt-vhdl should produce
-- for a purely bitwise Verilog bus module. mask.v:
--   module mask(input [15:0] a,b, output [15:0] y);
--     assign y = (a & b) | (~a & ~b);
-- translates (logic3d today) to logic3d_vector(15 downto 0) + l3d_and/or/not;
-- the packed form is l3dw_vector(1 downto 0) (16 wires / 8) + l3dw_and/or/not.
-- Same random stimulus through the l3dw mask and a std_logic reference; the
-- unpacked value planes must match bit-for-bit.
library ieee; use ieee.std_logic_1164.all;
library sv2vhdl; use sv2vhdl.logic3dw_pkg.all;
entity mask_w is
  port (a, b : in l3dw_vector(1 downto 0); y : out l3dw_vector(1 downto 0));
end entity;
architecture from_verilog of mask_w is
begin
  y <= l3dw_or(l3dw_and(a, b), l3dw_and(l3dw_not(a), l3dw_not(b)));
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
library sv2vhdl; use sv2vhdl.logic3dw_pkg.all;
use std.env.stop; use std.textio.all;
entity logic3dw2 is end entity;
architecture t of logic3dw2 is
  signal aw, bw, yw : l3dw_vector(1 downto 0);
  function pack16(v : integer) return l3dw_vector is
    variable r : l3dw_vector(1 downto 0);
  begin
    r(0) := l3dw((v mod 256) + 16#FF00#);          -- wires 0..7  + driven plane
    r(1) := l3dw(((v/256) mod 256) + 16#FF00#);    -- wires 8..15
    return r;
  end function;
  function unpack16(w : l3dw_vector) return integer is
  begin return (integer(w(0)) mod 256) + (integer(w(1)) mod 256)*256; end function;
begin
  dut: entity work.mask_w port map (a => aw, b => bw, y => yw);
  process
    variable lfsr : unsigned(31 downto 0) := x"1234abcd";
    variable av, bv, ref, got : integer; variable l : line; variable fails : natural := 0;
    procedure adv is begin
      for i in 0 to 15 loop
        lfsr := lfsr(30 downto 0) & (lfsr(31) xor lfsr(21) xor lfsr(1) xor lfsr(0));
      end loop;
    end procedure;
  begin
    for k in 1 to 2000 loop
      adv; av := to_integer(lfsr(15 downto 0));
      adv; bv := to_integer(lfsr(15 downto 0));
      aw <= pack16(av); bw <= pack16(bv);
      wait for 1 ns;
      ref := to_integer((to_unsigned(av,16) and to_unsigned(bv,16))
                     or (not to_unsigned(av,16) and not to_unsigned(bv,16)));
      got := unpack16(yw);
      if got /= ref then
        fails := fails+1;
        write(l, string'("MISMATCH a=")); write(l,av); write(l,string'(" b=")); write(l,bv);
        write(l,string'(" got=")); write(l,got); write(l,string'(" ref=")); write(l,ref);
        writeline(output,l);
      end if;
    end loop;
    assert fails=0 report "l3dw mask mismatch" severity failure;
    report "logic3dw2 PASS";
    stop;
  end process;
end architecture;
