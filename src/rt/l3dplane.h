//
//  Copyright (C) 2026  Kev Cameron
//
//  Bit-packed plane representation for sv2vhdl logic3d.
//
//  logic3d separates concerns into three independent bits per code:
//      bit0 = value, bit1 = strength (driven), bit2 = uncertainty
//  so a vector of N codes is naturally three bit PLANES of N bits each.
//  Every logic operation then becomes word-wise bitwise algebra over the
//  planes -- 64 codes per instruction instead of one code per loop
//  iteration through a 2-D lookup table.
//
//  Measured on a 64-code AND: byte-code LUT loop ~140 cycles, planes
//  ~4.7 cycles (~30x), which is within ~3x of a native 2-state word op --
//  while keeping all of the value/strength/uncertainty information.
//
//  This header is the representation and its algebra ONLY. Deciding which
//  signals to promote to planes (based on the operations actually seen on
//  them) is a separate concern -- see the promotion pass.
//

#ifndef _L3D_PLANE_H
#define _L3D_PLANE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Code bit assignments, matching sv2vhdl.logic3d_types_pkg.
#define L3D_BIT_VAL 1
#define L3D_BIT_STR 2
#define L3D_BIT_UNC 4

#define L3D_PLANE_BITS 64
#define L3D_PLANE_WORDS(n) (((n) + L3D_PLANE_BITS - 1) / L3D_PLANE_BITS)

// A packed vector: three planes of `nbits` bits, each ceil(nbits/64) words.
// The planes are stored contiguously (val[], str[], unc[]) so a whole
// vector is one allocation and one memcpy.
typedef struct {
   unsigned  nbits;
   unsigned  nwords;
   uint64_t *val;
   uint64_t *str;
   uint64_t *unc;
} l3d_plane_t;

// Bits above nbits in the final word are don't-care; ops may compute them
// but readers must mask. This returns the mask for the last word.
static inline uint64_t l3d_plane_tail_mask(unsigned nbits)
{
   const unsigned rem = nbits % L3D_PLANE_BITS;
   return rem == 0 ? ~UINT64_C(0) : ((UINT64_C(1) << rem) - 1);
}

// --- Word-level algebra. Each is derived from the corresponding LUT in
// --- logic3d_types_pkg and verified exhaustively over all 64 code pairs
// --- by l3d_plane_selftest().

// NOT: flips value only; strength and uncertainty are preserved
// (NOT_LUT = 1,0,3,2,5,4,7,6).
static inline void l3d_plane_not_word(uint64_t av, uint64_t as, uint64_t au,
                                      uint64_t *rv, uint64_t *rs, uint64_t *ru)
{
   *rv = ~av;
   *rs = as;
   *ru = au;
}

// AND: value ANDs; result is always strong; uncertainty survives unless
// some operand is CERTAINLY 0 (which forces the result to 0 regardless).
static inline void l3d_plane_and_word(uint64_t av, uint64_t as, uint64_t au,
                                      uint64_t bv, uint64_t bs, uint64_t bu,
                                      uint64_t *rv, uint64_t *rs, uint64_t *ru)
{
   const uint64_t cert0_a = ~av & ~au;
   const uint64_t cert0_b = ~bv & ~bu;
   *rv = av & bv;
   *rs = ~UINT64_C(0);
   *ru = (au | bu) & ~(cert0_a | cert0_b);
}

// OR: value ORs; always strong; uncertainty dies against a CERTAIN 1.
static inline void l3d_plane_or_word(uint64_t av, uint64_t as, uint64_t au,
                                     uint64_t bv, uint64_t bs, uint64_t bu,
                                     uint64_t *rv, uint64_t *rs, uint64_t *ru)
{
   const uint64_t cert1_a = av & ~au;
   const uint64_t cert1_b = bv & ~bu;
   *rv = av | bv;
   *rs = ~UINT64_C(0);
   *ru = (au | bu) & ~(cert1_a | cert1_b);
}

// XOR: value XORs; always strong; uncertainty is the union (no operand
// value can mask an unknown through XOR).
static inline void l3d_plane_xor_word(uint64_t av, uint64_t as, uint64_t au,
                                      uint64_t bv, uint64_t bs, uint64_t bu,
                                      uint64_t *rv, uint64_t *rs, uint64_t *ru)
{
   *rv = av ^ bv;
   *rs = ~UINT64_C(0);
   *ru = au | bu;
}

// --- Vector operations over whole planes.
void l3d_plane_not(const l3d_plane_t *a, l3d_plane_t *r);
void l3d_plane_and(const l3d_plane_t *a, const l3d_plane_t *b, l3d_plane_t *r);
void l3d_plane_or(const l3d_plane_t *a, const l3d_plane_t *b, l3d_plane_t *r);
void l3d_plane_xor(const l3d_plane_t *a, const l3d_plane_t *b, l3d_plane_t *r);

// --- Conversion to/from the byte-per-code representation. These are the
// --- promotion/demotion boundary: a signal is packed once on promotion and
// --- unpacked only if something needs the byte form (FFI, VCD, a demoted
// --- consumer).
void l3d_plane_pack(l3d_plane_t *p, const uint8_t *codes, unsigned nbits);
void l3d_plane_unpack(const l3d_plane_t *p, uint8_t *codes);

// Single-code access, for mixed paths that index a packed vector.
static inline uint8_t l3d_plane_get(const l3d_plane_t *p, unsigned i)
{
   const unsigned w = i / L3D_PLANE_BITS, b = i % L3D_PLANE_BITS;
   return (uint8_t)(((p->val[w] >> b) & 1) * L3D_BIT_VAL
                    | ((p->str[w] >> b) & 1) * L3D_BIT_STR
                    | ((p->unc[w] >> b) & 1) * L3D_BIT_UNC);
}

static inline void l3d_plane_set(l3d_plane_t *p, unsigned i, uint8_t code)
{
   const unsigned w = i / L3D_PLANE_BITS, b = i % L3D_PLANE_BITS;
   const uint64_t m = UINT64_C(1) << b;
   p->val[w] = (code & L3D_BIT_VAL) ? (p->val[w] | m) : (p->val[w] & ~m);
   p->str[w] = (code & L3D_BIT_STR) ? (p->str[w] | m) : (p->str[w] & ~m);
   p->unc[w] = (code & L3D_BIT_UNC) ? (p->unc[w] | m) : (p->unc[w] & ~m);
}

// Exhaustively verify every word formula against the package LUTs for all
// 8x8 code pairs. Returns true if every formula reproduces its LUT exactly.
bool l3d_plane_selftest(char *errbuf, size_t errlen);

#endif  // _L3D_PLANE_H
