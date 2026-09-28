-- sv_math_pkg.vhd — Verilog system math/random functions for VHDL
--
-- Wraps C implementations in libsv_math.so via VHPIDIRECT.
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
    -- Random (simple — uses internal global seed)
    -- ============================================================

    impure function random return integer;
    procedure srandom(seed : in integer);

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

    impure function random return integer is begin end function;
    attribute foreign of random [return integer] : function is "VHPIDIRECT sv_random";

    procedure srandom(seed : in integer) is begin end procedure;
    attribute foreign of srandom [integer] : procedure is "VHPIDIRECT sv_srandom";

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
