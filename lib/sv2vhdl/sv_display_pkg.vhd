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
    -- Verilog %c: the low 8 bits as one ASCII character
    function sv_cstr(v : std_logic_vector) return string;
    -- $write line buffering: partial text accumulates until a newline
    -- (vvp semantics — consecutive $write calls build one output line).
    -- sv_write_buf appends and flushes embedded newlines; sv_display_line
    -- flushes pending text & s as a complete line; sv_write_flush emits
    -- any residue (called by $finish).
    procedure sv_write_buf(s : string);
    procedure sv_display_line(s : string);
    procedure sv_write_flush;
    -- $timeformat(units, precision, suffix, min_width): set the global %t format.
    procedure sv_set_timeformat(u : integer; pr : integer; suf : string;
                                w : integer);
    -- Verilog %t: format a time `value` (in the calling scope's time units,
    -- signed power of 10) per the current $timeformat (or the default derived
    -- from `scope_prec` if $timeformat was never called).
    impure function sv_tstr(value : integer; scope_units : integer;
                            scope_prec : integer) return string;
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
                -- IEEE 1364: NUL characters in %s output are not printed
                -- (embedded as well as leading; nvc's report would otherwise
                -- escape them as a literal \000).
                if code /= 0 then
                    cnt := cnt + 1;
                    res(cnt) := character'val(code);
                end if;
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

    -- ---- $timeformat / %t ------------------------------------------------
    -- Global (runtime, mutable) $timeformat state. VHDL requires a shared
    -- variable to have a protected type.
    type t_timeformat is protected
        procedure set(u : integer; pr : integer; suf : string; w : integer);
        impure function is_set   return boolean;
        impure function get_u    return integer;
        impure function get_prec return integer;
        impure function get_w    return integer;
        impure function get_suf  return string;
    end protected;

    type t_timeformat is protected body
        variable f_set   : boolean := false;
        variable f_units : integer := 0;
        variable f_prec  : integer := 0;
        variable f_width : integer := 0;
        variable f_suf   : string(1 to 64) := (others => ' ');
        variable f_len   : integer := 0;
        procedure set(u : integer; pr : integer; suf : string; w : integer) is
        begin
            f_set := true; f_units := u; f_prec := pr; f_width := w;
            f_len := suf'length;
            if f_len > 64 then f_len := 64; end if;
            f_suf := (others => ' ');
            if f_len > 0 then
                f_suf(1 to f_len) := suf(suf'left to suf'left + f_len - 1);
            end if;
        end procedure;
        impure function is_set   return boolean is begin return f_set;   end;
        impure function get_u    return integer is begin return f_units; end;
        impure function get_prec return integer is begin return f_prec;  end;
        impure function get_w    return integer is begin return f_width; end;
        impure function get_suf  return string  is begin return f_suf(1 to f_len); end;
    end protected body;

    shared variable g_timeformat : t_timeformat;

    procedure sv_set_timeformat(u : integer; pr : integer; suf : string;
                                w : integer) is
    begin
        g_timeformat.set(u, pr, suf, w);
    end procedure;

    -- Right-justify `s` in a field of `w` blanks (no truncation if longer).
    function rjust(s : string; w : integer) return string is
    begin
        if s'length >= w then
            return s;
        end if;
        return (1 to w - s'length => ' ') & s;
    end function;

    -- Non-negative integer as a decimal with `p` fractional digits, e.g.
    -- (3000, 6) -> "0.003000", (12345, 2) -> "123.45", (0, 0) -> "0".
    function dec_with_point(n : natural; p : natural) return string is
        constant s : string := integer'image(n);
        variable pad : integer;
    begin
        if p = 0 then
            return s;
        end if;
        if s'length <= p then
            pad := p - s'length;
            return "0." & (1 to pad => '0') & s;
        end if;
        return s(s'left to s'left + (s'length - p) - 1) & "."
             & s(s'left + (s'length - p) to s'right);
    end function;

    impure function sv_tstr(value : integer; scope_units : integer;
                            scope_prec : integer) return string is
        variable u, p, w, e : integer;
        variable av, scaled, pw10 : integer;
        variable neg : boolean;
    begin
        if g_timeformat.is_set then
            u := g_timeformat.get_u; p := g_timeformat.get_prec;
            w := g_timeformat.get_w;
        else
            -- default %t: units = simulation precision, 0 decimals, width 20.
            u := scope_prec; p := 0; w := 20;
        end if;
        neg := value < 0;
        av  := abs(value);
        -- scale so the result carries `p` fractional digits:
        -- scaled = round(value * 10^(scope_units - u + p))
        e := scope_units - u + p;
        if e >= 0 then
            scaled := av;
            for i in 1 to e loop scaled := scaled * 10; end loop;
        else
            pw10 := 1;
            for i in 1 to -e loop pw10 := pw10 * 10; end loop;
            scaled := (av + pw10 / 2) / pw10;   -- round to nearest
        end if;
        -- Assemble: [-]<digits>.<frac><suffix>, right-justified in width w.
        -- get_suf is the empty string when $timeformat was never called.
        if neg then
            return rjust("-" & dec_with_point(scaled, p) & g_timeformat.get_suf, w);
        else
            return rjust(dec_with_point(scaled, p) & g_timeformat.get_suf, w);
        end if;
    end function;

    function sv_cstr(v : std_logic_vector) return string is
        alias av : std_logic_vector(v'length - 1 downto 0) is v;
        variable ci : natural := 0;
    begin
        for b in 0 to 7 loop
            if b <= av'high and av(b) = '1' then
                ci := ci + 2 ** b;
            end if;
        end loop;
        return "" & character'val(ci);
    end function;

    type t_wbuf is protected
        procedure append(s : string);
        procedure flush_line;
        procedure flush_partial;
    end protected;

    type t_wbuf is protected body
        variable buf : string(1 to 8192);
        variable len : natural := 0;

        procedure append(s : string) is
        begin
            for k in s'range loop
                if s(k) = LF then
                    report buf(1 to len);
                    len := 0;
                elsif len < buf'length then
                    len := len + 1;
                    buf(len) := s(k);
                end if;
            end loop;
        end procedure;

        procedure flush_line is
        begin
            report buf(1 to len);
            len := 0;
        end procedure;

        procedure flush_partial is
        begin
            if len > 0 then
                report buf(1 to len);
                len := 0;
            end if;
        end procedure;
    end protected body;

    shared variable g_wbuf : t_wbuf;

    procedure sv_write_buf(s : string) is
    begin
        g_wbuf.append(s);
    end procedure;

    procedure sv_display_line(s : string) is
    begin
        -- $display: pending partial text & s form one line; embedded
        -- newlines split exactly as vvp splits them
        g_wbuf.append(s);
        g_wbuf.flush_line;
    end procedure;

    procedure sv_write_flush is
    begin
        g_wbuf.flush_partial;
    end procedure;

end package body;

---------------------------------------------------------------------------
-- %v strength formatting.  A separate package so only %v-using designs
-- reference the VHPIDIRECT query (hosted by libresolver.so).
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

package sv_strength_pkg is
    impure function sv_vstr(v : logic3d; path : string) return string;
    -- Vector %v: per-bit strength tokens, MSB first, joined with '_'
    -- (each bit is its own kernel net, keyed "path(i)")
    impure function sv_vstr(v : logic3d_vector; path : string) return string;
    -- $swrite support: pack a formatted string into a logic3d_vector
    -- as 8-bit ASCII, right-justified, zero-filled (the Verilog
    -- string-in-reg convention)
    function sv_str2vec(s : string; w : natural) return logic3d_vector;
end package sv_strength_pkg;

package body sv_strength_pkg is

    impure function sv_net_strength(path : string) return integer is
    begin
        -- Stub body; replaced by VHPIDIRECT at load time
        return -1;
    end function;
    attribute foreign of sv_net_strength [string return integer] : function is
        "VHPIDIRECT sv2vhdl_net_strength";

    function strength_prefix(sc : integer) return string is
    begin
        if sc >= 16 then
            return "Su";
        elsif sc >= 8 then
            return "St";
        elsif sc >= 4 then
            return "Pu";
        else
            return "We";
        end if;
    end function;

    impure function sv_vstr(v : logic3d; path : string) return string is
        variable q, vl, sc, fl : integer;
    begin
        q := sv_net_strength(path);
        if q >= 0 then
            vl := q / 65536;
            sc := (q / 256) mod 256;
            fl := q mod 256;
            if fl = 2 or fl = 5 then                     -- UNDRIVEN
                return "HiZ";
            elsif fl = 1 or fl = 4 then                  -- UNKNOWN
                return strength_prefix(sc) & "X";
            elsif vl >= 127 then
                return strength_prefix(sc) & "1";
            else
                return strength_prefix(sc) & "0";
            end if;
        end if;

        -- Not a kernel net: derive from the value alphabet — plain
        -- drivers are strong, H/L are the weak codes
        case v is
            when L3D_0 => return "St0";
            when L3D_1 => return "St1";
            when L3D_L => return "We0";
            when L3D_H => return "We1";
            when L3D_Z => return "HiZ";
            when L3D_W => return "WeX";
            when others => return "StX";
        end case;
    end function;

    impure function sv_vstr(v : logic3d_vector; path : string) return string is
        variable r   : string(1 to 4 * v'length - 1);
        variable pos : positive := 1;
    begin
        -- Translator vectors are (msb downto 0): 'range iterates MSB
        -- first, matching Verilog %v bit order.  Every scalar token is
        -- exactly 3 characters.
        for i in v'range loop
            r(pos to pos + 2) :=
                sv_vstr(v(i), path & "(" & integer'image(i) & ")");
            if i /= v'right then
                r(pos + 3) := '_';
                pos := pos + 4;
            end if;
        end loop;
        return r;
    end function;

    function sv_str2vec(s : string; w : natural) return logic3d_vector is
        variable r  : logic3d_vector(w - 1 downto 0) := (others => L3D_0);
        variable ci : natural;
    begin
        -- Last character occupies bits 7..0; unused high bits stay 0
        for k in 0 to s'length - 1 loop
            exit when k * 8 + 7 > w - 1;
            ci := character'pos(s(s'right - k));
            for b in 0 to 7 loop
                if (ci / 2 ** b) mod 2 = 1 then
                    r(k * 8 + b) := L3D_1;
                end if;
            end loop;
        end loop;
        return r;
    end function;

end package body sv_strength_pkg;
