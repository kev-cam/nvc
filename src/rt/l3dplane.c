//
//  Copyright (C) 2026  Kev Cameron
//
//  Bit-packed plane representation for sv2vhdl logic3d -- see l3dplane.h.
//

#include "rt/l3dplane.h"

#include <stdio.h>
#include <string.h>

// The reference tables from sv2vhdl.logic3d_types_pkg. The word formulas
// in the header must reproduce these exactly; l3d_plane_selftest() proves
// it over the whole 8x8 space rather than trusting the derivation.
static const uint8_t L3D_NOT_LUT[8] = { 1, 0, 3, 2, 5, 4, 7, 6 };

static const uint8_t L3D_AND_LUT[8][8] = {
   {2,2,2,2,2,2,2,2},{2,3,2,3,6,7,6,7},{2,2,2,2,2,2,2,2},{2,3,2,3,6,7,6,7},
   {2,6,2,6,6,6,6,6},{2,7,2,7,6,7,6,7},{2,6,2,6,6,6,6,6},{2,7,2,7,6,7,6,7},
};

static const uint8_t L3D_OR_LUT[8][8] = {
   {2,3,2,3,6,7,6,7},{3,3,3,3,3,3,3,3},{2,3,2,3,6,7,6,7},{3,3,3,3,3,3,3,3},
   {6,3,6,3,6,7,6,7},{7,3,7,3,7,7,7,7},{6,3,6,3,6,7,6,7},{7,3,7,3,7,7,7,7},
};

static const uint8_t L3D_XOR_LUT[8][8] = {
   {2,3,2,3,6,7,6,7},{3,2,3,2,7,6,7,6},{2,3,2,3,6,7,6,7},{3,2,3,2,7,6,7,6},
   {6,7,6,7,6,7,6,7},{7,6,7,6,7,6,7,6},{6,7,6,7,6,7,6,7},{7,6,7,6,7,6,7,6},
};

void l3d_plane_not(const l3d_plane_t *a, l3d_plane_t *r)
{
   for (unsigned w = 0; w < a->nwords; w++)
      l3d_plane_not_word(a->val[w], a->str[w], a->unc[w],
                         &r->val[w], &r->str[w], &r->unc[w]);
}

void l3d_plane_and(const l3d_plane_t *a, const l3d_plane_t *b, l3d_plane_t *r)
{
   for (unsigned w = 0; w < a->nwords; w++)
      l3d_plane_and_word(a->val[w], a->str[w], a->unc[w],
                         b->val[w], b->str[w], b->unc[w],
                         &r->val[w], &r->str[w], &r->unc[w]);
}

void l3d_plane_or(const l3d_plane_t *a, const l3d_plane_t *b, l3d_plane_t *r)
{
   for (unsigned w = 0; w < a->nwords; w++)
      l3d_plane_or_word(a->val[w], a->str[w], a->unc[w],
                        b->val[w], b->str[w], b->unc[w],
                        &r->val[w], &r->str[w], &r->unc[w]);
}

void l3d_plane_xor(const l3d_plane_t *a, const l3d_plane_t *b, l3d_plane_t *r)
{
   for (unsigned w = 0; w < a->nwords; w++)
      l3d_plane_xor_word(a->val[w], a->str[w], a->unc[w],
                         b->val[w], b->str[w], b->unc[w],
                         &r->val[w], &r->str[w], &r->unc[w]);
}

void l3d_plane_pack(l3d_plane_t *p, const uint8_t *codes, unsigned nbits)
{
   p->nbits = nbits;
   p->nwords = L3D_PLANE_WORDS(nbits);

   memset(p->val, 0, p->nwords * sizeof(uint64_t));
   memset(p->str, 0, p->nwords * sizeof(uint64_t));
   memset(p->unc, 0, p->nwords * sizeof(uint64_t));

   for (unsigned i = 0; i < nbits; i++) {
      const unsigned w = i / L3D_PLANE_BITS, b = i % L3D_PLANE_BITS;
      const uint64_t m = UINT64_C(1) << b;
      const uint8_t c = codes[i];
      if (c & L3D_BIT_VAL) p->val[w] |= m;
      if (c & L3D_BIT_STR) p->str[w] |= m;
      if (c & L3D_BIT_UNC) p->unc[w] |= m;
   }
}

void l3d_plane_unpack(const l3d_plane_t *p, uint8_t *codes)
{
   for (unsigned i = 0; i < p->nbits; i++)
      codes[i] = l3d_plane_get(p, i);
}

bool l3d_plane_selftest(char *errbuf, size_t errlen)
{
   // Drive every code pair through the word formulas in a single lane and
   // compare against the tables. Lane 0 is enough for correctness of the
   // formulas (they are bitwise and lane-independent), but run a second
   // lane at a non-zero bit position to catch any accidental lane coupling.
   const unsigned lanes[2] = { 0, 37 };

   for (int li = 0; li < 2; li++) {
      const unsigned L = lanes[li];
      const uint64_t m = UINT64_C(1) << L;

      for (int a = 0; a < 8; a++) {
         const uint64_t av = (a & L3D_BIT_VAL) ? m : 0;
         const uint64_t as = (a & L3D_BIT_STR) ? m : 0;
         const uint64_t au = (a & L3D_BIT_UNC) ? m : 0;

         uint64_t rv, rs, ru;
         l3d_plane_not_word(av, as, au, &rv, &rs, &ru);
         uint8_t got = (uint8_t)(((rv >> L) & 1) * L3D_BIT_VAL
                                 | ((rs >> L) & 1) * L3D_BIT_STR
                                 | ((ru >> L) & 1) * L3D_BIT_UNC);
         if (got != L3D_NOT_LUT[a]) {
            snprintf(errbuf, errlen, "NOT lane %u code %d: got %u want %u",
                     L, a, got, L3D_NOT_LUT[a]);
            return false;
         }

         for (int b = 0; b < 8; b++) {
            const uint64_t bv = (b & L3D_BIT_VAL) ? m : 0;
            const uint64_t bs = (b & L3D_BIT_STR) ? m : 0;
            const uint64_t bu = (b & L3D_BIT_UNC) ? m : 0;

            struct { const char *name; const uint8_t (*lut)[8];
                     void (*fn)(uint64_t,uint64_t,uint64_t,
                                uint64_t,uint64_t,uint64_t,
                                uint64_t*,uint64_t*,uint64_t*); } ops[] = {
               { "AND", L3D_AND_LUT, l3d_plane_and_word },
               { "OR",  L3D_OR_LUT,  l3d_plane_or_word  },
               { "XOR", L3D_XOR_LUT, l3d_plane_xor_word },
            };

            for (size_t o = 0; o < sizeof(ops)/sizeof(ops[0]); o++) {
               (*ops[o].fn)(av, as, au, bv, bs, bu, &rv, &rs, &ru);
               got = (uint8_t)(((rv >> L) & 1) * L3D_BIT_VAL
                               | ((rs >> L) & 1) * L3D_BIT_STR
                               | ((ru >> L) & 1) * L3D_BIT_UNC);
               const uint8_t want = ops[o].lut[a][b];
               if (got != want) {
                  snprintf(errbuf, errlen,
                           "%s lane %u codes %d,%d: got %u want %u",
                           ops[o].name, L, a, b, got, want);
                  return false;
               }
            }
         }
      }
   }

   return true;
}
