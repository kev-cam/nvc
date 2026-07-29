-- Packed 3D-Logic word (l3dw): 8 wires per element, bit-planar.
--
-- One element is a 32-bit word carrying FOUR byte-planes:
--   byte 0  value      bit i = logic value of wire i        (i = 0..7)
--   byte 1  driven     bit i = wire i is driven (strong)    (logic3d bit 1)
--   byte 2  kind hi    K1 of wire i's certainty code        (logic3d bit 2)
--   byte 3  kind lo    K0 of wire i's certainty code
--
-- CERTAINTY IS A 2-BIT ENUM, not a single "suspect" bit.  The two planes give
-- four codes per wire, ordered so that NUMERIC MAX IS SEMANTIC DOMINANCE:
--
--   K1 K0   meaning
--    0  0   certain      -- the value plane is the answer
--    0  1   W  / '-'     -- weak unknown / don't care
--    1  0   X            -- driven, contended
--    1  1   U            -- uninitialised; dominates everything (IEEE 1164)
--
-- so propagating uncertainty through a gate is a byte-parallel 2-bit MAX.
--
-- BACKWARD COMPATIBLE BY CONSTRUCTION: K1 is the byte that used to be the lone
-- `uncertain` plane, and K0 is the byte that used to be reserved.  Any word
-- written before this change has K0 = 0, so its uncertain wires read as X --
-- exactly what they meant.  L3DW_X is bit-for-bit unchanged.
--
-- 'Z' still folds onto X.  High-impedance belongs in the DRIVEN plane, not the
-- certainty enum, but `mk` currently forces every result driven (0xFF), so
-- l3dw has no per-wire undriven state to map it to.  Giving the gate formulas
-- a real driven plane is the follow-on that makes Z distinct; it is deliberately
-- not bundled here.
--
-- The point of the representation: a bus operation is a byte-parallel bitwise
-- formula over the planes, so 8 wires are computed per byte-op with no
-- cross-wire carry.  A wide vector is 4 bytes / 8 wires vs std_logic's 8 bytes.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;

package logic3dw_pkg is
  -- FULL SIGNED 32-BIT RANGE, and that is load-bearing: byte 3 spans bits
  -- 24..31, and the old `range 0 to 2147483647` stops at bit 30, so a fourth
  -- byte-plane simply could not be addressed.  nvc sizes this to a 4-byte
  -- element either way => clean int32 stride for the JIT intrinsic.
  type l3dw is range -2147483647-1 to 2147483647;         -- 4-byte word
  type l3dw_vector is array (natural range <>) of l3dw;

  -- Whole-word constants (all 8 wires the same state), for vector defaults
  -- `(others => L3DW_x)`.  Layout value | driven<<8 | K1<<16 | K0<<24.
  constant L3DW_0 : l3dw := 16#00FF00#;   -- all wires driven certain 0
  constant L3DW_1 : l3dw := 16#00FFFF#;   -- all wires driven certain 1
  constant L3DW_X : l3dw := 16#FFFF00#;   -- all wires driven, contended
  -- K0 = 0xFF sets bit 31, so these are NEGATIVE as signed integers.  The hex
  -- in each comment is the actual bit pattern; the decimal is that pattern
  -- reinterpreted as signed 32-bit, which is what the type can hold.
  constant L3DW_W : l3dw := -16776961;    -- 16#FF00FF00#: weak unknown
  constant L3DW_U : l3dw := -256;         -- 16#FFFFFF00#: uninitialised

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
  constant ALL1 : unsigned(7 downto 0) := (others => '1');

  function nwords(nwires : natural) return natural is
  begin return (nwires + 7) / 8; end function;

  -- Plane access by SLICE rather than by mod/div.  Reinterpreting the word as
  -- 32 bits is exact for negative values (which byte 3 now produces), and it
  -- takes the division off the vector hot path.
  function wb(w : l3dw) return unsigned is
  begin return unsigned(to_signed(integer(w), 32)); end function;

  function vp(w : l3dw) return unsigned is begin return wb(w)( 7 downto  0); end;
  function dp(w : l3dw) return unsigned is begin return wb(w)(15 downto  8); end;
  function k1(w : l3dw) return unsigned is begin return wb(w)(23 downto 16); end;
  function k0(w : l3dw) return unsigned is begin return wb(w)(31 downto 24); end;

  -- Build a word.  Every result is driven, as before.
  function mk(v, kh, kl : unsigned) return l3dw is
  begin return l3dw(to_integer(signed(kl & kh & ALL1 & v))); end function;

  -- Byte-parallel 2-bit MAX of two certainty codes.  Dominance is numeric
  -- because the codes are ordered certain < W < X < U, so this is an ordinary
  -- magnitude compare done bit-planar over 8 wires at once.
  procedure kmax(a1, a0, b1, b0 : in unsigned;
                 r1, r0 : out unsigned) is
    variable agt : unsigned(7 downto 0);
  begin
    agt := (a1 and not b1) or ((not (a1 xor b1)) and a0 and not b0);
    r1  := (agt and a1) or ((not agt) and b1);
    r0  := (agt and a0) or ((not agt) and b0);
  end procedure;

  function to_l3dw(s : std_logic_vector) return l3dw_vector is
    variable r : l3dw_vector(0 to nwords(s'length) - 1) := (others => 0);
    variable v, kh, kl : unsigned(7 downto 0);
    variable sn : std_logic_vector(s'length - 1 downto 0) := s;  -- 0 = LSB
    variable bit : natural;
  begin
    for g in r'range loop
      v := (others => '0'); kh := (others => '0'); kl := (others => '0');
      for i in 0 to 7 loop
        bit := g * 8 + i;
        if bit < sn'length then
          case sn(bit) is
            when '1' | 'H' => v(i)  := '1';                      -- certain 1
            when '0' | 'L' => null;                              -- certain 0
            when 'U'       => kh(i) := '1'; kl(i) := '1';        -- U
            when 'W' | '-' => kl(i) := '1';                      -- W
            when others    => kh(i) := '1';                      -- X, and Z
          end case;
        end if;
      end loop;
      r(g) := mk(v, kh, kl);
    end loop;
    return r;
  end function;

  function to_slv(w : l3dw_vector; nwires : natural) return std_logic_vector is
    variable r : std_logic_vector(nwires - 1 downto 0) := (others => '0');
    variable v, kh, kl : unsigned(7 downto 0);
    variable bit : natural;
  begin
    for g in 0 to w'length - 1 loop
      v := vp(w(w'low + g)); kh := k1(w(w'low + g)); kl := k0(w(w'low + g));
      for i in 0 to 7 loop
        bit := g * 8 + i;
        if bit < nwires then
          if    kh(i) = '1' and kl(i) = '1' then r(bit) := 'U';
          elsif kh(i) = '1'                 then r(bit) := 'X';
          elsif kl(i) = '1'                 then r(bit) := 'W';
          elsif v(i)  = '1'                 then r(bit) := '1';
          else                                   r(bit) := '0';
          end if;
        end if;
      end loop;
    end loop;
    return r;
  end function;

  function l3dw_and(a, b : l3dw_vector) return l3dw_vector is
    variable r : l3dw_vector(a'range);
    variable Va, Vb, Vc, Ua, Ub, c0, keep : unsigned(7 downto 0);
    variable Ka1, Ka0, Kb1, Kb0, Kr1, Kr0 : unsigned(7 downto 0);
  begin
    for i in a'range loop
      Va := vp(a(i)); Ka1 := k1(a(i)); Ka0 := k0(a(i));
      Vb := vp(b(i)); Kb1 := k1(b(i)); Kb0 := k0(b(i));
      Ua := Ka1 or Ka0;  Ub := Kb1 or Kb0;      -- uncertain at all
      Vc := Va and Vb;
      c0 := (not (Va or Ua)) or (not (Vb or Ub));   -- some operand certainly 0
      keep := not c0;
      kmax(Ka1, Ka0, Kb1, Kb0, Kr1, Kr0);
      r(i) := mk(Vc, Kr1 and keep, Kr0 and keep);
    end loop;
    return r;
  end function;

  function l3dw_or(a, b : l3dw_vector) return l3dw_vector is
    variable r : l3dw_vector(a'range);
    variable Va, Vb, Vc, Ua, Ub, c1, keep : unsigned(7 downto 0);
    variable Ka1, Ka0, Kb1, Kb0, Kr1, Kr0 : unsigned(7 downto 0);
  begin
    for i in a'range loop
      Va := vp(a(i)); Ka1 := k1(a(i)); Ka0 := k0(a(i));
      Vb := vp(b(i)); Kb1 := k1(b(i)); Kb0 := k0(b(i));
      Ua := Ka1 or Ka0;  Ub := Kb1 or Kb0;
      Vc := Va or Vb;
      c1 := (Va and (not Ua)) or (Vb and (not Ub));  -- some operand certainly 1
      keep := not c1;
      kmax(Ka1, Ka0, Kb1, Kb0, Kr1, Kr0);
      r(i) := mk(Vc, Kr1 and keep, Kr0 and keep);
    end loop;
    return r;
  end function;

  function l3dw_xor(a, b : l3dw_vector) return l3dw_vector is
    variable r : l3dw_vector(a'range);
    variable Ka1, Ka0, Kb1, Kb0, Kr1, Kr0 : unsigned(7 downto 0);
  begin
    for i in a'range loop
      Ka1 := k1(a(i)); Ka0 := k0(a(i));
      Kb1 := k1(b(i)); Kb0 := k0(b(i));
      -- xor cannot be forced by either operand, so uncertainty always survives
      kmax(Ka1, Ka0, Kb1, Kb0, Kr1, Kr0);
      r(i) := mk(vp(a(i)) xor vp(b(i)), Kr1, Kr0);
    end loop;
    return r;
  end function;

  function l3dw_not(a : l3dw_vector) return l3dw_vector is
    variable r : l3dw_vector(a'range);
  begin
    for i in a'range loop
      -- flip the value, carry the certainty code through unchanged
      r(i) := mk(not vp(a(i)), k1(a(i)), k0(a(i)));
    end loop;
    return r;
  end function;
end package body;
