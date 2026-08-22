//
//  Copyright (C) 2023-2024  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "util.h"
#include "ident.h"
#include "jit/jit-priv.h"
#include "jit/jit.h"
#include "option.h"
#include "rt/assert.h"
#include "jit/jit-exits.h"
#include "rt/rt.h"

#include <inttypes.h>
#include <stdio.h>
#include "thread.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef ARCH_ARM64
#define HAVE_NEON
#endif

#if defined HAVE_AVX2 || defined HAVE_SSE41
#include <x86intrin.h>
#endif

#ifdef HAVE_NEON
#include <arm_neon.h>
#endif

#if defined __GNUC__ && !defined __clang__
#pragma GCC optimize ("O2")
#endif

typedef enum {
   CPU_AVX2  = 0x1,
   CPU_SSE41 = 0x2,
   CPU_NEON  = 0x04,
} cpu_feature_t;

typedef struct {
   const char     *name;
   jit_entry_fn_t  entry;
   cpu_feature_t   feature;
   ident_t         ident;
} jit_intrinsic_t;

typedef enum {
   _U  = 0x0,
   _X  = 0x1,
   _0  = 0x2,
   _1  = 0x3,
   _Z  = 0x4,
   _W  = 0x5,
   _L  = 0x6,
   _H  = 0x7,
   _DC = 0x8
} std_ulogic_t;

#define IS_01(x) \
   _Generic((x),                                                        \
            uint64_t: (((x) & UINT64_C(0x0e0e0e0e0e0e0e0e))             \
                       == UINT64_C(0x0202020202020202)),                \
            uint8_t: ((x) & 0xe) == 0x02)

__attribute__((aligned(16)))
static const uint8_t cvt_to_x01[16] = {
   _X, _X, _0, _1, _X, _X, _0, _1, _X, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

static const uint8_t xor_table[16][16] = {
   // --------------------------------------------------
   // | U   X   0   1   Z   W   L   H   -          |   |
   // --------------------------------------------------
   {   _U, _U, _U, _U, _U, _U, _U, _U, _U   },  // | U |
   {   _U, _X, _X, _X, _X, _X, _X, _X, _X   },  // | X |
   {   _U, _X, _0, _1, _X, _X, _0, _1, _X   },  // | 0 |
   {   _U, _X, _1, _0, _X, _X, _1, _0, _X   },  // | 1 |
   {   _U, _X, _X, _X, _X, _X, _X, _X, _X   },  // | Z |
   {   _U, _X, _X, _X, _X, _X, _X, _X, _X   },  // | W |
   {   _U, _X, _0, _1, _X, _X, _0, _1, _X   },  // | L |
   {   _U, _X, _1, _0, _X, _X, _1, _0, _X   },  // | H |
   {   _U, _X, _X, _X, _X, _X, _X, _X, _X   },  // | - |
};

static const uint8_t and_table[16][16] = {
   // ---------------------------------------------------
   // |  U   X   0   1   Z   W   L   H   -          |   |
   // ---------------------------------------------------
   {    _U, _U, _0, _U, _U, _U, _0, _U, _U   },  // | U |
   {    _U, _X, _0, _X, _X, _X, _0, _X, _X   },  // | X |
   {    _0, _0, _0, _0, _0, _0, _0, _0, _0   },  // | 0 |
   {    _U, _X, _0, _1, _X, _X, _0, _1, _X   },  // | 1 |
   {    _U, _X, _0, _X, _X, _X, _0, _X, _X   },  // | Z |
   {    _U, _X, _0, _X, _X, _X, _0, _X, _X   },  // | W |
   {    _0, _0, _0, _0, _0, _0, _0, _0, _0   },  // | L |
   {    _U, _X, _0, _1, _X, _X, _0, _1, _X   },  // | H |
   {    _U, _X, _0, _X, _X, _X, _0, _X, _X   },  // | - |
};

static const uint8_t or_table[16][16] = {
   // ---------------------------------------------------
   // |  U   X   0   1   Z   W   L   H   -          |   |
   // ---------------------------------------------------
   {    _U, _U, _U, _1, _U, _U, _U, _1, _U   },  // | U |
   {    _U, _X, _X, _1, _X, _X, _X, _1, _X   },  // | X |
   {    _U, _X, _0, _1, _X, _X, _0, _1, _X   },  // | 0 |
   {    _1, _1, _1, _1, _1, _1, _1, _1, _1   },  // | 1 |
   {    _U, _X, _X, _1, _X, _X, _X, _1, _X   },  // | Z |
   {    _U, _X, _X, _1, _X, _X, _X, _1, _X   },  // | W |
   {    _U, _X, _0, _1, _X, _X, _0, _1, _X   },  // | L |
   {    _1, _1, _1, _1, _1, _1, _1, _1, _1   },  // | H |
   {    _U, _X, _X, _1, _X, _X, _X, _1, _X   },  // | - |
};

#if defined HAVE_SSE41
static const uint8_t not_table[1][16] = {
   // ---------------------------------------------------
   // |  U   X   0   1   Z   W   L   H   -          |   |
   {    _U, _X, _1, _0, _X, _X, _1, _0, _X   },
};
#endif

#if defined HAVE_SSE41 || defined HAVE_NEON

// Compressed lookup tables for vectorised intrinsics.  Note the
// vectorised intrinsics all rely on being able to read up to
// OVERRUN_MARGIN (defined in src/rt/mspace.c) bytes beyond the end of
// the input arrays.

__attribute__((aligned(16)))
static const uint8_t compress_left[16] = {
   _U,   _X,   _0,   _1,
   _X,   _X,   _0,   _1,
   _X,   0xff, 0xff, 0xff,
   0xff, 0xff, 0xff, 0xff,
};

__attribute__((aligned(16)))
static const uint8_t compress_right[16] = {
   _U << 2, _X << 2, _0 << 2, _1 << 2,
   _X << 2, _X << 2, _0 << 2, _1 << 2,
   _X << 2, 0xff,    0xff,    0xff,
   0xff,    0xff,    0xff,    0xff,
};

__attribute__((aligned(16)))
static const uint8_t small_or_table[4][4] = {
   // -----------------------------
   // |  U   X   0   1        |   |
   // -----------------------------
   {    _U, _U, _U, _1 },  // | U |
   {    _U, _X, _X, _1 },  // | X |
   {    _U, _X, _0, _1 },  // | 0 |
   {    _1, _1, _1, _1 },  // | 1 |
};

__attribute__((aligned(16)))
static const uint8_t small_and_table[4][4] = {
   // -----------------------------
   // |  U   X   0   1        |   |
   // -----------------------------
   {    _U, _U, _0, _U },  // | U |
   {    _U, _X, _0, _X },  // | X |
   {    _0, _0, _0, _0 },  // | 0 |
   {    _U, _X, _0, _1 },  // | 1 |
};

__attribute__((aligned(16)))
static const uint8_t small_xor_table[4][4] = {
   // -----------------------------
   // |  U   X   0   1        |   |
   // -----------------------------
   {    _U, _U, _U, _U },  // | U |
   {    _U, _X, _X, _X },  // | X |
   {    _U, _X, _0, _1 },  // | 0 |
   {    _U, _X, _1, _0 },  // | 1 |
};

__attribute__((aligned(16)))
static const uint8_t lane_iota[16] = {
   0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

static const uint32_t spread_nibble[16] = {
   0x02020202, 0x03020202, 0x02030202, 0x03030202,
   0x02020302, 0x03020302, 0x02030302, 0x03030302,
   0x02020203, 0x03020203, 0x02030203, 0x03030203,
   0x02020303, 0x03020303, 0x02030303, 0x03030303,
};

__attribute__((aligned(16)))
static const uint8_t reverse_lane[16] = {
   15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0
};

#ifdef HAVE_SSE41
__attribute__((aligned(16)))
static const uint8_t spread_shuffle[16] = {
   1, 1, 1, 1, 1, 1, 1, 1,
   0, 0, 0, 0, 0, 0, 0, 0
};

__attribute__((aligned(16)))
static const uint8_t spread_mask[16] = {
   0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01,
   0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01,
};
#endif   // HAVE_SSE41 (spread_shuffle / spread_mask)
#endif   // HAVE_SSE41 || HAVE_NEON (compressed lookup tables)

// TLAB overflow: route transient results that don't fit the 64K TLAB to the
// per-eval eval-arena (growable, reset each proc eval) -- the SAME arena the
// JIT's own aggregate allocator (__nvc_mspace_alloc) already uses. Previously
// this fell back to mspace_alloc (the collected heap), so the vector
// intrinsics were the dominant GC feeder (~25M allocs/run on VeeR hello);
// the arena keeps them off the collector entirely.
__attribute__((noinline, cold))
static void *__tlab_overflow(tlab_t *t, size_t size)
{
   jit_thread_local_t *thread = jit_thread_local();
   if (likely(thread->eval_arena != NULL))
      return eval_arena_alloc(thread->eval_arena, size);
   return mspace_alloc(t->mspace, size);
}

// Dispatched to the SSE4.1 or scalar packed-add at init (see jit_bind_intrinsic)
static void (*ieee_packed_add)(const uint8_t *, const uint8_t *,
                               int, int, uint8_t *);

__attribute__((always_inline))
static inline void *__tlab_alloc(tlab_t *t, size_t size, size_t align)
{
   assert(t->alloc <= t->limit);
   assert((t->alloc & (sizeof(double) - 1)) == 0);
   assert(align % sizeof(double) == 0);

   // Always allocate at least 16 bytes as the ieee_packed_add routines
   // will always write a full 128-bit vector
   const size_t alignup = ALIGN_UP(size, 16);
   const size_t base = ALIGN_UP(t->alloc, align);
   if (likely(base + alignup <= t->limit)) {
      t->alloc = base + alignup;
      return t->data + base;
   }
   else
      return __tlab_overflow(t, size);
}

__attribute__((always_inline))
static inline uint32_t __tlab_mark(tlab_t *t)
{
   return t->alloc;
}

__attribute__((always_inline))
static inline void __tlab_restore(tlab_t *t, uint32_t mark)
{
   assert(mark <= t->alloc);
   t->alloc = mark;
}

#if 0
static void print_bits(const char *tag, const uint8_t *bits, size_t size)
{
   static const char map[] = "UX01ZWLH-";

   printf("%s: ", tag);
   for (int i = 0; i < size; i++)
      printf("%c", map[bits[i]]);
   printf("\n");
}
#endif

#if 0
__attribute__((target("sse4.1")))
static void print_m128i(const char *tag, __m128i vec)
{
   uint8_t bytes[16];
   memcpy(bytes, &vec, sizeof(vec));

   printf("%s:", tag);
   for (int i = 0; i < 16; i++)
      printf(" %02x", bytes[i]);
   printf("\n");
}
#endif

__attribute__((cold, noinline))
static void ieee_msg_v(jit_func_t *func, jit_anchor_t *caller,
                       vhdl_severity_t severity, const char *fmt, va_list ap)
{
   jit_anchor_t frame = {
      .caller = caller,
      .func = func
   };

   jit_thread_local_t *thread = jit_thread_get();
   thread->anchor = &frame;

   diag_t *d = diag_new(get_diag_severity(severity), NULL);
   diag_vprintf(d, fmt, ap);
   diag_show_source(d, false);

   emit_vhdl_diag(d, severity);

   thread->anchor = NULL;
}

__attribute__((cold, noinline))
static void __ieee_msg(jit_func_t *func, jit_anchor_t *caller,
                       vhdl_severity_t severity, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);

   ieee_msg_v(func, caller, severity, fmt, ap);

   va_end(ap);
}

__attribute__((cold, noinline))
static void ieee_warn(jit_func_t *func, jit_anchor_t *caller,
                      const char *fmt, ...)
{
   if (opt_get_int(OPT_IEEE_WARNINGS) != IEEE_WARNINGS_ON)
      return;

   va_list ap;
   va_start(ap, fmt);

   ieee_msg_v(func, caller, SEVERITY_WARNING, fmt, ap);

   va_end(ap);
}

__attribute__((cold, noreturn, noinline))
static void fail_in_interpreter(jit_func_t *func, jit_anchor_t *caller,
                                jit_scalar_t *args, tlab_t *tlab)
{
   // Synopsys subprograms handle edge cases poorly: punt to the
   // interpreter to generate the error message
   func->entry = jit_interp;
   jit_interp(func, caller, args, tlab);
   fatal_trace("should not return here");
}

__attribute__((always_inline))
static inline void __invert_bits(const uint8_t *input, int size,
                                 uint8_t *result)
{
   for (int i = 0; i < size; i++) {
      assert(input[i] == _0 || input[i] == _1);
      result[i] = input[i] ^ 1;
   }
}

#ifdef HAVE_SSE41
__attribute__((target("sse4.1")))
static void std_to_x01_sse41(jit_func_t *func, jit_anchor_t *anchor,
                             jit_scalar_t *args, tlab_t *tlab)
{
   const int size = args[3].integer ^ (args[3].integer >> 63);
   const uint8_t *input = args[1].pointer;

   uint8_t *result = __tlab_alloc(tlab, ALIGN_UP(size, 16), 16);

   __m128i lookup = _mm_load_si128((const __m128i *)cvt_to_x01);

   for (int pos = 0; pos < size; pos += 16) {
      __m128i in = _mm_loadu_si128((const __m128i *)(input + pos));
      __m128i out = _mm_shuffle_epi8(lookup, in);
      _mm_store_si128((__m128i *)(result + pos), out);
   }

   args[0].pointer = result;
   args[1].integer = size - 1;
   args[2].integer = ~size;
}
#endif

static void std_to_x01(jit_func_t *func, jit_anchor_t *anchor,
                       jit_scalar_t *args, tlab_t *tlab)
{
   const int size = args[3].integer ^ (args[3].integer >> 63);
   const uint8_t *input = args[1].pointer;

   uint8_t *result = __tlab_alloc(tlab, size, 8);

   for (int i = 0; i < size; i++)
      result[i] = cvt_to_x01[input[i]];

   args[0].pointer = result;
   args[1].integer = size - 1;
   args[2].integer = ~size;
}

__attribute__((always_inline))
static inline bool __all_01(const void *vec, int size)
{
   int pos = 0;
   for (; pos + 7 < size; pos += 8) {
      const uint64_t u64 = unaligned_load(vec + pos, uint64_t);
      if (!IS_01(u64))
         return false;
   }

   for (; pos < size; pos++) {
      const uint8_t u8 = *(const uint8_t *)(vec + pos);
      if (!IS_01(u8))
         return false;
   }

   return true;
}

__attribute__((cold, noinline))
static uint8_t *ieee_to_01_slow(tlab_t *tlab, const uint8_t *input,
                                int size, uint8_t xmap)
{
   uint8_t *result = __tlab_alloc(tlab, size, 8);

   bool bad = false;
   for (int i = 0; i < size; i++) {
      const uint8_t elt = input[i];
      if (elt == _1 || elt == _H)
         result[i] = _1;
      else if (elt == _0 || elt == _L)
         result[i] = _0;
      else
         bad = true;
   }

   if (bad)
      memset(result, xmap, size);

   return result;
}

__attribute__((always_inline))
static inline uint8_t *__to_01(tlab_t *tlab, const uint8_t *input,
                               int size, uint8_t xmap)
{
   assert(size > 0);

   if (__all_01(input, size))
      return (uint8_t *)input;
   else
      return ieee_to_01_slow(tlab, input, size, xmap);
}

static void ieee_to_01(jit_func_t *func, jit_anchor_t *anchor,
                       jit_scalar_t *args, tlab_t *tlab)
{
   const int size = args[3].integer ^ (args[3].integer >> 63);
   const uint8_t *input = args[1].pointer;
   const uint8_t xmap = args[4].integer;

   if (size == 0) {
      ieee_warn(func, anchor,
                "NUMERIC_STD.TO_01: null detected, returning NAU");

      args[0].pointer = NULL;
      args[1].integer = 0;
      args[2].integer = -1;
   }
   else {
      args[0].pointer = __to_01(tlab, input, size, xmap);
      args[1].integer = size - 1;
      args[2].integer = ~size;
   }
}

__attribute__((always_inline))
static inline uint8_t *__resize_unsigned(tlab_t *tlab, const void *input,
                                         int size, int newsize)
{
   if (newsize < 1)
      return NULL;
   else if (size >= newsize)
      return (uint8_t *)input + size - newsize;
   else {
      uint8_t *result = __tlab_alloc(tlab, newsize, 8);

      const int pad = newsize - size;
      memset(result, _0, pad);
      memcpy(result + pad, input, size);

      return result;
   }
}

__attribute__((always_inline))
static inline uint8_t *__resize_signed(tlab_t *tlab, const void *input,
                                       int size, int newsize)
{
   if (newsize < 1)
      return NULL;
   else if (size == newsize)
      return (uint8_t *)input;
   else if (size == 0) {
      uint8_t *result = __tlab_alloc(tlab, newsize, 8);
      memset(result, _0, newsize);
      return result;
   }
   else {
      const int bound = MIN(size, newsize) - 1;
      assert(bound >= 0);

      uint8_t *result = __tlab_alloc(tlab, newsize, 8);
      memset(result, *(uint8_t *)input, newsize - bound);
      memcpy(result + newsize - bound, input + size - bound, bound);

      return result;
   }
}

static void ieee_resize_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                                 jit_scalar_t *args, tlab_t *tlab)
{
   const int size = args[3].integer ^ (args[3].integer >> 63);
   const int newsize = args[4].integer;
   const uint8_t *input = args[1].pointer;

   args[0].pointer = __resize_unsigned(tlab, input, size, newsize);
   args[1].integer = newsize - 1;
   args[2].integer = ~newsize;
}

static void ieee_resize_signed(jit_func_t *func, jit_anchor_t *anchor,
                               jit_scalar_t *args, tlab_t *tlab)
{
   const int size = args[3].integer ^ (args[3].integer >> 63);
   const int newsize = args[4].integer;
   const uint8_t *input = args[1].pointer;

   args[0].pointer = __resize_signed(tlab, input, size, newsize);
   args[1].integer = newsize - 1;
   args[2].integer = ~newsize;
}

__attribute__((always_inline))
static inline uint8_t __pack_low_bits(const void* vec)
{
   uint64_t bits = unaligned_load(vec, uint64_t);
   bits &= UINT64_C(0x0101010101010101);
   bits *= UINT64_C(0x8040201008040201);
   return bits >> 56;
}

__attribute__((always_inline))
static inline void __spread_bits_8(void *vec, uint8_t packed)
{
   unaligned_store(vec + 0, spread_nibble[(packed >> 4) & 0xf], uint32_t);
   unaligned_store(vec + 4, spread_nibble[(packed >> 0) & 0xf], uint32_t);
}

// Pack a byte-per-bit array (post-TO_01, so every byte is _0/_1) into the low
// `size` bits of a uint64_t, MSB first -- the inverse of __spread_bits. Lets a
// multiply whose full product fits in 64 bits be done as ONE native multiply
// instead of a shift-and-add loop over the byte array. Ported from upstream
// nvc 1.22 (our base is 1.18), reusing our existing pack/spread primitives
// rather than upstream's spread_nibble table.
static inline uint64_t __pack_to_u64(const uint8_t *arr, int size)
{
   uint64_t val = 0;
   int i = 0;
   for (; i + 8 <= size; i += 8)
      val = (val << 8) | __pack_low_bits(arr + i);
   for (; i < size; i++)
      val = (val << 1) | (arr[i] & 1);
   return val;
}

__attribute__((always_inline))
static inline void __unpack_from_u64(uint64_t val, uint8_t *result, int size)
{
   int pos = size;
   while (pos >= 8) {
      pos -= 8;
      __spread_bits_8(result + pos, val & 0xff);
      val >>= 8;
   }
   for (int i = pos - 1; i >= 0; i--) {
      result[i] = (val & 1) | 0x02;   // _0 / _1
      val >>= 1;
   }
}

__attribute__((always_inline))
static inline void __ieee_packed_add_scalar(const uint8_t *left,
                                            const uint8_t *right, int size,
                                            int carry, uint8_t *result)
{
   int pos = size - 8;
   for (; pos > 0; pos -= 8) {
      const unsigned lbyte = __pack_low_bits(left + pos);
      const unsigned rbyte = __pack_low_bits(right + pos);
      const unsigned sum = lbyte + rbyte + carry;

      __spread_bits_8(result + pos, sum);
      carry = !!(sum & 0x100);
   }

   for (; pos + 8 > 0; pos--) {
      const unsigned lbit = left[pos + 7] & 1;
      const unsigned rbit = right[pos + 7] & 1;
      const unsigned sum = lbit + rbit + carry;
      result[pos + 7] = (sum & 1) | 0x02;
      carry = !!(sum & 2);
   }
}

#ifdef HAVE_SSE41
__attribute__((target("sse4.1"), always_inline))
static inline __m128i __spread_bits_16_sse41_vec(uint16_t packed)
{
   const __m128i shuffle = _mm_load_si128((const __m128i *)spread_shuffle);
   const __m128i mask = _mm_load_si128((const __m128i *)spread_mask);
   const __m128i ones = _mm_set1_epi8(1);
   const __m128i twos = _mm_set1_epi8(2);

   __m128i bits = _mm_set1_epi16(packed);
   bits = _mm_shuffle_epi8(bits, shuffle);
   bits = _mm_and_si128(bits, mask);
   bits = _mm_cmpeq_epi8(bits, mask);
   bits = _mm_and_si128(bits, ones);
   bits = _mm_or_si128(bits, twos);

   return bits;
}

__attribute__((target("sse4.1"), always_inline))
static inline void __ieee_packed_add_sse41(const uint8_t *left,
                                           const uint8_t *right,
                                           int size, int carry,
                                           uint8_t *result)
{
   const __m128i reverse = _mm_load_si128((const __m128i *)reverse_lane);
   const __m128i ones = _mm_set1_epi8(1);

   const int prefix = size & 15;
   for (int pos = size - 16; pos >= prefix; pos -= 16) {
      __m128i lvec = _mm_loadu_si128((const __m128i *)(left + pos));
      __m128i rvec = _mm_loadu_si128((const __m128i *)(right + pos));

      lvec = _mm_shuffle_epi8(lvec, reverse);
      rvec = _mm_shuffle_epi8(rvec, reverse);

      lvec = _mm_and_si128(lvec, ones);
      rvec = _mm_and_si128(rvec, ones);

      lvec = _mm_slli_epi16(lvec, 7);
      rvec = _mm_slli_epi16(rvec, 7);

      const unsigned lbits = _mm_movemask_epi8(lvec);
      const unsigned rbits = _mm_movemask_epi8(rvec);
      const unsigned sum = lbits + rbits + carry;

      __m128i spread = __spread_bits_16_sse41_vec(sum);
      _mm_storeu_si128((__m128i *)(result + pos), spread);
      carry = !!(sum & 0x10000);
   }

   if (prefix > 0) {
      __m128i iota = _mm_load_si128((const __m128i *)lane_iota);
      __m128i mask = _mm_cmplt_epi8(iota, _mm_set1_epi8(prefix));
      __m128i lvec = _mm_loadu_si128((const __m128i *)left);
      __m128i rvec = _mm_loadu_si128((const __m128i *)right);

      lvec = _mm_shuffle_epi8(lvec, reverse);
      rvec = _mm_shuffle_epi8(rvec, reverse);

      lvec = _mm_and_si128(lvec, ones);
      rvec = _mm_and_si128(rvec, ones);

      lvec = _mm_slli_epi16(lvec, 7);
      rvec = _mm_slli_epi16(rvec, 7);

      const unsigned lbits = _mm_movemask_epi8(lvec) >> (16 - prefix);
      const unsigned rbits = _mm_movemask_epi8(rvec) >> (16 - prefix);
      const unsigned sum = lbits + rbits + carry;

      __m128i tail = __spread_bits_16_sse41_vec(sum << (16 - prefix));
      __m128i prev = _mm_loadu_si128((const __m128i *)result);
      __m128i out = _mm_blendv_epi8(prev, tail, mask);

      _mm_storeu_si128((__m128i *)result, out);
   }
}
#endif

__attribute__((always_inline))
static inline uint8_t *__to_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                                     tlab_t *tlab, int64_t arg, int size)
{
   const int roundup = (size + 7) & ~7;
   uint8_t *result = __tlab_alloc(tlab, roundup, 8);

   int last = 0;
   for (int pos = roundup - 8; pos >= 0; pos -= 8, arg >>= 8)
      __spread_bits_8(result + pos, (last = (arg & 0xff)));

   const uint8_t spill_mask = ~((1 << (8 - roundup + size)) - 1);
   if (unlikely(arg != 0 || (last & spill_mask)))
      ieee_warn(func, anchor, "NUMERIC_STD.TO_UNSIGNED: vector truncated");

   return result + roundup - size;
}

__attribute__((noinline))
static void ieee_plus_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                               jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   const int size = MAX(lsize, rsize);

   if (lsize == 0 || rsize == 0) {
      args[0].pointer = NULL;
      args[1].integer = 0;
      args[2].integer = -1;
   }
   else {
      left = __resize_unsigned(tlab, left, lsize, size);
      right = __resize_unsigned(tlab, right, rsize, size);

      left = __to_01(tlab, left, size, _X);
      right = __to_01(tlab, right, size, _X);

      if (left[0] == _X)
         args[0].pointer = left;
      else if (right[0] == _X)
         args[0].pointer = right;
      else {
         uint8_t *result = __tlab_alloc(tlab, size, 8);
         (*ieee_packed_add)(left, right, size, 0, result);
         args[0].pointer = result;
      }

      args[1].integer = size - 1;
      args[2].integer = ~size;
   }
}

__attribute__((noinline))
static void ieee_plus_unsigned_natural(jit_func_t *func, jit_anchor_t *anchor,
                                       jit_scalar_t *args, tlab_t *tlab)
{
   const int size = ffi_array_length(args[3].integer);
   uint8_t *left = args[1].pointer;
   const uint64_t right = args[4].integer;

   // Must be unconditional to generate warning on truncation
   uint8_t *result = __to_unsigned(func, anchor, tlab, right, size);

   if (size == 0) {
      args[0].pointer = NULL;
      args[1].integer = 0;
      args[2].integer = -1;
   }
   else {
      left = __to_01(tlab, left, size, _X);

      if (left[0] == _X || right == 0)
         args[0].pointer = left;
      else {
         (*ieee_packed_add)(left, result, size, 0, result);
         args[0].pointer = result;
      }

      args[1].integer = size - 1;
      args[2].integer = ~size;
   }
}

__attribute__((always_inline))
static inline void __commute_vector_scalar(jit_scalar_t *args)
{
   jit_scalar_t tmp = args[1];
   args[1] = args[2];
   args[2] = args[3];
   args[3] = args[4];
   args[4] = tmp;
}

static void ieee_plus_natural_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                                       jit_scalar_t *args, tlab_t *tlab)
{
   __commute_vector_scalar(args);
   ieee_plus_unsigned_natural(func, anchor, args, tlab);
}

static void ieee_plus_signed(jit_func_t *func, jit_anchor_t *anchor,
                             jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   const int size = MAX(lsize, rsize);

   if (lsize == 0 || rsize == 0) {
      args[0].pointer = NULL;
      args[1].integer = 0;
      args[2].integer = -1;
   }
   else {
      left = __resize_signed(tlab, left, lsize, size);
      right = __resize_signed(tlab, right, rsize, size);

      left = __to_01(tlab, left, size, _X);
      right = __to_01(tlab, right, size, _X);

      if (left[0] == _X)
         args[0].pointer = left;
      else if (right[0] == _X)
         args[0].pointer = right;
      else {
         uint8_t *result = __tlab_alloc(tlab, size, 8);
         (*ieee_packed_add)(left, right, size, 0, result);
         args[0].pointer = result;
      }

      args[1].integer = size - 1;
      args[2].integer = ~size;
   }
}

static void ieee_minus_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                                jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   const int size = MAX(lsize, rsize);

   if (lsize == 0 || rsize == 0) {
      args[0].pointer = NULL;
      args[1].integer = 0;
      args[2].integer = -1;
   }
   else {
      left = __resize_unsigned(tlab, left, lsize, size);
      right = __resize_unsigned(tlab, right, rsize, size);

      left = __to_01(tlab, left, size, _X);
      right = __to_01(tlab, right, size, _X);

      if (left[0] == _X)
         args[0].pointer = left;
      else if (right[0] == _X)
         args[0].pointer = right;
      else {
         uint8_t *result = __tlab_alloc(tlab, size, 8);
         __invert_bits(right, size, result);
         (*ieee_packed_add)(left, result, size, 1, result);
         args[0].pointer = result;
      }

      args[1].integer = size - 1;
      args[2].integer = ~size;
   }
}

static void ieee_minus_signed(jit_func_t *func, jit_anchor_t *anchor,
                              jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   const int size = MAX(lsize, rsize);

   if (lsize == 0 || rsize == 0) {
      args[0].pointer = NULL;
      args[1].integer = 0;
      args[2].integer = -1;
   }
   else {
      left = __resize_signed(tlab, left, lsize, size);
      right = __resize_signed(tlab, right, rsize, size);

      left = __to_01(tlab, left, size, _X);
      right = __to_01(tlab, right, size, _X);

      if (left[0] == _X)
         args[0].pointer = left;
      else if (right[0] == _X)
         args[0].pointer = right;
      else {
         uint8_t *result = __tlab_alloc(tlab, size, 8);
         __invert_bits(right, size, result);
         (*ieee_packed_add)(left, result, size, 1, result);
         args[0].pointer = result;
      }

      args[1].integer = size - 1;
      args[2].integer = ~size;
   }
}

static void ieee_mul_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                              jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   const int size = lsize + rsize;

   if (lsize == 0 || rsize == 0) {
      args[0].pointer = NULL;
      args[1].integer = 0;
      args[2].integer = -1;
   }
   else {
      uint8_t *result = __tlab_alloc(tlab, size, 8);
      const uint32_t mark = __tlab_mark(tlab);

      left = __to_01(tlab, left, lsize, _X);
      right = __to_01(tlab, right, rsize, _X);

      if (left[0] == _X || right[0] == _X)
         memset(result, _X, size);
      else if (size <= 64) {
         // Full product fits in 64 bits (size == lsize + rsize), so one native
         // multiply is exact -- no shift-and-add over the byte array.
         const uint64_t lval = __pack_to_u64(left, lsize);
         const uint64_t rval = __pack_to_u64(right, rsize);
         __unpack_from_u64(lval * rval, result, size);
      }
      else {
         memset(result, _0, size);

         uint8_t *adval = __resize_unsigned(tlab, right, rsize, size);
         for (int i = lsize - 1, shift = 0; i >= 0; i--, shift++) {
            if (left[i] == _1) {
               // Delay left-shift until value needed
               memmove(adval, adval + shift, size - shift);
               memset(adval + size - shift, _0, shift);
               shift = 0;

               ieee_packed_add(result, adval, size, 0, result);
            }
         }
      }

      __tlab_restore(tlab, mark);

      args[0].pointer = result;
      args[1].integer = size - 1;
      args[2].integer = ~size;
   }
}

static void ieee_mul_signed(jit_func_t *func, jit_anchor_t *anchor,
                            jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   const int size = lsize + rsize;

   if (lsize == 0 || rsize == 0) {
      args[0].pointer = NULL;
      args[1].integer = 0;
      args[2].integer = -1;
   }
   else {
      uint8_t *result = __tlab_alloc(tlab, size, 8);
      const uint32_t mark = __tlab_mark(tlab);

      left = __to_01(tlab, left, lsize, _X);
      right = __to_01(tlab, right, rsize, _X);

      if (left[0] == _X || right[0] == _X)
         memset(result, _X, size);
      else if (size <= 64) {
         // As the unsigned case, but sign-extend each operand to 64 bits first
         // so the native multiply sees the same value the byte array encodes.
         uint64_t lval = __pack_to_u64(left, lsize);
         uint64_t rval = __pack_to_u64(right, rsize);
         if (lsize < 64 && (left[0] & 1))
            lval |= ~UINT64_C(0) << lsize;
         if (rsize < 64 && (right[0] & 1))
            rval |= ~UINT64_C(0) << rsize;
         __unpack_from_u64((int64_t)lval * (int64_t)rval, result, size);
      }
      else {
         memset(result, _0, size);

         uint8_t *adval = __resize_signed(tlab, right, rsize, size);
         for (int i = lsize - 1, shift = 0; i >= 0; i--, shift++) {
            if (left[i] == _1) {
               // Delay left-shift until value needed
               memmove(adval, adval + shift, size - shift);
               memset(adval + size - shift, _0, shift);
               shift = 0;

               if (i == 0)
                  __invert_bits(adval, size, adval);

               (*ieee_packed_add)(result, adval, size, i == 0, result);
            }
         }
      }

      __tlab_restore(tlab, mark);

      args[0].pointer = result;
      args[1].integer = size - 1;
      args[2].integer = ~size;
   }
}

static void ieee_divmod(jit_func_t *func, jit_anchor_t *anchor,
                        jit_scalar_t *args, tlab_t *tlab)
{
   uint8_t *num = args[2].pointer;
   const int num_size = ffi_array_length(args[4].integer);
   uint8_t *denom = args[5].pointer;
   const int denom_size = ffi_array_length(args[7].integer);
   uint8_t *xquot = args[8].pointer;
   uint8_t *xremain = args[11].pointer;

   assert(ffi_array_length(args[10].integer) == num_size);
   assert(ffi_array_length(args[13].integer) == denom_size);

   const uint32_t mark = __tlab_mark(tlab);

   uint8_t *temp = __tlab_alloc(tlab, num_size + 1, 16);
   temp[0] = _0;
   memcpy(temp + 1, num, num_size);

   const int quot_size = MAX(num_size, denom_size);
   uint8_t *quot = __tlab_alloc(tlab, quot_size, 16);
   memset(quot, _0, quot_size);

   int topbit = -1;
   for (int j = denom_size - 1; j >= 0; j--) {
      if (denom[denom_size - 1 - j] == _1) {
         topbit = j;
         break;
      }
   }

   if (unlikely(topbit < 0))
      __ieee_msg(func, anchor, SEVERITY_ERROR,
                 "NUMERIC_STD.DIVMOD: DIV, MOD, or REM by zero");
   else {
      uint8_t *denom2 = __tlab_alloc(tlab, topbit + 2, 16);
      denom2[0] = _0;
      memcpy(denom2 + 1, denom + denom_size - 1 - topbit , topbit + 1);

      uint8_t *denom3 = __tlab_alloc(tlab, topbit + 2, 16);
      __invert_bits(denom2, topbit + 2, denom3);

      for (int j = num_size - (topbit + 1); j >= 0; j--) {
         uint8_t *slice = temp + num_size - (topbit + j + 1);

         int pos = 0;
         for (; pos < topbit + 1 && slice[pos] == denom2[pos]; pos++);

         if (slice[pos] >= denom2[pos]) {
            (*ieee_packed_add)(slice, denom3, topbit + 2, 1, slice);
            quot[quot_size - 1 - j] = _1;
         }
      }
   }

   memcpy(xquot, quot + quot_size - num_size, num_size);

   if (denom_size > num_size + 1) {
      memset(xremain, _0, denom_size - (num_size + 1));
      memcpy(xremain + denom_size - (num_size + 1), temp, num_size + 1);
   }
   else
      memcpy(xremain, temp + num_size + 1 - denom_size, denom_size);

   __tlab_restore(tlab, mark);
}

static bool ieee_unsigned_cmp(jit_func_t *func, jit_anchor_t *anchor,
                              jit_scalar_t *args, tlab_t *tlab,
                              uint8_t *lbyte, uint8_t *rbyte, const char *op)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   args[0].integer = 0;   // Return FALSE by default

   if (lsize < 1 || rsize < 1) {
      ieee_warn(func, anchor, "NUMERIC_STD.\"%s\": null argument detected, "
                "returning FALSE", op);
      return false;
   }

   const uint32_t mark = __tlab_mark(tlab);

   left = __to_01(tlab, left, lsize, _X);
   right = __to_01(tlab, right, rsize, _X);

   if (left[0] == _X || right[0] == _X) {
      ieee_warn(func, anchor, "NUMERIC_STD.\"%s\": metavalue detected, "
                "returning FALSE", op);
      __tlab_restore(tlab, mark);
      return false;
   }

   const int size = MAX(lsize, rsize);
   left = __resize_unsigned(tlab, left, lsize, size);
   right = __resize_unsigned(tlab, right, rsize, size);

   int pos = 0;
   for (; pos < size - 1 && left[pos] == right[pos]; pos++);

   *lbyte = left[pos];
   *rbyte = right[pos];

   __tlab_restore(tlab, mark);
   return true;
}

static void ieee_less_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                               jit_scalar_t *args, tlab_t *tlab)
{
   uint8_t left, right;
   if (!ieee_unsigned_cmp(func, anchor, args, tlab, &left, &right, "<"))
      return;

   args[0].integer = left < right;
}

static void ieee_greater_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                                  jit_scalar_t *args, tlab_t *tlab)
{
   uint8_t left, right;
   if (!ieee_unsigned_cmp(func, anchor, args, tlab, &left, &right, ">"))
      return;

   args[0].integer = left > right;
}

static void ieee_geq_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                              jit_scalar_t *args, tlab_t *tlab)
{
   uint8_t left, right;
   if (!ieee_unsigned_cmp(func, anchor, args, tlab, &left, &right, ">="))
      return;

   args[0].integer = left >= right;
}

static void ieee_leq_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                              jit_scalar_t *args, tlab_t *tlab)
{
   uint8_t left, right;
   if (!ieee_unsigned_cmp(func, anchor, args, tlab, &left, &right, "<="))
      return;

   args[0].integer = left <= right;
}

static void ieee_to_integer_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                                     jit_scalar_t *args, tlab_t *tlab)
{
   const int size = ffi_array_length(args[3].integer);
   uint8_t *arg = args[1].pointer;

   if (size < 1) {
      ieee_warn(func, anchor, "NUMERIC_STD.TO_INTEGER: "
                "null detected, returning 0");
      args[0].integer = 0;
      return;
   }

   const uint32_t mark = __tlab_mark(tlab);

   arg = __to_01(tlab, arg, size, _X);

   if (arg[0] == _X) {
      ieee_warn(func, anchor, "NUMERIC_STD.TO_INTEGER: "
                "metavalue detected, returning 0");
      __tlab_restore(tlab, mark);
      args[0].integer = 0;
      return;
   }

   const uint64_t val = __pack_to_u64(arg, size);

   if (size > 31) {
      // Determine if the result overflows in which case we re-execute
      // in the interpreter to generate the correct error

      if (size > 63) {
         for (int i = 0; i < size - 63; i++) {
            if (arg[i] != _0)
               fail_in_interpreter(func, anchor, args, tlab);
         }
      }

      if ((val & UINT64_C(0xffffffff80000000)) && standard() < STD_19)
         fail_in_interpreter(func, anchor, args, tlab);
   }

   args[0].integer = val;
   __tlab_restore(tlab, mark);
}

static void ieee_to_integer_signed(jit_func_t *func, jit_anchor_t *anchor,
                                   jit_scalar_t *args, tlab_t *tlab)
{
   const int size = ffi_array_length(args[3].integer);
   uint8_t *arg = args[1].pointer;
   const int intsize = standard() < STD_19 ? 32 : 64;

   if (size < 1) {
      ieee_warn(func, anchor, "NUMERIC_STD.TO_INTEGER: "
                "null detected, returning 0");
      args[0].integer = 0;
      return;
   }

   const uint32_t mark = __tlab_mark(tlab);

   arg = __to_01(tlab, arg, size, _X);

   if (arg[0] == _X) {
      ieee_warn(func, anchor, "NUMERIC_STD.TO_INTEGER: "
                "metavalue detected, returning 0");
      __tlab_restore(tlab, mark);
      args[0].integer = 0;
      return;
   }

   if (size > intsize) {
      // Determine if the result overflows in which case we re-execute
      // in the interpreter to generate the correct error
      for (int i = 1; i <= size - intsize; i++) {
         if (arg[i] != arg[0])
            fail_in_interpreter(func, anchor, args, tlab);
      }
   }

   int64_t result;
   if (arg[0] == _0)
      result = (int64_t)__pack_to_u64(arg, size);
   else {
      uint8_t *mag = __tlab_alloc(tlab, size, 16);
      __invert_bits(arg, size, mag);
      result = -(int64_t)__pack_to_u64(mag, size) - 1;
   }

   args[0].integer = result;
   __tlab_restore(tlab, mark);
}

#ifdef HAVE_SSE41
__attribute__((target("sse4.1")))
static void ieee_and_vector_sse41(jit_func_t *func, jit_anchor_t *anchor,
                                  jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   if (unlikely(lsize != rsize))
      __ieee_msg(func, anchor, SEVERITY_FAILURE,
                 "STD_LOGIC_1164.\"and\": arguments of overloaded 'and' "
                 "operator are not of the same length");
   else {
      uint8_t *result = __tlab_alloc(tlab, ALIGN_UP(lsize, 16), 16);

      __m128i left_tbl  = _mm_load_si128((const __m128i *)compress_left);
      __m128i right_tbl = _mm_load_si128((const __m128i *)compress_right);
      __m128i and_tbl   = _mm_load_si128((const __m128i *)small_and_table);

      for (int pos = 0; pos < lsize; pos += 16) {
         __m128i left1  = _mm_loadu_si128((const __m128i *)(left + pos));
         __m128i right1 = _mm_loadu_si128((const __m128i *)(right + pos));
         __m128i left2  = _mm_shuffle_epi8(left_tbl, left1);
         __m128i right2 = _mm_shuffle_epi8(right_tbl, right1);
         __m128i comb   = _mm_or_si128(left2, right2);
         __m128i and    = _mm_shuffle_epi8(and_tbl, comb);
         _mm_store_si128((__m128i *)(result + pos), and);
      }

      args[0].pointer = result;
      args[1].integer = 1;
      args[2].integer = lsize;
   }
}
#endif

#ifdef HAVE_NEON
static void ieee_and_vector_neon(jit_func_t *func, jit_anchor_t *anchor,
                                 jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   if (unlikely(lsize != rsize))
      __ieee_msg(func, anchor, SEVERITY_FAILURE,
                 "STD_LOGIC_1164.\"and\": arguments of overloaded 'and' "
                 "operator are not of the same length");
   else {
      uint8_t *result = __tlab_alloc(tlab, ALIGN_UP(lsize, 16), 16);

      uint8x16_t left_tbl  = vld1q_u8(compress_left);
      uint8x16_t right_tbl = vld1q_u8(compress_right);
      uint8x16_t and_tbl   = vld1q_u8((const uint8_t *)small_and_table);

      for (int pos = 0; pos < lsize; pos += 16) {
         uint8x16_t left1  = vld1q_u8(left + pos);
         uint8x16_t right1 = vld1q_u8(right + pos);
         uint8x16_t left2  = vqtbl1q_u8(left_tbl, left1);
         uint8x16_t right2 = vqtbl1q_u8(right_tbl, right1);
         uint8x16_t comb   = vorrq_u8(left2, right2);
         uint8x16_t and    = vqtbl1q_u8(and_tbl, comb);
         vst1q_u8(result + pos, and);
      }

      args[0].pointer = result;
      args[1].integer = 1;
      args[2].integer = lsize;
   }
}
#endif

static void ieee_and_vector(jit_func_t *func, jit_anchor_t *anchor,
                            jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   if (unlikely(lsize != rsize))
      __ieee_msg(func, anchor, SEVERITY_FAILURE,
                 "STD_LOGIC_1164.\"and\": arguments of overloaded 'and' "
                 "operator are not of the same length");
   else {
      uint8_t *result = __tlab_alloc(tlab, lsize, 8);

      for (int pos = 0; pos < lsize; pos++)
         result[pos] = and_table[left[pos]][right[pos]];

      args[0].pointer = result;
      args[1].integer = 1;
      args[2].integer = lsize;
   }
}

#ifdef HAVE_SSE41
__attribute__((target("sse4.1")))
static void ieee_or_vector_sse41(jit_func_t *func, jit_anchor_t *anchor,
                                 jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   if (unlikely(lsize != rsize))
      __ieee_msg(func, anchor, SEVERITY_FAILURE,
                 "STD_LOGIC_1164.\"or\": arguments of overloaded 'or' "
                 "operator are not of the same length");
   else {
      uint8_t *result = __tlab_alloc(tlab, ALIGN_UP(lsize, 16), 16);

      __m128i left_tbl  = _mm_load_si128((const __m128i *)compress_left);
      __m128i right_tbl = _mm_load_si128((const __m128i *)compress_right);
      __m128i or_tbl    = _mm_load_si128((const __m128i *)small_or_table);

      for (int pos = 0; pos < lsize; pos += 16) {
         __m128i left1  = _mm_loadu_si128((const __m128i *)(left + pos));
         __m128i right1 = _mm_loadu_si128((const __m128i *)(right + pos));
         __m128i left2  = _mm_shuffle_epi8(left_tbl, left1);
         __m128i right2 = _mm_shuffle_epi8(right_tbl, right1);
         __m128i comb   = _mm_or_si128(left2, right2);
         __m128i orr    = _mm_shuffle_epi8(or_tbl, comb);
         _mm_store_si128((__m128i *)(result + pos), orr);
      }

      args[0].pointer = result;
      args[1].integer = 1;
      args[2].integer = lsize;
   }
}
#endif

#ifdef HAVE_NEON
static void ieee_or_vector_neon(jit_func_t *func, jit_anchor_t *anchor,
                                jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   if (unlikely(lsize != rsize))
      __ieee_msg(func, anchor, SEVERITY_FAILURE,
                 "STD_LOGIC_1164.\"or\": arguments of overloaded 'or' "
                 "operator are not of the same length");
   else {
      uint8_t *result = __tlab_alloc(tlab, ALIGN_UP(lsize, 16), 16);

      uint8x16_t left_tbl  = vld1q_u8(compress_left);
      uint8x16_t right_tbl = vld1q_u8(compress_right);
      uint8x16_t or_tbl    = vld1q_u8((const uint8_t *)small_or_table);

      for (int pos = 0; pos < lsize; pos += 16) {
         uint8x16_t left1  = vld1q_u8(left + pos);
         uint8x16_t right1 = vld1q_u8(right + pos);
         uint8x16_t left2  = vqtbl1q_u8(left_tbl, left1);
         uint8x16_t right2 = vqtbl1q_u8(right_tbl, right1);
         uint8x16_t comb   = vorrq_u8(left2, right2);
         uint8x16_t orr    = vqtbl1q_u8(or_tbl, comb);
         vst1q_u8(result + pos, orr);
      }

      args[0].pointer = result;
      args[1].integer = 1;
      args[2].integer = lsize;
   }
}
#endif

static void ieee_or_vector(jit_func_t *func, jit_anchor_t *anchor,
                                jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   if (unlikely(lsize != rsize))
      __ieee_msg(func, anchor, SEVERITY_FAILURE,
                 "STD_LOGIC_1164.\"or\": arguments of overloaded 'or' "
                 "operator are not of the same length");
   else {
      uint8_t *result = __tlab_alloc(tlab, ALIGN_UP(lsize, 16), 16);

      for (int pos = 0; pos < lsize; pos++)
         result[pos] = or_table[left[pos]][right[pos]];

      args[0].pointer = result;
      args[1].integer = 1;
      args[2].integer = lsize;
   }
}

#ifdef HAVE_SSE41
__attribute__((target("sse4.1")))
static void ieee_xor_vector_sse41(jit_func_t *func, jit_anchor_t *anchor,
                                  jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   if (unlikely(lsize != rsize))
      __ieee_msg(func, anchor, SEVERITY_FAILURE,
                 "STD_LOGIC_1164.\"xor\": arguments of overloaded 'xor' "
                 "operator are not of the same length");
   else {
      uint8_t *result = __tlab_alloc(tlab, ALIGN_UP(lsize, 16), 16);

      __m128i left_tbl  = _mm_load_si128((const __m128i *)compress_left);
      __m128i right_tbl = _mm_load_si128((const __m128i *)compress_right);
      __m128i xor_tbl   = _mm_load_si128((const __m128i *)small_xor_table);

      for (int pos = 0; pos < lsize; pos += 16) {
         __m128i left1  = _mm_loadu_si128((const __m128i *)(left + pos));
         __m128i right1 = _mm_loadu_si128((const __m128i *)(right + pos));
         __m128i left2  = _mm_shuffle_epi8(left_tbl, left1);
         __m128i right2 = _mm_shuffle_epi8(right_tbl, right1);
         __m128i comb   = _mm_or_si128(left2, right2);
         __m128i xor    = _mm_shuffle_epi8(xor_tbl, comb);
         _mm_store_si128((__m128i *)(result + pos), xor);
      }

      args[0].pointer = result;
      args[1].integer = 1;
      args[2].integer = lsize;
   }
}
#endif

#ifdef HAVE_NEON
static void ieee_xor_vector_neon(jit_func_t *func, jit_anchor_t *anchor,
                                 jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   if (unlikely(lsize != rsize))
      __ieee_msg(func, anchor, SEVERITY_FAILURE,
                 "STD_LOGIC_1164.\"xor\": arguments of overloaded 'xor' "
                 "operator are not of the same length");
   else {
      uint8_t *result = __tlab_alloc(tlab, ALIGN_UP(lsize, 16), 16);

      uint8x16_t left_tbl  = vld1q_u8(compress_left);
      uint8x16_t right_tbl = vld1q_u8(compress_right);
      uint8x16_t xor_tbl   = vld1q_u8((const uint8_t *)small_xor_table);

      for (int pos = 0; pos < lsize; pos += 16) {
         uint8x16_t left1  = vld1q_u8(left + pos);
         uint8x16_t right1 = vld1q_u8(right + pos);
         uint8x16_t left2  = vqtbl1q_u8(left_tbl, left1);
         uint8x16_t right2 = vqtbl1q_u8(right_tbl, right1);
         uint8x16_t comb   = vorrq_u8(left2, right2);
         uint8x16_t xor    = vqtbl1q_u8(xor_tbl, comb);
         vst1q_u8(result + pos, xor);
      }

      args[0].pointer = result;
      args[1].integer = 1;
      args[2].integer = lsize;
   }
}
#endif

static void std_xor_vector(jit_func_t *func, jit_anchor_t *anchor,
                           jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   uint8_t *left = args[1].pointer;
   uint8_t *right = args[4].pointer;

   if (unlikely(lsize != rsize))
      __ieee_msg(func, anchor, SEVERITY_FAILURE,
                 "STD_LOGIC_1164.\"xor\": arguments of overloaded 'xor' "
                 "operator are not of the same length");
   else {
      uint8_t *result = __tlab_alloc(tlab, lsize, 8);

      for (int pos = 0; pos < lsize; pos++)
         result[pos] = xor_table[left[pos]][right[pos]];

      args[0].pointer = result;
      args[1].integer = 1;
      args[2].integer = lsize;
   }
}

#ifdef HAVE_SSE41
__attribute__((target("sse4.1")))
static void ieee_not_vector_sse41(jit_func_t *func, jit_anchor_t *anchor,
                                  jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   uint8_t *left = args[1].pointer;

   uint8_t *result = __tlab_alloc(tlab, ALIGN_UP(lsize, 16), 16);

   __m128i not_tbl = _mm_load_si128((const __m128i *)not_table);

   for (int pos = 0; pos < lsize; pos += 16) {
      __m128i input  = _mm_loadu_si128((const __m128i *)(left + pos));
      __m128i output = _mm_shuffle_epi8(not_tbl, input);
      _mm_store_si128((__m128i *)(result + pos), output);
   }

   args[0].pointer = result;
   args[1].integer = 1;
   args[2].integer = lsize;
}
#endif

static void ieee_to_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                             jit_scalar_t *args, tlab_t *tlab)
{
   const int64_t size = args[2].integer;
   uint64_t arg = args[1].integer;

   if (size < 1) {
      args[0].pointer = NULL;
      args[1].integer = 0;
      args[2].integer = -1;
   }
   else {
      args[0].pointer = __to_unsigned(func, anchor, tlab, arg, size);
      args[1].integer = size - 1;
      args[2].integer = ~size;
   }
}

static void ieee_to_signed(jit_func_t *func, jit_anchor_t *anchor,
                           jit_scalar_t *args, tlab_t *tlab)
{
   const int64_t size = args[2].integer;
   int64_t arg = args[1].integer;

   if (size < 1) {
      args[0].pointer = NULL;
      args[1].integer = 0;
      args[2].integer = -1;
   }
   else {
      const int roundup = (size + 7) & ~7;
      uint8_t *result = __tlab_alloc(tlab, roundup, 8), last = 0;
      for (int pos = roundup - 8; pos >= 0; pos -= 8, arg >>= 8)
         __spread_bits_8(result + pos, (last = (arg & 0xff)));

      const uint8_t spill_mask = ~((1 << (8 - roundup + size)) - 1);
      if (unlikely((arg != 0 && arg != -1)
                   || (arg == 0 && result[roundup - size] == _1)
                   || (arg == -1 && result[roundup - size] == _0)
                   || (arg == 0 && (last & spill_mask) != 0)
                   || (arg == -1 && (last & spill_mask) != spill_mask)))
         ieee_warn(func, anchor, "NUMERIC_STD.TO_SIGNED: vector truncated");

      args[0].pointer = result + roundup - size;
      args[1].integer = size - 1;
      args[2].integer = ~size;
   }
}

__attribute__((always_inline))
static inline bool __is_x(uint8_t arg)
{
   return arg == _U || arg == _X || arg == _Z || arg == _W || arg == _DC;
}

__attribute__((always_inline))
static inline void __make_binary(jit_func_t *func, jit_anchor_t *anchor,
                                 tlab_t *tlab, const uint8_t *input,
                                 uint8_t *result, int size)
{
   static const uint8_t tbl_BINARY[] = { _X, _X, _0, _1, _X, _X, _0, _1, _X };

   for (int i = 0; i < size; i++) {
      if (unlikely(__is_x(input[i]))) {
         ieee_warn(func, anchor, "There is an 'U'|'X'|'W'|'Z'|'-' in an "
                     "arithmetic operand, the result will be 'X'(es).");
         memset(result, _X, size);
         break;
      }

      result[i] = tbl_BINARY[input[i]];
   }
}

__attribute__((always_inline))
static inline const uint8_t *__conv_unsigned(jit_func_t *func,
                                             jit_anchor_t *anchor,
                                             tlab_t *tlab,
                                             const void *input,
                                             int oldsize, int size)
{
   assert(size > 0);

   if (oldsize == size && __all_01(input, oldsize))
      return input;

   const int maxsize = MAX(oldsize, size);
   const int pad = size - oldsize;
   uint8_t *result = __tlab_alloc(tlab, maxsize, 8);

   __make_binary(func, anchor, tlab, input, result + pad, oldsize);

   if (unlikely(result[pad] == _X))
      memset(result, _X, size);
   else
      memset(result, _0, pad);

   return result;
}

__attribute__((always_inline))
static inline const uint8_t *__conv_signed(jit_func_t *func,
                                           jit_anchor_t *anchor,
                                           tlab_t *tlab,
                                           const void *input,
                                           int oldsize, int size)
{
   assert(size > 0);

   if (oldsize == size && __all_01(input, oldsize))
      return input;

   const int maxsize = MAX(oldsize, size);
   const int pad = size - oldsize;
   uint8_t *result = __tlab_alloc(tlab, maxsize, 8);

   __make_binary(func, anchor, tlab, input, result + pad, oldsize);

   if (unlikely(result[pad] == _X))
      memset(result, _X, size);
   else
      memset(result, *(uint8_t *)input, pad);

   return result;
}

static void synopsys_plus_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                                   jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   const uint8_t *left = args[1].pointer;
   const uint8_t *right = args[4].pointer;

   const int length = MAX(lsize, rsize);
   uint8_t *result = __tlab_alloc(tlab, length, 8);

   if (unlikely(length == 0))
      fail_in_interpreter(func, anchor, args, tlab);

   left = __conv_unsigned(func, anchor, tlab, left, lsize, length);
   right = __conv_unsigned(func, anchor, tlab, right, rsize, length);

   if (unlikely(left[0] == _X || right[0] == _X))
      memset(result, _X, length);
   else
      (*ieee_packed_add)(left, right, length, 0, result);

   args[0].pointer = result;
   args[1].integer = length - 1;
   args[2].integer = ~length;
}

static void synopsys_plus_signed(jit_func_t *func, jit_anchor_t *anchor,
                                 jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   const uint8_t *left = args[1].pointer;
   const uint8_t *right = args[4].pointer;

   const int length = MAX(lsize, rsize);
   uint8_t *result = __tlab_alloc(tlab, length, 8);

   if (unlikely(length == 0))
      fail_in_interpreter(func, anchor, args, tlab);

   left = __conv_signed(func, anchor, tlab, left, lsize, length);
   right = __conv_signed(func, anchor, tlab, right, rsize, length);

   if (unlikely(left[0] == _X || right[0] == _X))
      memset(result, _X, length);
   else
      (*ieee_packed_add)(left, right, length, 0, result);

   args[0].pointer = result;
   args[1].integer = length - 1;
   args[2].integer = ~length;
}

static void synopsys_plus_unsigned_logic(jit_func_t *func, jit_anchor_t *anchor,
                                         jit_scalar_t *args, tlab_t *tlab)
{
   uint8_t right[] = { args[4].integer };
   args[4].pointer = right;
   args[5].integer = 0;
   args[6].integer = 1;

   synopsys_plus_unsigned(func, anchor, args, tlab);
}

static void synopsys_plus_logic_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                                         jit_scalar_t *args, tlab_t *tlab)
{
   uint8_t left[] = { args[1].integer };
   args[6].integer = args[4].integer;
   args[5].integer = args[3].integer;
   args[4].pointer = args[2].pointer;
   args[1].pointer = left;
   args[2].integer = 0;
   args[3].integer = 1;

   synopsys_plus_unsigned(func, anchor, args, tlab);
}

static void synopsys_minus_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                                    jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   const uint8_t *left = args[1].pointer;
   const uint8_t *right = args[4].pointer;

   const int length = MAX(lsize, rsize);
   uint8_t *result = __tlab_alloc(tlab, length, 8);

   if (unlikely(length == 0))
      fail_in_interpreter(func, anchor, args, tlab);

   left = __conv_unsigned(func, anchor, tlab, left, lsize, length);
   right = __conv_unsigned(func, anchor, tlab, right, rsize, length);

   if (unlikely(left[0] == _X || right[0] == _X))
      memset(result, _X, length);
   else {
      __invert_bits(right, length, result);
      (*ieee_packed_add)(left, result, length, 1, result);
   }

   args[0].pointer = result;
   args[1].integer = length - 1;
   args[2].integer = ~length;
}

static void synopsys_mul_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                                  jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   const uint8_t *left = args[1].pointer;
   const uint8_t *right = args[4].pointer;

   if (unlikely(lsize == 0 || rsize == 0))
      fail_in_interpreter(func, anchor, args, tlab);

   left = __conv_unsigned(func, anchor, tlab, left, lsize, lsize);
   right = __conv_unsigned(func, anchor, tlab, right, rsize, rsize);

   const int length = lsize + rsize;
   uint8_t *pa = __tlab_alloc(tlab, length, 8);

   if (unlikely(left[0] == _X || right[0] == _X))
      memset(pa, _X, length);
   else {
      const uint32_t mark = __tlab_mark(tlab);
      uint8_t *ba = __tlab_alloc(tlab, length, 8);

      memset(pa, _0, length);
      memset(ba, _0, length - rsize);
      memcpy(ba + length - rsize, right, rsize);

      for (int i = lsize - 1, shift = 0; i >= 0; i--, shift++) {
         if (left[i] == _1) {
            // Delay left-shift until value needed
            memmove(ba, ba + shift, length - shift);
            memset(ba + length - shift, _0, shift);
            shift = 0;

            (*ieee_packed_add)(pa, ba, length, 0, pa);
         }
      }

      __tlab_restore(tlab, mark);
   }

   args[0].pointer = pa;
   args[1].integer = length - 1;
   args[2].integer = ~length;
}

static void synopsys_mul_signed(jit_func_t *func, jit_anchor_t *anchor,
                                jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   const uint8_t *left = args[1].pointer;
   const uint8_t *right = args[4].pointer;

   if (unlikely(lsize == 0 || rsize == 0))
      fail_in_interpreter(func, anchor, args, tlab);

   left = __conv_signed(func, anchor, tlab, left, lsize, lsize);
   right = __conv_signed(func, anchor, tlab, right, rsize, rsize);

   const int length = lsize + rsize;
   uint8_t *pa = __tlab_alloc(tlab, length, 8);

   if (unlikely(left[0] == _X || right[0] == _X))
      memset(pa, _X, length);
   else {
      const uint32_t mark = __tlab_mark(tlab);
      uint8_t *ba = __tlab_alloc(tlab, length, 8);

      memset(pa, _0, length);
      memset(ba, right[0], length - rsize);
      memcpy(ba + length - rsize, right, rsize);

      for (int i = lsize - 1, shift = 0; i >= 0; i--, shift++) {
         if (left[i] == _1) {
            // Delay left-shift until value needed
            memmove(ba, ba + shift, length - shift);
            memset(ba + length - shift, _0, shift);
            shift = 0;

            if (i == 0)
               __invert_bits(ba, length, ba);

            (*ieee_packed_add)(pa, ba, length, i == 0, pa);
         }
      }

      __tlab_restore(tlab, mark);
   }

   args[0].pointer = pa;
   args[1].integer = length - 1;
   args[2].integer = ~length;
}

static void synopsys_eql_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                                  jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   const uint8_t *left = args[1].pointer;
   const uint8_t *right = args[4].pointer;

   const int length = MAX(lsize, rsize);

   if (unlikely(length == 0))
      fail_in_interpreter(func, anchor, args, tlab);

   const uint32_t mark = __tlab_mark(tlab);

   left = __conv_unsigned(func, anchor, tlab, left, lsize, length);
   right = __conv_unsigned(func, anchor, tlab, right, rsize, length);

   args[0].integer = (memcmp(left, right, length) == 0);

   __tlab_restore(tlab, mark);
}

static void synopsys_eql_signed(jit_func_t *func, jit_anchor_t *anchor,
                                jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[3].integer);
   const int rsize = ffi_array_length(args[6].integer);
   const uint8_t *left = args[1].pointer;
   const uint8_t *right = args[4].pointer;

   const int length = MAX(lsize, rsize);

   if (unlikely(length == 0))
      fail_in_interpreter(func, anchor, args, tlab);

   const uint32_t mark = __tlab_mark(tlab);

   left = __conv_signed(func, anchor, tlab, left, lsize, length);
   right = __conv_signed(func, anchor, tlab, right, rsize, length);

   args[0].integer = (memcmp(left, right, length) == 0);

   __tlab_restore(tlab, mark);
}

#ifdef HAVE_SSE41
__attribute__((target("sse4.1")))
static void byte_vector_equal_sse41(jit_func_t *func, jit_anchor_t *anchor,
                                    jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[2].integer);
   const int rsize = ffi_array_length(args[5].integer);
   uint8_t *left = args[0].pointer;
   uint8_t *right = args[3].pointer;

   args[0].integer = 0;

   if (lsize != rsize)
      return;

   __m128i allmask = _mm_set1_epi8(0xff);

   int pos = 0;
   for (; pos + 15 < lsize; pos += 16) {
      __m128i left1  = _mm_loadu_si128((const __m128i *)(left + pos));
      __m128i right1 = _mm_loadu_si128((const __m128i *)(right + pos));
      __m128i xor    = _mm_xor_si128(left1, right1);
      if (!_mm_test_all_zeros(xor, allmask))
         return;
   }

   if (pos < lsize) {
      __m128i iota   = _mm_load_si128((const __m128i *)lane_iota);
      __m128i mask   = _mm_cmplt_epi8(iota, _mm_set1_epi8(lsize - pos));
      __m128i left1  = _mm_loadu_si128((const __m128i *)(left + pos));
      __m128i right1 = _mm_loadu_si128((const __m128i *)(right + pos));
      __m128i xor    = _mm_xor_si128(left1, right1);
      if (!_mm_test_all_zeros(xor, mask))
         return;
   }

   args[0].integer = 1;
}
#endif

#ifdef HAVE_NEON
static void byte_vector_equal_neon(jit_func_t *func, jit_anchor_t *anchor,
                                   jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[2].integer);
   const int rsize = ffi_array_length(args[5].integer);
   uint8_t *left = args[0].pointer;
   uint8_t *right = args[3].pointer;

   args[0].integer = 0;

   if (lsize != rsize)
      return;

   int pos = 0;
   for (; pos + 15 < lsize; pos += 16) {
      uint8x16_t left1  = vld1q_u8(left + pos);
      uint8x16_t right1 = vld1q_u8(right + pos);
      uint8x16_t xor    = veorq_u8(left1, right1);
      uint64x2_t cast   = vreinterpretq_u64_u8(xor);
      if (vgetq_lane_u64(cast, 0) || vgetq_lane_u64(cast, 1))
         return;
   }

   if (pos < lsize) {
      uint8x16_t iota   = vld1q_u8(lane_iota);
      uint8x16_t mask   = vcltq_u8(iota, vdupq_n_u8(lsize - pos));
      uint8x16_t left1  = vld1q_u8(left + pos);
      uint8x16_t right1 = vld1q_u8(right + pos);
      uint8x16_t xor    = veorq_u8(left1, right1);
      uint8x16_t and    = vandq_u8(xor, mask);
      uint64x2_t cast   = vreinterpretq_u64_u8(and);
      if (vgetq_lane_u64(cast, 0) || vgetq_lane_u64(cast, 1))
         return;
   }

   args[0].integer = 1;
}
#endif

static void byte_vector_equal(jit_func_t *func, jit_anchor_t *anchor,
                              jit_scalar_t *args, tlab_t *tlab)
{
   const int lsize = ffi_array_length(args[2].integer);
   const int rsize = ffi_array_length(args[5].integer);
   uint8_t *left = args[0].pointer;
   uint8_t *right = args[3].pointer;

   args[0].integer = (lsize == rsize) && (memcmp(left, right, lsize) == 0);
}

static void ieee_math_sin(jit_func_t *func, jit_anchor_t *anchor,
                          jit_scalar_t *args, tlab_t *tlab)
{
   args[0].real = sin(args[1].real);
}

static void ieee_math_cos(jit_func_t *func, jit_anchor_t *anchor,
                          jit_scalar_t *args, tlab_t *tlab)
{
   args[0].real = cos(args[1].real);
}

static void ieee_math_log(jit_func_t *func, jit_anchor_t *anchor,
                          jit_scalar_t *args, tlab_t *tlab)
{
   args[0].real = log(args[1].real);
}

static void ieee_math_log2(jit_func_t *func, jit_anchor_t *anchor,
                          jit_scalar_t *args, tlab_t *tlab)
{
   args[0].real = log2(args[1].real);
}

static void ieee_math_log10(jit_func_t *func, jit_anchor_t *anchor,
                            jit_scalar_t *args, tlab_t *tlab)
{
   args[0].real = log10(args[1].real);
}

static void ieee_math_round(jit_func_t *func, jit_anchor_t *anchor,
                            jit_scalar_t *args, tlab_t *tlab)
{
   args[0].real = round(args[1].real);
}

static void ieee_math_trunc(jit_func_t *func, jit_anchor_t *anchor,
                            jit_scalar_t *args, tlab_t *tlab)
{
   args[0].real = trunc(args[1].real);
}

static void ieee_math_floor(jit_func_t *func, jit_anchor_t *anchor,
                            jit_scalar_t *args, tlab_t *tlab)
{
   args[0].real = floor(args[1].real);
}

static void ieee_math_pow_real(jit_func_t *func, jit_anchor_t *anchor,
                               jit_scalar_t *args, tlab_t *tlab)
{
   args[0].real = pow(args[1].real, args[2].real);
}

static void ieee_math_pow_integer(jit_func_t *func, jit_anchor_t *anchor,
                                  jit_scalar_t *args, tlab_t *tlab)
{
   args[0].real = pow((double)args[1].integer, args[2].real);
}

static void ieee_math_exp(jit_func_t *func, jit_anchor_t *anchor,
                          jit_scalar_t *args, tlab_t *tlab)
{
   args[0].real = exp(args[1].real);
}

static void std_textio_consume(jit_func_t *func, jit_anchor_t *anchor,
                               jit_scalar_t *args, tlab_t *tlab)
{
   ffi_uarray_t **line = args[2].pointer;
   const int nchars = args[3].integer;

   assert(*line != NULL);

   const int length = ffi_array_length((*line)->dims[0].length);
   assert(nchars <= length);

   (*line)->ptr += nchars;
   (*line)->dims[0].left = 1;
   (*line)->dims[0].length = length - nchars;

   args[0].pointer = NULL;
}

static void std_textio_shrink(jit_func_t *func, jit_anchor_t *anchor,
                              jit_scalar_t *args, tlab_t *tlab)
{
   ffi_uarray_t **line = args[2].pointer;
   const int nchars = args[3].integer;

   assert(*line != NULL);

   (*line)->dims[0].left = 1;
   (*line)->dims[0].length = nchars;

   args[0].pointer = NULL;
}

////////////////////////////////////////////////////////////////////////////////
// sv2vhdl logic3d operations
//
// logic3d is a 3-bit code (bit0=value, bit1=strength, bit2=uncertainty)
// declared as a subtype of NATURAL, so vector elements arrive here with
// INTEGER representation: int32 lanes holding codes 0..7.  These intrinsics
// replace the package's per-element LUT loops with branch-free bitwise
// formulas over whole lanes (verified against the LUTs by the l3dplane
// selftest and the intrinsics-on/off differential test).
//
// Semantics must match the VHDL bodies in lib/sv2vhdl/logic3d_types_pkg.vhd
// EXACTLY, including the length-mismatch and ascending-range cases: an
// intrinsic binding is permanent, so there is no falling back per call.
//
// Memory mapping used throughout: for the bodies' loops the operand element
// of iteration i is at mem[len-1-i] regardless of range direction, so with
// a descending result the whole loop is the unit-stride tail
//    result[j] = f(a[j], b[j + lb - la])
// and only the ascending-result case (never emitted by tgt-vhdl) needs the
// reversed write result[i] = f(a[la-1-i], b[lb-1-i]).

#define L3D_C0 2   // L3D_0: driven, certain 0
#define L3D_C1 3   // L3D_1: driven, certain 1
#define L3D_CX 6   // L3D_X: driven, uncertain 0
#define L3D_CU 7   // L3D_U: driven, uncertain 1

// AND_LUT: value ANDs; result always strong; uncertainty survives unless
// some operand is certainly 0
__attribute__((always_inline))
static inline int32_t __l3d_and_code(int32_t a, int32_t b)
{
   const int32_t va = a & 1, vb = b & 1;
   const int32_t ua = (a >> 2) & 1, ub = (b >> 2) & 1;
   const int32_t cert0 = ((va | ua) ^ 1) | ((vb | ub) ^ 1);
   return (va & vb) | 2 | (((ua | ub) & (cert0 ^ 1)) << 2);
}

// OR_LUT: value ORs; always strong; uncertainty dies against a certain 1
__attribute__((always_inline))
static inline int32_t __l3d_or_code(int32_t a, int32_t b)
{
   const int32_t va = a & 1, vb = b & 1;
   const int32_t ua = (a >> 2) & 1, ub = (b >> 2) & 1;
   const int32_t cert1 = (va & (ua ^ 1)) | (vb & (ub ^ 1));
   return (va | vb) | 2 | (((ua | ub) & (cert1 ^ 1)) << 2);
}

// XOR_LUT: value XORs; always strong; uncertainty is the union
__attribute__((always_inline))
static inline int32_t __l3d_xor_code(int32_t a, int32_t b)
{
   return ((a ^ b) & 1) | 2 | (((a | b) & 4));
}

#define L3D_BINOP_BODY(CODE_FN)                                         \
   const int la = ffi_array_length(args[3].integer);                    \
   const int lb = ffi_array_length(args[6].integer);                    \
   const bool adesc = args[3].integer < 0;                              \
   const int32_t *adata = args[1].pointer;                              \
   const int32_t *bdata = args[4].pointer;                              \
   const int64_t aleft = args[2].integer, acode = args[3].integer;      \
                                                                        \
   if (la == 0) {                                                       \
      args[0].pointer = (void *)adata;                                  \
      args[1].integer = aleft;                                          \
      args[2].integer = acode;                                          \
      return;                                                           \
   }                                                                    \
                                                                        \
   const int n = MIN(la, lb);                                           \
   int32_t *result = __tlab_alloc(tlab, la * sizeof(int32_t), 8);       \
                                                                        \
   if (n < la) {                                                        \
      for (int j = 0; j < la; j++)                                      \
         result[j] = L3D_C0;                                            \
   }                                                                    \
                                                                        \
   /* Memory layout is FIRST-ELEMENT-FIRST regardless of range direction \
    * (an ascending (0 to 31) and a descending (31 downto 0) holding the  \
    * same number store identical bytes), so the op is tail-aligned       \
    * elementwise for EVERY direction mix.  The old ascending branch      \
    * REVERSED both operands into the result -- bit-reversing the output  \
    * whenever `a` was ascending (an unconstrained concat/aggregate       \
    * actual: {24'h0,a}^const gave 20a5a5a5 for a5a5a504 -- the EH1a      \
    * registered-LSU garbage-load class).  It had faithfully copied the   \
    * same bug from the l3d_* VHDL bodies' mixed-direction branches,      \
    * fixed in lockstep (logic3d_types_pkg.vhd).                          */ \
   {                                                                    \
      const int d = lb - la;                                            \
      for (int j = la - n; j < la; j++)                                 \
         result[j] = CODE_FN(adata[j], bdata[j + d]);                   \
   }                                                                    \
   (void)adesc;                                                         \
                                                                        \
   args[0].pointer = result;                                            \
   args[1].integer = aleft;                                             \
   args[2].integer = acode;

static void l3d_and_vector(jit_func_t *func, jit_anchor_t *anchor,
                           jit_scalar_t *args, tlab_t *tlab)
{
   L3D_BINOP_BODY(__l3d_and_code);
}

static void l3d_or_vector(jit_func_t *func, jit_anchor_t *anchor,
                          jit_scalar_t *args, tlab_t *tlab)
{
   L3D_BINOP_BODY(__l3d_or_code);
}

static void l3d_xor_vector(jit_func_t *func, jit_anchor_t *anchor,
                           jit_scalar_t *args, tlab_t *tlab)
{
   L3D_BINOP_BODY(__l3d_xor_code);
}

// NOT_LUT = (1,0,3,2,5,4,7,6): flip the value bit, keep strength and
// uncertainty.  Index-aligned, so memory-aligned 1:1.
static void l3d_not_vector(jit_func_t *func, jit_anchor_t *anchor,
                           jit_scalar_t *args, tlab_t *tlab)
{
   const int la = ffi_array_length(args[3].integer);
   const int32_t *adata = args[1].pointer;
   const int64_t aleft = args[2].integer, acode = args[3].integer;

   if (la == 0)
      args[0].pointer = (void *)adata;
   else {
      int32_t *result = __tlab_alloc(tlab, la * sizeof(int32_t), 8);
      for (int j = 0; j < la; j++)
         result[j] = adata[j] ^ 1;
      args[0].pointer = result;
   }

   args[1].integer = aleft;
   args[2].integer = acode;
}

////////////////////////////////////////////////////////////////////////////////
// Packed 3D-logic word (l3dw): one int32 lane holds a GROUP of 8 wires as byte
// planes -- byte0 value, byte1 driven, byte2 kind-hi (K1), byte3 kind-lo (K0).
// Certainty is a 2-BIT ENUM per wire, ordered so numeric max is semantic
// dominance: 00 certain, 01 W/'-', 10 X, 11 U. K1 is the byte that used to be
// the lone `uncertain` plane, so pre-existing words (K0 = 0) still read as X.
// MUST MATCH lib/sv2vhdl/logic3dw_pkg.vhd EXACTLY -- they are two independent
// implementations of one semantics, and test/regress/logic3dw1 is what holds
// them together. So a
// logic3dw_vector of W words carries 8*W wires at 4 bytes / 8 wires (vs
// logic3d's 4 bytes / wire and std_logic's 1 byte / wire).
//
// The gate LUTs use only the value and uncertain planes and always drive the
// output, so each per-wire __l3d_*_code formula becomes a BYTE-PARALLEL formula
// computing all 8 wires of a lane at once (no cross-wire carry): the scalar
// `x ^ 1` (bit) becomes 8-bit `~x & 0xFF`. Verified against the per-wire path
// and against std_logic on 2-state inputs by test/regress/logic3dw1.
//
// Same int32-lane ABI as logic3d, so the vector plumbing is shared; only the
// per-lane word function and the length-mismatch pad (driven certain 0 =
// 0x00FF00) differ.

#define L3DW_PAD0 0x00FF00   // value 0, driven 0xFF, uncertain 0

// Byte-parallel 2-bit MAX of two certainty codes -- the C twin of
// logic3dw_pkg's kmax. Because the codes are ordered certain(00) < W(01) <
// X(10) < U(11), dominance is an ordinary magnitude compare, done bit-planar
// over all 8 wires at once with no cross-wire carry.
#define L3DW_KMAX(A1, A0, B1, B0, R1, R0)                                   \
   do {                                                                     \
      const uint32_t agt_ =                                                 \
         ((A1) & ~(B1)) | (~((A1) ^ (B1)) & (A0) & ~(B0));                  \
      (R1) = ((agt_ & (A1)) | (~agt_ & (B1))) & 0xFFu;                      \
      (R0) = ((agt_ & (A0)) | (~agt_ & (B0))) & 0xFFu;                      \
   } while (0)

__attribute__((always_inline))
static inline int32_t __l3dw_and_word(int32_t a, int32_t b)
{
   const uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
   const uint32_t Va = ua & 0xFF, Ka1 = (ua >> 16) & 0xFF, Ka0 = (ua >> 24) & 0xFF;
   const uint32_t Vb = ub & 0xFF, Kb1 = (ub >> 16) & 0xFF, Kb0 = (ub >> 24) & 0xFF;
   const uint32_t Ua = Ka1 | Ka0, Ub = Kb1 | Kb0;       // uncertain at all
   const uint32_t Vc = Va & Vb;
   const uint32_t c0 = (~(Va | Ua) | ~(Vb | Ub)) & 0xFF;   // some operand cert 0
   const uint32_t keep = ~c0 & 0xFF;
   uint32_t Kr1, Kr0;
   L3DW_KMAX(Ka1, Ka0, Kb1, Kb0, Kr1, Kr0);
   return (int32_t)(Vc | 0xFF00u | ((Kr1 & keep) << 16) | ((Kr0 & keep) << 24));
}

__attribute__((always_inline))
static inline int32_t __l3dw_or_word(int32_t a, int32_t b)
{
   const uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
   const uint32_t Va = ua & 0xFF, Ka1 = (ua >> 16) & 0xFF, Ka0 = (ua >> 24) & 0xFF;
   const uint32_t Vb = ub & 0xFF, Kb1 = (ub >> 16) & 0xFF, Kb0 = (ub >> 24) & 0xFF;
   const uint32_t Ua = Ka1 | Ka0, Ub = Kb1 | Kb0;
   const uint32_t Vc = Va | Vb;
   const uint32_t c1 = ((Va & ~Ua) | (Vb & ~Ub)) & 0xFF;   // some operand cert 1
   const uint32_t keep = ~c1 & 0xFF;
   uint32_t Kr1, Kr0;
   L3DW_KMAX(Ka1, Ka0, Kb1, Kb0, Kr1, Kr0);
   return (int32_t)(Vc | 0xFF00u | ((Kr1 & keep) << 16) | ((Kr0 & keep) << 24));
}

__attribute__((always_inline))
static inline int32_t __l3dw_xor_word(int32_t a, int32_t b)
{
   const uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
   const uint32_t Ka1 = (ua >> 16) & 0xFF, Ka0 = (ua >> 24) & 0xFF;
   const uint32_t Kb1 = (ub >> 16) & 0xFF, Kb0 = (ub >> 24) & 0xFF;
   const uint32_t Vc = (ua ^ ub) & 0xFF;
   // xor cannot be forced by either operand, so uncertainty always survives
   uint32_t Kr1, Kr0;
   L3DW_KMAX(Ka1, Ka0, Kb1, Kb0, Kr1, Kr0);
   return (int32_t)(Vc | 0xFF00u | (Kr1 << 16) | (Kr0 << 24));
}

// The 2-state fast path is the whole point of the packed word: once reset has
// cleared the uncertain planes, a bus op is a pure value-plane bitwise op over
// int32 lanes -- 8 wires per byte, 32 wires per SSE op, twice std_logic's 16.
// AND/OR/XOR ignore the input driven plane and force driven on output, so only
// the uncertain plane gates the fast path. `((a OP b) & 0xFF) | 0xFF00` takes
// the value byte, forces driven=0xFF, leaves uncertain=0 -- exactly the full
// formula's result when both operands are certain.
#define L3DW_BINOP_BODY(WORD_FN, VAL_OP)                                \
   const int la = ffi_array_length(args[3].integer);                    \
   const int lb = ffi_array_length(args[6].integer);                    \
   const bool adesc = args[3].integer < 0;                              \
   const int32_t *adata = args[1].pointer;                              \
   const int32_t *bdata = args[4].pointer;                              \
   const int64_t aleft = args[2].integer, acode = args[3].integer;      \
                                                                        \
   if (la == 0) {                                                       \
      args[0].pointer = (void *)adata;                                  \
      args[1].integer = aleft;                                          \
      args[2].integer = acode;                                          \
      return;                                                           \
   }                                                                    \
                                                                        \
   const int n = MIN(la, lb);                                           \
   int32_t *result = __tlab_alloc(tlab, la * sizeof(int32_t), 8);       \
                                                                        \
   if (n < la) {                                                        \
      for (int j = 0; j < la; j++)                                      \
         result[j] = L3DW_PAD0;                                         \
   }                                                                    \
                                                                        \
   if (adesc) {                                                         \
      const int d = lb - la;                                            \
      /* Single pass: write the 2-state value-plane result AND accumulate   \
         the operands' uncertain planes. VAL_OP is correct whenever every   \
         wire is certain; if any uncertain bit turned up, recompute the     \
         whole vector with the full formula (rare after reset). */          \
      int32_t unc = 0;                                                  \
      for (int j = la - n; j < la; j++) {                              \
         const int32_t a = adata[j], b = bdata[j + d];                 \
         result[j] = (VAL_OP);                                         \
         unc |= a | b;                                                 \
      }                                                                 \
      /* BOTH kind planes, not just K1: a W wire (K0 only) would otherwise \
         take the 2-state path and lose its uncertainty silently. */        \
      if (unlikely((uint32_t)unc & 0x00FF0000u)) {                                \
         for (int j = la - n; j < la; j++)                             \
            result[j] = WORD_FN(adata[j], bdata[j + d]);               \
      }                                                                 \
   }                                                                    \
   else {                                                               \
      for (int i = 0; i < n; i++)                                       \
         result[i] = WORD_FN(adata[la - 1 - i], bdata[lb - 1 - i]);     \
   }                                                                    \
                                                                        \
   args[0].pointer = result;                                            \
   args[1].integer = aleft;                                             \
   args[2].integer = acode;

static void l3dw_and_vector(jit_func_t *func, jit_anchor_t *anchor,
                            jit_scalar_t *args, tlab_t *tlab)
{
   L3DW_BINOP_BODY(__l3dw_and_word, a & b);
}

static void l3dw_or_vector(jit_func_t *func, jit_anchor_t *anchor,
                           jit_scalar_t *args, tlab_t *tlab)
{
   L3DW_BINOP_BODY(__l3dw_or_word, a | b);
}

static void l3dw_xor_vector(jit_func_t *func, jit_anchor_t *anchor,
                            jit_scalar_t *args, tlab_t *tlab)
{
   L3DW_BINOP_BODY(__l3dw_xor_word, (a ^ b) ^ 0xFF00);
}

// NOT: flip the value plane, keep driven and uncertain planes.
static void l3dw_not_vector(jit_func_t *func, jit_anchor_t *anchor,
                            jit_scalar_t *args, tlab_t *tlab)
{
   const int la = ffi_array_length(args[3].integer);
   const int32_t *adata = args[1].pointer;
   const int64_t aleft = args[2].integer, acode = args[3].integer;

   if (la == 0)
      args[0].pointer = (void *)adata;
   else {
      int32_t *result = __tlab_alloc(tlab, la * sizeof(int32_t), 8);
      for (int j = 0; j < la; j++) {
         const int32_t a = adata[j];
         result[j] = (int32_t)(((uint32_t)~a & 0xFFu)
                               | ((uint32_t)a & 0xFFFFFF00u));  // ~value, keep drv+kind
      }
      args[0].pointer = result;
   }

   args[1].integer = aleft;
   args[2].integer = acode;
}

// Verilog '==' returning a 1-bit logic3d: value-plane equality over the
// common low bits, uncertainty flag if any scanned bit is uncertain
// (certainty is metadata and never gates the value)
static void l3d_eq_vector(jit_func_t *func, jit_anchor_t *anchor,
                          jit_scalar_t *args, tlab_t *tlab)
{
   const int la = ffi_array_length(args[3].integer);
   const int lb = ffi_array_length(args[6].integer);
   const int32_t *adata = args[1].pointer;
   const int32_t *bdata = args[4].pointer;

   const int n = MIN(la, lb);
   int32_t neq = 0, unc = 0;
   for (int i = 0; i < n; i++) {
      const int32_t a = adata[la - 1 - i], b = bdata[lb - 1 - i];
      neq |= (a ^ b) & 1;
      unc |= (a | b) & 4;
   }

   args[0].integer = neq ? (unc ? L3D_CX : L3D_C0)
                         : (unc ? L3D_CU : L3D_C1);
}

// Boolean "=" via to_u on both sides then NUMERIC_STD."=": numeric equality
// of the value planes (leading zeros of the longer operand must be 0-valued)
static void l3d_eq_bool(jit_func_t *func, jit_anchor_t *anchor,
                        jit_scalar_t *args, tlab_t *tlab)
{
   const int la = ffi_array_length(args[3].integer);
   const int lb = ffi_array_length(args[6].integer);
   const int32_t *adata = args[1].pointer;
   const int32_t *bdata = args[4].pointer;

   const int n = MIN(la, lb);
   int32_t neq = 0;
   for (int i = 0; i < n; i++)
      neq |= (adata[la - 1 - i] ^ bdata[lb - 1 - i]) & 1;
   for (int j = 0; j < la - n; j++)
      neq |= adata[j] & 1;
   for (int j = 0; j < lb - n; j++)
      neq |= bdata[j] & 1;

   args[0].integer = !neq;
}

// result(i) := '1' when is_one(a(i)) else '0' -- index-aligned; result
// bounds are a's; unsigned elements are STD_ULOGIC bytes ('0'=2, '1'=3)
static void l3d_to_unsigned_vec(jit_func_t *func, jit_anchor_t *anchor,
                                jit_scalar_t *args, tlab_t *tlab)
{
   const int la = ffi_array_length(args[3].integer);
   const int32_t *adata = args[1].pointer;
   const int64_t aleft = args[2].integer, acode = args[3].integer;

   if (la == 0)
      args[0].pointer = (void *)adata;
   else {
      uint8_t *result = __tlab_alloc(tlab, la, 8);
      for (int j = 0; j < la; j++)
         result[j] = 2 + (adata[j] & 1);
      args[0].pointer = result;
   }

   args[1].integer = aleft;
   args[2].integer = acode;
}

// result(i) := L3D_1 when a(i) = '1' else L3D_0 -- only enum '1' (=3) maps
// to L3D_1; 'H' etc. map to L3D_0, matching the body's a(i)='1' test
static void l3d_from_unsigned(jit_func_t *func, jit_anchor_t *anchor,
                              jit_scalar_t *args, tlab_t *tlab)
{
   const int la = ffi_array_length(args[3].integer);
   const uint8_t *adata = args[1].pointer;
   const int64_t aleft = args[2].integer, acode = args[3].integer;

   if (la == 0)
      args[0].pointer = (void *)adata;
   else {
      int32_t *result = __tlab_alloc(tlab, la * sizeof(int32_t), 8);
      for (int j = 0; j < la; j++)
         result[j] = 2 + (adata[j] == 3);
      args[0].pointer = result;
   }

   args[1].integer = aleft;
   args[2].integer = acode;
}

// procedure to_u(a : logic3d_vector; u : out unsigned): fill u from a with
// a'left as MSB; the write index k starts at u'high and pins at u'low, so a
// longer `a` overwrites u's last position with each later element.
// Procedure convention: args[0]=suspend state, args[1]=context, params
// from args[2].
static void l3d_to_u_proc(jit_func_t *func, jit_anchor_t *anchor,
                          jit_scalar_t *args, tlab_t *tlab)
{
   const int la = ffi_array_length(args[4].integer);
   const int lu = ffi_array_length(args[7].integer);
   const int32_t *adata = args[2].pointer;
   uint8_t *udata = args[5].pointer;
   const bool udesc = args[7].integer < 0;

   if (lu > 0) {
      for (int j = 0; j < la; j++) {
         const int k = (j < lu) ? j : lu - 1;
         udata[udesc ? k : lu - 1 - k] = 2 + (adata[j] & 1);
      }
   }

   args[0].pointer = NULL;
}

// --- Native value-domain logic3d ops. logic3d's value plane IS the 2-state
// --- value (already 0/1), so arithmetic, comparison and index-by-value work
// --- directly on the value bit with NO conversion to numeric_std unsigned --
// --- eliminating the to_u / TO_01 / unsigned_to_l3d round-trip the package
// --- bodies do (the dominant interp cost). a[0] is the MSB, a[la-1] the LSB.

// Unsigned value of a logic3d_vector's value plane, capped at 31 bits to match
// to_integer's `p < 31` guard (result is a NATURAL / 32-bit integer).
static int64_t l3d_uval31(const int32_t *a, int la)
{
   int64_t v = 0;
   const int n = la < 31 ? la : 31;
   for (int p = 0; p < n; p++)      // p=0 is the LSB (a[la-1])
      v |= (int64_t)(a[la - 1 - p] & 1) << p;
   return v;
}

// to_integer(logic3d_vector) -> natural
static void l3d_to_integer_vec(jit_func_t *func, jit_anchor_t *anchor,
                               jit_scalar_t *args, tlab_t *tlab)
{
   args[0].integer = l3d_uval31(args[1].pointer,
                                ffi_array_length(args[3].integer));
}

// l3d_index(a, signed) -> integer: value-plane index (uncertain bits index by
// their value). Unsigned = to_integer(l3d_to_unsigned); signed = two's
// complement (to_integer(l3d_to_signed)).
static void l3d_index_vec(jit_func_t *func, jit_anchor_t *anchor,
                          jit_scalar_t *args, tlab_t *tlab)
{
   const int la = ffi_array_length(args[3].integer);
   const int32_t *a = args[1].pointer;
   const bool sgn = args[4].integer != 0;

   if (!sgn) {
      args[0].integer = l3d_uval31(a, la);
      return;
   }

   // Signed: sign bit is a'left = a[0]. Build the two's-complement value.
   int64_t v = 0;
   const int n = la < 63 ? la : 63;
   for (int p = 0; p < n; p++)
      v |= (int64_t)(a[la - 1 - p] & 1) << p;
   if (la >= 1 && la <= 63 && (a[0] & 1))
      v |= ~(((int64_t)1 << (la - 1 == 63 ? 63 : la)) - 1);   // sign-extend
   args[0].integer = v;
}

// Unsigned comparison of two value planes (a[0]=MSB). Returns <0, 0, >0.
static int l3d_ucmp(const int32_t *a, int la, const int32_t *b, int lb)
{
   const int W = la > lb ? la : lb;
   for (int k = 0; k < W; k++) {    // k=0 is the MSB of the W-wide compare
      const int abit = (k < W - la) ? 0 : (a[la - W + k] & 1);
      const int bbit = (k < W - lb) ? 0 : (b[lb - W + k] & 1);
      if (abit != bbit)
         return abit < bbit ? -1 : 1;
   }
   return 0;
}

static void l3d_lt_vec(jit_func_t *func, jit_anchor_t *anchor,
                       jit_scalar_t *args, tlab_t *tlab)
{
   args[0].integer = l3d_ucmp(args[1].pointer, ffi_array_length(args[3].integer),
                              args[4].pointer, ffi_array_length(args[6].integer))
                     < 0;
}

static void l3d_gt_vec(jit_func_t *func, jit_anchor_t *anchor,
                       jit_scalar_t *args, tlab_t *tlab)
{
   args[0].integer = l3d_ucmp(args[1].pointer, ffi_array_length(args[3].integer),
                              args[4].pointer, ffi_array_length(args[6].integer))
                     > 0;
}

// "+"/"-"(a, b): value-plane ripple add/subtract. numeric_std returns
// UNSIGNED(max(la,lb)-1 downto 0), so the result is W = max(la,lb) codes,
// descending, each a driven-certain L3D_0 (2) or L3D_1 (3). Subtraction is
// a + ~b + 1 (two's complement, carry out dropped -- modular). No conversion
// to unsigned, no TO_01.
static void l3d_addsub(jit_scalar_t *args, tlab_t *tlab, int sub)
{
   const int la = ffi_array_length(args[3].integer);
   const int lb = ffi_array_length(args[6].integer);
   const int32_t *a = args[1].pointer;
   const int32_t *b = args[4].pointer;

   const int W = la > lb ? la : lb;
   if (W == 0) {
      args[0].pointer = NULL;
      args[1].integer = 0;
      args[2].integer = -1;
      return;
   }

   int32_t *r = __tlab_alloc(tlab, (size_t)W * sizeof(int32_t), 8);
   if (W <= 64) {
      // Pack the value bits into one machine word and add ONCE: the
      // bit-serial ripple below cost ~8 ops per bit and l3d_addsub was
      // 6.8% of VeeR interp sim time.  Semantics are identical for every
      // input: both forms read only bit 0 of each lane (value plane) and
      // emit driven-certain bits (2|bit).
      uint64_t av = 0, bv = 0;
      for (int s = 0; s < la; s++)
         av |= (uint64_t)(a[la - 1 - s] & 1) << s;
      for (int s = 0; s < lb; s++)
         bv |= (uint64_t)(b[lb - 1 - s] & 1) << s;
      const uint64_t sv = sub ? av - bv : av + bv;
      for (int s = 0; s < W; s++)
         r[W - 1 - s] = 2 + (int32_t)((sv >> s) & 1);
   }
   else {
      int carry = sub;   // subtract: start carry at 1 for a + ~b + 1
      for (int s = 0; s < W; s++) {   // s = significance, 0 = LSB
         const int abit = (s < la) ? (a[la - 1 - s] & 1) : 0;
         int bbit = (s < lb) ? (b[lb - 1 - s] & 1) : 0;
         if (sub) bbit ^= 1;
         const int sum = abit + bbit + carry;
         r[W - 1 - s] = 2 + (sum & 1);   // 2=L3D_0, 3=L3D_1 (driven, certain)
         carry = sum >> 1;
      }
   }

   args[0].pointer = r;
   args[1].integer = W - 1;
   args[2].integer = ~W;   // (W-1 downto 0)
}

static void l3d_add_vector(jit_func_t *func, jit_anchor_t *anchor,
                           jit_scalar_t *args, tlab_t *tlab)
{
   l3d_addsub(args, tlab, 0);
}

static void l3d_sub_vector(jit_func_t *func, jit_anchor_t *anchor,
                           jit_scalar_t *args, tlab_t *tlab)
{
   l3d_addsub(args, tlab, 1);
}

// "*"(a, b): value-plane multiply. The l3d wrapper truncates the numeric_std
// product back to a'length (result(a'length-1 downto 0)), so only the low la
// columns are needed. Schoolbook column-sum then carry-propagate -- no unsigned
// conversion.
static void l3d_mul_vector(jit_func_t *func, jit_anchor_t *anchor,
                           jit_scalar_t *args, tlab_t *tlab)
{
   const int la = ffi_array_length(args[3].integer);
   const int lb = ffi_array_length(args[6].integer);
   const int32_t *a = args[1].pointer;
   const int32_t *b = args[4].pointer;

   if (la == 0) {
      args[0].pointer = NULL;
      args[1].integer = 0;
      args[2].integer = -1;
      return;
   }

   int32_t *r = __tlab_alloc(tlab, (size_t)la * sizeof(int32_t), 8);
   int64_t *col = __tlab_alloc(tlab, (size_t)la * sizeof(int64_t), 8);
   memset(col, 0, (size_t)la * sizeof(int64_t));

   for (int s = 0; s < la; s++) {          // s,j = significance of a,b
      if (!(a[la - 1 - s] & 1)) continue;
      for (int j = 0; j < lb && s + j < la; j++)   // truncate at column la
         if (b[lb - 1 - j] & 1) col[s + j]++;
   }

   int64_t carry = 0;
   for (int c = 0; c < la; c++) {
      const int64_t v = col[c] + carry;
      r[la - 1 - c] = 2 + (int)(v & 1);
      carry = v >> 1;
   }

   args[0].pointer = r;
   args[1].integer = la - 1;
   args[2].integer = ~la;
}

// Signed comparison of two value planes (a[0]=MSB=sign, two's complement).
static int l3d_scmp(const int32_t *a, int la, const int32_t *b, int lb)
{
   const int asign = (la > 0) ? (a[0] & 1) : 0;
   const int bsign = (lb > 0) ? (b[0] & 1) : 0;
   if (asign != bsign)
      return asign ? -1 : 1;   // negative < positive

   const int W = la > lb ? la : lb;
   for (int k = 0; k < W; k++) {   // sign-extend both to W, compare MSB-first
      const int abit = (k < W - la) ? asign : (a[la - W + k] & 1);
      const int bbit = (k < W - lb) ? bsign : (b[lb - W + k] & 1);
      if (abit != bbit)
         return abit < bbit ? -1 : 1;
   }
   return 0;
}

#define L3D_SCMP_INTRIN(name, op)                                          \
   static void name(jit_func_t *func, jit_anchor_t *anchor,                \
                    jit_scalar_t *args, tlab_t *tlab)                      \
   {                                                                       \
      args[0].integer =                                                    \
         l3d_scmp(args[1].pointer, ffi_array_length(args[3].integer),      \
                  args[4].pointer, ffi_array_length(args[6].integer)) op;  \
   }

L3D_SCMP_INTRIN(l3d_lt_s_vec, < 0)
L3D_SCMP_INTRIN(l3d_gt_s_vec, > 0)
L3D_SCMP_INTRIN(l3d_le_s_vec, <= 0)
L3D_SCMP_INTRIN(l3d_ge_s_vec, >= 0)

// l3d_resize_s(a, new_size): signed resize of the value plane (numeric_std
// SIGNED resize semantics -- sign-fill, keep low bits + sign). a[0]=MSB=sign.
static void l3d_resize_s_vec(jit_func_t *func, jit_anchor_t *anchor,
                             jit_scalar_t *args, tlab_t *tlab)
{
   const int la = ffi_array_length(args[3].integer);
   const int32_t *a = args[1].pointer;
   const int nw = args[4].integer;

   if (nw < 1) {
      args[0].pointer = NULL;
      args[1].integer = 0;
      args[2].integer = -1;
      return;
   }

   int32_t *r = __tlab_alloc(tlab, (size_t)nw * sizeof(int32_t), 8);
   const int sign = (la > 0) ? (a[0] & 1) : 0;
   for (int i = 0; i < nw; i++)   // sign-fill (covers MSB + any extension)
      r[i] = 2 + sign;

   const int lo = (la < nw ? la : nw) - 1;   // copy significance 0..lo-1
   for (int i = 0; i < lo; i++)
      r[nw - 1 - i] = 2 + (a[la - 1 - i] & 1);

   args[0].pointer = r;
   args[1].integer = nw - 1;
   args[2].integer = ~nw;
}

#define UU "36IEEE.NUMERIC_STD.UNRESOLVED_UNSIGNED"
#define U "25IEEE.NUMERIC_STD.UNSIGNED"
#define US "34IEEE.NUMERIC_STD.UNRESOLVED_SIGNED"
#define S "23IEEE.NUMERIC_STD.SIGNED"
#define NS "IEEE.NUMERIC_STD."
#define SL "IEEE.STD_LOGIC_1164."
#define MR "IEEE.MATH_REAL."
#define ST "STD.STANDARD."
#define TI "STD.TEXTIO."
#define LN "15STD.TEXTIO.LINE"
#define SA "IEEE.STD_LOGIC_ARITH."
#define SU "IEEE.STD_LOGIC_UNSIGNED."
#define SS "IEEE.STD_LOGIC_SIGNED."
#define AU "29IEEE.STD_LOGIC_ARITH.UNSIGNED"
#define AS "27IEEE.STD_LOGIC_ARITH.SIGNED"
#define L3P "SV2VHDL.LOGIC3D_TYPES_PKG."
#define L3 "33SV2VHDL.LOGIC3D_TYPES_PKG.LOGIC3D"
#define L3V "40SV2VHDL.LOGIC3D_TYPES_PKG.LOGIC3D_VECTOR"
#define L3WP "SV2VHDL.LOGIC3DW_PKG."
#define L3WV "32SV2VHDL.LOGIC3DW_PKG.L3DW_VECTOR"

// #74 fuse: net-fusion primitives (SV2VHDL.FUSE_PKG).  Signal-class
// arguments arrive as two slots {sig_shared_t *, offset}; args[0] is the
// context pointer and also receives the scalar boolean result.
// NVC_FUSE_ARGS=1 dumps the raw arg block for empirical layout checks.
static void sv2vhdl_undriven(jit_func_t *func, jit_anchor_t *anchor,
                             jit_scalar_t *args, tlab_t *tlab)
{
   if (unlikely(getenv("NVC_FUSE_ARGS") != NULL))
      fprintf(stderr, "#UNDRIVEN args %p %p %"PRIi64" %p %"PRIi64"\n",
              args[0].pointer, args[1].pointer, args[2].integer,
              args[3].pointer, args[4].integer);

   args[0].integer = x_signal_undriven(args[1].pointer, args[2].integer);
}

static void sv2vhdl_fuse_try(jit_func_t *func, jit_anchor_t *anchor,
                             jit_scalar_t *args, tlab_t *tlab)
{
   if (unlikely(getenv("NVC_FUSE_ARGS") != NULL))
      fprintf(stderr, "#FUSE_TRY args %p %p %"PRIi64" %p %"PRIi64"\n",
              args[0].pointer, args[1].pointer, args[2].integer,
              args[3].pointer, args[4].integer);

   args[0].integer = x_fuse_signals(args[1].pointer, args[2].integer,
                                    args[3].pointer, args[4].integer);
}

static jit_intrinsic_t intrinsic_list[] = {
   { "SV2VHDL.FUSE_PKG.UNDRIVEN(sU)B", sv2vhdl_undriven },
   { "SV2VHDL.FUSE_PKG.FUSE_TRY(sUsU)B", sv2vhdl_fuse_try },
   { L3WP "L3DW_AND(" L3WV L3WV ")" L3WV, l3dw_and_vector },
   { L3WP "L3DW_OR("  L3WV L3WV ")" L3WV, l3dw_or_vector },
   { L3WP "L3DW_XOR(" L3WV L3WV ")" L3WV, l3dw_xor_vector },
   { L3WP "L3DW_NOT(" L3WV ")" L3WV, l3dw_not_vector },
   { NS "\"+\"(" U U ")" U, ieee_plus_unsigned },
   { NS "\"+\"(" UU UU ")" UU, ieee_plus_unsigned },
   { NS "\"+\"(" U "N)" U, ieee_plus_unsigned_natural },
   { NS "\"+\"(" UU "N)" UU, ieee_plus_unsigned_natural },
   { NS "\"+\"(N" U ")" U, ieee_plus_natural_unsigned },
   { NS "\"+\"(N" UU ")" UU, ieee_plus_natural_unsigned },
   { NS "\"+\"(" S S ")" S, ieee_plus_signed },
   { NS "\"+\"(" US US ")" US, ieee_plus_signed },
   { NS "\"-\"(" U U ")" U, ieee_minus_unsigned },
   { NS "\"-\"(" UU UU ")" UU, ieee_minus_unsigned },
   { NS "\"-\"(" S S ")" S, ieee_minus_signed },
   { NS "\"-\"(" US US ")" US, ieee_minus_signed },
   { NS "\"*\"(" U U ")" U, ieee_mul_unsigned },
   { NS "\"*\"(" UU UU ")" UU, ieee_mul_unsigned },
   { NS "\"*\"(" S S ")" S, ieee_mul_signed },
   { NS "\"*\"(" US US ")" US, ieee_mul_signed },
   { NS "DIVMOD(" UU UU UU UU ")", ieee_divmod },
   { NS "DIVMOD(" U U U U ")", ieee_divmod },
   { NS "\"<\"(" U U ")B", ieee_less_unsigned },
   { NS "\"<\"(" UU UU ")B" , ieee_less_unsigned },
   { NS "\">\"(" U U ")B", ieee_greater_unsigned },
   { NS "\">\"(" UU UU ")B" , ieee_greater_unsigned },
   { NS "\">=\"(" U U ")B", ieee_geq_unsigned },
   { NS "\">=\"(" UU UU ")B" , ieee_geq_unsigned },
   { NS "\"<=\"(" U U ")B", ieee_leq_unsigned },
   { NS "\"<=\"(" UU UU ")B" , ieee_leq_unsigned },
   { NS "TO_INTEGER(" U ")N", ieee_to_integer_unsigned },
   { NS "TO_INTEGER(" UU ")N", ieee_to_integer_unsigned },
   { NS "TO_INTEGER(" S ")I", ieee_to_integer_signed },
   { NS "TO_INTEGER(" US ")I", ieee_to_integer_signed },
#ifdef HAVE_SSE41
   { SL "TO_X01(V)V", std_to_x01_sse41, CPU_SSE41 },
   { SL "TO_X01(Y)Y", std_to_x01_sse41, CPU_SSE41 },
#endif
   { SL "TO_X01(V)V", std_to_x01 },
   { SL "TO_X01(Y)Y", std_to_x01 },
   { NS "TO_01(" U "L)" U, ieee_to_01 },
   { NS "TO_01(" UU "U)" UU, ieee_to_01 },
   { NS "TO_01(" S "L)" U, ieee_to_01 },
   { NS "TO_01(" US "U)" UU, ieee_to_01 },
   { NS "RESIZE(" U "N)" U, ieee_resize_unsigned },
   { NS "RESIZE(" UU "N)" UU, ieee_resize_unsigned },
   { NS "RESIZE(" S "N)" S, ieee_resize_signed },
   { NS "RESIZE(" US "N)" US, ieee_resize_signed },
#ifdef HAVE_SSE41
   { SL "\"and\"(VV)V", ieee_and_vector_sse41, CPU_SSE41 },
   { SL "\"and\"(YY)Y", ieee_and_vector_sse41, CPU_SSE41 },
#endif
#ifdef HAVE_NEON
   { SL "\"and\"(VV)V", ieee_and_vector_neon, CPU_NEON },
   { SL "\"and\"(YY)Y", ieee_and_vector_neon, CPU_NEON },
#endif
   { SL "\"and\"(VV)V", ieee_and_vector },
   { SL "\"and\"(YY)Y", ieee_and_vector },
#ifdef HAVE_SSE41
   { SL "\"or\"(VV)V", ieee_or_vector_sse41, CPU_SSE41 },
   { SL "\"or\"(YY)Y", ieee_or_vector_sse41, CPU_SSE41 },
#endif
#ifdef HAVE_NEON
   { SL "\"or\"(VV)V", ieee_or_vector_neon, CPU_NEON },
   { SL "\"or\"(YY)Y", ieee_or_vector_neon, CPU_NEON },
#endif
   { SL "\"or\"(VV)V", ieee_or_vector },
   { SL "\"or\"(YY)Y", ieee_or_vector },
#ifdef HAVE_SSE41
   { SL "\"xor\"(VV)V", ieee_xor_vector_sse41, CPU_SSE41 },
   { SL "\"xor\"(YY)Y", ieee_xor_vector_sse41, CPU_SSE41 },
#endif
#ifdef HAVE_NEON
   { SL "\"xor\"(VV)V", ieee_xor_vector_neon, CPU_NEON },
   { SL "\"xor\"(YY)Y", ieee_xor_vector_neon, CPU_NEON },
#endif
   { SL "\"xor\"(VV)V", std_xor_vector },
   { SL "\"xor\"(YY)Y", std_xor_vector },
#ifdef HAVE_SSE41
   { SL "\"not\"(V)V", ieee_not_vector_sse41, CPU_SSE41 },
   { SL "\"not\"(Y)Y", ieee_not_vector_sse41, CPU_SSE41 },
#endif
   { NS "TO_UNSIGNED(NN)" U, ieee_to_unsigned },
   { NS "TO_UNSIGNED(NN)" UU, ieee_to_unsigned },
   { NS "TO_SIGNED(IN)" S, ieee_to_signed },
   { NS "TO_SIGNED(IN)" US, ieee_to_signed },
#ifdef HAVE_SSE41
   { SL "\"=\"(VV)B$predef", byte_vector_equal_sse41, CPU_SSE41 },
   { SL "\"=\"(YY)B$predef", byte_vector_equal_sse41, CPU_SSE41 },
   { ST "\"=\"(QQ)B$predef", byte_vector_equal_sse41, CPU_SSE41 },
   { ST "\"=\"(SS)B$predef", byte_vector_equal_sse41, CPU_SSE41 },
#endif
#ifdef HAVE_NEON
   { SL "\"=\"(VV)B$predef", byte_vector_equal_neon, CPU_NEON },
   { SL "\"=\"(YY)B$predef", byte_vector_equal_neon, CPU_NEON },
   { ST "\"=\"(QQ)B$predef", byte_vector_equal_neon, CPU_NEON },
   { ST "\"=\"(SS)B$predef", byte_vector_equal_neon, CPU_NEON },
#endif
   { SL "\"=\"(VV)B$predef", byte_vector_equal },
   { SL "\"=\"(YY)B$predef", byte_vector_equal },
   { ST "\"=\"(QQ)B$predef", byte_vector_equal },
   { ST "\"=\"(SS)B$predef", byte_vector_equal },
   { MR "SIN(R)R", ieee_math_sin },
   { MR "COS(R)R", ieee_math_cos },
   { MR "LOG(R)R", ieee_math_log },
   { MR "LOG2(R)R", ieee_math_log2 },
   { MR "LOG10(R)R", ieee_math_log10 },
   { MR "ROUND(R)R", ieee_math_round },
   { MR "TRUNC(R)R", ieee_math_trunc },
   { MR "FLOOR(R)R", ieee_math_floor },
   { MR "\"**\"(RR)R", ieee_math_pow_real },
   { MR "\"**\"(IR)R", ieee_math_pow_integer },
   { MR "EXP(R)R", ieee_math_exp },
   { TI "CONSUME(" LN "N)", std_textio_consume },
   { TI "SHRINK(" LN "N)", std_textio_shrink },
   { SA "\"+\"(" AU AU ")" AU, synopsys_plus_unsigned },
   { SA "\"+\"(" AU AU ")V", synopsys_plus_unsigned },
   { SU "\"+\"(VV)V", synopsys_plus_unsigned },
   { SU "\"+\"(YY)Y", synopsys_plus_unsigned },
   { SA "\"+\"(" AS AS ")" AS, synopsys_plus_signed },
   { SA "\"+\"(" AS AS ")V", synopsys_plus_signed },
   { SS "\"+\"(VV)V", synopsys_plus_signed },
   { SS "\"+\"(YY)Y", synopsys_plus_signed },
   { SA "\"+\"(" AU "U)" AU, synopsys_plus_unsigned_logic },
   { SA "\"+\"(" AU "U)V", synopsys_plus_unsigned_logic },
   { SU "\"+\"(VL)V", synopsys_plus_unsigned_logic },
   { SU "\"+\"(YL)Y", synopsys_plus_unsigned_logic },
   { SA "\"+\"(U" AU ")" AU, synopsys_plus_logic_unsigned },
   { SA "\"+\"(U" AU ")V", synopsys_plus_logic_unsigned },
   { SU "\"+\"(LV)V", synopsys_plus_logic_unsigned },
   { SU "\"+\"(LY)Y", synopsys_plus_logic_unsigned },
   { SA "\"-\"(" AU AU ")" AU, synopsys_minus_unsigned },
   { SA "\"-\"(" AU AU ")V", synopsys_minus_unsigned },
   { SU "\"-\"(VV)V", synopsys_minus_unsigned },
   { SU "\"-\"(YY)Y", synopsys_minus_unsigned },
   { SA "\"*\"(" AU AU ")" AU, synopsys_mul_unsigned },
   { SA "\"*\"(" AU AU ")V", synopsys_mul_unsigned },
   { SU "\"*\"(VV)V", synopsys_mul_unsigned },
   { SU "\"*\"(YY)Y", synopsys_mul_unsigned },
   { SA "\"*\"(" AS AS ")" AS, synopsys_mul_signed },
   { SA "\"*\"(" AS AS ")V", synopsys_mul_signed },
   { SS "\"*\"(VV)V", synopsys_mul_signed },
   { SS "\"*\"(YY)Y", synopsys_mul_signed },
   { SA "\"=\"(" AU AU ")B", synopsys_eql_unsigned },
   { SA "\"=\"(" AU AU ")B", synopsys_eql_unsigned },
   { SU "\"=\"(VV)B", synopsys_eql_unsigned },
   { SU "\"=\"(YY)B", synopsys_eql_unsigned },
   { SA "\"=\"(" AS AS ")B", synopsys_eql_signed },
   { SA "\"=\"(" AS AS ")B", synopsys_eql_signed },
   { SS "\"=\"(VV)B", synopsys_eql_signed },
   { SS "\"=\"(YY)B", synopsys_eql_signed },
   { L3P "L3D_AND(" L3V L3V ")" L3V, l3d_and_vector },
   { L3P "L3D_OR(" L3V L3V ")" L3V, l3d_or_vector },
   { L3P "L3D_XOR(" L3V L3V ")" L3V, l3d_xor_vector },
   { L3P "L3D_NOT(" L3V ")" L3V, l3d_not_vector },
   { L3P "\"=\"(" L3V L3V ")" L3, l3d_eq_vector },
   { L3P "\"=\"(" L3V L3V ")B", l3d_eq_bool },
   { L3P "L3D_TO_UNSIGNED(" L3V ")" U, l3d_to_unsigned_vec },
   { L3P "UNSIGNED_TO_L3D(" U ")" L3V, l3d_from_unsigned },
   { L3P "TO_U(" L3V U ")", l3d_to_u_proc },
   { L3P "TO_INTEGER(" L3V ")N", l3d_to_integer_vec },
   { L3P "L3D_INDEX(" L3V "B)I", l3d_index_vec },
   { L3P "\"<\"(" L3V L3V ")B", l3d_lt_vec },
   { L3P "\">\"(" L3V L3V ")B", l3d_gt_vec },
   { L3P "\"+\"(" L3V L3V ")" L3V, l3d_add_vector },
   { L3P "\"-\"(" L3V L3V ")" L3V, l3d_sub_vector },
   { L3P "\"*\"(" L3V L3V ")" L3V, l3d_mul_vector },
   { L3P "L3D_LT_S(" L3V L3V ")B", l3d_lt_s_vec },
   { L3P "L3D_GT_S(" L3V L3V ")B", l3d_gt_s_vec },
   { L3P "L3D_LE_S(" L3V L3V ")B", l3d_le_s_vec },
   { L3P "L3D_GE_S(" L3V L3V ")B", l3d_ge_s_vec },
   { L3P "L3D_RESIZE_S(" L3V "N)" L3V, l3d_resize_s_vec },
   { NULL, NULL }
};

jit_entry_fn_t jit_bind_intrinsic(ident_t name)
{
   INIT_ONCE({
         const bool want_intrinsics = !!opt_get_int(OPT_JIT_INTRINSICS);

#if ASAN_ENABLED
         const bool want_vector = false;   // Reads past end of input (benign)
#else
         const bool want_vector = !!opt_get_int(OPT_VECTOR_INTRINSICS);
#endif

         ieee_packed_add = __ieee_packed_add_scalar;

         cpu_feature_t mask = 0;
#ifdef HAVE_SSE41
         if (want_vector && __builtin_cpu_supports("sse4.1")) {
            mask |= CPU_SSE41;
            ieee_packed_add = __ieee_packed_add_sse41;
         }
#endif
#if HAVE_AVX2
         if (want_vector && __builtin_cpu_supports("avx2"))
            mask |= CPU_AVX2;
#endif
#ifdef HAVE_NEON
         if (want_vector)
            mask |= CPU_NEON;
#endif

         for (jit_intrinsic_t *it = intrinsic_list; it->name; it++) {
            if (it->feature && !(it->feature & mask))
               continue;
            else if (want_intrinsics)
               it->ident = ident_new(it->name);
         }
      });

   for (const jit_intrinsic_t *it = intrinsic_list; it->name; it++) {
      if (it->ident == name)
         return it->entry;
   }

   return NULL;
}
