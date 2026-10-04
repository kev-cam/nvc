-- sv_math_pkg.vhd — Verilog system math/random functions for VHDL
--
-- Wraps C in libsv_math.so via VHPIDIRECT; random/srandom are plain VHDL.
-- Usage:
--   library sv2vhdl;
--   use sv2vhdl.sv_math_pkg.all;
--
-- Load at runtime:
--   nvc --std=2040 --load=.../libsv_math.so -e ...
--

library sv2vhdl;
use sv2vhdl.logic3d_types_pkg.all;   -- $value$plusargs vector results

package sv_math_pkg is

    -- ============================================================
    -- Math functions (IEEE 1800 / Verilog-2005)
    -- ============================================================

    -- Single-argument: real -> real
    function sqrt(x : real) return real;
    function ln(x : real) return real;
    function log10(x : real) return real;
    function exp(x : real) return real;
    function ceil(x : real) return real;
    function floor(x : real) return real;
    function sin(x : real) return real;
    function cos(x : real) return real;
    function tan(x : real) return real;
    function asin(x : real) return real;
    function acos(x : real) return real;
    function atan(x : real) return real;
    function sinh(x : real) return real;
    function cosh(x : real) return real;
    function tanh(x : real) return real;
    function asinh(x : real) return real;
    function acosh(x : real) return real;
    function atanh(x : real) return real;

    -- Double-argument: (real, real) -> real
    function pow(x, y : real) return real;
    function atan2(y, x : real) return real;
    function hypot(x, y : real) return real;

    -- ============================================================
    -- Utility
    -- ============================================================

    -- $clog2: ceiling log2 (bit-width computation)
    function clog2(n : integer) return integer;

    -- ============================================================
    -- Conversions
    -- ============================================================

    function itor(n : integer) return real;   -- integer to real
    function rtoi(x : real) return integer;   -- real to integer (truncate)

    -- ============================================================
    -- Plusargs
    -- ============================================================

    -- $test$plusargs: 1 when any +plusarg on the simulator command line
    -- starts with prefix, else 0.  Implemented in libresolver (VHPI tool argv).
    impure function sv_test_plusargs(prefix : string) return integer;

    -- $value$plusargs(fmt, var).  fmt is "<prefix>%<code>" (d o h x b s e f g).
    -- sv_value_plusargs: 1 when a +plusarg starts with fmt's prefix.
    -- sv_plusarg_vec / sv_plusarg_real: that plusarg's text converted per
    -- fmt's code, with Icarus semantics (bad digits -> all x, '-' -> two's
    -- complement, x/z digits, %s right-aligned chars).  C in libresolver.
    impure function sv_value_plusargs(fmt : string) return integer;
    impure function sv_plusarg_vec(fmt : string; w : positive) return logic3d_vector;
    impure function sv_plusarg_real(fmt : string) return real;
    -- Verilog string held in a vector (8 bits/char, NULs dropped) -> string.
    function l3d_to_string(v : logic3d_vector) return string;

    -- ============================================================
    -- Random: vvp's generator and numbers (plain VHDL, see the body)
    -- ============================================================

    -- Verilog $random: a draw from the design-wide seed, an int32.
    impure function random return integer;
    procedure srandom(seed : in integer);

    -- SystemVerilog $urandom / $urandom_range: vvp draws them from a second
    -- design-wide seed (vpi/sys_random.c urandom()), apart from $random's.
    -- Each result is a 32-bit unsigned value, returned as the integer with
    -- the same 32 bits (two's complement), and so are the bounds:
    --   sv_urandom              a full-range draw plus 2**31 (mod 2**32);
    --   sv_urandom_range(M, m)  vvp's urandom(M, m): a value in [m, M], the
    --                           bounds swapped when M < m (unsigned), no
    --                           draw at all when they are equal.
    impure function sv_urandom return integer;
    impure function sv_urandom_range(maxval, minval : integer) return integer;

    -- Verilog $random(seed) / SystemVerilog $urandom(seed): vvp's draw from
    -- a seed variable, rtl_dist_uniform(&seed, INT32_MIN, INT32_MAX) -- the
    -- value drawn and the seed the call leaves behind differ.  The seed is
    -- read as vvp's vpiIntVal reads it: its low 32 bits, an x or z bit as 0.
    --   sv_random_value / sv_urandom_value  the 32-bit value ($urandom's is
    --                                       the draw plus 2**31);
    --   sv_random_next                      the advanced seed, at the
    --                                       variable's width (bits past 31
    --                                       copy bit 31, as vvp writes an
    --                                       integer into a wider variable);
    --   sv_urandom_seed                     $urandom(seed) also leaves the
    --                                       advanced seed in the $urandom
    --                                       generator.
    function sv_random_value(seed : logic3d_vector) return logic3d_vector;
    function sv_urandom_value(seed : logic3d_vector) return logic3d_vector;
    function sv_random_next(seed : logic3d_vector) return logic3d_vector;
    procedure sv_urandom_seed(seed : in logic3d_vector);

    -- ============================================================
    -- Distribution functions (explicit seed via inout)
    -- IEEE 1364-2005 Annex B algorithms
    -- ============================================================

    procedure dist_uniform(
        seed : inout integer;
        start_val, end_val : in integer;
        result : out integer);

    procedure dist_normal(
        seed : inout integer;
        mean, std_dev : in integer;
        result : out integer);

    procedure dist_exponential(
        seed : inout integer;
        mean : in integer;
        result : out integer);

    procedure dist_poisson(
        seed : inout integer;
        mean : in integer;
        result : out integer);

    procedure dist_chi_square(
        seed : inout integer;
        df : in integer;
        result : out integer);

    procedure dist_t(
        seed : inout integer;
        df : in integer;
        result : out integer);

    procedure dist_erlang(
        seed : inout integer;
        k, mean : in integer;
        result : out integer);

end package sv_math_pkg;

package body sv_math_pkg is

    -- ============================================================
    -- Math functions
    -- ============================================================

    function sqrt(x : real) return real is begin end function;
    attribute foreign of sqrt [real return real] : function is "VHPIDIRECT sv_sqrt";

    function ln(x : real) return real is begin end function;
    attribute foreign of ln [real return real] : function is "VHPIDIRECT sv_ln";

    function log10(x : real) return real is begin end function;
    attribute foreign of log10 [real return real] : function is "VHPIDIRECT sv_log10";

    function exp(x : real) return real is begin end function;
    attribute foreign of exp [real return real] : function is "VHPIDIRECT sv_exp";

    function ceil(x : real) return real is begin end function;
    attribute foreign of ceil [real return real] : function is "VHPIDIRECT sv_ceil";

    function floor(x : real) return real is begin end function;
    attribute foreign of floor [real return real] : function is "VHPIDIRECT sv_floor";

    function sin(x : real) return real is begin end function;
    attribute foreign of sin [real return real] : function is "VHPIDIRECT sv_sin";

    function cos(x : real) return real is begin end function;
    attribute foreign of cos [real return real] : function is "VHPIDIRECT sv_cos";

    function tan(x : real) return real is begin end function;
    attribute foreign of tan [real return real] : function is "VHPIDIRECT sv_tan";

    function asin(x : real) return real is begin end function;
    attribute foreign of asin [real return real] : function is "VHPIDIRECT sv_asin";

    function acos(x : real) return real is begin end function;
    attribute foreign of acos [real return real] : function is "VHPIDIRECT sv_acos";

    function atan(x : real) return real is begin end function;
    attribute foreign of atan [real return real] : function is "VHPIDIRECT sv_atan";

    function sinh(x : real) return real is begin end function;
    attribute foreign of sinh [real return real] : function is "VHPIDIRECT sv_sinh";

    function cosh(x : real) return real is begin end function;
    attribute foreign of cosh [real return real] : function is "VHPIDIRECT sv_cosh";

    function tanh(x : real) return real is begin end function;
    attribute foreign of tanh [real return real] : function is "VHPIDIRECT sv_tanh";

    function asinh(x : real) return real is begin end function;
    attribute foreign of asinh [real return real] : function is "VHPIDIRECT sv_asinh";

    function acosh(x : real) return real is begin end function;
    attribute foreign of acosh [real return real] : function is "VHPIDIRECT sv_acosh";

    function atanh(x : real) return real is begin end function;
    attribute foreign of atanh [real return real] : function is "VHPIDIRECT sv_atanh";

    function pow(x, y : real) return real is begin end function;
    attribute foreign of pow [real, real return real] : function is "VHPIDIRECT sv_pow";

    function atan2(y, x : real) return real is begin end function;
    attribute foreign of atan2 [real, real return real] : function is "VHPIDIRECT sv_atan2";

    function hypot(x, y : real) return real is begin end function;
    attribute foreign of hypot [real, real return real] : function is "VHPIDIRECT sv_hypot";

    -- ============================================================
    -- Utility
    -- ============================================================

    function clog2(n : integer) return integer is begin end function;
    attribute foreign of clog2 [integer return integer] : function is "VHPIDIRECT sv_clog2";

    -- ============================================================
    -- Conversions
    -- ============================================================

    function itor(n : integer) return real is begin end function;
    attribute foreign of itor [integer return real] : function is "VHPIDIRECT sv_itor";

    function rtoi(x : real) return integer is begin end function;
    attribute foreign of rtoi [real return integer] : function is "VHPIDIRECT sv_rtoi";

    impure function sv_test_plusargs(prefix : string) return integer is begin end function;
    attribute foreign of sv_test_plusargs [string return integer] : function is "VHPIDIRECT sv_test_plusargs";

    impure function sv_value_plusargs(fmt : string) return integer is begin end function;
    attribute foreign of sv_value_plusargs [string return integer] : function is "VHPIDIRECT sv_value_plusargs";

    impure function sv_plusarg_real(fmt : string) return real is begin end function;
    attribute foreign of sv_plusarg_real [string return real] : function is "VHPIDIRECT sv_plusarg_real";

    -- C fills bits(1 to w), MSB first, with '0' '1' 'x' 'z'.
    procedure sv_plusarg_bits(fmt : in string; bits : out string) is begin end procedure;
    attribute foreign of sv_plusarg_bits [string, string] : procedure is "VHPIDIRECT sv_plusarg_bits";

    impure function sv_plusarg_vec(fmt : string; w : positive) return logic3d_vector is
        variable buf : string(1 to w) := (others => '0');
        variable r   : logic3d_vector(w - 1 downto 0);
    begin
        sv_plusarg_bits(fmt, buf);
        for i in 1 to w loop
            case buf(i) is
                when '1'    => r(w - i) := L3D_1;
                when 'x'    => r(w - i) := L3D_X;
                when 'z'    => r(w - i) := L3D_Z;
                when others => r(w - i) := L3D_0;
            end case;
        end loop;
        return r;
    end function;

    function l3d_to_string(v : logic3d_vector) return string is
        constant n : natural := (v'length + 7) / 8;
        alias    vv : logic3d_vector(v'length - 1 downto 0) is v;
        variable s : string(1 to n);
        variable k : natural := 0;
        variable c : natural;
    begin
        for j in n - 1 downto 0 loop           -- char j = bits j*8+7 .. j*8
            c := 0;
            for b in 7 downto 0 loop
                c := c * 2;
                if j * 8 + b < v'length then
                    c := c + (vv(j * 8 + b) mod 2);   -- value plane
                end if;
            end loop;
            if c /= 0 then
                k := k + 1;
                s(k) := character'val(c);
            end if;
        end loop;
        return s(1 to k);
    end function;

    -- ============================================================
    -- Random
    -- ============================================================
    --
    -- The IEEE 1364 generator (17.9.3) as vvp runs it (vpi/sys_random.c):
    -- rtl_dist_uniform(&seed, start, end) on a uint32 seed, so a testbench
    -- draws the numbers vvp (and VCS) draw.  An unseeded $random draws from
    -- one design-wide seed (17.9.1, "an internal seed"), $urandom and
    -- $urandom_range from a second (vvp's urandom() keeps its own); both
    -- start at 0, a 0 seed standing for 259341593.  $random(seed) and
    -- $urandom(seed) draw from the caller's seed variable.
    --
    -- Plain VHDL, not VHPIDIRECT: it needs no --load'ed library.  nvc's own
    -- Verilog route (nvc --std=2040 -a x.v ...) loads none, and cannot (with
    -- a VHPI plugin loaded, a unit that fell back to the native Verilog
    -- parser stops the run).  Bit-exact with the C:
    --   * the uint32 seed arithmetic is done in 16-bit halves, as INTEGER
    --     is 32 bits under --std=2040 (and 64 under 2019);
    --   * the doubles are computed in the C's operation order (every step
    --     is one IEEE double operation, rounded the same way);
    --   * C's truncating (int32_t) casts are done by hand (sv_trunc32):
    --     integer() rounds.  The one full-range draw outside int32 (newseed
    --     >> 9 = 2**23 - 1, r just over 2**31) is INT32_MIN, x86-64's
    --     cvttsd2si result that vvp returns;
    --   * a 32-bit unsigned result (2**31 added, mod 2**32) is computed in
    --     integer arithmetic (sv_flip32): numeric_std's "xor" on a vector
    --     can crash nvc after some thousands of calls in one process
    --     activation.
    -- A design-wide seed sits in a protected-type shared variable, as
    -- $timeformat's state does in sv_display_pkg.

    constant SV_INT32_MIN : integer := -2147483647 - 1;
    constant SV_INT32_MAX : integer := 2147483647;

    -- C's (int32_t) x on x86-64: truncation toward zero, and INT32_MIN
    -- (cvttsd2si's "integer indefinite") when that is outside int32.
    function sv_trunc32(x : real) return integer is
        variable k : integer;
    begin
        if x >= 2147483648.0 or x <= -2147483648.0 then
            return SV_INT32_MIN;
        elsif x >= 2147483647.0 then
            return SV_INT32_MAX;               -- integer() could overflow
        end if;
        k := integer(x);                       -- rounds to the nearest
        if x >= 0.0 and real(k) > x then
            k := k - 1;
        elsif x < 0.0 and real(k) < x then
            k := k + 1;
        end if;
        return k;
    end function;

    -- The 32 bits of x plus 2**31 (mod 2**32): x's sign bit flipped -- a
    -- uint32 <-> its int32 offset by INT32_MIN, as urandom() converts
    function sv_flip32(x : integer) return integer is
    begin
        if x >= 0 then
            return x - 2147483647 - 1;
        else
            return x + 2147483647 + 1;
        end if;
    end function;

    -- One step of uniform()'s generator on the uint32 seed hi * 65536 + lo:
    -- newseed = 69069 * oldseed + 1 (mod 2**32), 69069 = 65536 + 3533, a 0
    -- seed standing for 259341593.
    procedure sv_rng_step(hi, lo : inout natural) is
        variable p, nh, nl : natural;
    begin
        if hi = 0 and lo = 0 then
            hi := 259341593 / 65536;
            lo := 259341593 mod 65536;
        end if;
        p  := 3533 * lo + 1;
        nl := p mod 65536;
        nh := (lo + 3533 * hi + p / 65536) mod 65536;
        hi := nh;
        lo := nl;
    end procedure;

    -- rtl_dist_uniform(&seed, st, en) on the seed (hi, lo): a value in
    -- [st, en], the seed stepped once -- except when st >= en, which is st
    -- and leaves the seed alone.  uniform()'s bounds are rtl_dist_uniform's:
    -- [st, en + 1), or (st - 1, en] when en is INT32_MAX, or the full range.
    procedure sv_rng_uniform(hi, lo : inout natural; st, en : in integer;
                             i : out integer) is
        constant P23 : real := 0.00000011920928955078125;    -- 2**-23, d
        variable a, b, c, r : real;
        variable k : integer;
    begin
        if st >= en then
            i := st;
            return;
        end if;
        if en /= SV_INT32_MAX then
            a := real(st);
            b := real(en) + 1.0;                           -- end++
        elsif st /= SV_INT32_MIN then
            a := real(st) - 1.0;                           -- start--
            b := real(en);
        else
            a := real(st);
            b := real(en);
        end if;
        -- uniform(): c = 1.0 + (newseed >> 9) * 2**-23;  c = c + c * d;
        -- c = (b - a) * (c - 1.0) + a
        sv_rng_step(hi, lo);
        c := 1.0 + real(hi * 128 + lo / 512) * P23;
        c := c + c * P23;
        c := (b - a) * (c - 1.0) + a;
        if en /= SV_INT32_MAX then
            r := c;
        elsif st /= SV_INT32_MIN then
            r := c + 1.0;
        else
            r := (c + 2147483648.0) / 4294967295.0;
            r := r * 4294967296.0 - 2147483648.0;
        end if;
        -- i = r >= 0 ? (int32_t) r : (int32_t) (r - 1), r - 1 rounded first
        if r >= 0.0 then
            k := sv_trunc32(r);
        else
            k := sv_trunc32(r - 1.0);
        end if;
        if en /= SV_INT32_MAX then
            if k < st then k := st; end if;
            if k > en then k := en; end if;                -- i >= end + 1
        elsif st /= SV_INT32_MIN then
            if k <= st - 1 then k := st; end if;           -- i <= start - 1
            if k > en then k := en; end if;
        end if;
        i := k;
    end procedure;

    -- A seed variable as vvp's vpiIntVal reads it: its low 32 bits, a bit
    -- that is 1 (strong or weak) is 1, an x or z bit 0; the uint32 halves.
    procedure sv_seed_halves(seed : in logic3d_vector; hi, lo : out natural) is
        alias s : logic3d_vector(seed'length - 1 downto 0) is seed;
        variable h, l : natural := 0;
    begin
        for i in 31 downto 16 loop
            h := h * 2;
            if i < seed'length and (s(i) = L3D_1 or s(i) = L3D_H) then
                h := h + 1;
            end if;
        end loop;
        for i in 15 downto 0 loop
            l := l * 2;
            if i < seed'length and (s(i) = L3D_1 or s(i) = L3D_H) then
                l := l + 1;
            end if;
        end loop;
        hi := h;
        lo := l;
    end procedure;

    -- The uint32 hi * 65536 + lo as w logic3d bits, LSB-aligned; bits past
    -- 31 copy bit 31 (vvp's vpi_put_value of an integer sign-extends it into
    -- a wider variable).
    function sv_halves_to_l3d(hi, lo : natural; w : positive) return logic3d_vector is
        variable r : logic3d_vector(w - 1 downto 0);
        variable h : natural := hi;
        variable l : natural := lo;
        variable b : logic3d := L3D_0;
    begin
        for i in 0 to w - 1 loop
            if i < 16 then
                if l mod 2 = 1 then b := L3D_1; else b := L3D_0; end if;
                l := l / 2;
            elsif i < 32 then
                if h mod 2 = 1 then b := L3D_1; else b := L3D_0; end if;
                h := h / 2;
            end if;                                  -- past 31: bit 31 again
            r(i) := b;
        end loop;
        return r;
    end function;

    -- An int32 as 32 logic3d bits, two's complement
    function sv_int32_to_l3d(v : integer) return logic3d_vector is
    begin
        if v >= 0 then
            return sv_halves_to_l3d(v / 65536, v mod 65536, 32);
        else                                         -- v + 2**31 >= 0
            return sv_halves_to_l3d((v + 2147483647 + 1) / 65536 + 32768,
                                    (v + 2147483647 + 1) mod 65536, 32);
        end if;
    end function;

    -- $random(seed)'s full-range draw from a seed variable: the value (an
    -- int32) and the advanced seed's halves
    procedure sv_seeded_draw(seed : in logic3d_vector; hi, lo : out natural;
                             v : out integer) is
        variable h, l : natural;
        variable i : integer;
    begin
        sv_seed_halves(seed, h, l);
        sv_rng_uniform(h, l, SV_INT32_MIN, SV_INT32_MAX, i);
        hi := h;
        lo := l;
        v := i;
    end procedure;

    type t_sv_rng is protected
        procedure set_seed(s : integer);
        procedure set_halves(h, l : natural);
        impure function draw(st, en : integer) return integer;
    end protected;

    type t_sv_rng is protected body
        -- the uint32 seed, hi * 65536 + lo
        variable hi : natural range 0 to 65535 := 0;
        variable lo : natural range 0 to 65535 := 0;

        procedure set_seed(s : integer) is
            variable l : natural range 0 to 65535;
        begin
            l  := s mod 65536;                       -- the low 16 bits
            lo := l;
            hi := ((s - l) / 65536) mod 65536;       -- the next 16, two's complement
        end procedure;

        procedure set_halves(h, l : natural) is
        begin
            hi := h;
            lo := l;
        end procedure;

        -- rtl_dist_uniform(&seed, st, en) on this seed
        impure function draw(st, en : integer) return integer is
            variable h : natural := hi;
            variable l : natural := lo;
            variable i : integer;
        begin
            sv_rng_uniform(h, l, st, en, i);
            hi := h;
            lo := l;
            return i;
        end function;
    end protected body;

    shared variable sv_rng  : t_sv_rng;    -- $random's internal seed
    shared variable sv_urng : t_sv_rng;    -- $urandom's (vvp keeps a second)

    impure function random return integer is
    begin
        return sv_rng.draw(SV_INT32_MIN, SV_INT32_MAX);
    end function;

    procedure srandom(seed : in integer) is
    begin
        sv_rng.set_seed(seed);
    end procedure;

    -- urandom(0, UINT32_MAX, 0)
    impure function sv_urandom return integer is
    begin
        return sv_flip32(sv_urng.draw(SV_INT32_MIN, SV_INT32_MAX));
    end function;

    -- sys_urandom_range_calltf: urandom(0, max, min), the bounds swapped
    -- when min > max (unsigned: the order of the offset int32s)
    impure function sv_urandom_range(maxval, minval : integer) return integer is
        variable mx : integer := sv_flip32(maxval);
        variable mn : integer := sv_flip32(minval);
        variable t : integer;
    begin
        if mn > mx then
            t := mn;
            mn := mx;
            mx := t;
        end if;
        return sv_flip32(sv_urng.draw(mn, mx));
    end function;

    function sv_random_value(seed : logic3d_vector) return logic3d_vector is
        variable h, l : natural;
        variable v : integer;
    begin
        sv_seeded_draw(seed, h, l, v);
        return sv_int32_to_l3d(v);
    end function;

    function sv_urandom_value(seed : logic3d_vector) return logic3d_vector is
        variable h, l : natural;
        variable v : integer;
    begin
        sv_seeded_draw(seed, h, l, v);
        return sv_int32_to_l3d(sv_flip32(v));
    end function;

    function sv_random_next(seed : logic3d_vector) return logic3d_vector is
        variable h, l : natural;
        variable v : integer;
    begin
        sv_seeded_draw(seed, h, l, v);
        return sv_halves_to_l3d(h, l, seed'length);
    end function;

    -- urandom(&seed, ...) sets its generator to the caller's seed, draws,
    -- and hands the advanced seed back: the generator keeps it too
    procedure sv_urandom_seed(seed : in logic3d_vector) is
        variable h, l : natural;
        variable v : integer;
    begin
        sv_seeded_draw(seed, h, l, v);
        sv_urng.set_halves(h, l);
    end procedure;

    -- ============================================================
    -- Distribution procedures
    -- ============================================================

    procedure dist_uniform(
        seed : inout integer;
        start_val, end_val : in integer;
        result : out integer) is begin end procedure;
    attribute foreign of dist_uniform [integer, integer, integer, integer] :
        procedure is "VHPIDIRECT sv_dist_uniform";

    procedure dist_normal(
        seed : inout integer;
        mean, std_dev : in integer;
        result : out integer) is begin end procedure;
    attribute foreign of dist_normal [integer, integer, integer, integer] :
        procedure is "VHPIDIRECT sv_dist_normal";

    procedure dist_exponential(
        seed : inout integer;
        mean : in integer;
        result : out integer) is begin end procedure;
    attribute foreign of dist_exponential [integer, integer, integer] :
        procedure is "VHPIDIRECT sv_dist_exponential";

    procedure dist_poisson(
        seed : inout integer;
        mean : in integer;
        result : out integer) is begin end procedure;
    attribute foreign of dist_poisson [integer, integer, integer] :
        procedure is "VHPIDIRECT sv_dist_poisson";

    procedure dist_chi_square(
        seed : inout integer;
        df : in integer;
        result : out integer) is begin end procedure;
    attribute foreign of dist_chi_square [integer, integer, integer] :
        procedure is "VHPIDIRECT sv_dist_chi_square";

    procedure dist_t(
        seed : inout integer;
        df : in integer;
        result : out integer) is begin end procedure;
    attribute foreign of dist_t [integer, integer, integer] :
        procedure is "VHPIDIRECT sv_dist_t";

    procedure dist_erlang(
        seed : inout integer;
        k, mean : in integer;
        result : out integer) is begin end procedure;
    attribute foreign of dist_erlang [integer, integer, integer, integer] :
        procedure is "VHPIDIRECT sv_dist_erlang";

end package body sv_math_pkg;
