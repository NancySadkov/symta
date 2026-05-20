/* runtime/jit_sbc.c -- glue between the JIT translator (jit.c)
 * and the loaded-SBC structures (sif.h).
 *
 * This file is kept separate from jit.c so the latter's
 * standalone self-test (`gcc -DJIT_SELF_TEST`) doesn't need to
 * drag in the full runtime headers.  jit_sbc.c only compiles
 * in normal builds where common.h / symta.h / sif.h are
 * available.
 *
 * Step 5d scope: an audit function that counts how many of an
 * SBC's functions the translator accepts.  Doesn't actually
 * replace any dispatch -- that's step 5e.  Useful right now as
 * a "how close is the JIT to handling real code?" gauge.
 */

#include "common.h"
#include "symta.h"
#include "sif.h"
#include "jit.h"

#include <stdio.h>
#include <stdlib.h>

/* Trampoline helper for SBC_LD4_0..SBC_LD4_F.  Mirrors the
 * interpreter body in sbc.c:1129:
 *   L[dst] = ((void**)O_PTR(L[src]))[index]
 * O_PTR un-tags the dyn into a heap pointer.  Requires access
 * to HEAP_BASE (api_g.heap0) and the GID_SHFT constant from
 * runtime/symta.h. */
static void jit_rt_ld4_impl(int64_t *L, int dst, int src, int index) {
  void **base = (void**)O_PTR((dyn)L[src]);
  L[dst] = (int64_t)base[index];
}

/* Trampoline helpers for SBC_ARGLIST0..3 + SBC_CALL/CALLIR.
 *
 * ARGLIST opcodes set up `api.args` for the upcoming CALL:
 * ARGLIST(n) allocates an n-slot list and stores it in
 * api.args; STARG(i, v) sets api.args[i] = v.  The CALL macro
 * then dispatches to the closure's installed handler.
 *
 * The interpreter version of SBC_CALL also writes
 *   api.frame->pin = pin
 * before the CALL so a later stack trace can recover the
 * caller's source line via the lineno side-table.  We skip
 * that here -- the JIT'd code doesn't carry a `pin`, so
 * stack frames from JIT'd callsites won't have row/col info.
 * Trade-off acceptable for the coverage win; can revisit if
 * traces from JIT'd code prove unusable. */
static void jit_rt_arglist0_impl(int64_t *L, int a, int b, int c) {
  (void)L; (void)a; (void)b; (void)c;
  ARGLIST(0);
}
static void jit_rt_arglist1_impl(int64_t *L, int a, int b, int c) {
  (void)b; (void)c;
  ARGLIST(1);
  STARG(0, ((dyn*)L)[a]);
}
static void jit_rt_arglist2_impl(int64_t *L, int a, int b, int c) {
  (void)c;
  ARGLIST(2);
  STARG(0, ((dyn*)L)[a]);
  STARG(1, ((dyn*)L)[b]);
}
static void jit_rt_arglist3_impl(int64_t *L, int a, int b, int c) {
  ARGLIST(3);
  STARG(0, ((dyn*)L)[a]);
  STARG(1, ((dyn*)L)[b]);
  STARG(2, ((dyn*)L)[c]);
}
static void jit_rt_arglist4_impl(int64_t *L, int packed, int u1, int u2) {
  (void)u1; (void)u2;
  int a0 = (uint32_t)packed         & 0xFF;
  int a1 = ((uint32_t)packed >> 8)  & 0xFF;
  int a2 = ((uint32_t)packed >> 16) & 0xFF;
  int a3 = ((uint32_t)packed >> 24) & 0xFF;
  ARGLIST(4);
  STARG(0, ((dyn*)L)[a0]);
  STARG(1, ((dyn*)L)[a1]);
  STARG(2, ((dyn*)L)[a2]);
  STARG(3, ((dyn*)L)[a3]);
}
static void jit_rt_arglist5_impl(int64_t *L, int packed, int a4, int u2) {
  (void)u2;
  int a0 = (uint32_t)packed         & 0xFF;
  int a1 = ((uint32_t)packed >> 8)  & 0xFF;
  int a2 = ((uint32_t)packed >> 16) & 0xFF;
  int a3 = ((uint32_t)packed >> 24) & 0xFF;
  ARGLIST(5);
  STARG(0, ((dyn*)L)[a0]);
  STARG(1, ((dyn*)L)[a1]);
  STARG(2, ((dyn*)L)[a2]);
  STARG(3, ((dyn*)L)[a3]);
  STARG(4, ((dyn*)L)[a4]);
}

/* SBC_MOVETX / MOVETX8: L[dst] = sbc->tx[src] (text constant
 * table lookup).  Packed arg: dst in upper 32, src in lower 32. */
static void jit_rt_movetx_impl(int64_t *L, struct sbc_t *sbc, uint64_t packed) {
  uint32_t dst = (uint32_t)(packed >> 32);
  uint32_t src = (uint32_t)(packed & 0xFFFFFFFF);
  ((dyn*)L)[dst] = sbc->tx[src];
}
static void jit_rt_call_impl(int64_t *L, int dst, int fn, int unused) {
  (void)unused;
  CALL(((dyn*)L)[dst], ((dyn*)L)[fn]);
}
static void jit_rt_callir_impl(int64_t *L, int fn, int u1, int u2) {
  (void)u1; (void)u2;
  dyn dummy;
  CALL(dummy, ((dyn*)L)[fn]);
}
static void jit_rt_callt_impl(int64_t *L, int dst, int fn, int unused) {
  (void)unused;
  CALL_TAGGED(((dyn*)L)[dst], ((dyn*)L)[fn]);
}
static void jit_rt_calltir_impl(int64_t *L, int fn, int u1, int u2) {
  (void)u1; (void)u2;
  dyn dummy;
  CALL_TAGGED(dummy, ((dyn*)L)[fn]);
}

/* MCALL dispatcher.  Mirrors the MCACHE_CALL macro in sbc.c
 * but reads the mcache slot index from the packed arg instead
 * of the bytecode stream's RD16.  Cache layout: see runtime/
 * sif.h's mcache_t.  Hit fast path: one load of mce->fn.  Miss
 * path: call get_method_for_tag and fill the triple. */
static void jit_rt_mcall_impl(int64_t *L, struct sbc_t *sbc, uint64_t packed) {
  uint32_t mcache_idx = (uint32_t)((packed >> 48) & 0xFFFF);
  int      met        = (int)((packed >> 32) & 0xFFFF);
  uint32_t obj        = (uint32_t)((packed >> 16) & 0xFFFF);
  uint32_t dst        = (uint32_t)(packed & 0xFFFF);
  int m = sbc->mt[met];
  api.method = m;
  dyn oo = ((dyn*)L)[obj];
  mcache_t *mce = &sbc->mcaches[mcache_idx];
  uint32_t tid = O_TAG(oo);
  dyn mfn;
  if (mce->tid != tid || mce->mid != (uint32_t)m) {
    mfn = get_method_for_tag(m, tid);
    mce->tid = tid;
    mce->mid = (uint32_t)m;
    mce->fn  = mfn;
  } else {
    mfn = mce->fn;
  }
  CALL(((dyn*)L)[dst], mfn);
}
static void jit_rt_mcallir_impl(int64_t *L, struct sbc_t *sbc, uint64_t packed) {
  uint32_t mcache_idx = (uint32_t)((packed >> 48) & 0xFFFF);
  int      met        = (int)((packed >> 32) & 0xFFFF);
  uint32_t obj        = (uint32_t)((packed >> 16) & 0xFFFF);
  int m = sbc->mt[met];
  api.method = m;
  dyn oo = ((dyn*)L)[obj];
  mcache_t *mce = &sbc->mcaches[mcache_idx];
  uint32_t tid = O_TAG(oo);
  dyn mfn;
  if (mce->tid != tid || mce->mid != (uint32_t)m) {
    mfn = get_method_for_tag(m, tid);
    mce->tid = tid;
    mce->mid = (uint32_t)m;
    mce->fn  = mfn;
  } else {
    mfn = mce->fn;
  }
  dyn dummy;
  CALL(dummy, mfn);
}

/* Trampoline helper for SBC_COPY.  Mirrors sbc.c:1153:
 *   COPY(L[dst], dindex, L[src], sindex)
 *   == LSET(L[dst], dindex, LGET(L[src], sindex))
 * The two 16-bit field indices are packed in the 4th arg:
 *   [31:16] = dindex (target field)
 *   [15: 0] = sindex (source field) */
static void jit_rt_copy_impl(int64_t *L, int dst, int src, int packed) {
  int dindex = (packed >> 16) & 0xFFFF;
  int sindex =  packed        & 0xFFFF;
  COPY(((dyn*)L)[dst], dindex, ((dyn*)L)[src], sindex);
}

/* Trampoline helper for SBC_LIST.  Mirrors sbc.c:973:
 *   LIST(L[dst], size)  (== OBJECT(dst, T_LIST, size) == gc_alloc).
 * `size` is the number of slots in the new list, not a slot
 * index -- the third "slot" arg of helper3 is repurposed as
 * a literal. */
static void jit_rt_list_impl(int64_t *L, int dst, int size, int unused) {
  (void)unused;
  LIST(((dyn*)L)[dst], size);
}

/* Trampoline helper for SBC_ST4_0..SBC_ST4_F.  Mirrors the
 * interpreter body in sbc.c:1089:
 *   STOR(L[dst], index, L[src])  (== LSET / lsetm)
 * `dst` is the slot holding the target heap object; `src` is
 * the slot holding the value to write into its `index`-th
 * field.  lsetm handles the GC write barrier. */
static void jit_rt_st4_impl(int64_t *L, int dst, int src, int index) {
  STOR((dyn)L[dst], index, (dyn)L[src]);
}

/* Trampoline helper for SBC_CLOSURE.  Mirrors sbc.c:864:
 *   void *fn = (void*)sbc->hooks[idx];
 *   CLOSURE(L[dst], fn, size);
 *
 * Args packed into one 64-bit word so the call fits in three
 * integer-arg registers (L, sbc, packed) -- no Win64 stack arg
 * required.  See the BC_CLOSURE handler in jit.c for the
 * packing layout. */
static void jit_rt_closure_impl(int64_t *L, struct sbc_t *sbc, uint64_t packed) {
  uint32_t dst  = (uint32_t)(packed >> 32);
  uint32_t idx  = (uint32_t)((packed >> 16) & 0xFFFF);
  uint32_t size = (uint32_t)(packed & 0xFF);
  void *fn = (void*)sbc->hooks[idx];
  CLOSURE(((dyn*)L)[dst], fn, size);
}

/* Install the helpers on first audit so the standalone JIT
 * self-test (which doesn't link jit_sbc.c) keeps the pointers
 * NULL and bails out cleanly on the corresponding opcodes. */
static void jit_install_helpers_once(void) {
  if (jit_rt_ld4_helper) return;
  jit_rt_ld4_helper     = jit_rt_ld4_impl;
  jit_rt_st4_helper     = jit_rt_st4_impl;
  jit_rt_list_helper    = jit_rt_list_impl;
  jit_rt_copy_helper    = jit_rt_copy_impl;
  jit_rt_closure_helper = jit_rt_closure_impl;
  jit_rt_arglist0_helper = jit_rt_arglist0_impl;
  jit_rt_arglist1_helper = jit_rt_arglist1_impl;
  jit_rt_arglist2_helper = jit_rt_arglist2_impl;
  jit_rt_arglist3_helper = jit_rt_arglist3_impl;
  jit_rt_call_helper     = jit_rt_call_impl;
  jit_rt_callir_helper   = jit_rt_callir_impl;
  jit_rt_callt_helper    = jit_rt_callt_impl;
  jit_rt_calltir_helper  = jit_rt_calltir_impl;
  jit_rt_mcall_helper    = jit_rt_mcall_impl;
  jit_rt_mcallir_helper  = jit_rt_mcallir_impl;
  jit_rt_arglist4_helper = jit_rt_arglist4_impl;
  jit_rt_arglist5_helper = jit_rt_arglist5_impl;
  jit_rt_movetx_helper   = jit_rt_movetx_impl;
}

/* Read a 24-bit little-endian unsigned int.  Mirrors the
 * RD24 macro used by the interpreter's main dispatch loop. */
static uint32_t fntbl_read24(const uint8_t *p) {
  return (uint32_t)p[0]
       | ((uint32_t)p[1] << 8)
       | ((uint32_t)p[2] << 16);
}

/* Compare two uint32_t for qsort. */
static int u32_cmp(const void *a, const void *b) {
  uint32_t ua = *(const uint32_t*)a;
  uint32_t ub = *(const uint32_t*)b;
  return (ua > ub) - (ua < ub);
}

int sbc_jit_audit(struct sbc_t *sbc) {
  if (!sbc || !sbc->fntbl || sbc->fntbl_sz < 3) return 0;
  jit_install_helpers_once();

  int nfns = (int)(sbc->fntbl_sz / 3);
  if (nfns <= 0) return 0;

  /* Collect each function's entry offset; sort so consecutive
   * offsets bound each function's bytecode range.  Sentinel at
   * the end is the start of the next section (code_sz). */
  uint32_t *offs = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(nfns + 1));
  if (!offs) return 0;
  for (int i = 0; i < nfns; i++) {
    offs[i] = fntbl_read24(sbc->fntbl + i * 3);
  }
  offs[nfns] = sbc->code_sz;
  qsort(offs, (size_t)nfns + 1, sizeof(uint32_t), u32_cmp);

  /* Each function's on-disk layout (matches `sbc_exec_fn`):
   *   [0]    SBC_SUBR opcode
   *   [1..2] subroutine identifier (LE u16)
   *   [3..4] nvars                  (LE u16)
   *   [5..]  bytecode body
   * So the body starts 5 bytes past the offset.  Body length
   * is (next-offset) - (this-offset) - 5. */
  const size_t HEADER = 5;

  int jit_count = 0;
  int skipped_short = 0;
  uint32_t fail_hist[256] = {0};
  for (int i = 0; i < nfns; i++) {
    uint32_t start = offs[i];
    uint32_t end   = offs[i + 1];
    if (end <= start + HEADER) { skipped_short++; continue; }
    /* Defensive: bytecode start must be SBC_SUBR.  If it isn't,
     * the offsets table is malformed -- skip cleanly. */
    if (sbc->code[start] != SBC_SUBR) { skipped_short++; continue; }

    const uint8_t *body = sbc->code + start + HEADER;
    size_t body_len = (size_t)end - start - HEADER;

    jit_buf *jit = jit_translate_with_sbc(body, body_len);
    if (jit) {
      jit_count++;
      jit_buf_free(jit);
    } else {
      fail_hist[jit_last_fail_opcode]++;
    }
  }

  free(offs);
  if (skipped_short) {
    fprintf(stderr,
            "jit-audit: %d/%d functions translatable "
            "(%d skipped as too-short)\n",
            jit_count, nfns, skipped_short);
  } else {
    fprintf(stderr,
            "jit-audit: %d/%d functions translatable\n",
            jit_count, nfns);
  }

  /* List the 5 opcodes that blocked translation most often.
   * Sort by descending count via a simple selection. */
  int top = 0;
  uint8_t shown[256] = {0};
  fprintf(stderr, "jit-audit: top blockers:");
  while (top < 5) {
    uint32_t best_count = 0;
    int      best_op = -1;
    for (int op = 0; op < 256; op++) {
      if (shown[op]) continue;
      if (fail_hist[op] > best_count) {
        best_count = fail_hist[op];
        best_op = op;
      }
    }
    if (best_op < 0 || best_count == 0) break;
    fprintf(stderr, "  0x%02X(%u)", best_op, (unsigned)best_count);
    shown[best_op] = 1;
    top++;
  }
  fprintf(stderr, "\n");

  return jit_count;
}
