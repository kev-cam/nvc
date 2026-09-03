//
//  Copyright (C) 2022-2024  Nick Gasson
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
#include "cpustate.h"
#include "debug.h"
#include "hash.h"
#include "ident.h"
#include "jit/jit-priv.h"
#include "option.h"
#include "printf.h"
#include "thread.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>

#if defined __MINGW32__
#include <winnt.h>
#elif defined __APPLE__
#include <mach-o/loader.h>
#include <mach-o/reloc.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>
#include <mach-o/arm64/reloc.h>
#include <mach-o/x86_64/reloc.h>
#else
#include <elf.h>
#endif

#ifdef HAVE_CAPSTONE
#include <capstone.h>
#endif

#ifndef R_AARCH64_MOVW_UABS_G0_NC
#define R_AARCH64_MOVW_UABS_G0_NC 264
#endif

#ifndef R_AARCH64_MOVW_UABS_G1_NC
#define R_AARCH64_MOVW_UABS_G1_NC 266
#endif

#ifndef R_AARCH64_MOVW_UABS_G2_NC
#define R_AARCH64_MOVW_UABS_G2_NC 268
#endif

#ifndef R_AARCH64_MOVW_UABS_G3
#define R_AARCH64_MOVW_UABS_G3 269
#endif

#ifndef SHT_X86_64_UNWIND
#define SHT_X86_64_UNWIND 0x70000001
#endif

#ifndef IMAGE_REL_ARM64_BRANCH26
#define IMAGE_REL_ARM64_BRANCH26 0x03
#endif

#ifndef IMAGE_REL_ARM64_ADDR32NB
#define IMAGE_REL_ARM64_ADDR32NB 0x02
#endif

#ifndef IMAGE_REL_ARM64_PAGEBASE_REL21
#define IMAGE_REL_ARM64_PAGEBASE_REL21 0x04
#endif

#ifndef IMAGE_REL_ARM64_PAGEOFFSET_12A
#define IMAGE_REL_ARM64_PAGEOFFSET_12A 0x06
#endif

#ifndef IMAGE_REL_ARM64_PAGEOFFSET_12L
#define IMAGE_REL_ARM64_PAGEOFFSET_12L 0x07
#endif

#define CODE_PAGE_ALIGN   4096
#define CODE_PAGE_SIZE    0x400000
#define THREAD_CACHE_SIZE 0x10000
#define CODE_BLOB_ALIGN   256
#define MIN_BLOB_SIZE     0x4000

#define __IMM64(x) __IMM32(x), __IMM32((x) >> 32)
#define __IMM32(x) __IMM16(x), __IMM16((x) >> 16)
#define __IMM16(x) (x) & 0xff, ((x) >> 8) & 0xff

STATIC_ASSERT(MIN_BLOB_SIZE <= THREAD_CACHE_SIZE);
STATIC_ASSERT(MIN_BLOB_SIZE % CODE_BLOB_ALIGN == 0);
STATIC_ASSERT(CODE_PAGE_SIZE % THREAD_CACHE_SIZE == 0);

typedef struct _code_page code_page_t;

typedef struct {
   uintptr_t  addr;
   char      *text;
} code_comment_t;

typedef struct {
   unsigned        count;
   unsigned        max;
   code_comment_t *comments;
} code_debug_t;

typedef struct _code_span {
   code_cache_t *owner;
   code_span_t  *next;
   ident_t       name;
   uint8_t      *base;
   void         *entry;
   size_t        entry_size;   // extent of the entry STT_FUNC symbol (0 if unknown)
   size_t        size;
#ifdef DEBUG
   code_debug_t  debug;
#endif
} code_span_t;

typedef struct _patch_list {
   patch_list_t    *next;
   uint8_t         *wptr;
   jit_label_t      label;
   code_patch_fn_t  fn;
} patch_list_t;

typedef struct _code_page {
   code_cache_t *owner;
   code_page_t  *next;
   uint8_t      *mem;
} code_page_t;

typedef struct _code_cache {
   nvc_lock_t   lock;
   code_page_t *pages;
   code_span_t *spans;
   code_span_t *freelist[MAX_THREADS];
   code_span_t *globalfree;
   shash_t     *symbols;
   FILE        *perfmap;
#ifdef HAVE_CAPSTONE
   csh          capstone;
#endif
#ifdef DEBUG
   size_t       used;
#endif
} code_cache_t;

static void code_disassemble(code_span_t *span, uintptr_t mark,
                             struct cpu_state *cpu);

static void code_cache_unwinder(uintptr_t addr, debug_frame_t *frame,
                                void *context)
{
   code_cache_t *code = context;

   const uint8_t *pc = (uint8_t *)addr;
   for (code_span_t *span = code->spans; span; span = span->next) {
      if (pc >= span->base && pc < span->base + span->size) {
         frame->kind = FRAME_VHDL;
         frame->disp = pc - span->base;
         frame->symbol = istr(span->name);
      }
   }
}

static void code_fault_handler(int sig, void *addr, struct cpu_state *cpu,
                               void *context)
{
   code_page_t *page = context;

   const uint8_t *pc = (uint8_t *)cpu->pc;
   if (pc < page->mem || pc > page->mem + CODE_PAGE_SIZE)
      return;

   uintptr_t mark = cpu->pc;
#ifndef __MINGW32__
   if (sig == SIGTRAP)
      mark--;   // Point to faulting instruction
#endif

   for (code_span_t *span = page->owner->spans; span; span = span->next) {
      if (pc >= span->base && pc < span->base + span->size && span->name)
         code_disassemble(span, mark, cpu);
   }
}

#ifdef DEBUG
static bool code_cache_contains(code_cache_t *code, uint8_t *base, size_t size)
{
   assert_lock_held(&code->lock);

   for (code_page_t *p = code->pages; p; p = p->next) {
      if (base >= p->mem && base + size <= p->mem + CODE_PAGE_SIZE)
         return true;
   }

   return false;
}
#endif

static code_span_t *code_span_new(code_cache_t *code, ident_t name,
                                  uint8_t *base, size_t size)
{
   SCOPED_LOCK(code->lock);

   assert(code_cache_contains(code, base, size));

   code_span_t *span = xcalloc(sizeof(code_span_t));
   span->name  = name;
   span->next  = code->spans;
   span->base  = base;
   span->entry = base;
   span->size  = size;
   span->owner = code;

   code->spans = span;
   return span;
}

static void code_page_new(code_cache_t *code)
{
   assert_lock_held(&code->lock);

   code_page_t *page = xcalloc(sizeof(code_page_t));
   page->owner = code;
   page->next  = code->pages;
   page->mem   = map_jit_pages(CODE_PAGE_ALIGN, CODE_PAGE_SIZE);

   add_fault_handler(code_fault_handler, page);
   debug_add_unwinder(page->mem, CODE_PAGE_SIZE, code_cache_unwinder, code);

   code->pages = page;

   code_span_t *span = xcalloc(sizeof(code_span_t));
   span->next  = code->spans;
   span->base  = page->mem;
   span->size  = CODE_PAGE_SIZE;
   span->owner = code;

   code->globalfree = code->spans = span;
}

code_cache_t *code_cache_new(void)
{
   code_cache_t *code = xcalloc(sizeof(code_cache_t));

   {
      SCOPED_LOCK(code->lock);
      code_page_new(code);
   }

#ifdef HAVE_CAPSTONE
#if defined ARCH_X86_64
   if (cs_open(CS_ARCH_X86, CS_MODE_64, &(code->capstone)) != CS_ERR_OK)
      fatal_trace("failed to init capstone for x86_64");
#elif defined ARCH_ARM64
   if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &(code->capstone)) != CS_ERR_OK)
      fatal_trace("failed to init capstone for Arm64");
#else
#error Cannot configure capstone for this architecture
#endif

   if (cs_option(code->capstone, CS_OPT_DETAIL, 1) != CS_ERR_OK)
      fatal_trace("failed to set capstone detailed mode");
#endif

   shash_t *s = shash_new(32);

   extern void __nvc_putpriv(jit_handle_t, void *);
   extern void __nvc_sched_waveform(jit_anchor_t *, jit_scalar_t *, tlab_t *);
   extern void __nvc_sched_process(jit_anchor_t *, jit_scalar_t *, tlab_t *);
   extern void __nvc_test_event(jit_anchor_t *, jit_scalar_t *, tlab_t *);
   extern void __nvc_last_event(jit_anchor_t *, jit_scalar_t *, tlab_t *);

   shash_put(s, "__nvc_sched_waveform", &__nvc_sched_waveform);
   shash_put(s, "__nvc_sched_process", &__nvc_sched_process);
   shash_put(s, "__nvc_test_event", &__nvc_test_event);
   shash_put(s, "__nvc_last_event", &__nvc_last_event);
   shash_put(s, "__nvc_mspace_alloc", &__nvc_mspace_alloc);
   shash_put(s, "__nvc_eval_alloc", &__nvc_eval_alloc);
   shash_put(s, "__nvc_putpriv", &__nvc_putpriv);
   shash_put(s, "__nvc_do_exit", &__nvc_do_exit);
   shash_put(s, "__nvc_pack", &__nvc_pack);
   shash_put(s, "__nvc_unpack", &__nvc_unpack);
   shash_put(s, "__nvc_vec4op", &__nvc_vec4op);
   shash_put(s, "memmove", &memmove);
   shash_put(s, "memcpy", &memcpy);
   shash_put(s, "memset", &memset);
   shash_put(s, "pow", &pow);
   shash_put(s, "ldexp", &ldexp);
   shash_put(s, "exp2", &exp2);

#if defined __APPLE__ && defined ARCH_ARM64
   shash_put(s, "bzero", &bzero);
#elif defined __APPLE__ && defined ARCH_X86_64
   shash_put(s, "__bzero", &bzero);
#elif defined __MINGW32__ && defined ARCH_X86_64
   extern void ___chkstk_ms(void);
   shash_put(s, "___chkstk_ms", &___chkstk_ms);
#endif

   store_release(&code->symbols, s);

   return code;
}

void code_cache_free(code_cache_t *code)
{
   for (code_page_t *it = code->pages, *tmp; it; it = tmp) {
      debug_remove_unwinder(it->mem);
      remove_fault_handler(code_fault_handler, it);

      nvc_munmap(it->mem, CODE_PAGE_SIZE);

      tmp = it->next;
      free(it);
   }

   for (code_span_t *it = code->spans, *tmp; it; it = tmp) {
      tmp = it->next;
      DEBUG_ONLY(free(it->debug.comments));
      free(it);
   }

#ifdef HAVE_CAPSTONE
   cs_close(&(code->capstone));
#endif

#ifdef DEBUG
   if (code->used > 0)
      debugf("JIT code footprint: %zu bytes", code->used);
#endif

   shash_free(code->symbols);
   free(code);
}

#ifdef HAVE_CAPSTONE
static int code_print_spaces(int col, int tab)
{
   for (; col < tab; col++)
      fputc(' ', stdout);
   return col;
}
#endif

#if defined DEBUG && HAVE_CAPSTONE
static int code_comment_compare(const void *a, const void *b)
{
   const code_comment_t *ca = a;
   const code_comment_t *cb = b;

   if (ca->addr < cb->addr)
      return -1;
   else if (ca->addr > cb->addr)
      return 1;
   else
      return 0;
}
#endif

static void code_disassemble(code_span_t *span, uintptr_t mark,
                             struct cpu_state *cpu)
{
   SCOPED_LOCK(span->owner->lock);

   printf("--");

   const int namelen = ident_len(span->name);
   for (int i = 0; i < 72 - namelen; i++)
      fputc('-', stdout);

   printf(" %s ----\n", istr(span->name));

#ifdef HAVE_CAPSTONE
   cs_insn *insn = cs_malloc(span->owner->capstone);

#ifdef DEBUG
   qsort(span->debug.comments, span->debug.count, sizeof(code_comment_t),
         code_comment_compare);
   code_comment_t *comment = span->debug.comments;
#endif

   const uint8_t *const eptr = span->base + span->size;
   for (const uint8_t *ptr = span->base; ptr < eptr; ) {
      uint64_t address = (uint64_t)ptr;

#ifdef DEBUG
      for (; comment < span->debug.comments + span->debug.count
              && comment->addr <= address; comment++)
         printf("%30s;; %s\n", "", comment->text);
#endif

      int zeros = 0;
      for (const uint8_t *zp = ptr; zp < eptr && *zp == 0; zp++, zeros++);

      if (zeros > 8 || zeros == eptr - ptr) {
         printf("%30s;; skipping %d zero bytes\n", "", zeros);
         ptr += zeros;
         continue;
      }

      size_t size = eptr - ptr;
      int col = 0;
      if (cs_disasm_iter(span->owner->capstone, &ptr, &size, &address, insn)) {
         char hex1[33], *p = hex1;
         for (size_t k = 0; k < insn->size; k++)
            p += checked_sprintf(p, hex1 + sizeof(hex1) - p, "%02x",
                                 insn->bytes[k]);

         col = printf("%-12" PRIx64 " %-16.16s %s %s", insn->address,
                          hex1, insn->mnemonic, insn->op_str);

#ifdef ARCH_X86_64
         if (strcmp(insn->mnemonic, "movabs") == 0) {
            const cs_x86_op *src = &(insn->detail->x86.operands[1]);
            if (src->type == X86_OP_IMM) {
               const char *sym = debug_symbol_name((void *)src->imm);
               if (sym != NULL) {
                  col = code_print_spaces(col, 60);
                  col += printf(" ; %s", sym);
               }
            }
         }
#endif

         if (strlen(hex1) > 16)
            col = printf("\n%15s -%-16s", "", hex1 + 16) - 1;
      }
      else {
#ifdef ARCH_ARM64
         col = printf("%-12" PRIx64 " %-16.08x %s 0x%08x", (uint64_t)ptr,
                      *(uint32_t *)ptr, ".word", *(uint32_t *)ptr);
         ptr += 4;
#else
         col = printf("%-12" PRIx64 " %-16.02x %s 0x%02x", (uint64_t)ptr,
                      *ptr, ".byte", *ptr);
         ptr++;
#endif
      }

      if (mark != 0 && (ptr >= eptr || address > mark)) {
         col = code_print_spaces(col, 66);
         printf("<=============\n");
         if (cpu != NULL) {
#ifdef ARCH_X86_64
            const char *names[] = {
               "RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI",
               "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"
            };
            for (int i = 0; i < ARRAY_LEN(names); i++)
               printf("\t%s\t%"PRIxPTR"\n", names[i], cpu->regs[i]);
#else
            for (int i = 0; i < 32; i++)
               printf("\tR%d\t%"PRIxPTR"\n", i, cpu->regs[i]);
#endif
         }
         mark = 0;
      }
      else
         printf("\n");
   }

   cs_free(insn, 1);
#else
   jit_hexdump(span->base, span->size, 16, (void *)mark, "");
#endif

   for (int i = 0; i < 80; i++)
      fputc('-', stdout);
   printf("\n");
   fflush(stdout);
}

static void code_write_perf_map(code_span_t *span)
{
   SCOPED_LOCK(span->owner->lock);

   if (span->owner->perfmap == NULL) {
      char *fname LOCAL = xasprintf("/tmp/perf-%d.map", getpid());
      if ((span->owner->perfmap = fopen(fname, "w")) == NULL) {
         warnf("cannot create %s: %s", fname, last_os_error());
         opt_set_int(OPT_PERF_MAP, 0);
         return;
      }
      else
         debugf("writing perf map to %s", fname);
   }

   fprintf(span->owner->perfmap, "%p 0x%zx %s\n", span->base, span->size,
           istr(span->name));
   fflush(span->owner->perfmap);
}

code_blob_t *code_blob_new(code_cache_t *code, ident_t name, size_t hint)
{
   code_span_t **freeptr = &(code->freelist[thread_id()]);

   code_span_t *free = relaxed_load(freeptr);
   if (free == NULL) {
      free = code_span_new(code, NULL, code->pages->mem, 0);
      relaxed_store(freeptr, free);
   }

   const size_t reqsz = hint ?: MIN_BLOB_SIZE;

   if (free->size < reqsz) {
      SCOPED_LOCK(code->lock);

#ifdef DEBUG
      if (free->size > 0)
         debugf("thread %d needs new code cache from global free list "
                "(requested %zu bytes, wasted %zu bytes)",
                thread_id(), reqsz, free->size);
#endif

      const size_t chunksz = MAX(reqsz, THREAD_CACHE_SIZE);
      const size_t alignedsz = ALIGN_UP(chunksz, CODE_BLOB_ALIGN);

      if (alignedsz > code->globalfree->size) {
         DEBUG_ONLY(debugf("requesting new %d byte code page", CODE_PAGE_SIZE));
         code_page_new(code);
         assert(code->globalfree->size == CODE_PAGE_SIZE);
      }

      const size_t take = MIN(code->globalfree->size, alignedsz);

      free->size = take;
      free->base = code->globalfree->base;

      code->globalfree->base += take;
      code->globalfree->size -= take;
   }

   assert(reqsz <= free->size);
   assert(((uintptr_t)free->base & (CODE_BLOB_ALIGN - 1)) == 0);

   code_span_t *span = code_span_new(code, name, free->base, free->size);

   free->base += span->size;
   free->size -= span->size;

   code_blob_t *blob = xcalloc(sizeof(code_blob_t));
   blob->span = span;
   blob->wptr = span->base;

   thread_wx_mode(WX_WRITE);

   return blob;
}

// ---- Entry-publication watches ---------------------------------------------
// Consumers (the fused-block emitter in rt/model.c) bake the current value
// of a jit_func_t's entry pointer as an immediate call target.  A later
// tier-up publish through code_blob_finalise must tell them so they can
// re-patch.  The registry is a simple global array: watch/unwatch are
// single-writer (model thread), notify may run on any thread (async cgen),
// all three serialised by one lock.  Notify runs the callbacks under the
// lock, so unwatch returning means no callback with that ctx can be running.

typedef struct {
   jit_entry_fn_t *slot;
   code_watch_fn_t fn;
   void           *ctx;
} entry_watch_t;

static entry_watch_t *entry_watches = NULL;
static unsigned       n_entry_watches = 0;
static unsigned       max_entry_watches = 0;
static nvc_lock_t     entry_watch_lock = 0;

void code_entry_watch(jit_entry_fn_t *slot, code_watch_fn_t fn, void *ctx)
{
   SCOPED_LOCK(entry_watch_lock);

   if (n_entry_watches == max_entry_watches) {
      max_entry_watches = MAX(max_entry_watches * 2, 64);
      entry_watches = xrealloc_array(entry_watches, max_entry_watches,
                                     sizeof(entry_watch_t));
   }

   entry_watches[n_entry_watches++] =
      (entry_watch_t){ .slot = slot, .fn = fn, .ctx = ctx };
}

void code_entry_unwatch(void *ctx)
{
   SCOPED_LOCK(entry_watch_lock);

   unsigned wptr = 0;
   for (unsigned i = 0; i < n_entry_watches; i++) {
      if (entry_watches[i].ctx != ctx)
         entry_watches[wptr++] = entry_watches[i];
   }
   n_entry_watches = wptr;
}

static void code_entry_notify(jit_entry_fn_t *slot, jit_entry_fn_t fn)
{
   SCOPED_LOCK(entry_watch_lock);

   for (unsigned i = 0; i < n_entry_watches; i++) {
      if (entry_watches[i].slot == slot)
         (*entry_watches[i].fn)(slot, fn, entry_watches[i].ctx);
   }
}

void code_blob_finalise(code_blob_t *blob, jit_entry_fn_t *entry)
{
   code_span_t *span = blob->span;
   span->size = blob->wptr - span->base;

   code_span_t *freespan = relaxed_load(&(span->owner->freelist[thread_id()]));
   assert(freespan->size == 0);

   ihash_free(blob->labels);
   blob->labels = NULL;

   if (unlikely(blob->patches != NULL))
      fatal_trace("not all labels in %s were patched", istr(span->name));
   else if (unlikely(blob->overflow)) {
      // Return all the memory
      freespan->size = freespan->base - span->base;
      freespan->base = span->base;
      free(blob);
      return;
   }
   else if (span->size == 0)
      fatal_trace("code span %s is empty", istr(span->name));

   uint8_t *aligned = ALIGN_UP(blob->wptr, CODE_BLOB_ALIGN);
   freespan->size = freespan->base - aligned;
   freespan->base = aligned;

   if (opt_get_verbose(OPT_ASM_VERBOSE, istr(span->name))) {
      nvc_printf("\n$bold$$blue$");
      code_disassemble(span, 0, NULL);
      nvc_printf("$$\n");
   }

   __builtin___clear_cache((char *)span->base, (char *)blob->wptr);

   thread_wx_mode(WX_EXECUTE);

   store_release(entry, (jit_entry_fn_t)span->entry);

   // Tell anyone who baked the old entry as an immediate (fused blocks)
   if (unlikely(relaxed_load(&n_entry_watches) > 0))
      code_entry_notify(entry, (jit_entry_fn_t)span->entry);

   DEBUG_ONLY(relaxed_add(&span->owner->used, span->size));
   free(blob);

   if (opt_get_int(OPT_PERF_MAP))
      code_write_perf_map(span);
}

__attribute__((cold, noinline))
static void code_blob_overflow(code_blob_t *blob)
{
   warnf("JIT code buffer for %s too small", istr(blob->span->name));
   for (patch_list_t *it = blob->patches, *tmp; it; it = tmp) {
      tmp = it->next;
      free(it);
   }
   blob->patches = NULL;
   blob->overflow = true;
}

void code_blob_emit(code_blob_t *blob, const uint8_t *bytes, size_t len)
{
   if (unlikely(blob->overflow))
      return;
   else if (unlikely(blob->wptr + len > blob->span->base + blob->span->size)) {
      code_blob_overflow(blob);
      return;
   }

   memcpy(blob->wptr, bytes, len);
   blob->wptr += len;
}

void code_blob_align(code_blob_t *blob, unsigned align)
{
#ifdef ARCH_X86_64
   const uint8_t pad[] = { 0x90 };
#else
   const uint8_t pad[] = { 0x00 };
#endif

   assert(is_power_of_2(align));
   assert(align % ARRAY_LEN(pad) == 0);

   while (((uintptr_t)blob->wptr & (align - 1)) && !blob->overflow)
      code_blob_emit(blob, pad, ARRAY_LEN(pad));
}

void code_blob_mark(code_blob_t *blob, jit_label_t label)
{
   if (unlikely(blob->overflow))
      return;
   else if (blob->labels == NULL)
      blob->labels = ihash_new(256);

   ihash_put(blob->labels, label, blob->wptr);

   for (patch_list_t **p = &(blob->patches); *p; ) {
      if ((*p)->label == label) {
         patch_list_t *next = (*p)->next;
         (*(*p)->fn)(blob, label, (*p)->wptr, blob->wptr);
         free(*p);
         *p = next;
      }
      else
         p = &((*p)->next);
   }
}

void code_blob_patch(code_blob_t *blob, jit_label_t label, code_patch_fn_t fn)
{
   void *ptr = NULL;
   if (unlikely(blob->overflow))
      return;
   else if (blob->labels != NULL && (ptr = ihash_get(blob->labels, label)))
      (*fn)(blob, label, blob->wptr, ptr);
   else {
      patch_list_t *new = xmalloc(sizeof(patch_list_t));
      new->next  = blob->patches;
      new->fn    = fn;
      new->label = label;
      new->wptr  = blob->wptr;

      blob->patches = new;
   }
}

#ifdef DEBUG
static void code_blob_print_value(text_buf_t *tb, jit_value_t value)
{
   switch (value.kind) {
   case JIT_VALUE_REG:
      tb_printf(tb, "R%d", value.reg);
      break;
   case JIT_VALUE_INT64:
      if (value.int64 < 4096)
         tb_printf(tb, "#%"PRIi64, value.int64);
      else
         tb_printf(tb, "#0x%"PRIx64, value.int64);
      break;
   case JIT_VALUE_DOUBLE:
      tb_printf(tb, "%%%g", value.dval);
      break;
   case JIT_ADDR_CPOOL:
      tb_printf(tb, "[CP+%"PRIi64"]", value.int64);
      break;
   case JIT_ADDR_REG:
      tb_printf(tb, "[R%d", value.reg);
      if (value.disp != 0)
         tb_printf(tb, "+%d", value.disp);
      tb_cat(tb, "]");
      break;
   case JIT_ADDR_ABS:
      tb_printf(tb, "[#%016"PRIx64"]", value.int64);
      break;
   case JIT_ADDR_COVER:
      tb_printf(tb, "@%"PRIi64, value.int64);
      break;
   case JIT_VALUE_LABEL:
      tb_printf(tb, "%d", value.label);
      break;
   case JIT_VALUE_HANDLE:
      tb_printf(tb, "<%d>", value.handle);
      break;
   case JIT_VALUE_EXIT:
      tb_printf(tb, "%s", jit_exit_name(value.exit));
      break;
   case JIT_VALUE_LOC:
      tb_printf(tb, "<%s:%d>", loc_file_str(&value.loc), value.loc.first_line);
      break;
   case JIT_VALUE_LOCUS:
      tb_printf(tb, "%p", value.locus);
      break;
   case JIT_VALUE_VPOS:
      tb_printf(tb, "%u:%u", value.vpos.block, value.vpos.op);
      break;
   default:
      tb_cat(tb, "???");
   }
}

static void code_blob_add_comment(code_blob_t *blob, uintptr_t addr, char *text)
{
   code_debug_t *dbg = &(blob->span->debug);

   if (dbg->count == dbg->max) {
      dbg->max = MAX(128, dbg->max * 2);
      dbg->comments = xrealloc_array(dbg->comments, dbg->max,
                                     sizeof(code_comment_t));
   }

   dbg->comments[dbg->count].addr = addr;
   dbg->comments[dbg->count].text = text;
   dbg->count++;
}

void code_blob_print_ir(code_blob_t *blob, jit_ir_t *ir)
{
   LOCAL_TEXT_BUF tb = tb_new();
   tb_printf(tb, "%s%s", jit_op_name(ir->op), jit_cc_name(ir->cc));

   if (ir->size != JIT_SZ_UNSPEC)
      tb_printf(tb, ".%d", 1 << (3 + ir->size));

   tb_printf(tb, "%*.s", (int)MAX(0, 10 - tb_len(tb)), "");

   if (ir->result != JIT_REG_INVALID)
      tb_printf(tb, "R%d", ir->result);

   if (ir->arg1.kind != JIT_VALUE_INVALID) {
      if (ir->result != JIT_REG_INVALID)
         tb_cat(tb, ", ");
      code_blob_print_value(tb, ir->arg1);
   }

   if (ir->arg2.kind != JIT_VALUE_INVALID) {
      tb_cat(tb, ", ");
      code_blob_print_value(tb, ir->arg2);
   }

   code_blob_add_comment(blob, (uintptr_t)blob->wptr, tb_claim(tb));
}

void code_blob_printf(code_blob_t *blob, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);

   char *text = xvasprintf(fmt, ap);
   code_blob_add_comment(blob, (uintptr_t)blob->wptr, text);

   va_end(ap);
}

__attribute__((format(printf, 3, 4)))
static void debug_reloc(code_blob_t *blob, void *patch, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);

   char *text = xvasprintf(fmt, ap);
   code_blob_add_comment(blob, (uintptr_t)patch, text);

   va_end(ap);
}
#else
#define debug_reloc(...)
#endif   // DEBUG

#ifdef ARCH_ARM64
static void arm64_patch_page_offset21(code_blob_t *blob, uint32_t *patch,
                                      void *ptr)
{
   switch ((*patch >> 23) & 0x7f) {
   case 0b1111010:   // LDR (immediate, SIMD&FP)
   case 0b1110010:   // LDR (immediate)
      assert(*patch & (1 << 30));  // Quadword
      assert(((uintptr_t)ptr & 7) == 0);
      *patch |= (((uintptr_t)ptr & 0xfff) >> 3) << 10;
      break;
   case 0b0100010:   // ADD (immediate)
      *patch |= ((uintptr_t)ptr & 0xfff) << 10;
      break;
   default:
      blob->span->size = blob->wptr - blob->span->base;
      code_disassemble(blob->span, (uintptr_t)patch, NULL);
      fatal_trace("cannot patch instruction");
   }
}

static void arm64_patch_page_base_rel21(uint32_t *patch, void *ptr)
{
   const intptr_t dst_page = (intptr_t)ptr & ~UINT64_C(0xfff);
   const intptr_t src_page = (intptr_t)patch & ~UINT64_C(0xfff);
   const intptr_t upper21 = (dst_page - src_page) >> 12;
   assert(upper21 >= -(1 << 20) && upper21 < (1 << 20));
   *patch &= ~((0x3 << 29) | (0x7ffff << 5));
   *patch |= (upper21 & 3) << 29;
   *patch |= ((upper21 >> 2) & 0x7ffff) << 5;
}
#endif

static void *code_emit_trampoline(code_blob_t *blob, void *dest)
{
#if defined ARCH_X86_64
   const uint8_t veneer[] = {
      0x48, 0xb8, __IMM64((uintptr_t)dest),  // MOVABS RAX, dest
      0xff, 0xe0                             // CALL RAX
   };
#elif defined ARCH_ARM64
   const uint8_t veneer[] = {
      0x50, 0x00, 0x00, 0x58,   // LDR X16, [PC+8]
      0x00, 0x02, 0x1f, 0xd6,   // BR X16
      __IMM64((uintptr_t)dest)
   };
#else
   should_not_reach_here();
#endif

   void *prev = memmem(blob->veneers, blob->wptr - blob->veneers,
                       veneer, ARRAY_LEN(veneer));
   if (prev != NULL)
      return prev;
   else {
      DEBUG_ONLY(code_blob_printf(blob, "Trampoline for %p", dest));

      void *addr = blob->wptr;
      code_blob_emit(blob, veneer, ARRAY_LEN(veneer));
      return addr;
   }
}

#if !defined __MINGW32__ && !defined __APPLE__
static void *code_emit_got(code_blob_t *blob, void *dest)
{
   const uint8_t data[] = { __IMM64((uintptr_t)dest) };

   void *prev = memmem(blob->veneers, blob->veneers - blob->wptr,
                       data, ARRAY_LEN(data));
   if (prev != NULL)
      return prev;
   else {
      DEBUG_ONLY(code_blob_printf(blob, "GOT entry for %p", dest));

      void *addr = blob->wptr;
      code_blob_emit(blob, data, ARRAY_LEN(data));
      return addr;
   }
}
#endif

#if defined __MINGW32__
static void code_load_pe(code_blob_t *blob, const void *data, size_t size)
{
   const IMAGE_FILE_HEADER *imghdr = data;

   switch (imghdr->Machine) {
   case IMAGE_FILE_MACHINE_AMD64:
   case IMAGE_FILE_MACHINE_ARM64:
      break;
   default:
      fatal_trace("unknown target machine %x", imghdr->Machine);
   }

   const IMAGE_SYMBOL *symtab = data + imghdr->PointerToSymbolTable;
   const char *strtab = data + imghdr->PointerToSymbolTable
      + imghdr->NumberOfSymbols * sizeof(IMAGE_SYMBOL);

   const IMAGE_SECTION_HEADER *sections =
      data + IMAGE_SIZEOF_FILE_HEADER + imghdr->SizeOfOptionalHeader;

   void **load_addr LOCAL =
      xmalloc_array(imghdr->NumberOfSections, sizeof(void *));

   for (int i = 0; i < imghdr->NumberOfSections; i++) {
      if ((sections[i].Characteristics & IMAGE_SCN_CNT_CODE)
          || (sections[i].Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA)) {
         const int align = sections[i].Characteristics & IMAGE_SCN_ALIGN_MASK;
         code_blob_align(blob, 1 << ((align >> 20) - 1));
         load_addr[i] = blob->wptr;
         code_blob_emit(blob, data + sections[i].PointerToRawData,
                        sections[i].SizeOfRawData);
      }
      else if ((sections[i].Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA)
               && sections[i].Misc.VirtualSize > 0)
         fatal_trace("non-empty BSS not supported");
   }

   if (blob->overflow)
      return;   // Relocations might point outside of code span

   blob->veneers = blob->wptr;

   shash_t *external = load_acquire(&blob->span->owner->symbols);

   for (int i = 0; i < imghdr->NumberOfSections; i++) {
      const IMAGE_RELOCATION *relocs = data + sections[i].PointerToRelocations;
      for (int j = 0; j < sections[i].NumberOfRelocations; j++) {
         const char *name = NULL;
         char tmp[9];

         assert(relocs[j].SymbolTableIndex < imghdr->NumberOfSymbols);
         const IMAGE_SYMBOL *sym = symtab + relocs[j].SymbolTableIndex;

         if (sym->N.Name.Short) {
            memcpy(tmp, sym->N.ShortName, 8);
            tmp[8] = '\0';
            name = tmp;
         }
         else
            name = strtab + sym->N.Name.Long;

         void *ptr = NULL;
         if (sym->SectionNumber > 0) {
            assert(sym->SectionNumber - 1 < imghdr->NumberOfSections);
            ptr = load_addr[sym->SectionNumber - 1] + sym->Value;
         }
         else
            ptr = shash_get(external, name);

         if (ptr == NULL && icmp(blob->span->name, name))
            ptr = blob->span->base;

         if (ptr == NULL)
            fatal_trace("failed to resolve symbol %s", name);

         void *patch = load_addr[i] + relocs[j].VirtualAddress;
         assert((uint8_t *)patch >= blob->span->base);
         assert((uint8_t *)patch < blob->span->base + blob->span->size);

         switch (relocs[j].Type) {
#if defined ARCH_X86_64
         case IMAGE_REL_AMD64_ADDR64:
            *(uint64_t *)patch += (uint64_t)ptr;
            break;
         case IMAGE_REL_AMD64_ADDR32NB:
            *(uint32_t *)patch += (uint32_t)(ptr - (void *)blob->span->base);
            break;
#elif defined ARCH_ARM64
         case IMAGE_REL_ARM64_BRANCH26:
            {
               void *veneer = code_emit_trampoline(blob, ptr);
               const ptrdiff_t pcrel = (veneer - patch) >> 2;
               *(uint32_t *)patch &= ~0x3ffffff;
               *(uint32_t *)patch |= pcrel & 0x3ffffff;
            }
            break;
         case IMAGE_REL_ARM64_ADDR32NB:
            *(uint32_t *)patch += (uint32_t)(ptr - (void *)blob->span->base);
            break;
         case IMAGE_REL_ARM64_PAGEBASE_REL21:
            arm64_patch_page_base_rel21(patch, ptr);
            break;
         case IMAGE_REL_ARM64_PAGEOFFSET_12A:
         case IMAGE_REL_ARM64_PAGEOFFSET_12L:
            arm64_patch_page_offset21(blob, patch, ptr);
            break;
#endif
         default:
            blob->span->size = blob->wptr - blob->span->base;
            code_disassemble(blob->span, (uintptr_t)patch, NULL);
            fatal_trace("cannot handle relocation type %d for symbol %s",
                        relocs[j].Type, name);
         }
      }

      if (strncmp((const char *)sections[i].Name, ".pdata",
                  IMAGE_SIZEOF_SHORT_NAME) == 0) {
         assert(sections[i].SizeOfRawData % sizeof(RUNTIME_FUNCTION) == 0);
         const int count = sections[i].SizeOfRawData / sizeof(RUNTIME_FUNCTION);
         const DWORD64 base = (DWORD64)blob->span->base;

         // TODO: we should also call RtlDeleteFunctionTable at some point
         if (!RtlAddFunctionTable(load_addr[i], count, base))
            fatal_trace("RtlAddFunctionTable failed: %s", last_os_error());
      }
   }

   for (int i = 0; i < imghdr->NumberOfSymbols; i++) {
      const IMAGE_SYMBOL *sym = &(symtab[i]);

      if (sym->SectionNumber == 0 || sym->N.Name.Short)
         continue;
      else if ((sym->Type >> 4) != IMAGE_SYM_DTYPE_FUNCTION)
         continue;
      else if (icmp(blob->span->name, strtab + sym->N.Name.Long)) {
         blob->span->entry = load_addr[sym->SectionNumber - 1] + sym->Value;
         break;
      }
   }
}
#elif defined __APPLE__
static void code_load_macho(code_blob_t *blob, const void *data, size_t size)
{
   const void *rptr = data;

   const struct mach_header_64 *fhdr = rptr;
   rptr += sizeof(struct mach_header_64);

   if (fhdr->magic != MH_MAGIC_64)
      fatal_trace("bad Mach-O magic %x", fhdr->magic);

   const struct segment_command_64 *seg = NULL;
   const struct symtab_command *symtab = NULL;

   void **load_addr LOCAL = NULL;

   for (int i = 0; i < fhdr->ncmds; i++) {
      const struct load_command *load = rptr;
      switch (load->cmd) {
      case LC_SEGMENT_64:
         {
            seg = rptr;
            load_addr = xmalloc_array(seg->nsects, sizeof(void *));

            for (int j = 0; j < seg->nsects; j++) {
               const struct section_64 *sec =
                  (void *)seg + sizeof(struct segment_command_64)
                  + j * sizeof(struct section_64);
               code_blob_align(blob, 1 << sec->align);
               load_addr[j] = blob->wptr;
               DEBUG_ONLY(code_blob_printf(blob, "%s", sec->sectname));
               code_blob_emit(blob, data + sec->offset, sec->size);
            }
         }
         break;
      case LC_SYMTAB:
         symtab = rptr;
         assert(symtab->cmdsize == sizeof(struct symtab_command));
         break;
      case LC_DATA_IN_CODE:
      case LC_LINKER_OPTIMIZATION_HINT:
      case LC_BUILD_VERSION:
      case LC_DYSYMTAB:
         break;
      default:
         warnf("unrecognised load command 0x%0x", load->cmd);
      }

      rptr += load->cmdsize;
   }
   assert(rptr == data + sizeof(struct mach_header_64) + fhdr->sizeofcmds);

   if (blob->overflow)
      return;   // Relocations might point outside of code span

   blob->veneers = blob->wptr;

   assert(seg != NULL);
   assert(symtab != NULL);

   shash_t *external = load_acquire(&blob->span->owner->symbols);

   for (int i = 0; i < seg->nsects; i++) {
      const struct section_64 *sec =
         (void *)seg + sizeof(struct segment_command_64)
         + i * sizeof(struct section_64);

      uint32_t addend = 0;
      for (int j = 0; j < sec->nreloc; j++) {
         const struct relocation_info *rel =
            data + sec->reloff + j * sizeof(struct relocation_info);
         const char *name = NULL;
         void *ptr = NULL;
         if (rel->r_extern) {
            assert(rel->r_symbolnum < symtab->nsyms);
            const struct nlist_64 *nl = data + symtab->symoff
               + rel->r_symbolnum * sizeof(struct nlist_64);
            name = data + symtab->stroff + nl->n_un.n_strx;

            if (nl->n_type & N_EXT) {
               if (icmp(blob->span->name, name + 1))
                  ptr = blob->span->base;
               else if ((ptr = shash_get(external, name + 1)) == NULL)
                  fatal_trace("failed to resolve symbol %s", name + 1);
            }
            else if (nl->n_sect != NO_SECT)
               ptr = blob->span->base + nl->n_value;
         }
         else
            ptr = blob->span->base;

         ptr += addend;
         addend = 0;

         void *patch = load_addr[i] + rel->r_address;
         assert((uint8_t *)patch >= blob->span->base);
         assert((uint8_t *)patch < blob->span->base + blob->span->size);

         switch (rel->r_type) {
#ifdef ARCH_ARM64
         case ARM64_RELOC_UNSIGNED:
            assert(rel->r_length == 3);
            *(void **)patch = ptr;
            break;
         case ARM64_RELOC_SUBTRACTOR:
            break;   // What is this?
         case ARM64_RELOC_GOT_LOAD_PAGEOFF12:
         case ARM64_RELOC_PAGEOFF12:
            arm64_patch_page_offset21(blob, patch, ptr);
            break;
         case ARM64_RELOC_GOT_LOAD_PAGE21:
         case ARM64_RELOC_PAGE21:
            arm64_patch_page_base_rel21(patch, ptr);
            break;
         case ARM64_RELOC_BRANCH26:
            {
               void *veneer = code_emit_trampoline(blob, ptr);
               const ptrdiff_t pcrel = (veneer - patch) >> 2;
               debug_reloc(blob, patch, "ARM64_RELOC_BRANCH26 %s PC%+"PRIiPTR,
                           name, pcrel);
               *(uint32_t *)patch &= ~0x3ffffff;
               *(uint32_t *)patch |= pcrel & 0x3ffffff;
            }
            break;
         case ARM64_RELOC_ADDEND:
            addend = rel->r_symbolnum;
            break;
#elif defined ARCH_X86_64
         case X86_64_RELOC_UNSIGNED:
            *(uint64_t *)patch += (uint64_t)ptr;
            break;
         case X86_64_RELOC_BRANCH:
            *(uint32_t *)patch += (uint32_t)(ptr - patch - 4);
            break;
#endif
         default:
            blob->span->size = blob->wptr - blob->span->base;
            code_disassemble(blob->span, (uintptr_t)patch, NULL);
            fatal_trace("cannot handle relocation type %d for symbol %s",
                        rel->r_type, name);
         }
      }
   }

   for (int i = 0; i < symtab->nsyms; i++) {
      const struct nlist_64 *sym =
         data + symtab->symoff + i * sizeof(struct nlist_64);

      if (sym->n_sect == NO_SECT || (sym->n_type & N_TYPE) != N_SECT)
         continue;

      const char *name = data + symtab->stroff + sym->n_un.n_strx;
      if (name[0] == '_' && icmp(blob->span->name, name + 1)) {
         blob->span->entry = load_addr[sym->n_sect - 1] + sym->n_value;
         break;
      }
   }
}
#elif !defined __MINGW32__
static void code_load_elf(code_blob_t *blob, const void *data, size_t size,
                          code_resolve_fn_t resolve, void *rctx)
{
   const Elf64_Ehdr *ehdr = data;

   if (ehdr->e_ident[EI_MAG0] != ELFMAG0
       || ehdr->e_ident[EI_MAG1] != ELFMAG1
       || ehdr->e_ident[EI_MAG2] != ELFMAG2
       || ehdr->e_ident[EI_MAG3] != ELFMAG3)
      fatal_trace("bad ELF magic");
   else if (ehdr->e_shentsize != sizeof(Elf64_Shdr))
      fatal_trace("bad section header size %d != %zu", ehdr->e_shentsize,
                  sizeof(Elf64_Shdr));

   const Elf64_Shdr *strtab_hdr =
      data + ehdr->e_shoff + ehdr->e_shstrndx * ehdr->e_shentsize;
   const char *strtab = data + strtab_hdr->sh_offset;

   void **load_addr LOCAL = xcalloc_array(ehdr->e_shnum, sizeof(void *));

   for (int i = 0; i < ehdr->e_shnum; i++) {
      const Elf64_Shdr *shdr = data + ehdr->e_shoff + i * ehdr->e_shentsize;

      switch (shdr->sh_type) {
      case SHT_PROGBITS:
         if (shdr->sh_flags & SHF_ALLOC) {
            code_blob_align(blob, shdr->sh_addralign);
            load_addr[i] = blob->wptr;
            DEBUG_ONLY(code_blob_printf(blob, "%s", strtab + shdr->sh_name));
            code_blob_emit(blob, data + shdr->sh_offset, shdr->sh_size);
         }
         break;

      case SHT_RELA:
         // Handled in second pass
         break;

      case SHT_NULL:
      case SHT_STRTAB:
      case SHT_X86_64_UNWIND:
         break;

      case SHT_SYMTAB:
         for (int i = 0; i < shdr->sh_size / shdr->sh_entsize; i++) {
            const Elf64_Sym *sym =
               data + shdr->sh_offset + i * shdr->sh_entsize;

            if (ELF64_ST_TYPE(sym->st_info) != STT_FUNC)
               continue;
            else if (!icmp(blob->span->name, strtab + sym->st_name))
               continue;
            else if (load_addr[sym->st_shndx] == NULL)
               fatal_trace("missing section %d for symbol %s", sym->st_shndx,
                           strtab + sym->st_name);
            else {
               blob->span->entry = load_addr[sym->st_shndx] + sym->st_value;
               blob->span->entry_size = sym->st_size;
               break;
            }
         }
         break;

      default:
         warnf("ignoring ELF section %s with type %x", strtab + shdr->sh_name,
               shdr->sh_type);
      }
   }

   if (blob->overflow)
      return;   // Relocations might point outside of code span

   blob->veneers = blob->wptr;

   shash_t *external = load_acquire(&blob->span->owner->symbols);

   for (int i = 0; i < ehdr->e_shnum; i++) {
      const Elf64_Shdr *shdr = data + ehdr->e_shoff + i * ehdr->e_shentsize;
      if (shdr->sh_type != SHT_RELA)
         continue;

      const Elf64_Shdr *mod =
         data + ehdr->e_shoff + shdr->sh_info * ehdr->e_shentsize;
      if (mod->sh_type != SHT_PROGBITS || !(mod->sh_flags & SHF_ALLOC))
         continue;
      else if (load_addr[shdr->sh_info] == NULL)
         fatal_trace("section %s not loaded", strtab + mod->sh_name);

      const Elf64_Shdr *symtab =
         data + ehdr->e_shoff + shdr->sh_link * ehdr->e_shentsize;
      if (symtab->sh_type != SHT_SYMTAB)
         fatal_trace("section %s is not a symbol table",
                     strtab + symtab->sh_name);

      const Elf64_Rela *endp = data + shdr->sh_offset + shdr->sh_size;
      for (const Elf64_Rela *r = data + shdr->sh_offset; r < endp; r++) {
         const Elf64_Sym *sym = data + symtab->sh_offset
            + ELF64_R_SYM(r->r_info) * symtab->sh_entsize;

         void *ptr = NULL;
         switch (ELF64_ST_TYPE(sym->st_info)) {
         case STT_NOTYPE:
         case STT_FUNC:
         case STT_OBJECT:
            if (sym->st_shndx == 0) {
               ptr = shash_get(external, strtab + sym->st_name);
               if (ptr == NULL && resolve != NULL)
                  ptr = (*resolve)(strtab + sym->st_name, rctx);
            }
            else
               ptr = load_addr[sym->st_shndx] + sym->st_value;
            break;
         case STT_SECTION:
            ptr = load_addr[sym->st_shndx];
            break;
         default:
            fatal_trace("cannot handle ELF symbol type %d",
                        ELF64_ST_TYPE(sym->st_info));
         }

         if (ptr == NULL)
            fatal_trace("cannot resolve symbol %s type %d",
                        strtab + sym->st_name, ELF64_ST_TYPE(sym->st_info));

         void *patch = load_addr[shdr->sh_info] + r->r_offset;
         assert(r->r_offset < mod->sh_size);

         switch (ELF64_R_TYPE(r->r_info)) {
         case R_X86_64_64:
            debug_reloc(blob, patch, "R_X86_64_64 %s", strtab + sym->st_name);
            *(uint64_t *)patch = (uint64_t)ptr + r->r_addend;
            break;
         case R_X86_64_PC32:
            {
               const ptrdiff_t pcrel = ptr + r->r_addend - patch;
               debug_reloc(blob, patch, "R_X86_64_PC32 %s PC%+"PRIiPTR,
                           strtab + sym->st_name, pcrel);
               assert(pcrel >= INT32_MIN && pcrel <= INT32_MAX);
               *(uint32_t *)patch = pcrel;
            }
            break;
         case R_X86_64_GOTPCREL:
            {
               void *got = code_emit_got(blob, ptr);
               const ptrdiff_t pcrel = got + r->r_addend - patch;
               debug_reloc(blob, patch, "R_X86_64_GOTPCREL %s PC%+"PRIiPTR,
                           strtab + sym->st_name, pcrel);
               assert(pcrel >= INT32_MIN && pcrel <= INT32_MAX);
               *(uint32_t *)patch = pcrel;
            }
            break;
         case R_X86_64_PLT32:
            {
               void *veneer = code_emit_trampoline(blob, ptr);
               const ptrdiff_t pcrel = veneer + r->r_addend - patch;
               debug_reloc(blob, patch, "R_X86_64_PLT32 %s PC%+"PRIiPTR,
                           strtab + sym->st_name, pcrel);
               assert(pcrel >= INT32_MIN && pcrel <= INT32_MAX);
               *(uint32_t *)patch = pcrel;
            }
            break;
         case R_AARCH64_CALL26:
            {
               void *veneer = code_emit_trampoline(blob, ptr);
               const ptrdiff_t pcrel = (veneer + r->r_addend - patch) >> 2;
               *(uint32_t *)patch &= ~0x3ffffff;
               *(uint32_t *)patch |= pcrel & 0x3ffffff;
            }
            break;
         case R_AARCH64_PREL64:
            *(uint64_t *)patch = ptr + r->r_addend - patch;
            break;
         case R_AARCH64_MOVW_UABS_G0_NC:
            *(uint32_t *)patch |=
               (((uintptr_t)ptr + r->r_addend) & 0xffff) << 5;
            break;
         case R_AARCH64_MOVW_UABS_G1_NC:
            *(uint32_t *)patch |=
               ((((uintptr_t)ptr + r->r_addend) >> 16) & 0xffff) << 5;
            break;
         case R_AARCH64_MOVW_UABS_G2_NC:
            *(uint32_t *)patch |=
               ((((uintptr_t)ptr + r->r_addend) >> 32) & 0xffff) << 5;
            break;
         case R_AARCH64_MOVW_UABS_G3:
            *(uint32_t *)patch |=
               ((((uintptr_t)ptr + r->r_addend) >> 48) & 0xffff) << 5;
            break;
         default:
            blob->span->size = blob->wptr - blob->span->base;
            code_disassemble(blob->span, (uintptr_t)patch, NULL);
            fatal_trace("cannot handle relocation type %ld for symbol %s",
                        ELF64_R_TYPE(r->r_info), strtab + sym->st_name);
         }
      }
   }
}
#endif

void code_load_object(code_blob_t *blob, const void *data, size_t size,
                      code_resolve_fn_t resolve, void *rctx)
{
#if defined __APPLE__
   (void)resolve; (void)rctx;   // Persistent cache is linux/x86_64 only
   code_load_macho(blob, data, size);
#elif defined __MINGW32__
   (void)resolve; (void)rctx;
   code_load_pe(blob, data, size);
#else
   code_load_elf(blob, data, size, resolve, rctx);
#endif
}

////////////////////////////////////////////////////////////////////////////////
// Body splice: copy an already-JIT-compiled entry function's machine code
// into another blob so it runs inline (fall-through) instead of via
// call/return.  NO recompilation: the LLVM JIT emits bodies with no
// RIP-relative references and no rel32 calls (every external reference is
// an absolute movabs), so the instruction stream is position-independent
// under copy.  The only rewriting needed is (a) each terminal RET becomes
// JMP rel32 to the end of the copy (+4 bytes per seam) and (b) intra-body
// relative branch displacements straddling a seam shift by 4 per seam
// crossed.  Anything outside that contract -- indirect jumps (tables),
// RIP-relative operands, relative calls, absolute immediates pointing into
// the copied extent (resume records), undecodable bytes -- DECLINES: the
// caller keeps the ordinary call site.  Priority queue, not admission gate.
//
// Single-caller discipline: the model thread only (fused block build).

#if defined ARCH_X86_64 && defined HAVE_CAPSTONE

typedef struct {
   uint32_t old_off;    // instruction start in the source extent
   uint32_t new_off;    // instruction start in the emitted copy
   uint8_t  size;       // source instruction size
   uint8_t  kind;       // SPLICE_* below
   uint8_t  imm_off;    // branch/movabs: immediate offset within insn
   uint8_t  imm_size;   // branch only: displacement size (1 or 4)
   uint16_t aux;        // movabs: capstone register id; tbl: table index
   uint64_t target;     // branch: target addr; movabs: imm value
} splice_insn_t;

#define SPLICE_COPY    0
#define SPLICE_RET     1
#define SPLICE_BRANCH  2
#define SPLICE_MOVABS  3   // movabs reg, imm64 (candidate jump-table base)
#define SPLICE_CMPIMM  5   // cmp reg, imm (candidate table bounds check)
#define SPLICE_TBLBASE 4   // movabs later claimed by an indirect jump: the
                           // imm is rewritten to the rebased table copy

// Jump table claimed by an indirect `jmp [base + idx*8]`: the absolute
// entries are code addresses inside the extent, so the whole table is
// copied into the blob (after the code) with every entry rebased.
typedef struct {
   uint64_t old_addr;   // table base in the source span
   uint32_t nentries;
   uint32_t new_off;    // offset of the rebased copy from the code start
} splice_table_t;

#define SPLICE_MAX_EXTENT  65536
#define SPLICE_MAX_INSNS   20000
#define SPLICE_MAX_TABLES  32
#define SPLICE_MAX_TBLENT  1024

static bool splice_debug(void)
{
   static int on = -1;
   if (on < 0)
      on = getenv("NVC_SPLICE_DEBUG") != NULL;
   return on;
}

#define SPLICE_DECLINE(fmt, ...) do {                                   \
      if (splice_debug())                                               \
         notef("splice: decline %s: " fmt, istr(blob->span->name),      \
               ##__VA_ARGS__);                                          \
      goto decline;                                                     \
   } while (0)

// Map a 32-bit GPR to its 64-bit parent (bounds check `cmp eax, N`
// guarding an index used as `[rdx + rax*8]`)
static uint16_t splice_reg64(uint16_t reg)
{
   switch (reg) {
   case X86_REG_EAX: return X86_REG_RAX;
   case X86_REG_EBX: return X86_REG_RBX;
   case X86_REG_ECX: return X86_REG_RCX;
   case X86_REG_EDX: return X86_REG_RDX;
   case X86_REG_ESI: return X86_REG_RSI;
   case X86_REG_EDI: return X86_REG_RDI;
   case X86_REG_EBP: return X86_REG_RBP;
   case X86_REG_ESP: return X86_REG_RSP;
   case X86_REG_R8D:  return X86_REG_R8;
   case X86_REG_R9D:  return X86_REG_R9;
   case X86_REG_R10D: return X86_REG_R10;
   case X86_REG_R11D: return X86_REG_R11;
   case X86_REG_R12D: return X86_REG_R12;
   case X86_REG_R13D: return X86_REG_R13;
   case X86_REG_R14D: return X86_REG_R14;
   case X86_REG_R15D: return X86_REG_R15;
   default: return reg;
   }
}

bool code_blob_splice(code_blob_t *blob, const void *entry, size_t extent)
{
   if (extent == 0 || extent > SPLICE_MAX_EXTENT)
      return false;

   static csh handle;
   static bool handle_valid = false;
   if (!handle_valid) {
      if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK)
         return false;
      cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
      handle_valid = true;
   }

   splice_insn_t *insns LOCAL =
      xmalloc_array(SPLICE_MAX_INSNS, sizeof(splice_insn_t));
   splice_table_t tables[SPLICE_MAX_TABLES];
   unsigned count = 0, ntables = 0;

   const uint64_t base = (uint64_t)(uintptr_t)entry;
   const uint8_t *code = entry;
   size_t left = extent;
   uint64_t address = base;

   cs_insn *ci = cs_malloc(handle);

   // Pass 1: decode, classify, place.  new_off tracks the emitted layout
   // (identical sizes except RET 1 -> JMP rel32 5).
   uint32_t new_off = 0;
   bool ok = true;
   while (left > 0) {
      if (count == SPLICE_MAX_INSNS)
         SPLICE_DECLINE("instruction budget exceeded");
      if (!cs_disasm_iter(handle, &code, &left, &address, ci))
         SPLICE_DECLINE("undecodable at +0x%"PRIx64, address - base);

      splice_insn_t *si = &(insns[count++]);
      si->old_off = (uint32_t)(ci->address - base);
      si->new_off = new_off;
      si->size    = ci->size;
      si->kind    = SPLICE_COPY;
      si->aux     = 0;

      const cs_x86 *x86 = &(ci->detail->x86);

      // RIP-relative memory operands never appear in JIT output; a copy
      // would silently read/write the wrong address, so refuse hard.
      for (int i = 0; i < x86->op_count; i++) {
         if (x86->operands[i].type == X86_OP_MEM
             && x86->operands[i].mem.base == X86_REG_RIP)
            SPLICE_DECLINE("RIP-relative operand at +0x%x", si->old_off);
      }

      const bool is_call = cs_insn_group(handle, ci, X86_GRP_CALL);
      const bool is_jump = cs_insn_group(handle, ci, X86_GRP_JUMP);

      if (ci->id == X86_INS_RET) {
         si->kind = SPLICE_RET;
         new_off += 5;         // becomes jmp rel32
         continue;
      }
      else if (ci->id == X86_INS_RETF || ci->id == X86_INS_IRET
               || ci->id == X86_INS_IRETD || ci->id == X86_INS_IRETQ)
         SPLICE_DECLINE("far/interrupt return at +0x%x", si->old_off);
      else if (is_call || is_jump) {
         const cs_x86_op *op = &(x86->operands[0]);
         if (op->type == X86_OP_IMM) {
            const uint64_t target = (uint64_t)op->imm;
            if (target >= base && target < base + extent) {
               si->kind     = SPLICE_BRANCH;
               si->imm_off  = x86->encoding.imm_offset;
               si->imm_size = x86->encoding.imm_size;
               si->aux      = (uint16_t)ci->id;
               si->target   = target;
               if (si->imm_size != 1 && si->imm_size != 4)
                  SPLICE_DECLINE("branch imm size %d at +0x%x",
                                 si->imm_size, si->old_off);
            }
            else
               // Relative transfer out of the extent: displacement would
               // need rebasing against a far-away blob; never seen in JIT
               // output (external refs are movabs+reg), so just refuse
               SPLICE_DECLINE("relative %s out of extent at +0x%x",
                              is_call ? "call" : "jump", si->old_off);
         }
         else if (is_jump) {
            // Indirect jump: admit ONLY the LLVM switch-table contract --
            //   cmp IDX, BOUND ; ja <default>
            //   movabs BASE, <table>
            //   jmp [BASE + IDX*8]
            // The table holds absolute code addresses inside the extent;
            // it is copied into the blob with every entry rebased and the
            // movabs immediate rewritten (see the check/emit passes).
            if (op->type != X86_OP_MEM
                || op->mem.base == X86_REG_INVALID
                || op->mem.index == X86_REG_INVALID
                || op->mem.scale != 8 || op->mem.disp != 0
                || op->mem.segment != X86_REG_INVALID)
               SPLICE_DECLINE("indirect jump at +0x%x", si->old_off);

            // The movabs that loaded the table base, within a short window
            int mov_i = -1;
            for (int k = (int)count - 2, w = 0; k >= 0 && w < 8; k--, w++) {
               if ((insns[k].kind == SPLICE_MOVABS
                    || insns[k].kind == SPLICE_TBLBASE)
                   && insns[k].aux == (uint16_t)op->mem.base) {
                  mov_i = k;
                  break;
               }
            }
            if (mov_i < 0)
               SPLICE_DECLINE("indirect jump without table base at +0x%x",
                              si->old_off);

            // The bounds check on the index register: `cmp IDX, N`
            // immediately followed by the guarding branch.  JA excludes
            // idx > N (N+1 entries); JAE/JGE exclude idx >= N (N
            // entries; LLVM range-checks negatives separately, exactly
            // as the original code does).
            int64_t nentries = -1;
            const uint16_t idx64 = splice_reg64((uint16_t)op->mem.index);
            for (int k = (int)count - 2, w = 0; k >= 0 && w < 8; k--, w++) {
               if (insns[k].kind != SPLICE_CMPIMM || insns[k].aux != idx64
                   || k + 1 >= (int)count
                   || insns[k+1].kind != SPLICE_BRANCH)
                  continue;
               const uint16_t br = insns[k+1].aux;
               if (br == (uint16_t)X86_INS_JA)
                  nentries = (int64_t)insns[k].target + 1;
               else if (br == (uint16_t)X86_INS_JAE
                        || br == (uint16_t)X86_INS_JGE)
                  nentries = (int64_t)insns[k].target;
               else
                  continue;
               break;
            }
            if (nentries <= 0 || nentries > SPLICE_MAX_TBLENT)
               SPLICE_DECLINE("unbounded indirect jump at +0x%x",
                              si->old_off);

            const uint64_t tbl_addr = insns[mov_i].target;
            if (tbl_addr >= base && tbl_addr < base + extent)
               SPLICE_DECLINE("jump table inside code extent at +0x%x",
                              si->old_off);

            unsigned t = 0;
            for (; t < ntables; t++)
               if (tables[t].old_addr == tbl_addr)
                  break;
            if (t == ntables) {
               if (ntables == SPLICE_MAX_TABLES)
                  SPLICE_DECLINE("table budget exceeded");
               tables[ntables++] = (splice_table_t){
                  .old_addr = tbl_addr,
                  .nentries = (uint32_t)nentries,
               };
            }
            else if (tables[t].nentries < (uint32_t)nentries)
               tables[t].nentries = (uint32_t)nentries;

            insns[mov_i].kind = SPLICE_TBLBASE;
            // jmp itself copies verbatim (registers unchanged)
         }
         // Indirect calls (movabs'd helper pointers) copy verbatim
      }
      else if ((ci->id == X86_INS_MOV || ci->id == X86_INS_MOVABS)
               && x86->op_count == 2
               && x86->operands[0].type == X86_OP_REG
               && x86->operands[1].type == X86_OP_IMM
               && x86->operands[1].size == 8) {
         const uint64_t imm = (uint64_t)x86->operands[1].imm;
         if (imm >= base && imm < base + extent)
            // Absolute immediate pointing back into the extent (e.g. a
            // baked resume address) would still target the ORIGINAL body
            SPLICE_DECLINE("self-referential imm64 at +0x%x", si->old_off);
         si->kind    = SPLICE_MOVABS;
         si->aux     = (uint16_t)x86->operands[0].reg;
         si->imm_off = x86->encoding.imm_offset;
         si->target  = imm;
      }
      else {
         if (ci->id == X86_INS_CMP && x86->op_count == 2
             && x86->operands[0].type == X86_OP_REG
             && x86->operands[1].type == X86_OP_IMM) {
            si->kind   = SPLICE_CMPIMM;
            si->aux    = splice_reg64((uint16_t)x86->operands[0].reg);
            si->target = (uint64_t)x86->operands[1].imm;
         }
         for (int i = 0; i < x86->op_count; i++) {
            if (x86->operands[i].type == X86_OP_IMM
                && x86->operands[i].size == 8
                && (uint64_t)x86->operands[i].imm >= base
                && (uint64_t)x86->operands[i].imm < base + extent)
               SPLICE_DECLINE("self-referential imm64 at +0x%x", si->old_off);
         }
      }

      new_off += ci->size;
   }
   goto have_insns;

 decline:
   ok = false;

 have_insns:
   cs_free(ci, 1);
   if (!ok)
      return false;

   const uint32_t new_code_end = new_off;

   // Table layout: 8-aligned block AFTER the copied code; rets jump past it
   uint32_t tables_base = (new_code_end + 7) & ~7u;
   uint32_t tables_off = tables_base, total_entries = 0;
   for (unsigned t = 0; t < ntables; t++) {
      tables[t].new_off = tables_off;
      tables_off += tables[t].nentries * 8;
      total_entries += tables[t].nentries;
   }
   const uint32_t new_total = tables_off;   // ret/continuation target

   // Check pass: resolve every internal branch target to an instruction
   // start and verify the recomputed displacement fits its encoding,
   // BEFORE any byte is emitted -- a decline must leave the blob intact.
   // The resolved displacement is cached in `target` for the emit pass.
   for (unsigned i = 0; i < count; i++) {
      splice_insn_t *si = &(insns[i]);
      if (si->kind != SPLICE_BRANCH)
         continue;

      const uint32_t target_old = (uint32_t)(si->target - base);

      unsigned lo = 0, hi = count;
      while (lo < hi) {
         const unsigned mid = (lo + hi) / 2;
         if (insns[mid].old_off < target_old)
            lo = mid + 1;
         else
            hi = mid;
      }
      if (lo == count || insns[lo].old_off != target_old) {
         if (splice_debug())
            notef("splice: decline %s: branch into mid-instruction +0x%x",
                  istr(blob->span->name), target_old);
         return false;
      }

      const int64_t disp = (int64_t)insns[lo].new_off
         - ((int64_t)si->new_off + si->size);

      if (si->imm_size == 1 && (disp < INT8_MIN || disp > INT8_MAX)) {
         if (splice_debug())
            notef("splice: decline %s: rel8 overflow at +0x%x",
                  istr(blob->span->name), si->old_off);
         return false;
      }

      si->target = (uint64_t)disp;   // repurposed: resolved displacement
   }

   // Check pass for tables: every entry must be an instruction start
   // inside the extent; resolve each to its new offset up front.
   uint32_t *tbl_newoffs LOCAL = total_entries > 0
      ? xmalloc_array(total_entries, sizeof(uint32_t)) : NULL;
   for (unsigned t = 0, e = 0; t < ntables; t++) {
      const uint64_t *src =
         (const uint64_t *)(uintptr_t)tables[t].old_addr;
      for (unsigned i = 0; i < tables[t].nentries; i++, e++) {
         const uint64_t tgt = src[i];
         if (tgt < base || tgt >= base + extent) {
            if (splice_debug())
               notef("splice: decline %s: table entry %u out of extent",
                     istr(blob->span->name), i);
            return false;
         }
         const uint32_t tgt_old = (uint32_t)(tgt - base);
         unsigned lo = 0, hi = count;
         while (lo < hi) {
            const unsigned mid = (lo + hi) / 2;
            if (insns[mid].old_off < tgt_old)
               lo = mid + 1;
            else
               hi = mid;
         }
         if (lo == count || insns[lo].old_off != tgt_old) {
            if (splice_debug())
               notef("splice: decline %s: table entry %u mid-instruction",
                     istr(blob->span->name), i);
            return false;
         }
         tbl_newoffs[e] = insns[lo].new_off;
      }
   }

   // Emit pass: cannot fail (blob overflow follows the normal
   // code_blob_emit protocol and is detected at finalise).
   uint8_t *const copy_base = blob->wptr;

   for (unsigned i = 0; i < count; i++) {
      const splice_insn_t *si = &(insns[i]);

      if (si->kind == SPLICE_RET) {
         const int32_t disp = (int32_t)(new_total - (si->new_off + 5));
         uint8_t jmp[5] = { 0xE9 };
         memcpy(jmp + 1, &disp, 4);
         code_blob_emit(blob, jmp, 5);
         continue;
      }

      uint8_t buf[16];
      assert(si->size <= sizeof buf);
      memcpy(buf, (const uint8_t *)entry + si->old_off, si->size);

      if (si->kind == SPLICE_BRANCH) {
         if (si->imm_size == 1)
            buf[si->imm_off] = (uint8_t)(int8_t)(int64_t)si->target;
         else {
            const int32_t d32 = (int32_t)(int64_t)si->target;
            memcpy(buf + si->imm_off, &d32, 4);
         }
      }
      else if (si->kind == SPLICE_TBLBASE) {
         unsigned t = 0;
         while (tables[t].old_addr != si->target)
            t++;
         const uint64_t new_addr =
            (uint64_t)(uintptr_t)(copy_base + tables[t].new_off);
         memcpy(buf + si->imm_off, &new_addr, 8);
      }

      code_blob_emit(blob, buf, si->size);
   }

   // Alignment padding, then the rebased tables
   static const uint8_t zero[8] = { 0 };
   code_blob_emit(blob, zero, tables_base - new_code_end);
   for (unsigned t = 0, e = 0; t < ntables; t++) {
      for (unsigned i = 0; i < tables[t].nentries; i++, e++) {
         const uint64_t abs =
            (uint64_t)(uintptr_t)(copy_base + tbl_newoffs[e]);
         code_blob_emit(blob, (const uint8_t *)&abs, 8);
      }
   }

   return true;
}

size_t code_blob_entry_size(const code_blob_t *blob)
{
   return blob->span->entry_size;
}

#else    // !ARCH_X86_64 || !HAVE_CAPSTONE

bool code_blob_splice(code_blob_t *blob, const void *entry, size_t extent)
{
   return false;
}

size_t code_blob_entry_size(const code_blob_t *blob)
{
   return 0;
}

#endif

////////////////////////////////////////////////////////////////////////////////
// Fail-soft validating prepass for persistent-cache objects.  The normal
// loader above trusts its input (it just emitted it) and fatals on any
// surprise; a cached file from disk gets NO such trust.  This probe
// bounds-checks every access against `size`, whitelists relocation
// types, and resolves every symbol -- all BEFORE any code blob memory is
// written (validate-before-mutate).  Anything unexpected returns false
// and the caller recompiles: a MISS is always safe.

#if defined __linux__ && defined ARCH_X86_64 && !defined __MINGW32__

static bool probe_str_ok(const char *strtab, size_t strtabsz, uint32_t off)
{
   if (off >= strtabsz)
      return false;
   return memchr(strtab + off, '\0', strtabsz - off) != NULL;
}

bool code_object_probe(code_cache_t *code, ident_t name, const void *data,
                       size_t size, code_resolve_fn_t resolve, void *rctx)
{
   if (size < sizeof(Elf64_Ehdr))
      return false;

   const Elf64_Ehdr *ehdr = data;
   if (ehdr->e_ident[EI_MAG0] != ELFMAG0
       || ehdr->e_ident[EI_MAG1] != ELFMAG1
       || ehdr->e_ident[EI_MAG2] != ELFMAG2
       || ehdr->e_ident[EI_MAG3] != ELFMAG3)
      return false;
   else if (ehdr->e_ident[EI_CLASS] != ELFCLASS64
            || ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
      return false;
   else if (ehdr->e_type != ET_REL || ehdr->e_machine != EM_X86_64)
      return false;
   else if (ehdr->e_shentsize != sizeof(Elf64_Shdr))
      return false;
   else if (ehdr->e_shnum == 0 || ehdr->e_shoff > size
            || ehdr->e_shnum > (size - ehdr->e_shoff) / sizeof(Elf64_Shdr))
      return false;
   else if (ehdr->e_shstrndx >= ehdr->e_shnum)
      return false;

#define PROBE_SHDR(i) \
   ((const Elf64_Shdr *)(data + ehdr->e_shoff + (i) * sizeof(Elf64_Shdr)))

   const Elf64_Shdr *strtab_hdr = PROBE_SHDR(ehdr->e_shstrndx);
   if (strtab_hdr->sh_type != SHT_STRTAB
       || strtab_hdr->sh_offset > size
       || strtab_hdr->sh_size > size - strtab_hdr->sh_offset)
      return false;

   const char *strtab = data + strtab_hdr->sh_offset;
   const size_t strtabsz = strtab_hdr->sh_size;

   bool *loadable LOCAL = xcalloc_array(ehdr->e_shnum, sizeof(bool));

   for (int i = 0; i < ehdr->e_shnum; i++) {
      const Elf64_Shdr *shdr = PROBE_SHDR(i);

      switch (shdr->sh_type) {
      case SHT_PROGBITS:
         if (shdr->sh_flags & SHF_ALLOC) {
            if (shdr->sh_offset > size || shdr->sh_size > size - shdr->sh_offset)
               return false;
            if (shdr->sh_addralign == 0
                || shdr->sh_addralign > CODE_BLOB_ALIGN
                || (shdr->sh_addralign & (shdr->sh_addralign - 1)) != 0)
               return false;
            loadable[i] = true;
         }
         break;
      case SHT_RELA:
         if (shdr->sh_entsize != sizeof(Elf64_Rela)
             || shdr->sh_offset > size
             || shdr->sh_size > size - shdr->sh_offset)
            return false;
         break;
      case SHT_SYMTAB:
         if (shdr->sh_entsize != sizeof(Elf64_Sym)
             || shdr->sh_offset > size
             || shdr->sh_size > size - shdr->sh_offset)
            return false;
         break;
      default:
         break;   // Ignored by the loader
      }
   }

   // The entry function must exist as an STT_FUNC symbol in a loaded
   // section or the loader would fall back to the span base
   bool entry_found = false;
   for (int i = 0; i < ehdr->e_shnum; i++) {
      const Elf64_Shdr *shdr = PROBE_SHDR(i);
      if (shdr->sh_type != SHT_SYMTAB)
         continue;

      for (int j = 0; j < shdr->sh_size / shdr->sh_entsize; j++) {
         const Elf64_Sym *sym = data + shdr->sh_offset + j * shdr->sh_entsize;
         if (!probe_str_ok(strtab, strtabsz, sym->st_name))
            return false;
         if (ELF64_ST_TYPE(sym->st_info) != STT_FUNC)
            continue;
         if (icmp(name, strtab + sym->st_name)) {
            if (sym->st_shndx >= ehdr->e_shnum || !loadable[sym->st_shndx])
               return false;
            entry_found = true;
         }
      }
   }

   if (!entry_found)
      return false;

   shash_t *external = load_acquire(&code->symbols);

   for (int i = 0; i < ehdr->e_shnum; i++) {
      const Elf64_Shdr *shdr = PROBE_SHDR(i);
      if (shdr->sh_type != SHT_RELA)
         continue;

      if (shdr->sh_info >= ehdr->e_shnum || shdr->sh_link >= ehdr->e_shnum)
         return false;

      const Elf64_Shdr *mod = PROBE_SHDR(shdr->sh_info);
      if (mod->sh_type != SHT_PROGBITS || !(mod->sh_flags & SHF_ALLOC))
         continue;   // Loader skips relocations for unloaded sections
      else if (!loadable[shdr->sh_info])
         return false;

      const Elf64_Shdr *symtab = PROBE_SHDR(shdr->sh_link);
      if (symtab->sh_type != SHT_SYMTAB)
         return false;

      const size_t nsyms = symtab->sh_size / symtab->sh_entsize;

      const Elf64_Rela *endp = data + shdr->sh_offset + shdr->sh_size;
      for (const Elf64_Rela *r = data + shdr->sh_offset; r < endp; r++) {
         if (ELF64_R_SYM(r->r_info) >= nsyms)
            return false;

         const Elf64_Sym *sym = data + symtab->sh_offset
            + ELF64_R_SYM(r->r_info) * symtab->sh_entsize;

         if (!probe_str_ok(strtab, strtabsz, sym->st_name))
            return false;

         switch (ELF64_ST_TYPE(sym->st_info)) {
         case STT_NOTYPE:
         case STT_FUNC:
         case STT_OBJECT:
            if (sym->st_shndx == 0) {
               void *ptr = shash_get(external, strtab + sym->st_name);
               if (ptr == NULL && resolve != NULL)
                  ptr = (*resolve)(strtab + sym->st_name, rctx);
               if (ptr == NULL)
                  return false;   // Unknown symbol: MISS, never fatal
            }
            else if (sym->st_shndx >= ehdr->e_shnum
                     || !loadable[sym->st_shndx])
               return false;
            break;
         case STT_SECTION:
            if (sym->st_shndx >= ehdr->e_shnum || !loadable[sym->st_shndx])
               return false;
            break;
         default:
            return false;
         }

         size_t width;
         switch (ELF64_R_TYPE(r->r_info)) {
         case R_X86_64_64:
            width = 8;
            break;
         case R_X86_64_PC32:
         case R_X86_64_GOTPCREL:
         case R_X86_64_PLT32:
            width = 4;
            break;
         default:
            return false;   // Foreign reloc flavour: MISS
         }

         if (r->r_offset > mod->sh_size || width > mod->sh_size - r->r_offset)
            return false;
      }
   }

#undef PROBE_SHDR

   return true;
}

#else

bool code_object_probe(code_cache_t *code, ident_t name, const void *data,
                       size_t size, code_resolve_fn_t resolve, void *rctx)
{
   return false;   // Cache unsupported on this platform: always a MISS
}

#endif  // __linux__ && ARCH_X86_64
