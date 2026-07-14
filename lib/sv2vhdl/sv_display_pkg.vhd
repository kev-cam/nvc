-- sv_display_pkg.vhd -- Verilog $display format helper functions
--
-- Provides lowercase hex/octal/binary/decimal string conversion
-- matching Verilog $display conventions (lowercase hex digits).
--
-- Usage:
--   library sv2vhdl;
--   use sv2vhdl.sv_display_pkg.all;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package sv_display_pkg is
    -- Lowercase hex string (Verilog %h/%x): "3ff" not "3FF"
    function sv_hstr(v : std_logic_vector) return string;
    -- Octal string (Verilog %o)
    function sv_ostr(v : std_logic_vector) return string;
    -- Binary string (Verilog %b)
    function sv_bstr(v : std_logic_vector) return string;
    -- Unsigned decimal string (Verilog %d)
    function sv_dstr(v : std_logic_vector) return string;
    -- Verilog %d with a field width: right-justify in `width` blanks.
    -- width <= string length => no padding (also covers %0d when width=0).
    function sv_dstr(v : std_logic_vector; width : integer) return string;
    -- Signed decimal (Verilog %d of a signed operand): two's-complement value.
    function sv_dstr_signed(v : std_logic_vector) return string;
    function sv_dstr_signed(v : std_logic_vector; width : integer) return string;
    -- Verilog %s: the vector as packed 8-bit ASCII (MSB byte first, leading
    -- null bytes suppressed).
    function sv_sstr(v : std_logic_vector) return string;
    -- Strip leading '0' characters (Verilog %0b/%0h/%0o minimum-width), keeping
    -- at least one character.
    function sv_strip0(s : string) return string;
end package;

package body sv_display_pkg is

    constant HEXCHARS : string(1 to 16) := "0123456789abcdef";

    -- Map a single std_logic to its Verilog 4-state bit character.
    --   '0'/'L' -> '0'   '1'/'H' -> '1'   'Z' -> 'z'
    --   'X'/'U'/'W'/'-'  -> 'x'
    function sv_bit(b : std_logic) return character is
    begin
        case b is
            when '0' | 'L' => return '0';
            when '1' | 'H' => return '1';
            when 'Z'       => return 'z';
            when others    => return 'x';  -- X, U, W, -
        end case;
    end function;

    -- Classify a single std_logic as one of value(0/1), z, or x.
    --   returns 0 -> known, contributes value; 1 -> z; 2 -> x
    -- val_bit is the 0/1 value for known bits (else 0).
    procedure classify(b : std_logic; kind : out natural; val_bit : out natural) is
    begin
        val_bit := 0;
        case b is
            when '0' | 'L' => kind := 0;
            when '1' | 'H' => kind := 0; val_bit := 1;
            when 'Z'       => kind := 1;
            when others    => kind := 2;  -- X, U, W, -
        end case;
    end procedure;

    -- Render one grouped digit (hex: gwidth=4, oct: gwidth=3) per Verilog:
    --   all-known    -> radix digit (lowercase)
    --   all-x        -> 'x'      all-z -> 'z'
    --   any x (mixed)-> 'X'      else any z (mixed) -> 'Z'
    -- grp holds the group's bits with grp(0) = LSB of the group; gs = # bits.
    function sv_digit(grp : std_logic_vector; gs : natural) return character is
        variable nx, nz, val, k, v : natural := 0;
    begin
        nx := 0; nz := 0; val := 0;
        for i in 0 to gs - 1 loop
            classify(grp(i), k, v);
            case k is
                when 0      => val := val + v * (2 ** i);
                when 1      => nz := nz + 1;
                when others => nx := nx + 1;
            end case;
        end loop;
        if nx = 0 and nz = 0 then
            return HEXCHARS(val + 1);
        elsif nx = gs then
            return 'x';
        elsif nz = gs then
            return 'z';
        elsif nx > 0 then
            return 'X';
        else
            return 'Z';
        end if;
    end function;

    -- Common radix-grouped renderer (gwidth = 4 for hex, 3 for octal).
    function sv_radix(v : std_logic_vector; gwidth : natural) return string is
        constant n     : natural := v'length;
        variable vv    : std_logic_vector(n - 1 downto 0) := v;
        constant ndig  : natural := (n + gwidth - 1) / gwidth;
        variable result: string(1 to ndig);
        variable gs    : natural;
        variable grp   : std_logic_vector(gwidth - 1 downto 0);
    begin
        for g in 0 to ndig - 1 loop
            -- bits [g*gwidth .. min(g*gwidth+gwidth-1, n-1)]
            if (g + 1) * gwidth <= n then
                gs := gwidth;
            else
                gs := n - g * gwidth;
            end if;
            grp := (others => '0');
            for i in 0 to gs - 1 loop
                grp(i) := vv(g * gwidth + i);
            end loop;
            -- result(1) is the most-significant digit (group ndig-1)
            result(ndig - g) := sv_digit(grp, gs);
        end loop;
        return result;
    end function;

    function sv_hstr(v : std_logic_vector) return string is
    begin
        return sv_radix(v, 4);
    end function;

    function sv_ostr(v : std_logic_vector) return string is
    begin
        return sv_radix(v, 3);
    end function;

    function sv_bstr(v : std_logic_vector) return string is
        constant n     : natural := v'length;
        variable vv    : std_logic_vector(n - 1 downto 0) := v;
        variable result: string(1 to n);
    begin
        -- MSB (vv(n-1)) leftmost. Each bit -> Verilog 0/1/x/z.
        for i in 0 to n - 1 loop
            result(n - i) := sv_bit(vv(i));
        end loop;
        return result;
    end function;

    -- Verilog %d unknown-bit convention: all-x -> "x", all-z -> "z", any x
    -- (mixed) -> "X", any z but no x -> "Z", else the decimal value.
    function sv_unknown_char(v : std_logic_vector) return string is
        variable any_x, any_z, all_x, all_z : boolean;
    begin
        any_x := false; any_z := false; all_x := true; all_z := true;
        for i in v'range loop
            case v(i) is
                when 'Z'                   => any_z := true; all_x := false;
                when '0' | '1' | 'L' | 'H' => all_x := false; all_z := false;
                when others                => any_x := true; all_z := false;
            end case;
        end loop;
        if all_x then return "x"; end if;
        if all_z then return "z"; end if;
        if any_x then return "X"; end if;
        if any_z then return "Z"; end if;
        return "";   -- all known: caller renders the decimal value
    end function;

    function sv_dstr(v : std_logic_vector) return string is
        constant n  : natural := v'length;
        variable vv : std_logic_vector(n - 1 downto 0) := v;
        variable u  : unsigned(n - 1 downto 0);
        constant uc : string := sv_unknown_char(vv);
    begin
        if uc'length > 0 then return uc; end if;
        for i in 0 to n - 1 loop
            if vv(i) = '1' or vv(i) = 'H' then u(i) := '1'; else u(i) := '0'; end if;
        end loop;
        return integer'image(to_integer(u));
    end function;

    -- Right-justify the %d decimal (or "x") in a field of `width` blanks.
    -- Verilog default %d pads to the operand's max-magnitude width; %0d and
    -- explicit narrow widths pass width <= length here and are returned as-is.
    function sv_dstr(v : std_logic_vector; width : integer) return string is
        constant s : string := sv_dstr(v);
    begin
        if width <= s'length then
            return s;
        end if;
        return (1 to width - s'length => ' ') & s;
    end function;

    -- Signed %d: interpret the 4-state vector as two's-complement. "x" if any
    -- bit is unknown, else the signed decimal (with '-' for negatives).
    function sv_dstr_signed(v : std_logic_vector) return string is
        constant n  : natural := v'length;
        variable vv : std_logic_vector(n - 1 downto 0) := v;
        variable s  : signed(n - 1 downto 0);
        constant uc : string := sv_unknown_char(vv);
    begin
        if uc'length > 0 then return uc; end if;
        for i in 0 to n - 1 loop
            if vv(i) = '1' or vv(i) = 'H' then s(i) := '1'; else s(i) := '0'; end if;
        end loop;
        return integer'image(to_integer(s));
    end function;

    function sv_dstr_signed(v : std_logic_vector; width : integer) return string is
        constant s : string := sv_dstr_signed(v);
    begin
        if width <= s'length then
            return s;
        end if;
        return (1 to width - s'length => ' ') & s;
    end function;

    -- Verilog %s: interpret the vector as packed 8-bit ASCII bytes, most
    -- significant byte first. Leading all-zero (null) bytes are suppressed.
    function sv_sstr(v : std_logic_vector) return string is
        constant n      : natural := v'length;
        variable vv     : std_logic_vector(n - 1 downto 0) := v;
        constant nbytes : natural := (n + 7) / 8;
        variable res    : string(1 to nbytes);
        variable cnt    : natural := 0;
        variable code   : natural;
        variable started : boolean := false;
    begin
        for b in nbytes - 1 downto 0 loop
            code := 0;
            for bit in 0 to 7 loop
                if (b * 8 + bit) < n then
                    if vv(b * 8 + bit) = '1' or vv(b * 8 + bit) = 'H' then
                        code := code + 2 ** bit;
                    end if;
                end if;
            end loop;
            if code /= 0 or started then
                started := true;
                cnt := cnt + 1;
                res(cnt) := character'val(code);
            end if;
        end loop;
        if cnt = 0 then
            return "";
        end if;
        return res(1 to cnt);
    end function;

    -- Suppress leading '0' chars (Verilog %0b/%0h/%0o); keep >= 1 char so a
    -- zero value still prints "0".
    function sv_strip0(s : string) return string is
        variable first : natural := s'left;
    begin
        while first < s'right and s(first) = '0' loop
            first := first + 1;
        end loop;
        return s(first to s'right);
    end function;

end package body;
