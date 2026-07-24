-- Packed 3D-Logic word (l3dw): 8 wires per element, bit-planar.
--
-- One element is a 24-bit word carrying three byte-planes:
--   byte 0  value      bit i = logic value of wire i        (i = 0..7)
--   byte 1  driven     bit i = wire i is driven (strong)    (logic3d bit 1)
--   byte 2  uncertain  bit i = wire i's value is suspect    (logic3d bit 2)
-- so wire i's scalar logic3d code is  value_i | driven_i<<1 | uncertain_i<<2,
-- exactly logic3d's 3-bit encoding transposed across a group of 8 wires.
--
-- The point: a bus operation is a byte-parallel bitwise formula over the
-- planes, so 8 wires are computed per byte-op. The gate LUTs use only the
-- value and uncertain planes and always drive the output, so the per-wire
-- __l3d_*_code formulas apply UNCHANGED to whole bytes (no cross-wire carry).
-- A wide vector of these words is 3 bytes / 8 wires vs std_logic's 8 bytes.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;

package logic3dw_pkg is
  -- Full 32-bit word: byte0 value, byte1 driven, byte2 uncertain, byte3
  -- reserved. Range spans bytes 0..2 (+ low bit of byte3); nvc sizes this to a
  -- 4-byte element => clean int32 stride for the intrinsic, and it is the
  -- "32-bit word" the representation is specified as.
  type l3dw is range 0 to 2147483647;                     -- 4-byte word
  type l3dw_vector is array (natural range <>) of l3dw;

  -- group count for N wires
  function nwords(nwires : natural) return natural;

  -- pack/unpack a std_logic_vector (LSB = wire 0 of word 0)
  function to_l3dw(s : std_logic_vector) return l3dw_vector;
  function to_slv (w : l3dw_vector; nwires : natural) return std_logic_vector;

  function l3dw_and(a, b : l3dw_vector) return l3dw_vector;
  function l3dw_or (a, b : l3dw_vector) return l3dw_vector;
  function l3dw_xor(a, b : l3dw_vector) return l3dw_vector;
  function l3dw_not(a : l3dw_vector) return l3dw_vector;
end package;

package body logic3dw_pkg is
  constant DRV : integer := 255 * 256;                    -- all-driven plane

  function nwords(nwires : natural) return natural is
  begin return (nwires + 7) / 8; end function;

  -- plane extractors / builder
  function vp(w : l3dw) return unsigned is
  begin return to_unsigned(integer(w) mod 256, 8); end function;
  function up(w : l3dw) return unsigned is
  begin return to_unsigned((integer(w) / 65536) mod 256, 8); end function;
  function mk(v, u : unsigned) return l3dw is
  begin return l3dw(to_integer(v) + DRV + to_integer(u) * 65536); end function;

  function to_l3dw(s : std_logic_vector) return l3dw_vector is
    variable r : l3dw_vector(0 to nwords(s'length) - 1) := (others => 0);
    variable v, u : unsigned(7 downto 0);
    variable sn : std_logic_vector(s'length - 1 downto 0) := s;  -- 0 = LSB
    variable bit : natural;
  begin
    for g in r'range loop
      v := (others => '0'); u := (others => '0');
      for i in 0 to 7 loop
        bit := g * 8 + i;
        if bit < sn'length then
          case sn(bit) is
            when '1' | 'H'       => v(i) := '1';
            when '0' | 'L'       => v(i) := '0';
            when others          => u(i) := '1';   -- X/Z/U/W/- : uncertain
          end case;
        end if;
      end loop;
      r(g) := mk(v, u);
    end loop;
    return r;
  end function;

  function to_slv(w : l3dw_vector; nwires : natural) return std_logic_vector is
    variable r : std_logic_vector(nwires - 1 downto 0) := (others => '0');
    variable v, u : unsigned(7 downto 0);
    variable bit : natural;
  begin
    for g in 0 to w'length - 1 loop
      v := vp(w(w'low + g)); u := up(w(w'low + g));
      for i in 0 to 7 loop
        bit := g * 8 + i;
        if bit < nwires then
          if u(i) = '1' then r(bit) := 'X';
          elsif v(i) = '1' then r(bit) := '1';
          else r(bit) := '0'; end if;
        end if;
      end loop;
    end loop;
    return r;
  end function;

  function l3dw_and(a, b : l3dw_vector) return l3dw_vector is
    variable r : l3dw_vector(a'range);
    variable Va, Ua, Vb, Ub, Vc, Uc, c0 : unsigned(7 downto 0);
  begin
    for i in a'range loop
      Va := vp(a(i)); Ua := up(a(i)); Vb := vp(b(i)); Ub := up(b(i));
      Vc := Va and Vb;
      c0 := (not (Va or Ua)) or (not (Vb or Ub));   -- some operand certainly 0
      Uc := (Ua or Ub) and (not c0);
      r(i) := mk(Vc, Uc);
    end loop;
    return r;
  end function;

  function l3dw_or(a, b : l3dw_vector) return l3dw_vector is
    variable r : l3dw_vector(a'range);
    variable Va, Ua, Vb, Ub, Vc, Uc, c1 : unsigned(7 downto 0);
  begin
    for i in a'range loop
      Va := vp(a(i)); Ua := up(a(i)); Vb := vp(b(i)); Ub := up(b(i));
      Vc := Va or Vb;
      c1 := (Va and (not Ua)) or (Vb and (not Ub));  -- some operand certainly 1
      Uc := (Ua or Ub) and (not c1);
      r(i) := mk(Vc, Uc);
    end loop;
    return r;
  end function;

  function l3dw_xor(a, b : l3dw_vector) return l3dw_vector is
    variable r : l3dw_vector(a'range);
    variable Va, Ua, Vb, Ub : unsigned(7 downto 0);
  begin
    for i in a'range loop
      Va := vp(a(i)); Ua := up(a(i)); Vb := vp(b(i)); Ub := up(b(i));
      r(i) := mk(Va xor Vb, Ua or Ub);
    end loop;
    return r;
  end function;

  function l3dw_not(a : l3dw_vector) return l3dw_vector is
    variable r : l3dw_vector(a'range);
    variable Va, Ua : unsigned(7 downto 0);
  begin
    for i in a'range loop
      Va := vp(a(i)); Ua := up(a(i));
      r(i) := mk(not Va, Ua);                         -- flip value, keep uncert
    end loop;
    return r;
  end function;
end package body;
