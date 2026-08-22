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

#include "util.h"
#include "hash.h"
#include "ident.h"
#include "jit/jit-cache.h"
#include "jit/jit-exits.h"
#include "jit/jit-priv.h"
#include "object.h"
#include "option.h"
#include "thread.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#if defined __linux__ && defined ARCH_X86_64
#define JIT_CACHE_SUPPORTED 1
#else
#define JIT_CACHE_SUPPORTED 0
#endif

#if JIT_CACHE_SUPPORTED

#include <dirent.h>
#include <fcntl.h>
#include <link.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

////////////////////////////////////////////////////////////////////////////////
// SHA-256 (FIPS 180-4), compact public-domain-style implementation

typedef struct {
   uint32_t state[8];
   uint64_t length;
   uint8_t  block[64];
   unsigned fill;
} sha256_t;

#define SHA256_LEN 32

static const uint32_t sha256_k[64] = {
   0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
   0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
   0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
   0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
   0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
   0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
   0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
   0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
   0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
   0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
   0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_init(sha256_t *c)
{
   static const uint32_t iv[8] = {
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
   };
   memcpy(c->state, iv, sizeof(iv));
   c->length = 0;
   c->fill = 0;
}

static void sha256_block(sha256_t *c, const uint8_t *p)
{
   uint32_t w[64];
   for (int i = 0; i < 16; i++)
      w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16)
         | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
   for (int i = 16; i < 64; i++) {
      const uint32_t s0 = ROR32(w[i-15], 7) ^ ROR32(w[i-15], 18)
         ^ (w[i-15] >> 3);
      const uint32_t s1 = ROR32(w[i-2], 17) ^ ROR32(w[i-2], 19)
         ^ (w[i-2] >> 10);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
   }

   uint32_t a = c->state[0], b = c->state[1], d3 = c->state[2];
   uint32_t d = c->state[3], e = c->state[4], f = c->state[5];
   uint32_t g = c->state[6], h = c->state[7];

   for (int i = 0; i < 64; i++) {
      const uint32_t s1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = h + s1 + ch + sha256_k[i] + w[i];
      const uint32_t s0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
      const uint32_t mj = (a & b) ^ (a & d3) ^ (b & d3);
      const uint32_t t2 = s0 + mj;
      h = g; g = f; f = e; e = d + t1;
      d = d3; d3 = b; b = a; a = t1 + t2;
   }

   c->state[0] += a; c->state[1] += b; c->state[2] += d3; c->state[3] += d;
   c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static void sha256_update(sha256_t *c, const void *data, size_t len)
{
   const uint8_t *p = data;
   c->length += len;

   if (c->fill > 0) {
      const size_t take = MIN(len, 64 - c->fill);
      memcpy(c->block + c->fill, p, take);
      c->fill += take;
      p += take;
      len -= take;
      if (c->fill == 64) {
         sha256_block(c, c->block);
         c->fill = 0;
      }
   }

   for (; len >= 64; p += 64, len -= 64)
      sha256_block(c, p);

   if (len > 0) {
      memcpy(c->block, p, len);
      c->fill = len;
   }
}

static void sha256_final(sha256_t *c, uint8_t out[SHA256_LEN])
{
   const uint64_t bits = c->length * 8;
   const uint8_t pad = 0x80;
   sha256_update(c, &pad, 1);

   const uint8_t zero = 0;
   while (c->fill != 56)
      sha256_update(c, &zero, 1);

   uint8_t lenbuf[8];
   for (int i = 0; i < 8; i++)
      lenbuf[i] = bits >> (56 - i*8);
   sha256_update(c, lenbuf, 8);

   assert(c->fill == 0);
   for (int i = 0; i < 8; i++) {
      out[4*i]   = c->state[i] >> 24;
      out[4*i+1] = c->state[i] >> 16;
      out[4*i+2] = c->state[i] >> 8;
      out[4*i+3] = c->state[i];
   }
}

////////////////////////////////////////////////////////////////////////////////
// Cache state

#define RECORD_MAGIC   "NJC1"
#define RECORD_VERSION 1
#define MANIFEST_MAGIC 0x4e4a4d31   // 'NJM1'
#define MAX_RECORD_SZ  (64 * 1024 * 1024)
#define DEFAULT_CAP    (UINT64_C(2) * 1024 * 1024 * 1024)
#define DEAD_GEN_AGE   (14 * 24 * 3600)
#define TMP_AGE        3600

typedef struct {
   char     magic[4];
   uint32_t version;
   uint32_t manifest_size;
   uint32_t object_size;
   uint8_t  digest[SHA256_LEN];   // SHA-256 over manifest || object
} record_header_t;

// Verify-mode record held between jit_cache_load and jit_cache_finish;
// per-call because tier-up bursts compile on multiple workers
typedef struct _jit_cache_pending {
   uint8_t *data;      // Whole record file contents
   size_t   size;
} jit_cache_pending_t;

typedef struct _jit_cache {
   jit_t     *jit;
   char      *gendir;      // <root>/<cache-key-prefix> generation directory
   char      *root;        // <root> = $XDG_CACHE_HOME/nvc/jit
   uint8_t    key[SHA256_LEN];
   int        drive_mode;
   bool       writable;    // Cleared on first write failure (relaxed)
   bool       verify;
   bool       stats;
   // Statistics: compiles run concurrently on the worker pool so every
   // counter uses relaxed atomics
   unsigned   n_hits;
   unsigned   n_miss_absent;
   unsigned   n_miss_record;     // bad header/size/digest
   unsigned   n_miss_manifest;   // manifest failed validation
   unsigned   n_miss_probe;      // object failed the fail-soft ELF probe
   unsigned   n_declines;
   unsigned   n_stores;
   unsigned   n_store_errors;
   unsigned   n_unstorable;   // emitted worse than achievable: not frozen
   unsigned   n_forced_fills;
   unsigned   n_verify_ok;
   unsigned   n_verify_fresh;
} jit_cache_t;

////////////////////////////////////////////////////////////////////////////////
// Hash helpers: every component is length- or width-delimited so no two
// distinct inputs can produce the same byte stream

static void hash_u8(sha256_t *c, uint8_t v)  { sha256_update(c, &v, 1); }

static void hash_u32(sha256_t *c, uint32_t v)
{
   uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 };
   sha256_update(c, b, 4);
}

static void hash_u64(sha256_t *c, uint64_t v)
{
   uint8_t b[8];
   for (int i = 0; i < 8; i++) b[i] = v >> (i*8);
   sha256_update(c, b, 8);
}

static void hash_str(sha256_t *c, const char *s)
{
   if (s == NULL)
      hash_u32(c, UINT32_MAX);
   else {
      const size_t len = strlen(s);
      hash_u32(c, len);
      sha256_update(c, s, len);
   }
}

////////////////////////////////////////////////////////////////////////////////
// Build identity (F1, hardened): ELF build-id notes of EVERY object in the
// process collected via dl_iterate_phdr.  The panel's "exe + libLLVM" form
// is insufficient in this tree: bin/nvc is a thin wrapper and ALL nvc code
// (JIT, runtime, this file) lives in libnvc.so, so a rebuild could keep
// the exe build-id while changing codegen -- the F1 misload replayed one
// level down.  Hashing every loaded object's identity over-invalidates on
// e.g. a libc update, which is the safe direction and costs one refill.
// Fallbacks when an object has no build-id note: exe => content hash;
// shared object => basename + size + mtime (better than nothing, and the
// dev toolchain always emits build-ids).

typedef struct {
   sha256_t *hash;
   bool      found_exe;
} buildid_scan_t;

static int build_id_iter_cb(struct dl_phdr_info *info, size_t size, void *ctx)
{
   buildid_scan_t *scan = ctx;

   const char *name = info->dlpi_name;
   const bool is_exe = (name == NULL || name[0] == '\0');

   const char *base = name;
   if (!is_exe) {
      const char *slash = strrchr(name, '/');
      base = slash != NULL ? slash + 1 : name;
   }

   bool found = false;
   for (int i = 0; i < info->dlpi_phnum; i++) {
      const ElfW(Phdr) *ph = &(info->dlpi_phdr[i]);
      if (ph->p_type != PT_NOTE)
         continue;

      const uint8_t *p = (const uint8_t *)(info->dlpi_addr + ph->p_vaddr);
      const uint8_t *end = p + ph->p_memsz;

      while (p + sizeof(ElfW(Nhdr)) <= end) {
         const ElfW(Nhdr) *nh = (const ElfW(Nhdr) *)p;
         const uint8_t *nname = p + sizeof(ElfW(Nhdr));
         const uint8_t *desc = nname + ((nh->n_namesz + 3) & ~3u);
         const uint8_t *next = desc + ((nh->n_descsz + 3) & ~3u);
         if (next > end || desc < p)
            break;

         if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4
             && memcmp(nname, "GNU", 4) == 0 && nh->n_descsz > 0) {
            hash_str(scan->hash, is_exe ? "exe" : base);
            hash_u32(scan->hash, nh->n_descsz);
            sha256_update(scan->hash, desc, nh->n_descsz);
            found = true;
            if (is_exe)
               scan->found_exe = true;
         }

         p = next;
      }
   }

   if (!found && !is_exe) {
      // No build-id note: fold in what identity we can get
      struct stat st;
      hash_str(scan->hash, base);
      if (stat(name, &st) == 0) {
         hash_u64(scan->hash, (uint64_t)st.st_size);
         hash_u64(scan->hash, (uint64_t)st.st_mtim.tv_sec);
         hash_u64(scan->hash, (uint64_t)st.st_mtim.tv_nsec);
      }
   }

   return 0;
}

static bool hash_self_exe_contents(sha256_t *c)
{
   int fd = open("/proc/self/exe", O_RDONLY);
   if (fd < 0)
      return false;

   const size_t bufsz = 1 << 16;
   uint8_t *buf LOCAL = xmalloc(bufsz);
   ssize_t n;
   while ((n = read(fd, buf, bufsz)) > 0)
      sha256_update(c, buf, n);

   close(fd);
   return n == 0;
}

static bool hash_build_identity(sha256_t *c)
{
   // Test hook: substitutes the build identity so poison tests can
   // simulate a rebuild; only ever perturbs the key (always safe)
   const char *spoof = getenv("NVC_JIT_CACHE_TEST_BUILDID");
   if (spoof != NULL) {
      hash_str(c, "test-build-id");
      hash_str(c, spoof);
      return true;
   }

   // Host-specific codegen (-mcpu=native class): a cache shared
   // between machines must key on the target identity or foreign
   // objects would fault with illegal instructions
   hash_str(c, jit_llvm_target_identity());

   buildid_scan_t scan = { .hash = c, .found_exe = false };
   dl_iterate_phdr(build_id_iter_cb, &scan);

   if (!scan.found_exe) {
      // No build-id note in the executable: content-hash it instead
      hash_str(c, "exe-content");
      if (!hash_self_exe_contents(c))
         return false;
   }

   return true;
}

////////////////////////////////////////////////////////////////////////////////
// Drive layout hashing (F5): field-by-field, the four per-process pointer
// members and any padding are excluded -- offsets/masks/constants are the
// drift detectors, addresses are load-resolved via nvc.model/gpar/...

static void hash_drive_layout(sha256_t *c)
{
   const jit_drive_layout_t *l = jit_drive_layout();

   hash_str(c, "drive-layout");
   hash_u8(c, l->valid);
   // model_var/par_active_var/trace_var/fast_driver_fn intentionally
   // NOT hashed: ASLR addresses would force a silent 100% miss
   hash_u32(c, l->signal_shared);
   hash_u32(c, l->signal_nexus);
   hash_u32(c, l->shared_flags);
   hash_u32(c, l->nexus_flags);
   hash_u32(c, l->nexus_size);
   hash_u32(c, l->nexus_n_sources);
   hash_u32(c, l->nexus_width);
   hash_u32(c, l->nexus_active_delta);
   hash_u32(c, l->nexus_sources);
   hash_u32(c, l->source_bits);
   hash_u8(c, l->source_fastqueued_mask);
   hash_u8(c, l->source_was_active_mask);
   hash_u32(c, l->source_when);
   hash_u32(c, l->source_value);
   hash_u32(c, l->model_now);
   hash_u32(c, l->model_iteration);
   hash_u32(c, l->model_next_is_delta);
   hash_u32(c, l->model_probe_member);
   hash_u32(c, l->model_thread0);
   hash_u32(c, l->thread_active_obj);
   hash_u32(c, l->wakeable_bits);
   hash_u8(c, l->wakeable_postponed_mask);
   hash_u32(c, l->driverq_tasks);
   hash_u32(c, l->driverq_count);
   hash_u32(c, l->driverq_max);
   hash_u32(c, l->task_size);
   hash_u32(c, l->task_fn);
   hash_u32(c, l->task_arg);
   hash_u32(c, l->net_f_fast_driver);

   // Test hook: simulates rt-struct drift for poison tests
   const char *flip = getenv("NVC_JIT_CACHE_TEST_LAYOUT");
   if (flip != NULL)
      hash_str(c, flip);
}

////////////////////////////////////////////////////////////////////////////////
// Resolved IR hashing + symbol table construction.  The SAME walk runs on
// the writer (manifest recording) and the reader (validation), and doubles
// as the source of truth for which nvc.* symbols a cached object may
// reference: the loader only resolves names pre-registered here, so a
// corrupt or foreign object can never trigger a fatal resolution path.

static const char *locus_sym_name(text_buf_t *tb, object_t *locus)
{
   ident_t module;
   ptrdiff_t disp;
   object_locus(locus, &module, &disp);

   tb_rewind(tb);
   tb_printf(tb, "nvc.locus.%s:%llx", istr(module), (unsigned long long)disp);
   return tb_get(tb);
}

static void scan_value(jit_t *j, jit_value_t value, sha256_t *c,
                       shash_t *symtab, text_buf_t *tb)
{
   if (c != NULL) hash_u8(c, value.kind);

   switch (value.kind) {
   case JIT_VALUE_INVALID:
      break;
   case JIT_VALUE_REG:
      if (c != NULL) hash_u32(c, value.reg);
      break;
   case JIT_VALUE_INT64:
      if (c != NULL) hash_u64(c, (uint64_t)value.int64);
      break;
   case JIT_VALUE_DOUBLE:
      {
         jit_scalar_t u = { .real = value.dval };
         if (c != NULL) hash_u64(c, (uint64_t)u.integer);
      }
      break;
   case JIT_ADDR_REG:
      if (c != NULL) {
         hash_u32(c, value.reg);
         hash_u32(c, (uint32_t)value.disp);
      }
      break;
   case JIT_ADDR_CPOOL:
   case JIT_ADDR_ABS:
   case JIT_ADDR_COVER:
      if (c != NULL) hash_u64(c, (uint64_t)value.int64);
      break;
   case JIT_VALUE_LABEL:
      if (c != NULL) hash_u32(c, value.label);
      break;
   case JIT_VALUE_HANDLE:
      // Hash handles BY NAME: numbering is per-run lazy-compile order
      if (value.handle == JIT_HANDLE_INVALID) {
         if (c != NULL) hash_str(c, NULL);
      }
      else {
         jit_func_t *hf = jit_get_func(j, value.handle);
         if (c != NULL) hash_str(c, istr(hf->name));
         if (symtab != NULL) {
            tb_rewind(tb);
            tb_printf(tb, "nvc.handle.%s", istr(hf->name));
            // Handle values are biased by +1 so handle 0 does not look
            // like an unresolved symbol; emission subtracts 1
            shash_put(symtab, tb_get(tb),
                      (void *)(uintptr_t)((uint32_t)hf->handle + 1));
         }
      }
      break;
   case JIT_VALUE_EXIT:
      // Raw exit numbers are safe: build identity keys the cache
      if (c != NULL) hash_u32(c, value.exit);
      break;
   case JIT_VALUE_LOC:
      if (c != NULL) {
         hash_str(c, loc_file_str(&value.loc));
         hash_u32(c, value.loc.first_line);
         hash_u32(c, value.loc.first_column);
      }
      break;
   case JIT_VALUE_VPOS:
      if (c != NULL) {
         hash_u32(c, value.vpos.block);
         hash_u32(c, value.vpos.op);
      }
      break;
   case JIT_VALUE_LOCUS:
      if (value.locus == NULL) {
         if (c != NULL) hash_str(c, NULL);
      }
      else {
         ident_t module;
         ptrdiff_t disp;
         object_locus(value.locus, &module, &disp);
         if (c != NULL) {
            hash_str(c, istr(module));
            hash_u64(c, (uint64_t)disp);
         }
         if (symtab != NULL)
            shash_put(symtab, (char *)locus_sym_name(tb, value.locus),
                      value.locus);
      }
      break;
   default:
      if (c != NULL) hash_u64(c, (uint64_t)value.int64);
      break;
   }
}

// Walk one function's resolved IR: optionally hash it, optionally add its
// loader-resolved symbols to `symtab`
static void scan_func(jit_t *j, jit_func_t *f, sha256_t *c, shash_t *symtab)
{
   LOCAL_TEXT_BUF tb = tb_new();

   assert(f->irbuf != NULL);

   if (c != NULL) {
      hash_str(c, "func");
      hash_str(c, istr(f->name));
      hash_u32(c, f->nirs);
      hash_u32(c, f->nregs);
      hash_u32(c, f->nvars);
      hash_u32(c, f->cpoolsz);
      hash_u32(c, f->framesz);
      hash_u64(c, f->spec.bits);

      for (int i = 0; i < f->nvars; i++) {
         hash_str(c, istr(f->linktab[i].name));
         hash_u32(c, f->linktab[i].offset);
      }

      if (f->object == NULL)
         hash_str(c, NULL);
      else {
         ident_t module;
         ptrdiff_t disp;
         object_locus(f->object, &module, &disp);
         hash_str(c, istr(module));
         hash_u64(c, (uint64_t)disp);
      }
   }

   if (symtab != NULL && f->cpool != NULL) {
      tb_rewind(tb);
      tb_printf(tb, "nvc.cpool.%s", istr(f->name));
      shash_put(symtab, tb_get(tb), f->cpool);
   }

   for (int i = 0; i < f->nirs; i++) {
      jit_ir_t *ir = &(f->irbuf[i]);

      if (c != NULL) {
         const uint16_t enc =
            (ir->op << 8) | (ir->size << 5) | (ir->target << 4) | ir->cc;
         hash_u32(c, enc);
         hash_u32(c, ir->result);
      }

      if (symtab != NULL) {
         if (ir->op == J_CALL && ir->arg1.kind == JIT_VALUE_HANDLE
             && ir->arg1.handle != JIT_HANDLE_INVALID) {
            jit_func_t *callee = jit_get_func(j, ir->arg1.handle);
            tb_rewind(tb);
            tb_printf(tb, "nvc.func.%s", istr(callee->name));
            shash_put(symtab, tb_get(tb), callee);
         }
         else if (ir->op == MACRO_GETPRIV
                  && ir->arg1.kind == JIT_VALUE_HANDLE
                  && ir->arg1.handle != JIT_HANDLE_INVALID) {
            jit_func_t *target = jit_get_func(j, ir->arg1.handle);
            tb_rewind(tb);
            tb_printf(tb, "nvc.priv.%s", istr(target->name));
            shash_put(symtab, tb_get(tb),
                      jit_get_privdata_ptr(target->jit, target));
         }
      }

      scan_value(j, ir->arg1, c, symtab, tb);
      scan_value(j, ir->arg2, c, symtab, tb);
   }
}

static void func_ir_hash(jit_t *j, jit_func_t *f, uint8_t out[SHA256_LEN])
{
   sha256_t c;
   sha256_init(&c);
   scan_func(j, f, &c, NULL);
   sha256_final(&c, out);
}

// The four runtime globals baked by the inline-drive body
static void add_drive_globals(shash_t *symtab)
{
   const jit_drive_layout_t *l = jit_drive_layout();
   if (!l->valid)
      return;

   shash_put(symtab, "nvc.model", l->model_var);
   shash_put(symtab, "nvc.gpar", l->par_active_var);
   shash_put(symtab, "nvc.traceon", l->trace_var);
   shash_put(symtab, "nvc.fastdrv", l->fast_driver_fn);
}

bool jit_cache_cacheable_ir(jit_func_t *f)
{
   if (f->irbuf == NULL)
      return false;

   for (int i = 0; i < f->nirs; i++) {
      jit_ir_t *ir = &(f->irbuf[i]);
      for (int a = 0; a < 2; a++) {
         const jit_value_t v = a == 0 ? ir->arg1 : ir->arg2;
         if (v.kind == JIT_ADDR_COVER)
            return false;   // Per-run counter layout (F14)
         else if (v.kind == JIT_ADDR_ABS && v.int64 != 0)
            return false;   // Defensive: irgen emits only null today
      }
   }

   return true;
}

////////////////////////////////////////////////////////////////////////////////
// Manifest: {own resolved-IR hash, ordered inlined-callee (name, hash)
// list, opt level, effective drive mode, helper-body set}.  Validity on
// read = every hash matches the CURRENT run's IR.

static void manifest_build(jit_t *j, jit_func_t *f, jit_func_t **inlined,
                           unsigned ninlined, int opt_level, int drive_mode,
                           uint64_t helper_mask, uint8_t **out, size_t *size)
{
   LOCAL_TEXT_BUF tb = tb_new();   // Used as a growable byte buffer

   uint8_t hdr[4 + 1 + 1 + 2 + 8 + 4];
   uint8_t *p = hdr;
   *p++ = MANIFEST_MAGIC & 0xff;
   *p++ = (MANIFEST_MAGIC >> 8) & 0xff;
   *p++ = (MANIFEST_MAGIC >> 16) & 0xff;
   *p++ = (MANIFEST_MAGIC >> 24) & 0xff;
   *p++ = opt_level;
   *p++ = drive_mode;
   *p++ = 0; *p++ = 0;
   for (int i = 0; i < 8; i++) *p++ = helper_mask >> (i*8);
   const uint32_t nfuncs = ninlined + 1;
   for (int i = 0; i < 4; i++) *p++ = nfuncs >> (i*8);
   tb_catn(tb, (const char *)hdr, sizeof(hdr));

   for (unsigned i = 0; i < nfuncs; i++) {
      jit_func_t *sf = i == 0 ? f : inlined[i - 1];
      const char *name = istr(sf->name);
      const uint32_t len = strlen(name);
      uint8_t lb[4] = { len, len >> 8, len >> 16, len >> 24 };
      tb_catn(tb, (const char *)lb, 4);
      tb_catn(tb, name, len);

      uint8_t hash[SHA256_LEN];
      func_ir_hash(j, sf, hash);
      tb_catn(tb, (const char *)hash, SHA256_LEN);
   }

   *size = tb_len(tb);
   *out = (uint8_t *)tb_claim(tb);
}

typedef struct {
   uint8_t  opt_level;
   uint8_t  drive_mode;
   uint64_t helper_mask;
   uint32_t nfuncs;
   const uint8_t *entries;   // Packed (len, name, hash) records
   size_t   entries_size;
} manifest_view_t;

static bool manifest_parse(const uint8_t *data, size_t size,
                           manifest_view_t *view)
{
   if (size < 20)
      return false;

   const uint32_t magic = data[0] | (data[1] << 8) | (data[2] << 16)
      | ((uint32_t)data[3] << 24);
   if (magic != MANIFEST_MAGIC)
      return false;

   view->opt_level  = data[4];
   view->drive_mode = data[5];

   view->helper_mask = 0;
   for (int i = 0; i < 8; i++)
      view->helper_mask |= (uint64_t)data[8 + i] << (i*8);

   view->nfuncs = 0;
   for (int i = 0; i < 4; i++)
      view->nfuncs |= (uint32_t)data[16 + i] << (i*8);

   view->entries = data + 20;
   view->entries_size = size - 20;

   if (view->nfuncs == 0 || view->nfuncs > 10000)
      return false;

   // Verify the packed entries are exactly consistent with nfuncs
   const uint8_t *p = view->entries, *end = view->entries + view->entries_size;
   for (uint32_t i = 0; i < view->nfuncs; i++) {
      if (p + 4 > end) return false;
      const uint32_t len = p[0] | (p[1] << 8) | (p[2] << 16)
         | ((uint32_t)p[3] << 24);
      if (len > 4096 || p + 4 + len + SHA256_LEN > end) return false;
      p += 4 + len + SHA256_LEN;
   }

   return p == end;
}

static const uint8_t *manifest_entry(const manifest_view_t *view, uint32_t nth,
                                     const char **name, uint32_t *namelen,
                                     const uint8_t **hash)
{
   const uint8_t *p = view->entries;
   for (uint32_t i = 0; i < nth; i++) {
      const uint32_t len = p[0] | (p[1] << 8) | (p[2] << 16)
         | ((uint32_t)p[3] << 24);
      p += 4 + len + SHA256_LEN;
   }

   const uint32_t len = p[0] | (p[1] << 8) | (p[2] << 16)
      | ((uint32_t)p[3] << 24);
   *namelen = len;
   *name = (const char *)(p + 4);
   *hash = p + 4 + len;
   return p;
}

////////////////////////////////////////////////////////////////////////////////
// Fail-soft IR fill: never fatal for names this design cannot supply

static bool safe_fill(jit_t *j, jit_func_t *f, jit_cache_t *jc)
{
   const func_state_t state = load_acquire(&f->state);

   if (state == JIT_FUNC_READY && f->irbuf != NULL)
      return true;
   else if (state == JIT_FUNC_ERROR || state == JIT_FUNC_COMPILING)
      return false;   // COMPILING => miss is the safe direction

   // Only fill from the design pack: jit_fill_irbuf is fatal when a body
   // is missing entirely, which a foreign manifest name must not trigger
   jit_pack_t *pack = jit_get_pack(j);
   if (pack == NULL || !jit_pack_has(pack, f->name))
      return false;

   relaxed_add(&jc->n_forced_fills, 1);
   jit_fill_irbuf(f);

   return load_acquire(&f->state) == JIT_FUNC_READY && f->irbuf != NULL;
}

////////////////////////////////////////////////////////////////////////////////
// Store layout: <root>/<gen>/<2hex>/<filekey32hex>-<manifest8hex>.o where
// <gen> is the first 16 hex chars of the cache key -- a generation is a
// directory, so dead-build sweeps are directory-level (F9)

static void to_hex(const uint8_t *bytes, size_t n, char *out)
{
   static const char digits[] = "0123456789abcdef";
   for (size_t i = 0; i < n; i++) {
      out[2*i]   = digits[bytes[i] >> 4];
      out[2*i+1] = digits[bytes[i] & 0xf];
   }
   out[2*n] = '\0';
}

static void file_key(jit_cache_t *jc, jit_func_t *f, char out[33])
{
   sha256_t c;
   sha256_init(&c);
   sha256_update(&c, jc->key, SHA256_LEN);
   hash_str(&c, istr(f->name));

   uint8_t digest[SHA256_LEN];
   sha256_final(&c, digest);
   to_hex(digest, 16, out);
}

static bool mkdir_soft(const char *path)
{
   if (mkdir(path, 0700) == 0 || errno == EEXIST)
      return true;
   return false;
}

static void record_digest(const uint8_t *manifest, size_t msize,
                          const uint8_t *object, size_t osize,
                          uint8_t out[SHA256_LEN])
{
   sha256_t c;
   sha256_init(&c);
   sha256_update(&c, manifest, msize);
   sha256_update(&c, object, osize);
   sha256_final(&c, out);
}

static void jit_cache_store(jit_cache_t *jc, jit_func_t *f,
                            const uint8_t *manifest, size_t msize,
                            const void *object, size_t osize)
{
   if (!relaxed_load(&jc->writable))
      return;

   char fkey[33];
   file_key(jc, f, fkey);

   uint8_t mh[SHA256_LEN];
   sha256_t c;
   sha256_init(&c);
   sha256_update(&c, manifest, msize);
   sha256_final(&c, mh);

   char mh8[9];
   to_hex(mh, 4, mh8);

   char *dir LOCAL = xasprintf("%s/%c%c", jc->gendir, fkey[0], fkey[1]);
   if (!mkdir_soft(dir))
      goto fail;

   char *tmp LOCAL = xasprintf("%s/tmpXXXXXX", dir);
   int fd = mkstemp(tmp);
   if (fd < 0)
      goto fail;

   record_header_t hdr;
   memcpy(hdr.magic, RECORD_MAGIC, 4);
   hdr.version = RECORD_VERSION;
   hdr.manifest_size = msize;
   hdr.object_size = osize;
   record_digest(manifest, msize, object, osize, hdr.digest);

   bool ok = write(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)
      && write(fd, manifest, msize) == (ssize_t)msize
      && write(fd, object, osize) == (ssize_t)osize
      && fsync(fd) == 0;

   close(fd);

   if (!ok) {
      unlink(tmp);
      goto fail;
   }

   char *final LOCAL = xasprintf("%s/%s-%s.o", dir, fkey, mh8);
   if (rename(tmp, final) != 0) {
      unlink(tmp);
      goto fail;
   }

   relaxed_add(&jc->n_stores, 1);
   return;

 fail:
   // First failure of any kind disables writes for the rest of the run
   // (ENOSPC/EDQUOT/read-only dir); reads stay enabled
   relaxed_store(&jc->writable, false);
   relaxed_add(&jc->n_store_errors, 1);
}

////////////////////////////////////////////////////////////////////////////////
// Record validation + load

// Read the whole record into a private buffer: NO long-lived mmap (F8),
// so a concurrent prune/unlink can never SIGBUS a reader
static uint8_t *read_record(const char *path, size_t *size)
{
   int fd = open(path, O_RDONLY);
   if (fd < 0)
      return NULL;

   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0
       || st.st_size > MAX_RECORD_SZ) {
      close(fd);
      return NULL;
   }

   uint8_t *buf = xmalloc(st.st_size);
   size_t off = 0;
   while (off < (size_t)st.st_size) {
      const ssize_t n = pread(fd, buf + off, st.st_size - off, off);
      if (n <= 0) {
         free(buf);
         close(fd);
         return NULL;
      }
      off += n;
   }

   close(fd);
   *size = st.st_size;
   return buf;
}

// Parse + digest-check a record buffer; returns pointers into it
static bool record_parse(const uint8_t *data, size_t size,
                         const uint8_t **manifest, size_t *msize,
                         const uint8_t **object, size_t *osize)
{
   if (size < sizeof(record_header_t))
      return false;

   const record_header_t *hdr = (const record_header_t *)data;
   if (memcmp(hdr->magic, RECORD_MAGIC, 4) != 0
       || hdr->version != RECORD_VERSION)
      return false;

   if ((uint64_t)hdr->manifest_size + hdr->object_size
       + sizeof(record_header_t) != size)
      return false;   // Truncated or padded: MISS

   *manifest = data + sizeof(record_header_t);
   *msize = hdr->manifest_size;
   *object = *manifest + hdr->manifest_size;
   *osize = hdr->object_size;

   uint8_t digest[SHA256_LEN];
   record_digest(*manifest, *msize, *object, *osize, digest);

   return memcmp(digest, hdr->digest, SHA256_LEN) == 0;
}

// Validate the manifest against the CURRENT run's IR, building the
// loader symbol table from the reader's own resolved IR on success.
// Any doubt (unknown callee, unfillable IR, hash mismatch) is a miss.
static shash_t *manifest_validate(jit_cache_t *jc, jit_func_t *f,
                                  const manifest_view_t *view)
{
   jit_t *j = jc->jit;

   if (view->drive_mode != jc->drive_mode)
      return NULL;   // Defensive: the cache key already covers this

   // Entry 0 must be this function with matching resolved-IR hash
   const char *name0;
   uint32_t len0;
   const uint8_t *hash0;
   manifest_entry(view, 0, &name0, &len0, &hash0);

   const char *fname = istr(f->name);
   if (strlen(fname) != len0 || memcmp(fname, name0, len0) != 0)
      return NULL;

   uint8_t cur[SHA256_LEN];
   func_ir_hash(j, f, cur);
   if (memcmp(cur, hash0, SHA256_LEN) != 0)
      return NULL;

   jit_func_t **callees LOCAL =
      xmalloc_array(view->nfuncs, sizeof(jit_func_t *));
   callees[0] = f;

   for (uint32_t i = 1; i < view->nfuncs; i++) {
      const char *name;
      uint32_t len;
      const uint8_t *hash;
      manifest_entry(view, i, &name, &len, &hash);

      char *cname LOCAL = xmalloc(len + 1);
      memcpy(cname, name, len);
      cname[len] = '\0';

      jit_handle_t handle = jit_lazy_compile(j, ident_new(cname));
      if (handle == JIT_HANDLE_INVALID)
         return NULL;

      jit_func_t *cf = jit_get_func(j, handle);
      if (!safe_fill(j, cf, jc))
         return NULL;

      func_ir_hash(j, cf, cur);
      if (memcmp(cur, hash, SHA256_LEN) != 0)
         return NULL;

      callees[i] = cf;
   }

   // Every hash matched: pre-resolve the complete symbol namespace from
   // the reader's own IR (validate-before-mutate, F11)
   shash_t *symtab = shash_new(64);
   for (uint32_t i = 0; i < view->nfuncs; i++)
      scan_func(j, callees[i], NULL, symtab);
   add_drive_globals(symtab);

   return symtab;
}

void *jit_cache_resolve(const char *name, void *ctx)
{
   return shash_get((shash_t *)ctx, name);
}

static int name_compare(const void *a, const void *b)
{
   return strcmp(*(const char **)a, *(const char **)b);
}

jit_cache_status_t jit_cache_load(jit_cache_t *jc, code_cache_t *code,
                                  jit_func_t *f,
                                  jit_cache_pending_t **pending)
{
   *pending = NULL;

   if (jc == NULL)
      return JIT_CACHE_MISS;

   char fkey[33];
   file_key(jc, f, fkey);

   char *dir LOCAL = xasprintf("%s/%c%c", jc->gendir, fkey[0], fkey[1]);

   DIR *d = opendir(dir);
   if (d == NULL) {
      relaxed_add(&jc->n_miss_absent, 1);
      return JIT_CACHE_MISS;
   }

   // Collect matching variant files (same function, different inline
   // closure); scan in sorted order for determinism
   char *names[16];
   unsigned nnames = 0;
   const struct dirent *de;
   while ((de = readdir(d)) != NULL && nnames < ARRAY_LEN(names)) {
      if (strncmp(de->d_name, fkey, 32) == 0 && de->d_name[32] == '-')
         names[nnames++] = xstrdup(de->d_name);
   }
   closedir(d);

   if (nnames == 0) {
      relaxed_add(&jc->n_miss_absent, 1);
      return JIT_CACHE_MISS;
   }

   qsort(names, nnames, sizeof(char *), name_compare);

   jit_cache_status_t status = JIT_CACHE_MISS;
   bool counted = false;

   for (unsigned i = 0; i < nnames && status == JIT_CACHE_MISS; i++) {
      char *path LOCAL = xasprintf("%s/%s", dir, names[i]);

      size_t size;
      uint8_t *data = read_record(path, &size);
      if (data == NULL) {
         if (!counted) { relaxed_add(&jc->n_miss_record, 1); counted = true; }
         continue;
      }

      const uint8_t *manifest, *object;
      size_t msize, osize;
      if (!record_parse(data, size, &manifest, &msize, &object, &osize)) {
         if (!counted) { relaxed_add(&jc->n_miss_record, 1); counted = true; }
         free(data);
         continue;
      }

      manifest_view_t view;
      if (!manifest_parse(manifest, msize, &view)) {
         if (!counted) { relaxed_add(&jc->n_miss_record, 1); counted = true; }
         free(data);
         continue;
      }

      shash_t *symtab = manifest_validate(jc, f, &view);
      if (symtab == NULL) {
         if (!counted) {
            relaxed_add(&jc->n_miss_manifest, 1);
            counted = true;
         }
         free(data);
         continue;
      }

      // Validate-before-mutate (F11): full fail-soft probe of the ELF
      // object -- bounds, reloc whitelist, symbol coverage -- before any
      // code blob is written
      if (!code_object_probe(code, f->name, object, osize,
                             jit_cache_resolve, symtab)) {
         relaxed_add(&jc->n_miss_probe, 1);
         counted = true;
         shash_free(symtab);
         free(data);
         continue;
      }

      if (jc->verify) {
         // Hold the record; the caller continues with a fresh cgen and
         // jit_cache_finish compares the two (F13: whole object)
         shash_free(symtab);
         jit_cache_pending_t *p = xcalloc(sizeof(jit_cache_pending_t));
         p->data = data;
         p->size = size;
         *pending = p;
         status = JIT_CACHE_VERIFY;
         break;
      }

      if (jit_is_shutdown(f->jit)) {
         shash_free(symtab);
         free(data);
         break;
      }

      // Publish exactly as the fresh path does: blob + load + finalise
      // (store_release entry + entry-watch notify for fused blocks)
      code_blob_t *blob = code_blob_new(code, f->name, osize);
      if (blob != NULL) {
         code_load_object(blob, object, osize, jit_cache_resolve, symtab);
         code_blob_finalise(blob, &(f->entry));

         relaxed_add(&jc->n_hits, 1);
         status = JIT_CACHE_HIT;

         // Touch-on-hit so mtime pruning is true LRU (F9); best-effort
         utimensat(AT_FDCWD, path, NULL, 0);
      }

      shash_free(symtab);
      free(data);
   }

   for (unsigned i = 0; i < nnames; i++)
      free(names[i]);

   if (status == JIT_CACHE_MISS && !counted)
      relaxed_add(&jc->n_miss_absent, 1);

   return status;
}

shash_t *jit_cache_finish(jit_cache_t *jc, jit_func_t *f,
                          jit_func_t **inlined, unsigned ninlined,
                          int opt_level, uint64_t helper_mask,
                          const void *obj_data, size_t obj_size,
                          bool storable, jit_cache_pending_t *pending)
{
   if (jc == NULL)
      return NULL;

   uint8_t *manifest;
   size_t msize;
   manifest_build(jc->jit, f, inlined, ninlined, opt_level, jc->drive_mode,
                  helper_mask, &manifest, &msize);

   bool store = true;

   if (pending != NULL) {
      // NVC_JIT_CACHE_VERIFY: compare the held record with the fresh
      // emission.  Equal manifests must mean byte-identical objects.
      const uint8_t *cmanifest, *cobject;
      size_t cmsize, cosize;
      if (record_parse(pending->data, pending->size, &cmanifest,
                       &cmsize, &cobject, &cosize)) {
         if (cmsize == msize && memcmp(cmanifest, manifest, msize) == 0) {
            if (cosize != obj_size
                || memcmp(cobject, obj_data, obj_size) != 0)
               fatal_trace("JIT cache verify FAILED for %s: cached object "
                           "(%zu bytes) differs from fresh emission "
                           "(%zu bytes) under an equal manifest",
                           istr(f->name), cosize, obj_size);
            relaxed_add(&jc->n_verify_ok, 1);
            store = false;   // Byte-identical record already on disk
         }
         else {
            // Legitimate inline-closure race: report, use FRESH
            debugf("JIT cache verify: manifest differs for %s "
                   "(inline closure race); using fresh code", istr(f->name));
            relaxed_add(&jc->n_verify_fresh, 1);
         }
      }
   }

   if (store && !storable)
      relaxed_add(&jc->n_unstorable, 1);
   else if (store)
      jit_cache_store(jc, f, manifest, msize, obj_data, obj_size);

   free(manifest);

   // Symbol table for publishing the fresh object
   shash_t *symtab = shash_new(64);
   scan_func(jc->jit, f, NULL, symtab);
   for (unsigned i = 0; i < ninlined; i++)
      scan_func(jc->jit, inlined[i], NULL, symtab);
   add_drive_globals(symtab);

   return symtab;
}

void jit_cache_pending_free(jit_cache_pending_t *pending)
{
   if (pending != NULL) {
      free(pending->data);
      free(pending);
   }
}

void jit_cache_decline(jit_cache_t *jc)
{
   if (jc != NULL)
      relaxed_add(&jc->n_declines, 1);
}

////////////////////////////////////////////////////////////////////////////////
// Startup GC (F9): sweep dead generations, orphaned tmp files, and
// enforce a default size cap with LRU (mtime, touched on hit) eviction

typedef struct {
   char    *path;
   time_t   mtime;
   uint64_t size;
} gc_entry_t;

static void gc_scan_dir(const char *dir, gc_entry_t **files, unsigned *nfiles,
                        unsigned *maxfiles, time_t *newest, time_t now)
{
   DIR *d = opendir(dir);
   if (d == NULL)
      return;

   const struct dirent *de;
   while ((de = readdir(d)) != NULL) {
      if (de->d_name[0] == '.')
         continue;

      char *path = xasprintf("%s/%s", dir, de->d_name);
      struct stat st;
      if (lstat(path, &st) != 0) {
         free(path);
         continue;
      }

      if (S_ISDIR(st.st_mode)) {
         gc_scan_dir(path, files, nfiles, maxfiles, newest, now);
         free(path);
      }
      else if (S_ISREG(st.st_mode)) {
         if (strncmp(de->d_name, "tmp", 3) == 0
             && now - st.st_mtime > TMP_AGE) {
            unlink(path);   // Orphan from a crashed writer
            free(path);
            continue;
         }

         if (newest != NULL && st.st_mtime > *newest)
            *newest = st.st_mtime;

         if (*nfiles == *maxfiles) {
            *maxfiles = MAX(64, *maxfiles * 2);
            *files = xrealloc_array(*files, *maxfiles, sizeof(gc_entry_t));
         }
         (*files)[(*nfiles)++] = (gc_entry_t){
            .path = path, .mtime = st.st_mtime, .size = st.st_size
         };
      }
      else
         free(path);
   }

   closedir(d);
}

static void gc_remove_tree(const char *dir)
{
   DIR *d = opendir(dir);
   if (d != NULL) {
      const struct dirent *de;
      while ((de = readdir(d)) != NULL) {
         if (de->d_name[0] == '.')
            continue;
         char *path LOCAL = xasprintf("%s/%s", dir, de->d_name);
         struct stat st;
         if (lstat(path, &st) != 0)
            continue;
         if (S_ISDIR(st.st_mode))
            gc_remove_tree(path);
         else
            unlink(path);
      }
      closedir(d);
   }
   rmdir(dir);
}

static int gc_mtime_compare(const void *a, const void *b)
{
   const gc_entry_t *ea = a, *eb = b;
   if (ea->mtime < eb->mtime) return -1;
   if (ea->mtime > eb->mtime) return 1;
   return strcmp(ea->path, eb->path);
}

static void jit_cache_gc(jit_cache_t *jc)
{
   const time_t now = time(NULL);
   const char *gen_name = strrchr(jc->gendir, '/');
   assert(gen_name != NULL);
   gen_name++;

   uint64_t cap = DEFAULT_CAP;
   const char *max = getenv("NVC_JIT_CACHE_MAX");
   if (max != NULL)
      cap = strtoull(max, NULL, 10);   // 0 = unlimited

   gc_entry_t *files = NULL;
   unsigned nfiles = 0, maxfiles = 0;

   DIR *d = opendir(jc->root);
   if (d == NULL)
      return;

   const struct dirent *de;
   while ((de = readdir(d)) != NULL) {
      if (de->d_name[0] == '.')
         continue;

      char *path LOCAL = xasprintf("%s/%s", jc->root, de->d_name);
      struct stat st;
      if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode))
         continue;

      const bool live = strcmp(de->d_name, gen_name) == 0;

      time_t newest = 0;
      const unsigned before = nfiles;
      gc_scan_dir(path, &files, &nfiles, &maxfiles, &newest, now);

      if (!live && now - newest > DEAD_GEN_AGE) {
         // Aggressive sweep of dead generations (stale nvc builds)
         gc_remove_tree(path);
         for (unsigned i = before; i < nfiles; i++)
            free(files[i].path);
         nfiles = before;
      }
   }
   closedir(d);

   if (cap > 0) {
      uint64_t total = 0;
      for (unsigned i = 0; i < nfiles; i++)
         total += files[i].size;

      if (total > cap) {
         qsort(files, nfiles, sizeof(gc_entry_t), gc_mtime_compare);
         for (unsigned i = 0; i < nfiles && total > cap; i++) {
            if (unlink(files[i].path) == 0 || errno == ENOENT)
               total -= files[i].size;
         }
      }
   }

   for (unsigned i = 0; i < nfiles; i++)
      free(files[i].path);
   free(files);
}

////////////////////////////////////////////////////////////////////////////////
// Open / close

static char *cache_root_dir(void)
{
   const char *override = getenv("NVC_JIT_CACHE_DIR");
   if (override != NULL && override[0] != '\0')
      return xasprintf("%s", override);

   const char *xdg = getenv("XDG_CACHE_HOME");
   if (xdg != NULL && xdg[0] != '\0')
      return xasprintf("%s/nvc/jit", xdg);

   const char *home = getenv("HOME");
   if (home != NULL && home[0] != '\0')
      return xasprintf("%s/.cache/nvc/jit", home);

   // No $XDG_CACHE_HOME and no $HOME: cache OFF entirely -- never any
   // shared fallback path (the accel /tmp precedent is a multi-user hole)
   return NULL;
}

jit_cache_t *jit_cache_open(jit_t *j, const char *triple, int code_model,
                            int drive_mode)
{
   const char *env = getenv("NVC_JIT_CACHE");
   if (env != NULL && env[0] == '0' && env[1] == '\0')
      return NULL;   // NVC_JIT_CACHE=0 is the escape hatch

   char *root = cache_root_dir();
   if (root == NULL)
      return NULL;

   sha256_t c;
   sha256_init(&c);
   hash_str(&c, "NVCJITCACHE1");   // Key format tag

   if (!hash_build_identity(&c)) {
      free(root);
      return NULL;   // Cannot establish build identity: cache off
   }

   // Human-readable garnish only; the build-id is the load-bearing part
   hash_str(&c, PACKAGE_STRING);
   hash_str(&c, LLVM_VERSION);
   hash_str(&c, triple);
   hash_u32(&c, code_model);
   hash_drive_layout(&c);
   hash_u32(&c, drive_mode);

   jit_cache_t *jc = xcalloc(sizeof(jit_cache_t));
   jc->jit        = j;
   jc->root       = root;
   jc->drive_mode = drive_mode;
   sha256_final(&c, jc->key);

   char gen[17];
   to_hex(jc->key, 8, gen);
   jc->gendir = xasprintf("%s/%s", root, gen);

   // Create the directory chain with 0700 (never group/world writable)
   for (char *p = jc->gendir + 1; *p != '\0'; p++) {
      if (*p == '/') {
         *p = '\0';
         mkdir(jc->gendir, 0700);   // Prefixes may exist with other modes
         *p = '/';
      }
   }
   jc->writable = mkdir_soft(jc->gendir);   // Read-only: reads still allowed

   const char *verify = getenv("NVC_JIT_CACHE_VERIFY");
   jc->verify = verify != NULL && atoi(verify) != 0;

   const char *stats = getenv("NVC_JIT_CACHE_STATS");
   jc->stats = stats != NULL && atoi(stats) != 0;

   if (jc->writable)
      jit_cache_gc(jc);

   return jc;
}

void jit_cache_close(jit_cache_t *jc)
{
   if (jc == NULL)
      return;

   if (jc->stats) {
      fprintf(stderr, "NVC_JIT_CACHE_STATS: hits=%u misses=%u (absent=%u "
              "record=%u manifest=%u probe=%u) declines=%u stores=%u "
              "store_errors=%u unstorable=%u forced_fills=%u verify_ok=%u verify_fresh=%u\n",
              jc->n_hits,
              jc->n_miss_absent + jc->n_miss_record + jc->n_miss_manifest
              + jc->n_miss_probe,
              jc->n_miss_absent, jc->n_miss_record, jc->n_miss_manifest,
              jc->n_miss_probe, jc->n_declines, jc->n_stores,
              jc->n_store_errors, jc->n_unstorable, jc->n_forced_fills,
              jc->n_verify_ok, jc->n_verify_fresh);
      fflush(stderr);
   }

   free(jc->gendir);
   free(jc->root);
   free(jc);
}

#else   // !JIT_CACHE_SUPPORTED

// Other architectures/platforms: compile-time unsupported means the cache
// is simply off (every lookup is a miss), never fatal (F12)

jit_cache_t *jit_cache_open(jit_t *j, const char *triple, int code_model,
                            int drive_mode)
{
   return NULL;
}

void jit_cache_close(jit_cache_t *jc) { }

bool jit_cache_cacheable_ir(jit_func_t *f) { return false; }

void jit_cache_decline(jit_cache_t *jc) { }

jit_cache_status_t jit_cache_load(jit_cache_t *jc, code_cache_t *code,
                                  jit_func_t *f,
                                  jit_cache_pending_t **pending)
{
   *pending = NULL;
   return JIT_CACHE_MISS;
}

shash_t *jit_cache_finish(jit_cache_t *jc, jit_func_t *f,
                          jit_func_t **inlined, unsigned ninlined,
                          int opt_level, uint64_t helper_mask,
                          const void *obj_data, size_t obj_size,
                          jit_cache_pending_t *pending)
{
   return NULL;
}

void jit_cache_pending_free(jit_cache_pending_t *pending) { }

void *jit_cache_resolve(const char *name, void *ctx) { return NULL; }

#endif  // JIT_CACHE_SUPPORTED
