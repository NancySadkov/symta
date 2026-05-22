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
/* Phase 2a-aware: emit UNWIND_INFO matching either the no-pins
 * (2 push + 1 sub-rsp) or the pin-active (5 push + 1 sub-rsp)
 * prologue.  `pinned_count > 0` means R13..R15 were pushed
 * after R12 (always all three when any one is in use).
 * `prologue_size` is the byte offset right after the final
 * sub-rsp K -- the function body starts there.
 *
 * Allocated unwind blob size: 4-byte header + 2 bytes per
 * UNWIND_CODE.  Pin-inactive: 3 codes -> 10 bytes.  Pin-active:
 * 6 codes -> 16 bytes.  We bump the allocation to 24 bytes
 * (header + 6 codes + pad to even) so both variants fit and
 * the RUNTIME_FUNCTION lands 4-aligned. */
static void jit_register_unwind_2arg(void *jit_code, size_t code_size,
                                      int pinned_count,
                                      uint16_t prologue_size) {
  size_t uw_off = (code_size + 3) & ~(size_t)3;
  size_t rt_off = uw_off + 24;  /* 24 bytes max for UNWIND_INFO */
  uint8_t *uw   = (uint8_t*)jit_code + uw_off;

  if (pinned_count > 0) {
    /* Pin-active 2-arg prologue layout (Win64, Phase 2b):
     *   0x00 (1 byte):  push rbx
     *   0x01 (2 bytes): push r12
     *   0x03 (2 bytes): push r13
     *   0x05 (2 bytes): push r14
     *   0x07 (2 bytes): push r15
     *   0x09 (1 byte):  push rsi          <-- Phase 2b, Stage 2
     *   0x0a (3 bytes): mov rbx, rcx
     *   0x0d (3 bytes): mov r12, rdx
     *   0x10 (variable): emit_reload_pinned   (not a prologue op
     *                                          per SEH; loads only)
     *   0x..: sub rsp, 40
     *   prologue_size:  body starts
     *
     * UNWIND_CODEs in REVERSE prologue order: alloc, push rsi,
     * push r15, push r14, push r13, push r12, push rbx.
     * Total: 7 codes -> 4-byte header + 14 bytes -> 18 bytes.
     * The 24-byte allocation has room. */
    uw[0] = 0x01;                       /* Version=1, Flags=0 */
    uw[1] = (uint8_t)prologue_size;     /* SizeOfProlog */
    uw[2] = 7;                          /* CountOfCodes */
    uw[3] = 0x00;                       /* FrameReg=0 */

    /* UWOP_ALLOC_SMALL 40 (OpInfo = 40/8 - 1 = 4) at prologue
     * end -- the sub rsp grew from 32 to 40 with the extra push. */
    uw[4]  = (uint8_t)prologue_size;
    uw[5]  = (4 << 4) | 2;
    /* UWOP_PUSH_NONVOL RSI at offset 10 (just after the 6th
     * push; 1+2+2+2+2+1 = 10).  RSI = reg 6, op_info=6. */
    uw[6]  = 10;  uw[7]  = (6  << 4) | 0;
    /* push r15 completes at offset 9. */
    uw[8]  = 9;   uw[9]  = (15 << 4) | 0;
    uw[10] = 7;   uw[11] = (14 << 4) | 0;
    uw[12] = 5;   uw[13] = (13 << 4) | 0;
    uw[14] = 3;   uw[15] = (12 << 4) | 0;
    uw[16] = 1;   uw[17] = (3  << 4) | 0;  /* RBX */
  } else {
    /* No-pins 2-arg prologue (matches jit_emit_prologue2):
     *   0x00 (1 byte):  push rbx
     *   0x01 (2 bytes): push r12
     *   0x03 (3 bytes): mov rbx, <arg0>
     *   0x06 (3 bytes): mov r12, <arg1>
     *   0x09 (4 bytes): sub rsp, 40
     *   0x0d:           body starts */
    uw[0] = 0x01;             /* Version=1, Flags=0 */
    uw[1] = (uint8_t)prologue_size;   /* SizeOfProlog (= 0x0d) */
    uw[2] = 0x03;             /* CountOfCodes */
    uw[3] = 0x00;             /* FrameReg=0 */

    /* UWOP_ALLOC_SMALL 40 (OpInfo = 40/8 - 1 = 4) at prologue end. */
    uw[4] = (uint8_t)prologue_size;
    uw[5] = (4 << 4) | 2;
    uw[6] = 0x03;  uw[7] = (12 << 4) | 0;  /* push R12 */
    uw[8] = 0x01;  uw[9] = (3  << 4) | 0;  /* push RBX */
  }

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
static void jit_register_unwind_2arg(void *jit_code, size_t code_size,
                                      int pinned_count,
                                      uint16_t prologue_size) {
  (void)jit_code; (void)code_size;
  (void)pinned_count; (void)prologue_size;
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

/* SBC_FXNLISTN: L[dst] = LIST(UNFXN(L[src])).  Mirrors sbc.c's
 * SBC_FXNLISTN body exactly -- the size comes from a runtime
 * slot containing a FXN-tagged int, not a literal. */
static void jit_rt_fxnlistn_impl(int64_t *L, int dst, int src, int u) {
  (void)u;
  FXNLISTN(((dyn*)L)[dst], ((dyn*)L)[src]);
}

/* SBC_NEG / SBC_ABS: unary arith.  Match sbc.c bodies.
 * NEG: T_INT inline (FXNNEG); else MCALL m_neg.
 * ABS: T_INT inline (|val|); T_FLOAT inline (fabsf); else MCALL m_abs. */
static void jit_rt_neg_impl(int64_t *L, int dst, int a, int u) {
  (void)u;
  dyn aa = ((dyn*)L)[a];
  if (TAGIS(T_INT, aa)) {
    FXNNEG(((dyn*)L)[dst], aa);
  } else {
    ARGLIST1(((dyn*)L)[a]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_neg);
  }
}
static void jit_rt_abs_impl(int64_t *L, int dst, int a, int u) {
  (void)u;
  dyn aa = ((dyn*)L)[a];
  if (TAGIS(T_INT, aa)) {
    int64_t val = UNFXN(aa);
    if (val < 0) aa = (dyn)(int64_t)FXN(-val);
    ((dyn*)L)[dst] = aa;
  } else if (TAGIS(T_FLOAT, aa)) {
    float fa;
    STFLT(fa, aa);
    if (fa < 0.0f) fa = -fa;
    LDFLT(((dyn*)L)[dst], fa);
  } else {
    ARGLIST1(((dyn*)L)[a]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_abs);
  }
}

/* SBC_FXNAND / IOR / XOR / SHL / SHR: bitwise ops with T_INT-T_INT
 * fast path and MCALL fallback (m_and/m_ior/m_xor/m_shl/m_shr). */
static void jit_rt_fxnand_impl(int64_t *L, int dst, int a, int b) {
  dyn aa = ((dyn*)L)[a], bb = ((dyn*)L)[b];
  if (TAGIS(T_INT, aa) && TAGIS(T_INT, bb)) FXNAND(((dyn*)L)[dst], aa, bb);
  else {
    ARGLIST2(((dyn*)L)[a], ((dyn*)L)[b]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_and);
  }
}
static void jit_rt_fxnior_impl(int64_t *L, int dst, int a, int b) {
  dyn aa = ((dyn*)L)[a], bb = ((dyn*)L)[b];
  if (TAGIS(T_INT, aa) && TAGIS(T_INT, bb)) FXNIOR(((dyn*)L)[dst], aa, bb);
  else {
    ARGLIST2(((dyn*)L)[a], ((dyn*)L)[b]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_ior);
  }
}
static void jit_rt_fxnxor_impl(int64_t *L, int dst, int a, int b) {
  dyn aa = ((dyn*)L)[a], bb = ((dyn*)L)[b];
  if (TAGIS(T_INT, aa) && TAGIS(T_INT, bb)) FXNXOR(((dyn*)L)[dst], aa, bb);
  else {
    ARGLIST2(((dyn*)L)[a], ((dyn*)L)[b]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_xor);
  }
}
static void jit_rt_fxnshl_impl(int64_t *L, int dst, int a, int b) {
  dyn aa = ((dyn*)L)[a], bb = ((dyn*)L)[b];
  if (TAGIS(T_INT, aa) && TAGIS(T_INT, bb)) FXNSHL(((dyn*)L)[dst], aa, bb);
  else {
    ARGLIST2(((dyn*)L)[a], ((dyn*)L)[b]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_shl);
  }
}
static void jit_rt_fxnshr_impl(int64_t *L, int dst, int a, int b) {
  dyn aa = ((dyn*)L)[a], bb = ((dyn*)L)[b];
  if (TAGIS(T_INT, aa) && TAGIS(T_INT, bb)) FXNSHR(((dyn*)L)[dst], aa, bb);
  else {
    ARGLIST2(((dyn*)L)[a], ((dyn*)L)[b]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_shr);
  }
}

/* SBC_FXNLSET (with-result variant).  Same dispatch as FXNLSETIR
 * but writes the call's return value into L[dst].  5 u16 operands
 * arrive as two packed u64s.
 *   packed1: [15:0]=dst [31:16]=src [47:32]=index [63:48]=val
 *   packed2: [15:0]=mcache_idx */
static void jit_rt_fxnlset_impl(int64_t *L, struct sbc_t *sbc,
                                uint64_t packed1, uint64_t packed2) {
  uint32_t dst   = (uint32_t)(packed1 & 0xFFFF);
  uint32_t src   = (uint32_t)((packed1 >> 16) & 0xFFFF);
  uint32_t index = (uint32_t)((packed1 >> 32) & 0xFFFF);
  uint32_t val   = (uint32_t)((packed1 >> 48) & 0xFFFF);
  uint32_t mcidx = (uint32_t)(packed2 & 0xFFFF);
  dyn ss = ((dyn*)L)[src];
  dyn ii = ((dyn*)L)[index];
  if (TAGIS(T_LIST, ss) && TAGIS(T_INT, ii)
      && (uint64_t)ii < (uint64_t)FXN(LIST_SIZE(ss))) {
    FXNLSET(((dyn*)L)[dst], ss, ii, ((dyn*)L)[val]);
  } else {
    /* Bug #14 fix: ARGLIST3 allocates; ss/ii are stale C-locals
     * that could be invalidated by GC during the LIST alloc.
     * Reload from frame slots after ARGLIST.  Tag bits in the stale
     * `ss` are still valid (encoding-stable across GC), so the
     * O_TAG-based mcache key below is safe to compute pre-reload. */
    ARGLIST(3);
    STARG(0, ((dyn*)L)[src]);
    STARG(1, ((dyn*)L)[index]);
    STARG(2, ((dyn*)L)[val]);
    api.method = m_set;
    mcache_t *mce = &sbc->mcaches[mcidx];
    uint32_t tid = O_TAG(((dyn*)L)[src]);
    dyn mfn;
    if (mce->tid != tid || mce->mid != (uint32_t)m_set) {
      mfn = get_method_for_tag(m_set, tid);
      mce->tid = tid;
      mce->mid = (uint32_t)m_set;
      mce->fn  = mfn;
    } else {
      mfn = mce->fn;
    }
    CALL(((dyn*)L)[dst], mfn);
  }
}

/* SBC_OBJECT: allocate a `size`-slot typed object with tid =
 * sbc->ty[tid].  size==0 emits MKIMM (no heap allocation -- the
 * dyn IS the tagged-zero immediate). */
static void jit_rt_object_impl(int64_t *L, struct sbc_t *sbc, uint64_t packed) {
  uint32_t dst  = (uint32_t)(packed & 0xFFFF);
  uint32_t tid  = (uint32_t)((packed >> 16) & 0xFFFF);
  uint32_t size = (uint32_t)((packed >> 32) & 0xFFFF);
  if (size) {
    OBJECT(((dyn*)L)[dst], sbc->ty[tid], size);
  } else {
    ((dyn*)L)[dst] = (dyn)(int64_t)MKIMM((int64_t)sbc->ty[tid], 0);
  }
}

/* SBC_ARGLIST / SBC_ARGLIST8: packed u64 layout
 *   bits 56..63: size  (1..7)
 *   bits  0.. 7: src0  (u8 slot index)
 *   bits  8..15: src1
 *   ...
 *   bits 48..55: src6
 * The two opcodes share an impl since the JIT translator already
 * truncated u16 src indices to u8 (with a runtime check that
 * they fit -- larger frames bail out at JIT-translate time). */
/* Packed layout:
 *   packed1: [56-63]=size (1..15), [0-7]=src0, ..., [48-55]=src6
 *   packed2: [0-7]=src7, [8-15]=src8, ..., [56-63]=src14 */
static void jit_rt_arglist_packed_impl(int64_t *L, struct sbc_t *sbc,
                                        uint64_t packed1, uint64_t packed2) {
  (void)sbc;
  int size = (int)((packed1 >> 56) & 0xFF);
  ARGLIST(size);
  for (int j = 0; j < size; j++) {
    int src;
    if (j < 7) src = (int)((packed1 >> (j * 8)) & 0xFF);
    else       src = (int)((packed2 >> ((j - 7) * 8)) & 0xFF);
    STARG(j, ((dyn*)L)[src]);
  }
}

/* SBC_SUBTYPE: add_subtype((int)sbc->ty[super], (int)sbc->ty[sub]). */
static void jit_rt_subtype_impl(int64_t *L, struct sbc_t *sbc, uint64_t packed) {
  (void)L;
  uint32_t super = (uint32_t)(packed & 0xFFFF);
  uint32_t sub   = (uint32_t)((packed >> 16) & 0xFFFF);
  add_subtype((int)(int64_t)sbc->ty[super], (int)(int64_t)sbc->ty[sub]);
}

/* SBC_MNAME: L[dst] = get_method_name(UNFXN(L[src])). */
static void jit_rt_mname_impl(int64_t *L, int dst, int src, int u) {
  (void)u;
  ((dyn*)L)[dst] = get_method_name(UNFXN(((dyn*)L)[src]));
}

/* SBC_TINIT: type init for named text.  Calls
 * set_type_size_and_name(sbc->ty[type], size, sbc->tx[name]). */
static void jit_rt_tinit_impl(int64_t *L, struct sbc_t *sbc, uint64_t packed) {
  (void)L;
  uint32_t type = (uint32_t)(packed & 0xFFFF);
  uint32_t size = (uint32_t)((packed >> 16) & 0xFFFF);
  uint32_t name = (uint32_t)((packed >> 32) & 0xFFFFFF);
  set_type_size_and_name((int64_t)sbc->ty[type], size, sbc->tx[name]);
}

/* SBC_TINITI: type init for immediate text.  Calls
 * set_type_size_and_name(sbc->ty[tag], size, name). */
static void jit_rt_tiniti_impl(int64_t *L, struct sbc_t *sbc,
                               uint64_t packed1, uint64_t packed2) {
  (void)L;
  uint32_t tag  = (uint32_t)(packed1 & 0xFFFF);
  uint32_t size = (uint32_t)((packed1 >> 16) & 0xFFFF);
  dyn name = (dyn)(int64_t)packed2;
  set_type_size_and_name((int64_t)sbc->ty[tag], size, name);
}

/* SBC_CURMET: L[dst] = api.curmet (currently executing method). */
static void jit_rt_curmet_impl(int64_t *L, int dst, int u1, int u2) {
  (void)u1; (void)u2;
  THIS_METHOD(((dyn*)L)[dst]);
}

/* Typed-float arith: detag, op, retag.  Mirrors sbc.c bodies. */
static void jit_rt_fadd_impl(int64_t *L, int dst, int a, int b) {
  float fa, fb;
  STFLT(fa, ((dyn*)L)[a]);
  STFLT(fb, ((dyn*)L)[b]);
  LDFLT(((dyn*)L)[dst], fa + fb);
}
static void jit_rt_fsub_impl(int64_t *L, int dst, int a, int b) {
  float fa, fb;
  STFLT(fa, ((dyn*)L)[a]);
  STFLT(fb, ((dyn*)L)[b]);
  LDFLT(((dyn*)L)[dst], fa - fb);
}
static void jit_rt_fmul_impl(int64_t *L, int dst, int a, int b) {
  float fa, fb;
  STFLT(fa, ((dyn*)L)[a]);
  STFLT(fb, ((dyn*)L)[b]);
  LDFLT(((dyn*)L)[dst], fa * fb);
}
static void jit_rt_fdiv_impl(int64_t *L, int dst, int a, int b) {
  float fa, fb;
  STFLT(fa, ((dyn*)L)[a]);
  STFLT(fb, ((dyn*)L)[b]);
  LDFLT(((dyn*)L)[dst], fa / fb);
}

/* SBC_DMET: define a method on a runtime type.
 *   add_method((int)sbc->ty[tyidx], sbc->mt[mtidx], L[handler]) */
static void jit_rt_dmet_impl(int64_t *L, struct sbc_t *sbc, uint64_t packed) {
  uint32_t tyidx   = (uint32_t)(packed & 0xFFFF);
  uint32_t mtidx   = (uint32_t)((packed >> 16) & 0xFFFFFF);
  uint32_t handler = (uint32_t)((packed >> 40) & 0xFFFF);
  add_method((int)(int64_t)sbc->ty[tyidx], sbc->mt[mtidx],
             (void*)((dyn*)L)[handler]);
}

/* SBC_FXNLSETIR: list-set with ignored result.  3-way dispatch:
 *   T_LIST + T_INT + in-bounds  -> direct FXNLSET (lsetm barrier)
 *   else                         -> MCACHE_CALL m_set on L[src]
 * Packed: [15:0]=src, [31:16]=index, [47:32]=val, [63:48]=mcache_idx. */
static void jit_rt_fxnlsetir_impl(int64_t *L, struct sbc_t *sbc, uint64_t packed) {
  uint32_t src   = (uint32_t)(packed & 0xFFFF);
  uint32_t index = (uint32_t)((packed >> 16) & 0xFFFF);
  uint32_t val   = (uint32_t)((packed >> 32) & 0xFFFF);
  uint32_t mcidx = (uint32_t)((packed >> 48) & 0xFFFF);
  dyn ss = ((dyn*)L)[src];
  dyn ii = ((dyn*)L)[index];
  if (TAGIS(T_LIST, ss) && TAGIS(T_INT, ii)
      && (uint64_t)ii < (uint64_t)FXN(LIST_SIZE(ss))) {
    dyn dummy;
    FXNLSET(dummy, ss, ii, ((dyn*)L)[val]);
    (void)dummy;
  } else {
    /* Bug #14 fix: reload from frame slots after ARGLIST.  See
     * jit_rt_fxnlset_impl for the rationale. */
    ARGLIST(3);
    STARG(0, ((dyn*)L)[src]);
    STARG(1, ((dyn*)L)[index]);
    STARG(2, ((dyn*)L)[val]);
    api.method = m_set;
    mcache_t *mce = &sbc->mcaches[mcidx];
    uint32_t tid = O_TAG(((dyn*)L)[src]);
    dyn mfn;
    if (mce->tid != tid || mce->mid != (uint32_t)m_set) {
      mfn = get_method_for_tag(m_set, tid);
      mce->tid = tid;
      mce->mid = (uint32_t)m_set;
      mce->fn  = mfn;
    } else {
      mfn = mce->fn;
    }
    dyn dummy;
    CALL(dummy, mfn);
    (void)dummy;
  }
}

/* SBC_LIST1 / SBC_LIST2 (RT-9 fused list literals).  Mirror the
 * interpreter bodies in sbc.c:SBC_LIST1 / SBC_LIST2 -- allocate
 * a size-1 or size-2 list and stash the source slot(s) into
 * field 0 (and 1, for LIST2).  These compress the common
 * `[A]` and `[A B]` patterns into a single trampoline call
 * instead of 2-3 jit_emit_call_helper3 invocations. */
static void jit_rt_list1_impl(int64_t *L, int dst, int x, int unused) {
  (void)unused;
  LIST(((dyn*)L)[dst], 1);
  LGET(((dyn*)L)[dst], 0) = ((dyn*)L)[x];
}
static void jit_rt_list2_impl(int64_t *L, int dst, int a, int b) {
  LIST(((dyn*)L)[dst], 2);
  LGET(((dyn*)L)[dst], 0) = ((dyn*)L)[a];
  LGET(((dyn*)L)[dst], 1) = ((dyn*)L)[b];
}

/* SBC_FXNSIZE: L[dst] = FXN(LIST_SIZE(L[src])).  Mirrors
 * sbc.c:SBC_FXNSIZE exactly; no MCACHE fallback. */
static void jit_rt_fxnsize_impl(int64_t *L, int dst, int src, int unused) {
  (void)unused;
  ((dyn*)L)[dst] = (dyn)(int64_t)FXN(LIST_SIZE(((dyn*)L)[src]));
}

/* SBC_MOVEEMT: L[dst] = Empty.  Simple deref of the global
 * api.empty_ field; called once per emission site. */
static void jit_rt_moveemt_impl(int64_t *L, int dst, int u1, int u2) {
  (void)u1; (void)u2;
  ((dyn*)L)[dst] = Empty;
}

/* SBC_FATAL: longjmps via fatal((char*)L[msg]).  Matches the
 * interpreter's `FATAL(L[msg])` exactly; the cast to char* is
 * the same trick the interpreter uses (a Symta text dyn's
 * heap layout starts with the c-string). */
static void jit_rt_fatal_impl(int64_t *L, int msg, int u1, int u2) {
  (void)u1; (void)u2;
  FATAL((char*)(((dyn*)L)[msg]));
}

/* SBC_FXNLT / FXNGT / FXNLTE / FXNGTE: untyped ordering ops.
 * T_INT-T_INT fast path inline; else MCALL into the appropriate
 * method id.  Helper3 sig: (L, dst, a, b). */
static void jit_rt_fxnlt_impl(int64_t *L, int dst, int a, int b) {
  dyn aa = ((dyn*)L)[a], bb = ((dyn*)L)[b];
  if (TAGIS(T_INT, aa) && TAGIS(T_INT, bb)) {
    FXNLT(((dyn*)L)[dst], aa, bb);
  } else {
    ARGLIST2(((dyn*)L)[a], ((dyn*)L)[b]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_lt);
  }
}
static void jit_rt_fxngt_impl(int64_t *L, int dst, int a, int b) {
  dyn aa = ((dyn*)L)[a], bb = ((dyn*)L)[b];
  if (TAGIS(T_INT, aa) && TAGIS(T_INT, bb)) {
    FXNGT(((dyn*)L)[dst], aa, bb);
  } else {
    ARGLIST2(((dyn*)L)[a], ((dyn*)L)[b]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_gt);
  }
}
static void jit_rt_fxnlte_impl(int64_t *L, int dst, int a, int b) {
  dyn aa = ((dyn*)L)[a], bb = ((dyn*)L)[b];
  if (TAGIS(T_INT, aa) && TAGIS(T_INT, bb)) {
    FXNLTE(((dyn*)L)[dst], aa, bb);
  } else {
    ARGLIST2(((dyn*)L)[a], ((dyn*)L)[b]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_lte);
  }
}
static void jit_rt_fxngte_impl(int64_t *L, int dst, int a, int b) {
  dyn aa = ((dyn*)L)[a], bb = ((dyn*)L)[b];
  if (TAGIS(T_INT, aa) && TAGIS(T_INT, bb)) {
    FXNGTE(((dyn*)L)[dst], aa, bb);
  } else {
    ARGLIST2(((dyn*)L)[a], ((dyn*)L)[b]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_gte);
  }
}

/* SBC_FXNTAG: L[dst] = FXN(O_TAG(L[src])).  Trivial. */
static void jit_rt_fxntag_impl(int64_t *L, int dst, int src, int u) {
  (void)u;
  ((dyn*)L)[dst] = (dyn)(int64_t)FXN(O_TAG(((dyn*)L)[src]));
}

/* SBC_MOVEIM: L[dst] = sbc->im[src].  Per-SBC imported-symbol
 * lookup.  Packed: [15:0]=dst (u16), [39:16]=src (u24). */
static void jit_rt_moveim_impl(int64_t *L, struct sbc_t *sbc, uint64_t packed) {
  uint32_t dst = (uint32_t)(packed & 0xFFFF);
  uint32_t src = (uint32_t)((packed >> 16) & 0xFFFFFF);
  ((dyn*)L)[dst] = sbc->im[src];
}

/* SBC_MOVEMT / SBC_MOVEMT8: L[dst] = FXN(sbc->mt[src]).  Method-
 * id table lookup; same packed layout as MOVEIM. */
static void jit_rt_movemt_impl(int64_t *L, struct sbc_t *sbc, uint64_t packed) {
  uint32_t dst = (uint32_t)(packed & 0xFFFF);
  uint32_t src = (uint32_t)((packed >> 16) & 0xFFFFFF);
  ((dyn*)L)[dst] = (dyn)(int64_t)FXN(sbc->mt[src]);
}

/* SBC_IMMEQ / SBC_IMMNE: 3-way dispatch matching sbc.c.
 *   T_INT-anything    -> bitwise compare on tagged dyns
 *   text-vs-text      -> texts_equal direct call
 *   otherwise         -> MCACHE_CALL m_eq / m_ne
 * Packed: [15:0]=dst, [31:16]=a_slot, [47:32]=b_slot,
 *         [63:48]=mcache_idx. */
static void jit_rt_immeq_impl(int64_t *L, struct sbc_t *sbc, uint64_t packed) {
  uint32_t dst   = (uint32_t)(packed & 0xFFFF);
  uint32_t a_sl  = (uint32_t)((packed >> 16) & 0xFFFF);
  uint32_t b_sl  = (uint32_t)((packed >> 32) & 0xFFFF);
  uint32_t mcidx = (uint32_t)((packed >> 48) & 0xFFFF);
  dyn aa = ((dyn*)L)[a_sl];
  dyn bb = ((dyn*)L)[b_sl];
  if (TAGIS(T_INT, aa)) {
    ((dyn*)L)[dst] = (dyn)(int64_t)FXN(aa == bb);
  } else if ((TAGIS(T_TEXT, aa) || TAGIS(T_FIXTEXT, aa))
          && (TAGIS(T_TEXT, bb) || TAGIS(T_FIXTEXT, bb))) {
    ((dyn*)L)[dst] = (dyn)(int64_t)FXN(texts_equal(aa, bb));
  } else {
    /* Bug #14 fix: ARGLIST2 allocates a fresh LIST, which can move
     * heap objects via GC.  Frame slots L[a_sl] / L[b_sl] are GC
     * roots (scanned via api.frame chain), but the C locals aa/bb
     * are NOT -- they cache a stale pointer if their target moves.
     * Re-load from the frame slots AFTER ARGLIST2 so STARG writes
     * the post-GC pointers into api.args.  Pre-ARGLIST aa is fine
     * for the O_TAG check below since the tag bits don't change. */
    ARGLIST(2);
    STARG(0, ((dyn*)L)[a_sl]);
    STARG(1, ((dyn*)L)[b_sl]);
    api.method = m_eq;
    mcache_t *mce = &sbc->mcaches[mcidx];
    uint32_t tid = O_TAG(((dyn*)L)[a_sl]);
    dyn mfn;
    if (mce->tid != tid || mce->mid != (uint32_t)m_eq) {
      mfn = get_method_for_tag(m_eq, tid);
      mce->tid = tid;
      mce->mid = (uint32_t)m_eq;
      mce->fn  = mfn;
    } else {
      mfn = mce->fn;
    }
    CALL(((dyn*)L)[dst], mfn);
  }
}
static void jit_rt_immne_impl(int64_t *L, struct sbc_t *sbc, uint64_t packed) {
  uint32_t dst   = (uint32_t)(packed & 0xFFFF);
  uint32_t a_sl  = (uint32_t)((packed >> 16) & 0xFFFF);
  uint32_t b_sl  = (uint32_t)((packed >> 32) & 0xFFFF);
  uint32_t mcidx = (uint32_t)((packed >> 48) & 0xFFFF);
  dyn aa = ((dyn*)L)[a_sl];
  dyn bb = ((dyn*)L)[b_sl];
  if (TAGIS(T_INT, aa)) {
    ((dyn*)L)[dst] = (dyn)(int64_t)FXN(aa != bb);
  } else if ((TAGIS(T_TEXT, aa) || TAGIS(T_FIXTEXT, aa))
          && (TAGIS(T_TEXT, bb) || TAGIS(T_FIXTEXT, bb))) {
    ((dyn*)L)[dst] = (dyn)(int64_t)FXN(!texts_equal(aa, bb));
  } else {
    /* Bug #14 fix: see jit_rt_immeq_impl above -- reload from frame
     * slots after ARGLIST so STARG sees post-GC pointers. */
    ARGLIST(2);
    STARG(0, ((dyn*)L)[a_sl]);
    STARG(1, ((dyn*)L)[b_sl]);
    api.method = m_ne;
    mcache_t *mce = &sbc->mcaches[mcidx];
    uint32_t tid = O_TAG(((dyn*)L)[a_sl]);
    dyn mfn;
    if (mce->tid != tid || mce->mid != (uint32_t)m_ne) {
      mfn = get_method_for_tag(m_ne, tid);
      mce->tid = tid;
      mce->mid = (uint32_t)m_ne;
      mce->fn  = mfn;
    } else {
      mfn = mce->fn;
    }
    CALL(((dyn*)L)[dst], mfn);
  }
}

/* SBC_INC / SBC_DEC: T_INT fast path = FXNADD/SUB(_, _, FXN(1)),
 * else MCALL m_inc / m_dec.  Helper3 sig: (L, dst, a, _). */
static void jit_rt_inc_impl(int64_t *L, int dst, int a, int u) {
  (void)u;
  dyn aa = ((dyn*)L)[a];
  if (TAGIS(T_INT, aa)) {
    FXNADD(((dyn*)L)[dst], aa, FXN(1));
  } else {
    ARGLIST1(((dyn*)L)[a]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_inc);
  }
}
static void jit_rt_dec_impl(int64_t *L, int dst, int a, int u) {
  (void)u;
  dyn aa = ((dyn*)L)[a];
  if (TAGIS(T_INT, aa)) {
    FXNSUB(((dyn*)L)[dst], aa, FXN(1));
  } else {
    ARGLIST1(((dyn*)L)[a]);
    MCALL(((dyn*)L)[dst], ((dyn*)L)[a], m_dec);
  }
}

/* SBC_NOT / SBC_GOT / SBC_NO: truthy/No comparisons.  Matches
 * the interpreter bodies exactly. */
static void jit_rt_not_impl(int64_t *L, int dst, int src, int u) {
  (void)u;
  ((dyn*)L)[dst] = ((dyn*)L)[src] ? (dyn)(int64_t)FXN(0) : (dyn)(int64_t)FXN(1);
}
static void jit_rt_got_impl(int64_t *L, int dst, int src, int u) {
  (void)u;
  ((dyn*)L)[dst] = (((dyn*)L)[src] != No) ? (dyn)(int64_t)FXN(1) : (dyn)(int64_t)FXN(0);
}
static void jit_rt_no_impl(int64_t *L, int dst, int src, int u) {
  (void)u;
  ((dyn*)L)[dst] = (((dyn*)L)[src] == No) ? (dyn)(int64_t)FXN(1) : (dyn)(int64_t)FXN(0);
}

/* SBC_FXNLGET: list-element-get with mcache fallback.  Mirrors
 * sbc.c:SBC_FXNLGET exactly -- T_LIST + T_INT + in-bounds fast
 * path; otherwise MCACHE_CALL(m_get).  Packed: [63:48]=mcache_idx
 * [47:32]=index_slot [31:16]=src_slot [15:0]=dst_slot. */
static void jit_rt_fxnlget_impl(int64_t *L, struct sbc_t *sbc, uint64_t packed) {
  uint32_t mcache_idx = (uint32_t)((packed >> 48) & 0xFFFF);
  uint32_t index_slot = (uint32_t)((packed >> 32) & 0xFFFF);
  uint32_t src        = (uint32_t)((packed >> 16) & 0xFFFF);
  uint32_t dst        = (uint32_t)(packed & 0xFFFF);
  dyn ss = ((dyn*)L)[src];
  dyn ii = ((dyn*)L)[index_slot];
  if (TAGIS(T_LIST, ss) && TAGIS(T_INT, ii)
      && (uint64_t)ii < (uint64_t)FXN(LIST_SIZE(ss))) {
    FXNLGET(((dyn*)L)[dst], ss, ii);
  } else {
    ARGLIST2(((dyn*)L)[src], ((dyn*)L)[index_slot]);
    api.method = m_get;
    mcache_t *mce = &sbc->mcaches[mcache_idx];
    uint32_t tid = O_TAG(((dyn*)L)[src]);
    dyn mfn;
    if (mce->tid != tid || mce->mid != (uint32_t)m_get) {
      mfn = get_method_for_tag(m_get, tid);
      mce->tid = tid;
      mce->mid = (uint32_t)m_get;
      mce->fn  = mfn;
    } else {
      mfn = mce->fn;
    }
    CALL(((dyn*)L)[dst], mfn);
  }
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

/* Public wrapper for sif2sbc.  Ensures helpers are installed
 * before the writer JIT-translates anything in AOT mode.  Same
 * once-only semantics as the static version below. */
void jit_install_helpers_public(void);

/* Helper-id -> live function pointer.  Indexed by JIT_HELPER_*
 * enum; entry 0 (JIT_HELPER_NONE) is left NULL so accidental
 * lookups against an un-tagged reloc surface immediately.
 *
 * Used by:
 *   - sbc_prepare's loader install path (step 6c) to patch
 *     each imm64 in a freshly-copied JIT blob.
 *   - the writer-side sanity check that every reloc collected
 *     during AOT translation refers to a known helper.
 *
 * The table is populated lazily after jit_install_helpers_once
 * runs, since the helper pointers themselves can be swapped
 * from int-only fallbacks to full impls. */
void *jit_helper_pointer(int helper_id);

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
  jit_rt_list1_helper    = jit_rt_list1_impl;
  jit_rt_list2_helper    = jit_rt_list2_impl;
  jit_rt_fxnsize_helper  = jit_rt_fxnsize_impl;
  jit_rt_moveemt_helper  = jit_rt_moveemt_impl;
  jit_rt_fatal_helper    = jit_rt_fatal_impl;
  jit_rt_fxnlget_helper  = jit_rt_fxnlget_impl;
  jit_rt_fxnlt_helper    = jit_rt_fxnlt_impl;
  jit_rt_fxngt_helper    = jit_rt_fxngt_impl;
  jit_rt_fxnlte_helper   = jit_rt_fxnlte_impl;
  jit_rt_fxngte_helper   = jit_rt_fxngte_impl;
  jit_rt_fxntag_helper   = jit_rt_fxntag_impl;
  jit_rt_not_helper      = jit_rt_not_impl;
  jit_rt_got_helper      = jit_rt_got_impl;
  jit_rt_no_helper       = jit_rt_no_impl;
  jit_rt_moveim_helper   = jit_rt_moveim_impl;
  jit_rt_inc_helper      = jit_rt_inc_impl;
  jit_rt_dec_helper      = jit_rt_dec_impl;
  jit_rt_movemt_helper   = jit_rt_movemt_impl;
  jit_rt_immeq_helper    = jit_rt_immeq_impl;
  jit_rt_immne_helper    = jit_rt_immne_impl;
  jit_rt_fxnlistn_helper = jit_rt_fxnlistn_impl;
  jit_rt_fxnlsetir_helper = jit_rt_fxnlsetir_impl;
  jit_rt_neg_helper      = jit_rt_neg_impl;
  jit_rt_abs_helper      = jit_rt_abs_impl;
  jit_rt_fxnand_helper   = jit_rt_fxnand_impl;
  jit_rt_fxnior_helper   = jit_rt_fxnior_impl;
  jit_rt_fxnxor_helper   = jit_rt_fxnxor_impl;
  jit_rt_fxnshl_helper   = jit_rt_fxnshl_impl;
  jit_rt_fxnshr_helper   = jit_rt_fxnshr_impl;
  jit_rt_fxnlset_helper  = jit_rt_fxnlset_impl;
  jit_rt_object_helper   = jit_rt_object_impl;
  jit_rt_arglist_n_helper = jit_rt_arglist_packed_impl;
  jit_rt_arglist8_n_helper = jit_rt_arglist_packed_impl;
  jit_rt_dmet_helper     = jit_rt_dmet_impl;
  jit_rt_tiniti_helper   = jit_rt_tiniti_impl;
  jit_rt_curmet_helper   = jit_rt_curmet_impl;
  jit_rt_fadd_helper     = jit_rt_fadd_impl;
  jit_rt_fsub_helper     = jit_rt_fsub_impl;
  jit_rt_fmul_helper     = jit_rt_fmul_impl;
  jit_rt_fdiv_helper     = jit_rt_fdiv_impl;
  jit_rt_tinit_helper    = jit_rt_tinit_impl;
  jit_rt_subtype_helper  = jit_rt_subtype_impl;
  jit_rt_mname_helper    = jit_rt_mname_impl;
  /* Step 12e: hand the inline LD4 emitter the address of
   * `api_g.heap0` (the field) so it can bake it as an imm64.  At
   * AOT install time the JIT_HELPER_AMP_HEAP0 reloc rebinds it
   * to the current process's field address. */
  jit_rt_heap0_addr      = (void*)&api_g.heap0;
  /* Step 12j: same shape for the inline LIST1 / LIST2 emitter. */
  jit_rt_hgp_addr        = (void*)&api_g.hgp;
  jit_rt_theap0_addr     = (void*)&api_g.theap0;
}

void jit_install_helpers_public(void) {
  jit_install_helpers_once();
}

void *jit_helper_pointer(int helper_id) {
  /* Caller must have called jit_install_helpers_public() first
   * so the full-dispatch impls are in place.  The table below is
   * evaluated each call rather than cached because the function-
   * pointer globals can legally be reassigned (e.g. swapping the
   * int-only fallbacks for the full versions, as
   * jit_install_helpers_once does). */
  switch (helper_id) {
    case JIT_HELPER_FXNADD:   return (void*)jit_rt_fxnadd_helper;
    case JIT_HELPER_FXNSUB:   return (void*)jit_rt_fxnsub_helper;
    case JIT_HELPER_FXNMUL:   return (void*)jit_rt_fxnmul_helper;
    case JIT_HELPER_FXNDIV:   return (void*)jit_rt_fxndiv_helper;
    case JIT_HELPER_FXNREM:   return (void*)jit_rt_fxnrem_helper;
    case JIT_HELPER_LD4:      return (void*)jit_rt_ld4_helper;
    case JIT_HELPER_ST4:      return (void*)jit_rt_st4_helper;
    case JIT_HELPER_LIST:     return (void*)jit_rt_list_helper;
    case JIT_HELPER_COPY:     return (void*)jit_rt_copy_helper;
    case JIT_HELPER_CLOSURE:  return (void*)jit_rt_closure_helper;
    case JIT_HELPER_ARGLIST0: return (void*)jit_rt_arglist0_helper;
    case JIT_HELPER_ARGLIST1: return (void*)jit_rt_arglist1_helper;
    case JIT_HELPER_ARGLIST2: return (void*)jit_rt_arglist2_helper;
    case JIT_HELPER_ARGLIST3: return (void*)jit_rt_arglist3_helper;
    case JIT_HELPER_ARGLIST4: return (void*)jit_rt_arglist4_helper;
    case JIT_HELPER_ARGLIST5: return (void*)jit_rt_arglist5_helper;
    case JIT_HELPER_CALL:     return (void*)jit_rt_call_helper;
    case JIT_HELPER_CALLIR:   return (void*)jit_rt_callir_helper;
    case JIT_HELPER_CALLT:    return (void*)jit_rt_callt_helper;
    case JIT_HELPER_CALLTIR:  return (void*)jit_rt_calltir_helper;
    case JIT_HELPER_MCALL:    return (void*)jit_rt_mcall_helper;
    case JIT_HELPER_MCALLIR:  return (void*)jit_rt_mcallir_helper;
    case JIT_HELPER_MOVETX:   return (void*)jit_rt_movetx_helper;
    case JIT_HELPER_LIST1:    return (void*)jit_rt_list1_helper;
    case JIT_HELPER_LIST2:    return (void*)jit_rt_list2_helper;
    case JIT_HELPER_FXNSIZE:  return (void*)jit_rt_fxnsize_helper;
    case JIT_HELPER_MOVEEMT:  return (void*)jit_rt_moveemt_helper;
    case JIT_HELPER_FATAL:    return (void*)jit_rt_fatal_helper;
    case JIT_HELPER_FXNLGET:  return (void*)jit_rt_fxnlget_helper;
    case JIT_HELPER_FXNLT:    return (void*)jit_rt_fxnlt_helper;
    case JIT_HELPER_FXNGT:    return (void*)jit_rt_fxngt_helper;
    case JIT_HELPER_FXNLTE:   return (void*)jit_rt_fxnlte_helper;
    case JIT_HELPER_FXNGTE:   return (void*)jit_rt_fxngte_helper;
    case JIT_HELPER_FXNTAG:   return (void*)jit_rt_fxntag_helper;
    case JIT_HELPER_NOT:      return (void*)jit_rt_not_helper;
    case JIT_HELPER_GOT:      return (void*)jit_rt_got_helper;
    case JIT_HELPER_NO:       return (void*)jit_rt_no_helper;
    case JIT_HELPER_MOVEIM:   return (void*)jit_rt_moveim_helper;
    case JIT_HELPER_INC:      return (void*)jit_rt_inc_helper;
    case JIT_HELPER_DEC:      return (void*)jit_rt_dec_helper;
    case JIT_HELPER_MOVEMT:   return (void*)jit_rt_movemt_helper;
    case JIT_HELPER_IMMEQ:    return (void*)jit_rt_immeq_helper;
    case JIT_HELPER_IMMNE:    return (void*)jit_rt_immne_helper;
    case JIT_HELPER_FXNLISTN: return (void*)jit_rt_fxnlistn_helper;
    case JIT_HELPER_FXNLSETIR: return (void*)jit_rt_fxnlsetir_helper;
    case JIT_HELPER_NEG:      return (void*)jit_rt_neg_helper;
    case JIT_HELPER_ABS:      return (void*)jit_rt_abs_helper;
    case JIT_HELPER_FXNAND:   return (void*)jit_rt_fxnand_helper;
    case JIT_HELPER_FXNIOR:   return (void*)jit_rt_fxnior_helper;
    case JIT_HELPER_FXNXOR:   return (void*)jit_rt_fxnxor_helper;
    case JIT_HELPER_FXNSHL:   return (void*)jit_rt_fxnshl_helper;
    case JIT_HELPER_FXNSHR:   return (void*)jit_rt_fxnshr_helper;
    case JIT_HELPER_FXNLSET:  return (void*)jit_rt_fxnlset_helper;
    case JIT_HELPER_OBJECT:   return (void*)jit_rt_object_helper;
    case JIT_HELPER_ARGLIST_N: return (void*)jit_rt_arglist_n_helper;
    case JIT_HELPER_ARGLIST8_N: return (void*)jit_rt_arglist8_n_helper;
    case JIT_HELPER_DMET:     return (void*)jit_rt_dmet_helper;
    case JIT_HELPER_TINITI:   return (void*)jit_rt_tiniti_helper;
    case JIT_HELPER_CURMET:   return (void*)jit_rt_curmet_helper;
    case JIT_HELPER_FADD:     return (void*)jit_rt_fadd_helper;
    case JIT_HELPER_FSUB:     return (void*)jit_rt_fsub_helper;
    case JIT_HELPER_FMUL:     return (void*)jit_rt_fmul_helper;
    case JIT_HELPER_FDIV:     return (void*)jit_rt_fdiv_helper;
    case JIT_HELPER_TINIT:    return (void*)jit_rt_tinit_helper;
    case JIT_HELPER_SUBTYPE:  return (void*)jit_rt_subtype_helper;
    case JIT_HELPER_MNAME:    return (void*)jit_rt_mname_helper;
    /* Step 12e: address-of-runtime-field relocs.  See jit.h. */
    case JIT_HELPER_AMP_HEAP0: return (void*)&api_g.heap0;
    case JIT_HELPER_AMP_HGP:   return (void*)&api_g.hgp;
    case JIT_HELPER_AMP_THEAP0: return (void*)&api_g.theap0;
    default:                  return NULL;
  }
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

  /* STACK-2: inline SUBR / PROLOGUE -- bumps the per-thread heap
   * arena rather than allocating a C-stack VLA, matching the
   * symta.h PROLOGUE macro.  Caller's CALL macro restores
   * api.arena_top after we return.  GC continues to walk the
   * api.frame chain regardless of where each frame physically
   * lives, so the JIT path inter-operates with arena-backed
   * interpreter frames and the few remaining VLA sites without
   * special casing. */
  void **L_blk_ = api.arena_top;
  api.arena_top = L_blk_ + FRAME_PREFIX_SLOTS + nvars;
  if (api.arena_top > api.arena_end)
    fatal("frame arena overflow (jit_adapter nvars=%d). Recompile with a larger arena.", nvars);
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

/* Step 6c: install pre-baked native code from the SBC's IA64
 * section.  Mirrors sbc_jit_install but consumes bytes the
 * writer already produced instead of re-running the JIT.
 *
 * For each populated directory entry (payload_offset != 0):
 *   1. Allocate a fresh executable mapping sized for code +
 *      unwind blob + slack.
 *   2. memcpy the code from the SBC file into the mapping.
 *   3. Walk the reloc table embedded right after the code; for
 *      each (offset, helper_id) entry, write the live helper
 *      pointer into the imm64 slot at `mapping + offset`.
 *   4. Register Windows SEH unwind info (no-op on POSIX).
 *   5. Build a jit_adapter_payload_t and overwrite the
 *      corresponding hooks_heap entry's handler+payload so
 *      CALL/MCALL dispatches into the native code.
 *
 * Gated on SYMTA_AOT_RUN=1 to keep the default behaviour
 * unchanged while the AOT path is stabilising.  Once
 * confidence is high we can flip to default-on whenever the
 * section is populated. */
int sbc_install_ia64(struct sbc_t *sbc) {
  if (!sbc || !sbc->ia64_table || !sbc->ia64_sz) return 0;
  jit_install_helpers_once();

  const uint8_t *sec = sbc->ia64_table;
  /* Validate section header.
   *
   * v2 layout (step 12j):
   *   sec[0..3]   : 'I' 'A' '6' '4' magic
   *   sec[4..5]   : version (= 2)
   *   sec[6..7]   : abi_tag (1 = Win64, 2 = SysV-x64)
   *   sec[8..11]  : nfns
   *   sec[12..]   : per-function directory
   *
   * v1 sections (no abi_tag, implicitly Win64) are rejected
   * universally: the caller (sbc.c) responds by running the
   * runtime JIT translator (sbc_jit_install) which emits
   * per-platform code.  This makes cross-platform SBCs safe:
   * a Win64-baked SBC loaded on Linux gracefully falls back
   * to runtime translation instead of installing foreign-ABI
   * native code and crashing on the first call. */
  if (sec[0] != 'I' || sec[1] != 'A' || sec[2] != '6' || sec[3] != '4') {
    fprintf(stderr, "ia64-install: bad magic in %s\n", sbc->filename);
    return 0;
  }
  uint16_t ver = (uint16_t)sec[4] | ((uint16_t)sec[5] << 8);
  if (ver != 2) {
    /* v1 or unknown -- caller's expected behaviour is to fall
     * back to runtime JIT translation.  Silent on the common
     * "v1 SBC built before step 12j" case; loud on actual
     * unknown future versions. */
    if (ver != 1) {
      fprintf(stderr, "ia64-install: unsupported section version %u in %s\n",
              ver, sbc->filename);
    }
    return 0;
  }
  uint16_t abi_tag = (uint16_t)sec[6] | ((uint16_t)sec[7] << 8);
#ifdef _WIN32
  uint16_t expected_abi = 1;  /* Win64 */
#else
  uint16_t expected_abi = 2;  /* SysV-x64 */
#endif
  if (abi_tag != expected_abi) {
    /* Cross-platform SBC: code was baked for a different ABI.
     * Caller falls back to runtime JIT.  No stderr noise --
     * this is the routine case for a Linux user running a
     * Windows-baked release SBC. */
    return 0;
  }
  uint32_t sec_nfns = (uint32_t)sec[8]
                    | ((uint32_t)sec[9]  << 8)
                    | ((uint32_t)sec[10] << 16)
                    | ((uint32_t)sec[11] << 24);
  uint32_t fntbl_nfns = sbc->fntbl_sz / 3;
  if (sec_nfns != fntbl_nfns) {
    fprintf(stderr, "ia64-install: nfn mismatch (sec=%u fntbl=%u) in %s\n",
            sec_nfns, fntbl_nfns, sbc->filename);
    return 0;
  }

  const uint8_t *dir = sec + 12;
  int installed = 0;
  for (uint32_t fi = 0; fi < sec_nfns; fi++) {
    const uint8_t *e = dir + fi * 16;
    uint32_t payload_off = (uint32_t)e[0]
                         | ((uint32_t)e[1] << 8)
                         | ((uint32_t)e[2] << 16)
                         | ((uint32_t)e[3] << 24);
    if (payload_off == 0) continue;  /* not translated; interpreter wins */

    uint32_t code_size = (uint32_t)e[4]
                       | ((uint32_t)e[5] << 8)
                       | ((uint32_t)e[6] << 16)
                       | ((uint32_t)e[7] << 24);
    uint16_t reloc_count = (uint16_t)e[8]  | ((uint16_t)e[9]  << 8);
    /* e[10..11]: prologue_size (Phase 2a-aware loaders) or 0
     * (older SBCs).  When 0, the no-pins fallback in
     * jit_register_unwind_2arg handles it via the hardcoded
     * 13-byte layout. */
    uint16_t prologue_size_e = (uint16_t)e[10] | ((uint16_t)e[11] << 8);
    uint16_t nvars = (uint16_t)e[12] | ((uint16_t)e[13] << 8);
    /* e[14..15]: pinned_count (low byte; Phase 2a-aware
     * loaders) or 0 (older SBCs).  Treat 0 as "no pins". */
    uint8_t  pinned_count_e = e[14];
    /* If e[10..11] is missing (older SBC bake), fall back to the
     * no-pins prologue length so the unwind data is at least
     * self-consistent. */
    uint16_t effective_prologue_size = prologue_size_e ? prologue_size_e
                                                       : (uint16_t)13;

    /* Bound-check the blob inside the section. */
    uint32_t blob_end = payload_off + code_size + (uint32_t)reloc_count * 8;
    if (blob_end > sec - sbc->tbls + sbc->tbls_sz) {
      fprintf(stderr, "ia64-install: blob fn[%u] out of bounds in %s\n",
              fi, sbc->filename);
      continue;
    }

    const uint8_t *code_src  = sec + payload_off;
    const uint8_t *reloc_src = code_src + code_size;

    /* Allocate executable memory.  jit_buf_new gives us a page-
     * aligned mapping; reserve room for code + 32 bytes for the
     * UNWIND_INFO + RUNTIME_FUNCTION the unwind registrar lays
     * out after the code. */
    jit_buf *jb = jit_buf_new((size_t)code_size + 64);
    if (!jb) continue;
    memcpy(jb->code, code_src, code_size);
    jb->len = code_size;

    /* Apply relocs: each entry says "the imm64 at jb->code+offset
     * needs to point at the live address of helper_id".  Each
     * relocation entry is 8 bytes: u32 offset, u8 helper_id, 3
     * pad bytes. */
    int reloc_fail = 0;
    for (uint16_t ri = 0; ri < reloc_count; ri++) {
      const uint8_t *r = reloc_src + ri * 8;
      uint32_t off = (uint32_t)r[0]
                   | ((uint32_t)r[1] << 8)
                   | ((uint32_t)r[2] << 16)
                   | ((uint32_t)r[3] << 24);
      uint8_t hid = r[4];
      if (off + 8 > code_size) {
        fprintf(stderr, "ia64-install: reloc offset %u out of range "
                "(code_size=%u) in fn[%u] of %s\n",
                off, code_size, fi, sbc->filename);
        reloc_fail = 1; break;
      }
      void *target = jit_helper_pointer((int)hid);
      if (!target) {
        fprintf(stderr, "ia64-install: unknown helper_id %u in fn[%u] "
                "of %s\n", hid, fi, sbc->filename);
        reloc_fail = 1; break;
      }
      uint64_t v = (uint64_t)(uintptr_t)target;
      uint8_t *dst = jb->code + off;
      for (int k = 0; k < 8; k++) dst[k] = (uint8_t)(v >> (k * 8));
    }
    if (reloc_fail) { jit_buf_free(jb); continue; }

    /* Register SEH unwind BEFORE finalize -- the unwind blob is
     * written into the same mapping (right after the code), and
     * POSIX finalize drops write permission on the page.
     *
     * Phase 2a: pass the recorded prologue_size + pinned_count
     * from the directory entry so the unwind data matches the
     * actual prologue layout. */
    void *jit_code = jit_buf_finalize(jb);
    jit_register_unwind_2arg(jit_code, jb->len,
                              (int)pinned_count_e,
                              effective_prologue_size);

    jit_adapter_payload_t *payload =
      (jit_adapter_payload_t*)malloc(sizeof(*payload));
    if (!payload) continue;
    payload->jit_body = jit_code;
    payload->sbc      = sbc;
    payload->nvars    = (int)nvars;

    /* Overwrite the hook entry so dispatch lands on native code. */
    uint32_t hook_idx = (uint32_t)sbc->hooks[fi];
    hooks_heap[hook_idx].handler = (psf_t)&jit_adapter;
    hooks_heap[hook_idx].payload = (uint8_t*)payload;
    installed++;
  }

  if (getenv("SYMTA_AOT_VERBOSE")) {
    fprintf(stderr, "ia64-install: %d/%u functions installed for %s\n",
            installed, sec_nfns, sbc->filename);
  }
  return installed;
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
     * entry just sits in memory until RtlUnwindEx looks it up.
     *
     * Phase 2a: pass jb->pinned_count + jb->prologue_size so
     * the unwind data matches the actual prologue layout. */
    jit_register_unwind_2arg(jit_code, jb->len,
                              jb->pinned_count,
                              jb->prologue_size);

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
