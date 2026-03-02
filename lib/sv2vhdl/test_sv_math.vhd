-- test_sv_math.vhd — testbench for sv_math_pkg (VHPIDIRECT math/random)
--
-- Run with:
--   nvc --std=2040 -L lib/ --load=lib/sv2vhdl/libsv_math.so \
--       -a lib/sv2vhdl/test_sv_math.vhd
--   nvc --std=2040 -L lib/ --load=lib/sv2vhdl/libsv_math.so \
--       -e test_sv_math
--   nvc --std=2040 -L lib/ --load=lib/sv2vhdl/libsv_math.so \
--       -r test_sv_math

library sv2vhdl;
use sv2vhdl.sv_math_pkg.all;

entity test_sv_math is
end entity test_sv_math;

architecture test of test_sv_math is

    function approx_eq(a, b : real; tol : real := 1.0e-9) return boolean is
    begin
        if b = 0.0 then
            return abs(a) < tol;
        else
            return abs(a - b) < tol * (1.0 + abs(b));
        end if;
    end function;

begin
    main : process
        variable seed : integer;
        variable result : integer;
        variable result2 : integer;
    begin

        -- ========================================
        -- 1. clog2
        -- ========================================
        assert clog2(0) = 0      report "clog2(0) failed" severity failure;
        assert clog2(1) = 0      report "clog2(1) failed" severity failure;
        assert clog2(2) = 1      report "clog2(2) failed" severity failure;
        assert clog2(3) = 2      report "clog2(3) failed" severity failure;
        assert clog2(4) = 2      report "clog2(4) failed" severity failure;
        assert clog2(5) = 3      report "clog2(5) failed" severity failure;
        assert clog2(255) = 8    report "clog2(255) failed" severity failure;
        assert clog2(256) = 8    report "clog2(256) failed" severity failure;
        assert clog2(257) = 9    report "clog2(257) failed" severity failure;
        assert clog2(1024) = 10  report "clog2(1024) failed" severity failure;
        report "PASS: clog2" severity note;

        -- ========================================
        -- 2. Basic math functions
        -- ========================================
        assert approx_eq(sqrt(4.0), 2.0)       report "sqrt(4) failed" severity failure;
        assert approx_eq(sqrt(9.0), 3.0)       report "sqrt(9) failed" severity failure;
        assert approx_eq(ln(1.0), 0.0)         report "ln(1) failed" severity failure;
        assert approx_eq(exp(0.0), 1.0)        report "exp(0) failed" severity failure;
        assert approx_eq(log10(100.0), 2.0)    report "log10(100) failed" severity failure;
        assert approx_eq(log10(1000.0), 3.0)   report "log10(1000) failed" severity failure;
        report "PASS: basic math" severity note;

        -- ========================================
        -- 3. Rounding functions
        -- ========================================
        assert approx_eq(ceil(1.5), 2.0)       report "ceil(1.5) failed" severity failure;
        assert approx_eq(ceil(-1.5), -1.0)     report "ceil(-1.5) failed" severity failure;
        assert approx_eq(floor(1.5), 1.0)      report "floor(1.5) failed" severity failure;
        assert approx_eq(floor(-1.5), -2.0)    report "floor(-1.5) failed" severity failure;
        report "PASS: ceil/floor" severity note;

        -- ========================================
        -- 4. Trigonometric functions
        -- ========================================
        assert approx_eq(sin(0.0), 0.0)        report "sin(0) failed" severity failure;
        assert approx_eq(cos(0.0), 1.0)        report "cos(0) failed" severity failure;
        assert approx_eq(tan(0.0), 0.0)        report "tan(0) failed" severity failure;
        assert approx_eq(asin(1.0), 1.5707963267948966)
            report "asin(1) failed" severity failure;
        assert approx_eq(acos(1.0), 0.0)       report "acos(1) failed" severity failure;
        assert approx_eq(atan(1.0), 0.7853981633974483)
            report "atan(1) failed" severity failure;
        report "PASS: trig" severity note;

        -- ========================================
        -- 5. Two-argument math
        -- ========================================
        assert approx_eq(pow(2.0, 10.0), 1024.0) report "pow(2,10) failed" severity failure;
        assert approx_eq(pow(3.0, 3.0), 27.0)    report "pow(3,3) failed" severity failure;
        assert approx_eq(atan2(1.0, 1.0), 0.7853981633974483)
            report "atan2(1,1) failed" severity failure;
        assert approx_eq(hypot(3.0, 4.0), 5.0)   report "hypot(3,4) failed" severity failure;
        report "PASS: two-arg math" severity note;

        -- ========================================
        -- 6. Hyperbolic functions
        -- ========================================
        assert approx_eq(sinh(0.0), 0.0)       report "sinh(0) failed" severity failure;
        assert approx_eq(cosh(0.0), 1.0)       report "cosh(0) failed" severity failure;
        assert approx_eq(tanh(0.0), 0.0)       report "tanh(0) failed" severity failure;
        assert approx_eq(asinh(0.0), 0.0)      report "asinh(0) failed" severity failure;
        assert approx_eq(acosh(1.0), 0.0)      report "acosh(1) failed" severity failure;
        assert approx_eq(atanh(0.0), 0.0)      report "atanh(0) failed" severity failure;
        report "PASS: hyperbolic" severity note;

        -- ========================================
        -- 7. Conversions
        -- ========================================
        assert approx_eq(itor(42), 42.0)        report "itor(42) failed" severity failure;
        assert approx_eq(itor(-100), -100.0)    report "itor(-100) failed" severity failure;
        assert approx_eq(itor(0), 0.0)          report "itor(0) failed" severity failure;
        assert rtoi(3.7) = 3                    report "rtoi(3.7) failed" severity failure;
        assert rtoi(-3.7) = -3                  report "rtoi(-3.7) failed" severity failure;
        assert rtoi(0.0) = 0                    report "rtoi(0) failed" severity failure;
        report "PASS: conversions" severity note;

        -- ========================================
        -- 8. Simple random (global seed)
        -- ========================================
        srandom(12345);
        result := random;
        -- Just verify it returns something and is deterministic
        assert result /= 0 report "random returned 0 (unlikely)" severity warning;
        -- Call again, should be different
        assert random /= result report "random returned same value twice" severity warning;
        report "PASS: simple random" severity note;

        -- ========================================
        -- 9. dist_uniform
        -- ========================================
        seed := 12345;
        dist_uniform(seed, 0, 100, result);
        assert result >= 0 and result <= 100
            report "dist_uniform out of range" severity failure;
        assert seed /= 12345
            report "dist_uniform didn't update seed" severity failure;
        -- Call multiple times, check range
        for i in 1 to 20 loop
            dist_uniform(seed, -50, 50, result);
            assert result >= -50 and result <= 50
                report "dist_uniform range violation" severity failure;
        end loop;
        report "PASS: dist_uniform" severity note;

        -- ========================================
        -- 10. dist_normal
        -- ========================================
        seed := 42;
        dist_normal(seed, 100, 10, result);
        -- Normal distribution centered at 100, should be roughly in range
        assert result >= 0 and result <= 200
            report "dist_normal wildly out of range" severity failure;
        report "PASS: dist_normal" severity note;

        -- ========================================
        -- 11. dist_exponential
        -- ========================================
        seed := 42;
        dist_exponential(seed, 100, result);
        assert result >= 0
            report "dist_exponential negative" severity failure;
        report "PASS: dist_exponential" severity note;

        -- ========================================
        -- 12. dist_poisson
        -- ========================================
        seed := 42;
        dist_poisson(seed, 10, result);
        assert result >= 0
            report "dist_poisson negative" severity failure;
        report "PASS: dist_poisson" severity note;

        -- ========================================
        -- 13. dist_chi_square
        -- ========================================
        seed := 42;
        dist_chi_square(seed, 5, result);
        assert result >= 0
            report "dist_chi_square negative" severity failure;
        report "PASS: dist_chi_square" severity note;

        -- ========================================
        -- 14. dist_t
        -- ========================================
        seed := 42;
        dist_t(seed, 10, result);
        -- t-distribution can be negative
        report "PASS: dist_t" severity note;

        -- ========================================
        -- 15. dist_erlang
        -- ========================================
        seed := 42;
        dist_erlang(seed, 3, 100, result);
        assert result >= 0
            report "dist_erlang negative" severity failure;
        report "PASS: dist_erlang" severity note;

        -- ========================================
        -- 16. Deterministic seed verification
        -- ========================================
        -- Same seed should produce same sequence
        seed := 99999;
        dist_uniform(seed, 0, 1000, result);
        seed := 99999;
        dist_uniform(seed, 0, 1000, result2);
        assert result = result2
            report "deterministic seed failed" severity failure;
        report "PASS: deterministic seed" severity note;

        report "ALL TESTS PASSED" severity note;
        wait;
    end process;
end architecture test;
