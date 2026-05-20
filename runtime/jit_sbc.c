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
#include <string.h>
#include <float.h>

#ifdef _WIN32
#include <windows.h>

/* Register Windows x64 SEH unwind info for a JIT'd function.
 *
 * Without this, RtlUnwindEx (called by longjmp / `bad`) sees
 * JIT'd frames as having no `.pdata` RUNTIME_FUNCTION entry
 * and either skips them silently or aborts the process.  The
 * step-5 static-check tests hit the abort path when two
 * JIT'd frames stack up (caller + JIT'd callee).
 *
 * Layout:  the jit_buf is page-aligned and oversized for the
 * code; we tuck the UNWIND_INFO and RUNTIME_FUNCTION right
 * after the code, in the same allocation.  BaseAddress passed
 * to RtlAddFunctionTable is the code start; the RVAs in
 * RUNTIME_FUNCTION are offsets from there.
 *
 * Only the 2-arg prologue is registered (the one used by
 * jit_translate_with_sbc / sbc_jit_install).  The 1-arg
 * variant is self-test only and never longjmps, so it doesn't
 * need unwind info. */
static void jit_register_unwind_2arg(void *jit_code, size_t code_size) {
  size_t uw_off = (code_size + 3) & ~(size_t)3;
  size_t rt_off = uw_off + 16;  /* 16 bytes max for UNWIND_INFO */
  uint8_t *uw   = (uint8_t*)jit_code + uw_off;

  /* 2-arg prologue layout (matches jit_emit_prologue2):
   *   0x00 (1 byte):  push rbx
   *   0x01 (2 bytes): push r12
   *   0x03 (3 bytes): mov rbx, <arg0>
   *   0x06 (3 bytes): mov r12, <arg1>
   *   0x09 (4 bytes): sub rsp, 40
   *   0x0d:           body starts
   *
   * UNWIND_INFO header (4 bytes) + UNWIND_CODEs (in REVERSE
   * prologue order, 2 bytes each). */
  uw[0] = 0x01;             /* Version=1, Flags=0      */
  uw[1] = 0x0d;             /* SizeOfProlog            */
  uw[2] = 0x03;             /* CountOfCodes            */
  uw[3] = 0x00;             /* FrameReg=0, FrameOff=0  */

  /* UNWIND_CODE 0: UWOP_ALLOC_SMALL 40 at offset 0x0d.
   *   CodeOffset = 0x0d
   *   UnwindOp   = 2 (UWOP_ALLOC_SMALL)
   *   OpInfo     = (40/8) - 1 = 4 */
  uw[4] = 0x0d;  uw[5] = (4 << 4) | 2;

  /* UNWIND_CODE 1: UWOP_PUSH_NONVOL R12 at offset 0x03.
   *   UnwindOp = 0, OpInfo = 12 (R12). */
  uw[6] = 0x03;  uw[7] = (12 << 4) | 0;

  /* UNWIND_CODE 2: UWOP_PUSH_NONVOL RBX at offset 0x01.
   *   UnwindOp = 0, OpInfo = 3 (RBX). */
  uw[8] = 0x01;  uw[9] = (3 << 4) | 0;

  /* RUNTIME_FUNCTION sits 4-aligned right after UNWIND_INFO. */
  PRUNTIME_FUNCTION rt = (PRUNTIME_FUNCTION)((uint8_t*)jit_code + rt_off);
  rt->BeginAddress = 0;
  rt->EndAddress   = (DWORD)code_size;
  rt->UnwindData   = (DWORD)uw_off;

  if (!RtlAddFunctionTable(rt, 1, (DWORD64)jit_code)) {
    fprintf(stderr, "jit-install: RtlAddFunctionTable failed at %p\n", jit_code);
  }
}
#else
/* POSIX: no-op for now.  Linux uses DWARF .eh_frame; we'd
 * need to emit __register_frame data.  Leaving as a TODO --
 * non-Windows builds still work for any JIT'd function that
 * doesn't have an inner longjmp through its frame. */
static void jit_register_unwind_2arg(void *jit_code, size_t code_size) {
  (void)jit_code; (void)code_size;
}
#endif

/* Full FXN* helpers.  Each mirrors the corresponding interpreter
 * opcode's 3-way dispatch from sbc.c -- int-int via the FXN*
 * macro, int-float via float promotion, non-int via MCALL to
 * the m_<op> method id.  Without these the JIT'd code uses the
 * int-only fallbacks in jit.c, which produces garbage for any
 * non-int operand (the game's `Seconds*$ups` where $ups is
 * float was the original failure mode). */
extern int m_add, m_sub, m_mul, m_div, m_rem;

static void jit_rt_fxnadd_full(int64_t *L, int dst, int a, int b) {
  dyn aa = (dyn)L[a];
  if (TAGIS(T_INT, aa)) {
    dyn bb = (dyn)L[b];
    if (TAGIS(T_INT, bb)) FXNADD(((dyn*)L)[dst], aa, bb);
    else {
      float fa, fb;
      fa = (float)UNFXN(aa);
      STFLT(fb, bb);
      LDFLT(((dyn*)L)[dst], fa + fb);
    }
  } else {
    ARGLIST2(((dyn*)L)[a], ((dyn*)L)[b]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_add);
  }
}
static void jit_rt_fxnsub_full(int64_t *L, int dst, int a, int b) {
  dyn aa = (dyn)L[a];
  if (TAGIS(T_INT, aa)) {
    dyn bb = (dyn)L[b];
    if (TAGIS(T_INT, bb)) FXNSUB(((dyn*)L)[dst], aa, bb);
    else {
      float fa, fb;
      fa = (float)UNFXN(aa);
      STFLT(fb, bb);
      LDFLT(((dyn*)L)[dst], fa - fb);
    }
  } else {
    ARGLIST2(((dyn*)L)[a], ((dyn*)L)[b]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_sub);
  }
}
static void jit_rt_fxnmul_full(int64_t *L, int dst, int a, int b) {
  dyn aa = (dyn)L[a];
  if (TAGIS(T_INT, aa)) {
    dyn bb = (dyn)L[b];
    if (TAGIS(T_INT, bb)) FXNMUL(((dyn*)L)[dst], aa, bb);
    else {
      float fa, fb;
      fa = (float)UNFXN(aa);
      STFLT(fb, bb);
      LDFLT(((dyn*)L)[dst], fa * fb);
    }
  } else {
    ARGLIST2(((dyn*)L)[a], ((dyn*)L)[b]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_mul);
  }
}
static void jit_rt_fxndiv_full(int64_t *L, int dst, int a, int b) {
  dyn aa = (dyn)L[a];
  if (TAGIS(T_INT, aa)) {
    dyn bb = (dyn)L[b];
    if (TAGIS(T_INT, bb)) FXNDIV(((dyn*)L)[dst], aa, bb);
    else {
      float fa, fb;
      fa = (float)UNFXN(aa);
      STFLT(fb, bb);
      if (fb == 0.0f) fb = FLT_MIN;
      LDFLT(((dyn*)L)[dst], fa / fb);
    }
  } else {
    ARGLIST2(((dyn*)L)[a], ((dyn*)L)[b]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_div);
  }
}
static void jit_rt_fxnrem_full(int64_t *L, int dst, int a, int b) {
  dyn aa = (dyn)L[a];
  if (TAGIS(T_INT, aa)) {
    dyn bb = (dyn)L[b];
    if (TAGIS(T_INT, bb)) FXNREM(((dyn*)L)[dst], aa, bb);
    else {
      float fa, fb, r;
      fa = (float)UNFXN(aa);
      STFLT(fb, bb);
      if (fb == 0.0f) fb = FLT_MIN;
      r = fa / fb;
      LDFLT(((dyn*)L)[dst], (r - (float)(int64_t)r) * fb);
    }
  } else {
    ARGLIST2(((dyn*)L)[a], ((dyn*)L)[b]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_rem);
  }
}

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
  /* Swap the FXN* helpers from int-only fallbacks to full
   * dispatch -- the game's `Seconds*$ups` etc. need this. */
  jit_rt_fxnadd_helper  = jit_rt_fxnadd_full;
  jit_rt_fxnsub_helper  = jit_rt_fxnsub_full;
  jit_rt_fxnmul_helper  = jit_rt_fxnmul_full;
  jit_rt_fxndiv_helper  = jit_rt_fxndiv_full;
  jit_rt_fxnrem_helper  = jit_rt_fxnrem_full;
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

/* ============================================================
 * Step 5k: dispatch wiring.
 *
 * Each JIT'd function gets a small heap-allocated payload
 * struct, and the corresponding hook in hooks_heap is rewritten
 * so that the handler is `jit_adapter` (instead of
 * `sbc_exec_fn`) and the payload is our struct instead of pin.
 *
 * `jit_adapter` matches the `dyn(uint8_t*)` signature the hook
 * system requires.  It allocates the function's frame on the
 * C stack (same VLA pattern as the SUBR macro), populates
 * L[0]=current closure / L[1]=current args, zeroes the rest,
 * then calls the JIT'd body with `(L, sbc)`.
 *
 * The CALL macro saves and restores `api.frame` around the
 * handler call, so the adapter doesn't need to restore it.
 *
 * Gated on SYMTA_JIT_RUN at sbc_prepare time -- so a normal
 * build / sweep / drift run is unaffected.  Setting both
 * SYMTA_JIT_AUDIT and SYMTA_JIT_RUN runs the audit first
 * (reporting coverage), then installs the JIT'd hooks. */

typedef struct {
  void *jit_body;        /* (dyn(*)(int64_t*, struct sbc_t*)) */
  struct sbc_t *sbc;
  int nvars;
} jit_adapter_payload_t;

static dyn jit_adapter(uint8_t *payload_ptr) {
  jit_adapter_payload_t *p = (jit_adapter_payload_t*)payload_ptr;
  int nvars = p->nvars;

  /* Inline SUBR / PROLOGUE: allocate frame on the C stack via
   * VLA, link into the api.frame chain, zero non-special
   * slots, then populate L[0]=closure / L[1]=args. */
  void *L_blk_[FRAME_PREFIX_SLOTS + nvars];
  frame_t *frm_ = (frame_t*)L_blk_;
  void **L = L_blk_ + FRAME_PREFIX_SLOTS;
  frm_->prev = api.frame;
  frm_->clsr = api.clsr_pending;
  frm_->pin = 0;
  frm_->nvars = nvars;
  api.frame = frm_;
  {
    void **q_ = L + 2;
    void **e_ = L + nvars;
    while (q_ < e_) *q_++ = 0;
  }
  L[0] = frm_->clsr;
  L[1] = api.args;

  /* Call the JIT'd body. */
  typedef dyn (*jit_fn_t)(int64_t*, struct sbc_t*);
  return ((jit_fn_t)p->jit_body)((int64_t*)L, p->sbc);
}

/* Rewrites hook entries for JIT'd functions so CALL/MCALL
 * dispatches through jit_adapter -> JIT'd body instead of
 * through sbc_exec_fn -> interpreter.
 *
 * Returns the number of functions successfully installed.
 *
 * Strategy: walk fntbl, translate each function via
 * jit_translate_with_sbc.  On success, allocate a payload
 * struct, finalize the jit_buf as executable, and overwrite
 * the corresponding hooks_heap entry's handler+payload.  On
 * failure, leave the hook untouched (interpreter handles it). */
int sbc_jit_install(struct sbc_t *sbc) {
  if (!sbc || !sbc->fntbl || sbc->fntbl_sz < 3) return 0;
  jit_install_helpers_once();

  /* Bisection helper: SYMTA_JIT_FILTER limits JIT install to
   * SBCs whose filename contains the given substring.  Useful
   * for narrowing down which SBC's JIT'd code is causing a
   * runtime crash.  Examples:
   *   SYMTA_JIT_FILTER=core_     -- only core_.sbc gets JIT
   *   SYMTA_JIT_FILTER=compiler  -- only compiler.sbc
   *   (unset)                    -- all SBCs (default) */
  {
    const char *filter = getenv("SYMTA_JIT_FILTER");
    if (filter && filter[0] && sbc->filename) {
      if (!strstr(sbc->filename, filter)) {
        if (getenv("SYMTA_JIT_VERBOSE")) {
          fprintf(stderr, "jit-install: SKIP %s (filter=%s)\n",
                  sbc->filename, filter);
        }
        return 0;
      }
    }
  }

  int nfns = (int)(sbc->fntbl_sz / 3);
  if (nfns <= 0) return 0;

  uint32_t *offs = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(nfns + 1));
  if (!offs) return 0;
  /* Build [(orig_idx, ofs)] table -- we need the original index
   * back since hooks[i] indexes by ORIGINAL ordering, not sort. */
  for (int i = 0; i < nfns; i++) {
    offs[i] = fntbl_read24(sbc->fntbl + i * 3);
  }
  offs[nfns] = sbc->code_sz;

  /* For body length we still need the sorted positions to know
   * where each function ends.  Make a sorted copy. */
  uint32_t *sorted = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(nfns + 1));
  if (!sorted) { free(offs); return 0; }
  for (int i = 0; i <= nfns; i++) sorted[i] = offs[i];
  qsort(sorted, (size_t)nfns + 1, sizeof(uint32_t), u32_cmp);

  int installed = 0;
  const size_t HEADER = 5;

  /* Bisection helpers:
   *   SYMTA_JIT_MAX_FN=N    -- install at most N functions
   *   SYMTA_JIT_SKIP_FN=N   -- install all EXCEPT the Nth
   * Combined with SYMTA_JIT_FILTER, isolates a single fn. */
  int max_fn  = -1;
  int skip_fn = -1;
  {
    const char *v;
    if ((v = getenv("SYMTA_JIT_MAX_FN"))  && v[0]) max_fn  = atoi(v);
    if ((v = getenv("SYMTA_JIT_SKIP_FN")) && v[0]) skip_fn = atoi(v);
  }

  for (int i = 0; i < nfns; i++) {
    if (max_fn >= 0 && installed >= max_fn) break;
    if (skip_fn >= 0 && installed == skip_fn) {
      /* Skip THIS one but keep going.  We need to bump
       * `installed` to keep the indices aligned with the
       * print log; do that by translating-and-discarding. */
      installed++;
      continue;
    }
    uint32_t start = offs[i];
    /* Find this start's slot in sorted -> next-sorted is the
     * upper bound on the body's bytecode range. */
    int s;
    for (s = 0; s < nfns; s++) if (sorted[s] == start) break;
    uint32_t end = sorted[s + 1];
    if (end <= start + HEADER) continue;
    if (sbc->code[start] != SBC_SUBR) continue;

    const uint8_t *body = sbc->code + start + HEADER;
    size_t body_len = (size_t)end - start - HEADER;
    int nvars = (int)((uint32_t)sbc->code[start + 3]
                    | ((uint32_t)sbc->code[start + 4] << 8));

    jit_buf *jb = jit_translate_with_sbc(body, body_len);
    if (!jb) continue;

    void *jit_code = jit_buf_finalize(jb);
    /* NOTE: we deliberately DON'T jit_buf_free(jb) -- the
     * executable memory must outlive sbc_prepare.  The
     * jit_buf struct itself leaks, but that's a few dozen
     * bytes per installed function (the code is what matters,
     * and we keep that). */

    /* Register Windows SEH unwind info so longjmp through this
     * frame works.  Zero-cost on the happy path -- the table
     * entry just sits in memory until RtlUnwindEx looks it up. */
    jit_register_unwind_2arg(jit_code, jb->len);

    jit_adapter_payload_t *payload =
      (jit_adapter_payload_t*)malloc(sizeof(*payload));
    if (!payload) continue;
    payload->jit_body = jit_code;
    payload->sbc      = sbc;
    payload->nvars    = nvars;

    /* Overwrite the hook entry. */
    uint32_t hook_idx = (uint32_t)sbc->hooks[i];
    hooks_heap[hook_idx].handler = (psf_t)&jit_adapter;
    hooks_heap[hook_idx].payload = (uint8_t*)payload;
    if (getenv("SYMTA_JIT_PRINT_FNS")) {
      /* Print each installed fn's index + byte range + name (if
       * fnmeta is populated) so we can map "the 38th translated
       * fn crashes" back to a specific source-level fn. */
      const char *fname = "?";
      int row = 0, col = 0;
      if (sbc->rtot && sbc->rtot[0].table) {
        fn_meta_t *t = (fn_meta_t*)sbc->rtot[0].table;
        if (i < (int)sbc->rtot[0].size && t[i].name) {
          fname = (const char*)t[i].name;
          row = t[i].row;
          col = t[i].col;
        }
      }
      fprintf(stderr, "jit-fn[%d]: idx=%d body_offset=0x%x len=%zu row=%d col=%d name=%s\n",
              installed, i, (unsigned)(start + HEADER), body_len, row, col, fname);
      /* Dump the opcode stream so we can see what shape this
       * function has -- helps spot which BC_* path is buggy. */
      fprintf(stderr, "  bytes:");
      for (size_t k = 0; k < body_len; k++) {
        fprintf(stderr, " %02x", body[k]);
      }
      fprintf(stderr, "\n");
      fflush(stderr);
    }
    installed++;
  }

  free(sorted);
  free(offs);
  /* Verbose install summary, gated to keep `SYMTA_JIT_RUN=1`
   * usable in test runs whose goldens include stderr.  Set
   * SYMTA_JIT_VERBOSE=1 alongside to see per-sbc counts. */
  if (getenv("SYMTA_JIT_VERBOSE")) {
    fprintf(stderr, "jit-install: %d/%d functions installed for %s\n",
            installed, nfns, sbc->filename);
  }
  return installed;
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
