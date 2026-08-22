//
//  Copyright (C) 2024  Nick Gasson
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

#ifndef _RT_COPY_H
#define _RT_COPY_H

#include "prim.h"

#include <string.h>

void _copy2(void *p1, void *p2, const void *src, size_t len);

__attribute__((always_inline))
static inline void copy2(void *p1, void *p2, const void *src, size_t len)
{
   if (len == 1) {
      *(unsigned char *)p1 = *(unsigned char *)p2;
      *(unsigned char *)p2 = *(const unsigned char *)src;
   }
   else
      return _copy2(p1, p2, src, len);
}

bool _cmp_bytes(const void *a, const void *b, size_t size)
   __attribute__((pure));

// The common sizes are small fixed widths: logic3d signals are four
// bytes per element with nexuses split per driven element, std_logic
// is one byte.  Compare those with single word loads (memcpy-style —
// the pointers are byte offsets into shared.data and need not be
// aligned) instead of paying the call, the per-call CPU-feature test
// and the masked SSE tail in _cmp_bytes, which also over-reads for
// small sizes.  The size branch predicts near-perfectly: it is
// constant per nexus and effectively constant per call-site stream.
__attribute__((always_inline))
static inline bool cmp_bytes(const void *a, const void *b, size_t size)
{
   switch (size) {
   case 4:
      {
         uint32_t ua, ub;
         memcpy(&ua, a, 4);
         memcpy(&ub, b, 4);
         return ua == ub;
      }
   case 8:
      {
         uint64_t ua, ub;
         memcpy(&ua, a, 8);
         memcpy(&ub, b, 8);
         return ua == ub;
      }
   case 1:
      return *(const unsigned char *)a == *(const unsigned char *)b;
   case 2:
      {
         uint16_t ua, ub;
         memcpy(&ua, a, 2);
         memcpy(&ub, b, 2);
         return ua == ub;
      }
   default:
      return _cmp_bytes(a, b, size);
   }
}

#endif   // _RT_COPY_H
