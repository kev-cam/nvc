/*
 * sv_math.c — C implementations of Verilog system functions for VHPIDIRECT
 *
 * Math functions are direct <math.h> wrappers.
 * Random/distribution algorithms from IEEE 1364-2005 Annex B
 * (via iverilog sys_random.c, Copyright (c) 2000-2022 Stephen Williams).
 *
 * Loaded by NVC via --load=libsv_math.so, called from sv_math_pkg.vhd.
 */

#include <math.h>
#include <stdint.h>
#include <limits.h>

/* ================================================================
 * Math functions — direct <math.h> wrappers
 * ================================================================ */

/* Single-argument: double -> double */
double sv_sqrt(double x)  { return sqrt(x); }
double sv_ln(double x)    { return log(x); }
double sv_log10(double x) { return log10(x); }
double sv_exp(double x)   { return exp(x); }
double sv_ceil(double x)  { return ceil(x); }
double sv_floor(double x) { return floor(x); }
double sv_sin(double x)   { return sin(x); }
double sv_cos(double x)   { return cos(x); }
double sv_tan(double x)   { return tan(x); }
double sv_asin(double x)  { return asin(x); }
double sv_acos(double x)  { return acos(x); }
double sv_atan(double x)  { return atan(x); }
double sv_sinh(double x)  { return sinh(x); }
double sv_cosh(double x)  { return cosh(x); }
double sv_tanh(double x)  { return tanh(x); }
double sv_asinh(double x) { return asinh(x); }
double sv_acosh(double x) { return acosh(x); }
double sv_atanh(double x) { return atanh(x); }

/* Double-argument: (double, double) -> double */
double sv_pow(double x, double y)   { return pow(x, y); }
double sv_atan2(double y, double x) { return atan2(y, x); }
double sv_hypot(double x, double y) { return hypot(x, y); }

/* ================================================================
 * $clog2 — ceiling log2
 * ================================================================ */

int32_t sv_clog2(int32_t n)
{
    uint32_t u = (uint32_t)n;
    int32_t r = 0;
    if (u == 0) return 0;
    u--;
    while (u > 0) { r++; u >>= 1; }
    return r;
}

/* ================================================================
 * Conversion functions
 * ================================================================ */

double  sv_itor(int32_t n) { return (double)n; }
int32_t sv_rtoi(double x)  { return (int32_t)x; }  /* truncate toward zero */

/* ================================================================
 * Random number generation — IEEE 1364-2005 Annex B algorithms
 * ================================================================ */

/*
 * Core LCG: seed = 69069 * seed + 1 (mod 2^32)
 * Returns double in [start, end)
 */
static double uniform(int32_t *seed, int32_t start, int32_t end)
{
    double d = 0.00000011920928955078125;  /* 2^(-23) */
    double a, b, c;
    uint32_t oldseed, newseed;

    oldseed = *seed;
    if (oldseed == 0)
        oldseed = 259341593;

    if (start >= end) {
        a = 0.0;
        b = 2147483647.0;
    } else {
        a = (double)start;
        b = (double)end;
    }

    newseed = 69069 * oldseed + 1;
    *seed = newseed;

    /* Convert seed bits to double in [1.0, 2.0) using top 23 bits */
    c = 1.0 + (newseed >> 9) * 0.00000011920928955078125;
    c = c + (c * d);
    c = ((b - a) * (c - 1.0)) + a;

    return c;
}

static double normal(int32_t *seed, int32_t mean, int32_t deviation)
{
    double v1, v2, s;

    s = 1.0;
    while ((s >= 1.0) || (s == 0.0)) {
        v1 = uniform(seed, -1, 1);
        v2 = uniform(seed, -1, 1);
        s = v1 * v1 + v2 * v2;
    }
    s = v1 * sqrt(-2.0 * log(s) / s);
    v1 = (double)deviation;
    v2 = (double)mean;

    return s * v1 + v2;
}

static double exponential(int32_t *seed, int32_t mean)
{
    double n;

    n = uniform(seed, 0, 1);
    if (n != 0.0) {
        n = -log(n) * mean;
    }

    return n;
}

static int32_t poisson(int32_t *seed, int32_t mean)
{
    int32_t n;
    double p, q;

    n = 0;
    q = -(double)mean;
    p = exp(q);
    q = uniform(seed, 0, 1);
    while (p < q) {
        n++;
        q = uniform(seed, 0, 1) * q;
    }

    return n;
}

static double chi_square(int32_t *seed, int32_t deg_of_free)
{
    double x;
    int32_t k;

    if (deg_of_free % 2) {
        x = normal(seed, 0, 1);
        x = x * x;
    } else {
        x = 0.0;
    }
    for (k = 2; k <= deg_of_free; k = k + 2) {
        x = x + 2 * exponential(seed, 1);
    }

    return x;
}

static double t_dist(int32_t *seed, int32_t deg_of_free)
{
    double x, chi2, dv, root;

    chi2 = chi_square(seed, deg_of_free);
    dv = chi2 / (double)deg_of_free;
    root = sqrt(dv);
    x = normal(seed, 0, 1) / root;

    return x;
}

static double erlangian(int32_t *seed, int32_t k, int32_t mean)
{
    double x, a, b;
    int32_t i;

    x = 1.0;
    for (i = 1; i <= k; i++) {
        x = x * uniform(seed, 0, 1);
    }
    a = (double)mean;
    b = (double)k;
    x = -a * log(x) / b;

    return x;
}

/* Round half-away-from-zero (IEEE 1364 convention) */
static int32_t round_half_away(double r)
{
    if (r >= 0)
        return (int32_t)(r + 0.5);
    else {
        r = -r;
        return -(int32_t)(r + 0.5);
    }
}

/* ================================================================
 * Public distribution wrappers
 *
 * For VHDL procedures with (seed : inout integer; ... ; result : out integer)
 * NVC VHPIDIRECT maps inout integer -> int32_t*, out integer -> int32_t*
 * ================================================================ */

static int32_t rtl_dist_uniform(int32_t *seed, int32_t start, int32_t end)
{
    double r;
    int32_t i;

    if (start >= end) return start;

    if (end != INT32_MAX) {
        end++;
        r = uniform(seed, start, end);
        if (r >= 0)
            i = (int32_t)r;
        else
            i = (int32_t)(r - 1);
        if (i < start) i = start;
        if (i >= end) i = end - 1;
    } else if (start != INT32_MIN) {
        start--;
        r = uniform(seed, start, end) + 1.0;
        if (r >= 0)
            i = (int32_t)r;
        else
            i = (int32_t)(r - 1);
        if (i <= start) i = start + 1;
        if (i > end) i = end;
    } else {
        r = (uniform(seed, start, end) + 2147483648.0) / 4294967295.0;
        r = r * 4294967296.0 - 2147483648.0;
        if (r >= 0)
            i = (int32_t)r;
        else
            i = (int32_t)(r - 1);
    }

    return i;
}

/* --- Simple random (global seed) --- */

static int32_t global_seed = 0;

/* procedure srandom(seed : in integer)
 * NVC VHPIDIRECT: in integer -> int32_t by value */
void sv_srandom(int32_t seed)
{
    global_seed = seed;
}

/* impure function random return integer
 * NVC VHPIDIRECT: no args, returns int32_t */
int32_t sv_random(void)
{
    return rtl_dist_uniform(&global_seed, INT32_MIN, INT32_MAX);
}

/* --- Distribution procedures ---
 * VHDL: procedure dist_X(seed : inout integer; ...; result : out integer)
 * C:    void sv_dist_X(int32_t *seed, ..., int32_t *result)
 */

void sv_dist_uniform(int32_t *seed, int32_t start, int32_t end, int32_t *result)
{
    *result = rtl_dist_uniform(seed, start, end);
}

void sv_dist_normal(int32_t *seed, int32_t mean, int32_t sd, int32_t *result)
{
    *result = round_half_away(normal(seed, mean, sd));
}

void sv_dist_exponential(int32_t *seed, int32_t mean, int32_t *result)
{
    if (mean > 0)
        *result = round_half_away(exponential(seed, mean));
    else
        *result = 0;
}

void sv_dist_poisson(int32_t *seed, int32_t mean, int32_t *result)
{
    if (mean > 0)
        *result = poisson(seed, mean);
    else
        *result = 0;
}

void sv_dist_chi_square(int32_t *seed, int32_t df, int32_t *result)
{
    if (df > 0)
        *result = round_half_away(chi_square(seed, df));
    else
        *result = 0;
}

void sv_dist_t(int32_t *seed, int32_t df, int32_t *result)
{
    if (df > 0)
        *result = round_half_away(t_dist(seed, df));
    else
        *result = 0;
}

void sv_dist_erlang(int32_t *seed, int32_t k, int32_t mean, int32_t *result)
{
    if (k > 0)
        *result = round_half_away(erlangian(seed, k, mean));
    else
        *result = 0;
}
