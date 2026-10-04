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
    -- from `scope_prec` if $timeformat was never called).  The scaling is
    -- exact decimal, for every count nvc passes: a 64-bit one such as the
    -- translator's $time, (now / (1000 ps)), after 2.147 s included.
    impure function sv_tstr(value : integer; scope_units : integer;
                            scope_prec : integer) return string;
    -- Verilog %t of a real `value` (in units of 10^scope_units s): scaled in
    -- double precision and printed with the $timeformat precision, C "%.*f",
    -- as vvp does; no INTEGER range limit and no rounding to whole units.
    impure function sv_tstr(value : real; scope_units : integer;
                            scope_prec : integer) return string;

    -- ---- File I/O: $fopen family, $fdisplay/$fwrite/$fstrobe/$fmonitor,
    -- ---- $fclose, $fflush ---------------------------------------------------
    -- vvp's semantics (vpi/sys_fileio.c, sys_display.c, vvp/vpi_mcd.cc).  A
    -- descriptor is the Verilog value's vpiIntVal (logic3d_types_pkg's
    -- sv_vec_int32).  Bit 31 set (a negative integer): a file descriptor
    -- (FD), index 0 stdin, 1 stdout, 2 stderr, files from 3 up, the lowest
    -- free index first.  Bit 31 clear: a multi-channel descriptor (MCD), bit
    -- 0 stdout, bits 1 .. 30 the files of the one-argument $fopen.  Text for
    -- stdout -- and for stderr: a run has one output stream -- goes through
    -- the $display line buffer above; other text reaches its files byte for
    -- byte.  Messages are vvp's, printed like $display output; `loc' is the
    -- call's "<file>:<line>", `task' its task name.  A relative file name is
    -- resolved by logic3d_types_pkg's sv_file_path.
    --
    -- $fopen(name): an MCD, 0 when the file cannot be opened.  kind: the vpi
    -- type of the name argument, for vvp's "not a valid string" message.
    impure function sv_fopen(name : string; kind : string; loc : string)
        return integer;
    -- $fopen(name, mode) and $fopenr/$fopenw/$fopena(name) (`task' names the
    -- call; only $fopen checks its mode): an FD, 0 on failure.  The files are
    -- text: "b" changes nothing, and "w+"/"a+" write as "w"/"a" do (no task
    -- reads a file back); "r+" (update in place) is refused with a warning.
    impure function sv_fopen(name : string; kind : string; mode : string;
                             task : string; loc : string) return integer;
    -- $fwrite / $fdisplay (s & LF): a warning for an invalid descriptor,
    -- nothing at all for 0 (vvp's get_fd_mcd_from_arg); the text is
    -- evaluated either way, as vvp evaluates the arguments
    procedure sv_fwrite(fd : integer; s : string; task : string; loc : string);
    procedure sv_fdisplay(fd : integer; s : string; task : string; loc : string);
    procedure sv_fclose(fd : integer; loc : string);
    procedure sv_fflush_all;
    procedure sv_fflush(fd : integer; loc : string);
    -- $fstrobe: sv_fstrobe_arm checks the descriptor of a call of the
    -- statement `key' (its request signal's 'path_name: one per instance)
    -- and records the call; at the end of the time step the statement's
    -- companion process writes each recorded call's text, evaluated for
    -- it, to that call's descriptor:
    --     while sv_fstrobe_next(key) loop sv_fput_due(<text> & LF); end loop;
    -- skipping a descriptor that was closed meanwhile (vvp's strobe_cb).
    procedure sv_fstrobe_arm(key : string; fd : integer; task : string;
                             loc : string);
    impure function sv_fstrobe_next(key : string) return boolean;
    -- $fmonitor: each call of statement `key' starts a monitor on its
    -- descriptor, which writes at the end of the call's time step and at the
    -- end of every later time step in which a monitored value changed
    -- (`changed'), until an $fclose of a descriptor it writes to (vvp's
    -- sys_monitor_fclose):
    --     while sv_fmonitor_next(key, <changed>) loop
    --         sv_fput_due(<text> & LF);
    --     end loop;
    -- $monitoroff and $monitoron leave it alone.
    procedure sv_fmonitor_arm(key : string; fd : integer; task : string;
                              loc : string);
    impure function sv_fmonitor_next(key : string; changed : boolean)
        return boolean;
    -- the text of the call sv_fstrobe_next / sv_fmonitor_next made due
    procedure sv_fput_due(s : string);
end package;

use std.textio.all;                    -- the files (TEXT, written raw)
use work.logic3d_types_pkg.all;        -- sv_file_path, sv_file_name_msg

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

    -- %t scales decimal digit strings, as vvp's get_time does, never INTEGER
    -- values: INTEGER is 32 bits under --std=2040, so 3 ms counted in ps
    -- (3e9) already overflowed it, while a time count has 64 bits.

    -- `n` zero digits (none for n <= 0).
    function zeros(n : integer) return string is
    begin
        if n <= 0 then
            return "";
        end if;
        return (1 to n => '0');
    end function;

    -- A decimal digit string plus one: "1299" -> "1300", "99" -> "100".
    function dec_incr(d : string) return string is
        variable r : string(1 to d'length) := d;
    begin
        for i in r'reverse_range loop
            if r(i) /= '9' then
                r(i) := character'succ(r(i));
                return r;
            end if;
            r(i) := '0';
        end loop;
        return "1" & r;
    end function;

    -- The decimal digit string `d` (no sign, no leading zeros) times 10^e:
    -- e >= 0 appends e zeros, e < 0 drops -e digits and rounds half up, e.g.
    -- ("3000000", 3) -> "3000000000", ("15", -1) -> "2", ("4", -1) -> "0".
    function dec_scale(d : string; e : integer) return string is
        constant n : natural := d'length;
        alias a : string(1 to n) is d;
    begin
        if e >= 0 then
            if a = "0" then
                return a;
            end if;
            return a & zeros(e);
        elsif n > -e then
            if a(n + e + 1) >= '5' then
                return dec_incr(a(1 to n + e));
            end if;
            return a(1 to n + e);
        elsif n = -e and a(1) >= '5' then
            return "1";
        else
            return "0";
        end if;
    end function;

    -- The decimal digit string `d` with `p` fractional digits, e.g.
    -- ("3000", 6) -> "0.003000", ("12345", 2) -> "123.45", ("0", 0) -> "0".
    function dec_with_point(d : string; p : natural) return string is
        constant n : natural := d'length;
        alias a : string(1 to n) is d;
    begin
        if p = 0 then
            return a;
        elsif n <= p then
            return "0." & zeros(p - n) & a;
        end if;
        return a(1 to n - p) & "." & a(n - p + 1 to n);
    end function;

    impure function sv_tstr(value : integer; scope_units : integer;
                            scope_prec : integer) return string is
        -- `value` is only read through its image.  nvc hands the parameter
        -- a count beyond INTEGER'HIGH intact (the translator's $time is
        -- (now / (1000 ps)), a 64-bit quotient), but an INTEGER variable or
        -- operation would cut it to 32 bits or fail.
        constant img : string := integer'image(value);
        constant neg : boolean := img(img'left) = '-';
        variable u, p, w : integer;
    begin
        if g_timeformat.is_set then
            u := g_timeformat.get_u; p := g_timeformat.get_prec;
            w := g_timeformat.get_w;
        else
            -- default %t: units = simulation precision, 0 decimals, width 20.
            u := scope_prec; p := 0; w := 20;
        end if;
        -- The digits of round(|value| * 10^(scope_units - u + p)), with the
        -- point p digits from the right: [-]<digits>.<frac><suffix>,
        -- right-justified in width w.  get_suf is the empty string when
        -- $timeformat was never called.
        if neg then
            return rjust("-" & dec_with_point(dec_scale(img(img'left + 1 to img'right),
                                                        scope_units - u + p), p)
                         & g_timeformat.get_suf, w);
        else
            return rjust(dec_with_point(dec_scale(img, scope_units - u + p), p)
                         & g_timeformat.get_suf, w);
        end if;
    end function;

    impure function sv_tstr(value : real; scope_units : integer;
                            scope_prec : integer) return string is
        variable u, p, w, shift : integer;
        variable pw10, scaled : real;
    begin
        if g_timeformat.is_set then
            u := g_timeformat.get_u; p := g_timeformat.get_prec;
            w := g_timeformat.get_w;
        else
            u := scope_prec; p := 0; w := 20;
        end if;
        -- vvp's get_time_real: value * 10^shift, or value / 10^-shift, then
        -- "%.<p>f" and the suffix.  10^k is built by multiplying, exact (as
        -- C's pow(10.0, k)) up to k = 22.
        shift := scope_units - u;
        pw10 := 1.0;
        for i in 1 to abs(shift) loop
            pw10 := pw10 * 10.0;
        end loop;
        if shift >= 0 then
            scaled := value * pw10;
        else
            scaled := value / pw10;
        end if;
        return rjust(to_string(scaled, "%." & integer'image(p) & "f")
                     & g_timeformat.get_suf, w);
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


    -- =====================================================================
    -- File I/O (see the package declaration).  A TEXT file is written with
    -- the implicit WRITE(f, string), which writes the characters and nothing
    -- else, so the bytes of $fwrite/$fdisplay reach the file unchanged.
    -- =====================================================================

    -- C's "%x" of a 32-bit value (no leading zeros)
    function hex32(i : integer) return string is
        constant u : unsigned(31 downto 0) := unsigned(to_signed(i, 32));
        variable r : string(1 to 8);
    begin
        for k in 0 to 7 loop
            r(8 - k) := HEXCHARS(to_integer(u(4 * k + 3 downto 4 * k)) + 1);
        end loop;
        for k in 1 to 7 loop
            if r(k) /= '0' then
                return r(k to 8);
            end if;
        end loop;
        return r(8 to 8);
    end function;

    function is_mcd(fd : integer) return boolean is
    begin
        return fd >= 0;
    end function;

    -- bit b (0 .. 30) of an MCD
    function mcd_bit(fd : integer; b : natural) return boolean is
    begin
        return (fd / 2 ** b) mod 2 = 1;
    end function;

    -- the index of an FD (its bits 30 .. 0)
    function fd_index(fd : integer) return natural is
    begin
        return (fd + 2147483647) + 1;
    end function;

    -- vvp's as_escaped, for a mode argument (printable here: the mode is
    -- at most three characters when it is shown)
    function c_esc(s : string) return string is
        variable r : string(1 to 4 * s'length + 1);
        variable k : natural := 0;
        variable c : natural;
    begin
        for i in s'range loop
            c := character'pos(s(i));
            if c >= 32 and c < 127 then
                k := k + 1;
                r(k) := s(i);
            else
                r(k + 1) := '\';
                r(k + 2) := character'val(48 + (c / 64) mod 8);
                r(k + 3) := character'val(48 + (c / 8) mod 8);
                r(k + 4) := character'val(48 + c mod 8);
                k := k + 4;
            end if;
        end loop;
        return r(1 to k);
    end function;

    -- One open file: an object per descriptor slot, made on first use and
    -- reused after a close
    type t_fslot is protected
        impure function open_file(name : string; m : file_open_kind)
            return boolean;
        procedure put(s : string);
        procedure close_file;
        procedure flush_file;
    end protected;

    type t_fslot is protected body
        file f : text;
        variable opened : boolean := false;

        impure function open_file(name : string; m : file_open_kind)
            return boolean is
            variable st : file_open_status;
        begin
            if opened then
                file_close(f);
                opened := false;
            end if;
            file_open(st, f, name, m);
            opened := st = OPEN_OK;
            return opened;
        end function;

        procedure put(s : string) is
        begin
            if opened then
                write(f, s);
            end if;
        end procedure;

        procedure close_file is
        begin
            if opened then
                file_close(f);
                opened := false;
            end if;
        end procedure;

        procedure flush_file is
        begin
            if opened then
                flush(f);
            end if;
        end procedure;
    end protected body;

    type t_fslot_ptr is access t_fslot;
    -- nvc lowers a protected type's methods with its objects: without an object
    -- of its own, a design that calls t_fslot.put through an access value but
    -- never reaches `new t_fslot' failed to elaborate ("missing vcode for
    -- SV2VHDL.SV_DISPLAY_PKG.T_FSLOT.PUT(S)")
    shared variable g_fslot_lowered : t_fslot;

    type t_chan is record
        slot     : t_fslot_ptr;
        is_open  : boolean;
        writable : boolean;
        touched  : natural;     -- the epoch of its last open or close
    end record;
    type t_chan_vec is array (natural range <>) of t_chan;
    type t_chan_vec_ptr is access t_chan_vec;

    constant FD_INCR : natural := 32;     -- vvp grows its FD table by 32 ...
    constant FD_MAX  : natural := 1024;   -- ... up to 1024 descriptors

    -- A $fstrobe call waiting for the end of its time step, or a running
    -- $fmonitor
    type t_arm is record
        key   : line;             -- the statement (an instance's)
        fd    : integer;
        epoch : natural;          -- the file epoch when it was armed
        first : boolean;          -- a monitor's first write is still due
    end record;
    type t_arm_vec is array (natural range <>) of t_arm;
    type t_arm_vec_ptr is access t_arm_vec;

    procedure arm_add(v : inout t_arm_vec_ptr; n : inout natural;
                      key : string; fd : integer; epoch : natural) is
        variable nv : t_arm_vec_ptr;
    begin
        if v = null then
            v := new t_arm_vec(0 to 15);
        elsif n > v'high then
            nv := new t_arm_vec(0 to 2 * v'length - 1);
            nv(0 to n - 1) := v(0 to n - 1);
            deallocate(v);
            v := nv;
        end if;
        v(n) := (new string'(key), fd, epoch, true);
        n := n + 1;
    end procedure;

    -- The descriptor tables.  MCD bit 0 and FDs 0 .. 2 are always open.
    type t_fio is protected
        impure function open_mcd(name : string) return integer;
        -- the index of the new FD; 0: the open failed, -1: the table is full
        impure function open_fd(name : string; m : file_open_kind;
                                writable : boolean) return integer;
        impure function valid(fd : integer) return boolean;
        procedure put(fd : integer; s : string);
        procedure close(fd : integer);
        procedure flush(fd : integer);
        procedure flush_all;
        procedure strobe_arm(key : string; fd : integer);
        impure function strobe_next(key : string) return boolean;
        procedure monitor_arm(key : string; fd : integer);
        impure function monitor_next(key : string; changed : boolean)
            return boolean;
        procedure put_due(s : string);
    end protected;

    type t_fio is protected body
        variable mcd : t_chan_vec(0 to 30);
        variable fds : t_chan_vec_ptr := null;
        variable ep  : natural := 0;
        variable strobes  : t_arm_vec_ptr := null;
        variable nstrobes : natural := 0;
        variable mons     : t_arm_vec_ptr := null;
        variable nmons    : natural := 0;
        variable due_fd   : integer := 0;     -- the call sv_fput_due writes
        variable scan     : integer := -1;    -- monitor_next's place, -1: idle

        procedure ensure_fds is
        begin
            if fds = null then
                fds := new t_chan_vec(0 to FD_INCR - 1);
                for i in 0 to 2 loop
                    fds(i).is_open := true;
                end loop;
            end if;
        end procedure;

        procedure touch(c : inout t_chan) is
        begin
            ep := ep + 1;
            c.touched := ep;
        end procedure;

        impure function open_mcd(name : string) return integer is
            variable p : t_fslot_ptr;
        begin
            for i in 1 to 30 loop
                if not mcd(i).is_open then
                    if mcd(i).slot = null then
                        mcd(i).slot := new t_fslot;
                    end if;
                    p := mcd(i).slot;
                    if not p.open_file(name, WRITE_MODE) then
                        return 0;
                    end if;
                    mcd(i).is_open := true;
                    mcd(i).writable := true;
                    touch(mcd(i));
                    return 2 ** i;
                end if;
            end loop;
            return 0;           -- every channel is in use
        end function;

        impure function open_fd(name : string; m : file_open_kind;
                                writable : boolean) return integer is
            variable p   : t_fslot_ptr;
            variable n   : t_chan_vec_ptr;
            variable idx : integer := -1;
        begin
            ensure_fds;
            for i in 3 to fds'high loop
                if not fds(i).is_open then
                    idx := i;
                    exit;
                end if;
            end loop;
            if idx < 0 then
                if fds'length >= FD_MAX then
                    return -1;
                end if;
                idx := fds'length;
                n := new t_chan_vec(0 to fds'length + FD_INCR - 1);
                n(fds'range) := fds.all;
                deallocate(fds);
                fds := n;
            end if;
            if fds(idx).slot = null then
                fds(idx).slot := new t_fslot;
            end if;
            p := fds(idx).slot;
            if not p.open_file(name, m) then
                return 0;
            end if;
            fds(idx).is_open := true;
            fds(idx).writable := writable;
            touch(fds(idx));
            return idx;
        end function;

        -- vvp's is_valid_fd_mcd (fd /= 0)
        impure function valid(fd : integer) return boolean is
            variable idx : natural;
        begin
            if is_mcd(fd) then
                for b in 1 to 30 loop
                    if mcd_bit(fd, b) and not mcd(b).is_open then
                        return false;
                    end if;
                end loop;
                return true;
            end if;
            ensure_fds;
            idx := fd_index(fd);
            return idx <= fds'high and fds(idx).is_open;
        end function;

        -- stdout: MCD bit 0, or FD 1 (and FD 2, stderr, which shares it)
        procedure put(fd : integer; s : string) is
            variable p   : t_fslot_ptr;
            variable idx : natural;
        begin
            if is_mcd(fd) then
                if mcd_bit(fd, 0) then
                    sv_write_buf(s);
                end if;
                for b in 1 to 30 loop
                    if mcd_bit(fd, b) and mcd(b).is_open then
                        p := mcd(b).slot;
                        p.put(s);
                    end if;
                end loop;
                return;
            end if;
            ensure_fds;
            idx := fd_index(fd);
            if idx = 1 or idx = 2 then
                sv_write_buf(s);
            elsif idx >= 3 and idx <= fds'high and fds(idx).is_open
                and fds(idx).writable then
                p := fds(idx).slot;
                p.put(s);
            end if;
        end procedure;

        procedure close(fd : integer) is
            variable p   : t_fslot_ptr;
            variable idx : natural;
        begin
            if is_mcd(fd) then
                if mcd_bit(fd, 0) then
                    touch(mcd(0));          -- stdout stays open
                end if;
                for b in 1 to 30 loop
                    if mcd_bit(fd, b) and mcd(b).is_open then
                        p := mcd(b).slot;
                        p.close_file;
                        mcd(b).is_open := false;
                        touch(mcd(b));
                    end if;
                end loop;
                return;
            end if;
            ensure_fds;
            idx := fd_index(fd);
            if idx <= 2 then
                touch(fds(idx));            -- stdin/stdout/stderr stay open
            elsif idx <= fds'high and fds(idx).is_open then
                p := fds(idx).slot;
                p.close_file;
                fds(idx).is_open := false;
                touch(fds(idx));
            end if;
        end procedure;

        procedure flush(fd : integer) is
            variable p   : t_fslot_ptr;
            variable idx : natural;
        begin
            if is_mcd(fd) then
                for b in 1 to 30 loop
                    if mcd_bit(fd, b) and mcd(b).is_open then
                        p := mcd(b).slot;
                        p.flush_file;
                    end if;
                end loop;
                return;
            end if;
            ensure_fds;
            idx := fd_index(fd);
            if idx >= 3 and idx <= fds'high and fds(idx).is_open
                and fds(idx).writable then
                p := fds(idx).slot;
                p.flush_file;
            end if;
        end procedure;

        procedure flush_all is
            variable p : t_fslot_ptr;
        begin
            for b in 1 to 30 loop
                if mcd(b).is_open then
                    p := mcd(b).slot;
                    p.flush_file;
                end if;
            end loop;
            if fds /= null then
                for i in 3 to fds'high loop
                    if fds(i).is_open and fds(i).writable then
                        p := fds(i).slot;
                        p.flush_file;
                    end if;
                end loop;
            end if;
        end procedure;

        -- an $fmonitor armed at epoch `armed' still runs: no descriptor it
        -- writes to was closed (or opened again) since -- vvp's
        -- sys_monitor_fclose cancels it at the $fclose
        impure function live(fd : integer; armed : natural) return boolean is
            variable idx : natural;
        begin
            if is_mcd(fd) then
                for b in 0 to 30 loop
                    if mcd_bit(fd, b) then
                        if (b > 0 and not mcd(b).is_open)
                            or mcd(b).touched > armed then
                            return false;
                        end if;
                    end if;
                end loop;
                return true;
            end if;
            ensure_fds;
            idx := fd_index(fd);
            return idx <= fds'high and fds(idx).is_open
                and fds(idx).touched <= armed;
        end function;

        procedure strobe_arm(key : string; fd : integer) is
        begin
            arm_add(strobes, nstrobes, key, fd, ep);
        end procedure;

        -- the first recorded call of statement `key', taken off the list
        -- (calls are recorded in call order)
        impure function strobe_next(key : string) return boolean is
        begin
            for i in 0 to nstrobes - 1 loop
                if strobes(i).key.all = key then
                    due_fd := strobes(i).fd;
                    deallocate(strobes(i).key);
                    for k in i to nstrobes - 2 loop
                        strobes(k) := strobes(k + 1);
                    end loop;
                    nstrobes := nstrobes - 1;
                    return true;
                end if;
            end loop;
            return false;
        end function;

        procedure monitor_arm(key : string; fd : integer) is
        begin
            arm_add(mons, nmons, key, fd, ep);
        end procedure;

        -- the next running monitor of statement `key' that writes now (a
        -- monitored value changed, or its first write is due); a scan of
        -- the list from where the last call left off, until it says false
        impure function monitor_next(key : string; changed : boolean)
            return boolean is
            variable i : natural;
            variable k : natural := 0;
        begin
            if scan < 0 then
                scan := 0;
            end if;
            while scan < nmons loop
                i := scan;
                scan := scan + 1;
                if mons(i).key.all = key and live(mons(i).fd, mons(i).epoch)
                    and (changed or mons(i).first) then
                    mons(i).first := false;
                    due_fd := mons(i).fd;
                    return true;
                end if;
            end loop;
            scan := -1;
            -- the end of a scan: forget the monitors an $fclose ended
            for j in 0 to nmons - 1 loop
                if live(mons(j).fd, mons(j).epoch) then
                    mons(k) := mons(j);
                    k := k + 1;
                else
                    deallocate(mons(j).key);
                end if;
            end loop;
            nmons := k;
            return false;
        end function;

        procedure put_due(s : string) is
        begin
            if due_fd /= 0 and valid(due_fd) then
                put(due_fd, s);
            end if;
        end procedure;
    end protected body;

    shared variable g_fio : t_fio;

    procedure fd_warn(fd : integer; task : string; loc : string) is
    begin
        if is_mcd(fd) then
            sv_display_line("WARNING: " & loc & ": invalid MCD (0x" & hex32(fd)
                            & ") given to " & task & "().");
        else
            sv_display_line("WARNING: " & loc & ": invalid file descriptor (0x"
                            & hex32(fd) & ") given to " & task & "().");
        end if;
    end procedure;

    impure function sv_fd_ok(fd : integer; task : string; loc : string)
        return boolean is
    begin
        if fd = 0 then
            return false;
        end if;
        if not g_fio.valid(fd) then
            fd_warn(fd, task, loc);
            return false;
        end if;
        return true;
    end function;

    procedure print_msg(s : string) is
    begin
        if s'length > 0 then
            sv_write_buf(s);
        end if;
    end procedure;

    impure function sv_fopen(name : string; kind : string; loc : string)
        return integer is
        constant msg : string := sv_file_name_msg(name, kind, "$fopen", loc);
    begin
        if msg'length > 0 then
            print_msg(msg);
            return 0;
        end if;
        return g_fio.open_mcd(sv_file_path(name));
    end function;

    impure function sv_fopen(name : string; kind : string; mode : string;
                             task : string; loc : string) return integer is
        alias md : string(1 to mode'length) is mode;
        constant name_msg : string := sv_file_name_msg(name, kind, task, loc);
        variable fail     : boolean := false;
        variable bin      : boolean := false;
        variable plus     : boolean := false;
        variable idx      : integer;
        variable writable : boolean := true;
        variable m        : file_open_kind := WRITE_MODE;
    begin
        if task = "$fopen" then
            -- vvp's sys_fopen_calltf checks the mode first
            if md'length = 0 then
                sv_display_line("WARNING: " & loc & ": $fopen's mode argument "
                                & "is not a valid string.");
                fail := true;
            end if;
            if md'length > 3 then
                sv_display_line("WARNING: " & loc & ": $fopen's mode argument ("
                                & c_esc(md) & ") is too long.");
                fail := true;
            else
                if md'length = 0 or (md(1) /= 'r' and md(1) /= 'w'
                                     and md(1) /= 'a') then
                    fail := true;
                else
                    for i in 2 to md'length loop
                        if md(i) = 'b' and not bin then
                            bin := true;
                        elsif md(i) = '+' and not plus then
                            plus := true;
                        else
                            fail := true;
                        end if;
                    end loop;
                end if;
                if fail then
                    sv_display_line("WARNING: " & loc & ": $fopen's mode "
                                    & "argument (" & c_esc(md)
                                    & ") is invalid.");
                end if;
            end if;
        end if;
        print_msg(name_msg);
        if fail or name_msg'length > 0 or md'length = 0 then
            return 0;
        end if;
        -- C fopen's modes on TEXT files ("b" changes nothing on POSIX)
        for i in 2 to md'length loop
            if md(i) = '+' then
                plus := true;
            end if;
        end loop;
        if md(1) = 'r' then
            if plus then
                -- read and write in place: textio has no such mode, so the
                -- file is not opened (never written wrongly)
                sv_display_line("WARNING: " & loc & ": " & task & ": mode """
                                & md & """ (read and update) is not supported; "
                                & "the file is not opened.");
                return 0;
            end if;
            m := READ_MODE;
            writable := false;
        elsif md(1) = 'a' then
            m := APPEND_MODE;
        end if;
        idx := g_fio.open_fd(sv_file_path(name), m, writable);
        if idx < 0 then
            sv_display_line("WARNING: " & loc & ": " & task & ": no more than "
                            & integer'image(FD_MAX) & " files can be open at once.");
            return 0;
        elsif idx = 0 then
            return 0;
        end if;
        return -2147483647 - 1 + idx;
    end function;

    procedure sv_fwrite(fd : integer; s : string; task : string;
                        loc : string) is
    begin
        if sv_fd_ok(fd, task, loc) then
            g_fio.put(fd, s);
        end if;
    end procedure;

    procedure sv_fdisplay(fd : integer; s : string; task : string;
                          loc : string) is
    begin
        if sv_fd_ok(fd, task, loc) then
            g_fio.put(fd, s & LF);
        end if;
    end procedure;

    procedure sv_fclose(fd : integer; loc : string) is
        variable idx : natural;
    begin
        if not sv_fd_ok(fd, "$fclose", loc) then
            return;
        end if;
        g_fio.close(fd);      -- also ends the $fmonitor calls on it
        if is_mcd(fd) then
            if mcd_bit(fd, 0) then
                sv_display_line("WARNING: " & loc & ": could not close MCD "
                                & "STDOUT (0x1) in $fclose().");
            end if;
        else
            idx := fd_index(fd);
            if idx = 0 then
                sv_display_line("WARNING: " & loc & ": could not close file "
                                & "descriptor STDIN (0x80000000) in $fclose().");
            elsif idx = 1 then
                sv_display_line("WARNING: " & loc & ": could not close file "
                                & "descriptor STDOUT (0x80000001) in $fclose().");
            elsif idx = 2 then
                sv_display_line("WARNING: " & loc & ": could not close file "
                                & "descriptor STDERR (0x80000002) in $fclose().");
            end if;
        end if;
    end procedure;

    procedure sv_fflush_all is
    begin
        g_fio.flush_all;
    end procedure;

    procedure sv_fflush(fd : integer; loc : string) is
    begin
        if sv_fd_ok(fd, "$fflush", loc) then
            g_fio.flush(fd);
        end if;
    end procedure;

    procedure sv_fstrobe_arm(key : string; fd : integer; task : string;
                             loc : string) is
    begin
        if sv_fd_ok(fd, task, loc) then
            g_fio.strobe_arm(key, fd);
        end if;
    end procedure;

    impure function sv_fstrobe_next(key : string) return boolean is
    begin
        return g_fio.strobe_next(key);
    end function;

    procedure sv_fmonitor_arm(key : string; fd : integer; task : string;
                              loc : string) is
    begin
        if sv_fd_ok(fd, task, loc) then
            g_fio.monitor_arm(key, fd);
        end if;
    end procedure;

    impure function sv_fmonitor_next(key : string; changed : boolean)
        return boolean is
    begin
        return g_fio.monitor_next(key, changed);
    end function;

    procedure sv_fput_due(s : string) is
    begin
        g_fio.put_due(s);
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
