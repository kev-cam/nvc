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
