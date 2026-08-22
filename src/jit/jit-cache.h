//
//  Copyright (C) 2026  Nick Gasson
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

#ifndef _JIT_CACHE_H
#define _JIT_CACHE_H

// Persistent JIT native-code cache.
//
// Stores the relocatable ELF object emitted by the LLVM tier together
// with a validation MANIFEST so a later run of the SAME nvc binary can
// replay code_blob_new + code_load_object + code_blob_finalise instead
// of re-running LLVM.  PRIME DIRECTIVE: any doubt is a MISS (recompile);
// a miss costs ~100ms of worker CPU, a misload breaks the promise.
//
// Key facts (see design doc):
//  - The cache key is fully DERIVED: ELF build-id of the running binary
//    (+ libLLVM) via dl_iterate_phdr, LLVM version, target triple, code
//    model, the pointer-sanitized jit_drive_layout_t blob, and the
//    effective NVC_INLINE_DRIVE mode.  No hand-bumped versions.
//  - Lookup key is (cache_key, function name); the manifest with the
//    writer's race-dependent inline closure lives INSIDE the record and
//    is validated against the CURRENT run's IR after reading.
//  - Records carry a SHA-256 digest over manifest+object, verified
//    before any parsing of the object.
//  - Loads are validate-before-mutate: every symbol is pre-resolved
//    from the reader's own validated IR into a private symbol table and
//    the ELF object is fully probed (bounds, reloc whitelist, symbol
//    coverage) before the code blob is touched.
//  - linux/x86_64 only this landing; elsewhere jit_cache_open returns
//    NULL and everything misses (never fatal).
//
// Lock order note (F11): jit_cache_load/finish run on JIT compile
// contexts.  NOTE a tier-up burst creates MULTIPLE workers (async_do
// grows the pool to npending+1, thread.c) so loads/stores for DIFFERENT
// functions run CONCURRENTLY: statistics are relaxed atomics, verify
// state is per-call (jit_cache_pending_t), and the store is one atomic
// rename per file.  Symbol-table construction may take jit_t.lock
// (jit_lazy_compile) and library/object-store locks via jit_fill_irbuf;
// all of that happens BEFORE code_blob_new so no code_cache_t lock is
// ever held while resolving.  The code cache lock is only taken inside
// code_blob_* which never calls back into the cache.

#include "util.h"
#include "jit/jit-priv.h"

typedef struct _jit_cache jit_cache_t;
typedef struct _jit_cache_pending jit_cache_pending_t;

typedef enum {
   JIT_CACHE_MISS,     // No valid record: continue with a fresh compile
   JIT_CACHE_HIT,      // Cached code was published; nothing more to do
   JIT_CACHE_VERIFY,   // Verify mode: record held in *pending, continue
                       // with fresh cgen then jit_cache_finish compares
} jit_cache_status_t;

// Returns NULL when the cache is disabled (env, platform, no cache dir)
jit_cache_t *jit_cache_open(jit_t *j, const char *triple, int code_model,
                            int drive_mode);
void jit_cache_close(jit_cache_t *jc);

// True if the function's IR bakes no per-run values that have no
// symbolic form (coverage counters, non-null JIT_ADDR_ABS)
bool jit_cache_cacheable_ir(jit_func_t *f);

// Count a function excluded from caching (stats only)
void jit_cache_decline(jit_cache_t *jc);

jit_cache_status_t jit_cache_load(jit_cache_t *jc, code_cache_t *code,
                                  jit_func_t *f,
                                  jit_cache_pending_t **pending);

// After a fresh cgen of a cacheable function: in verify mode compares
// the held record against the fresh object (fatal on divergence with
// equal manifests), otherwise stores a new record.  Returns the symbol
// table for publishing the fresh object (caller frees with shash_free).
// The pending record is NOT consumed: release it with
// jit_cache_pending_free on every exit path.
// storable=false still resolves symbols (and still verifies against a held
// record) but must NOT write the object: the caller knows this module came
// out worse than the same input could produce, and a cache that froze it
// would replay the inferior code on every later run
shash_t *jit_cache_finish(jit_cache_t *jc, jit_func_t *f,
                          jit_func_t **inlined, unsigned ninlined,
                          int opt_level, uint64_t helper_mask,
                          const void *obj_data, size_t obj_size,
                          bool storable, jit_cache_pending_t *pending);

void jit_cache_pending_free(jit_cache_pending_t *pending);

// code_resolve_fn_t adapter: ctx is the shash_t returned by
// jit_cache_finish (or built internally on the hit path)
void *jit_cache_resolve(const char *name, void *ctx);

#endif  // _JIT_CACHE_H
