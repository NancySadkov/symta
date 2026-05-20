/* runtime/jit.h -- x86_64 just-in-time SBC translator.
 *
 * Scope (step 0): allocate executable memory, append raw machine-
 * code bytes, freeze and hand back as a function pointer.  This
 * is the foundation for emitting native code from SBC opcodes.
 *
 * Target: x86_64 only (no ia32).  Windows uses VirtualAlloc with
 * PAGE_EXECUTE_READWRITE; POSIX uses mmap with PROT_EXEC|PROT_RW.
 *
 * Calling convention: Windows x64 ABI on Windows, System V on
 * POSIX.  The actual emitters are calling-convention-aware; this
 * scaffold just provides the buffer plumbing.
 *
 * Lifecycle:
 *   jit_buf *b = jit_buf_new(4096);
 *   jit_emit_u8(b, 0x48); jit_emit_u8(b, ...); ...
 *   void *fn = jit_buf_finalize(b);
 *   ((int64_t(*)(int64_t,int64_t))fn)(3, 5);  // -> 8
 *   ...
 *   jit_buf_free(b);
 */

#ifndef SYMTA_JIT_H
#define SYMTA_JIT_H

#include <stddef.h>
#include <stdint.h>

/* Step 6b: stable helper IDs for AOT relocation.
 *
 * Each opcode the JIT delegates to a runtime helper -- the FXN*
 * arith helpers, ARGLIST/CALL/MCALL dispatchers, LD4/ST4/COPY/
 * LIST/CLOSURE/MOVETX -- has one of these IDs.  When the JIT
 * runs in record-relocs mode the imm64 of each `mov rax, helper`
 * + `call rax` site is logged as (byte_offset, helper_id) into
 * `jit_buf.relocs`.  The writer side (sif2sbc) emits these into
 * the SBC's IA64 section; the loader (sbc_prepare) walks them
 * after copying the code into executable memory and patches each
 * imm64 to point at the LIVE address of the corresponding
 * helper in THIS process.
 *
 * IDs are stable across writer/loader versions -- new helpers
 * append to the end of this enum, never reorder.  JIT_HELPER_MAX
 * is the sentinel; helpers expand by adding entries before it. */
typedef enum {
  JIT_HELPER_NONE     = 0,
  JIT_HELPER_FXNADD   = 1,
  JIT_HELPER_FXNSUB   = 2,
  JIT_HELPER_FXNMUL   = 3,
  JIT_HELPER_FXNDIV   = 4,
  JIT_HELPER_FXNREM   = 5,
  JIT_HELPER_LD4      = 6,
  JIT_HELPER_ST4      = 7,
  JIT_HELPER_LIST     = 8,
  JIT_HELPER_COPY     = 9,
  JIT_HELPER_CLOSURE  = 10,
  JIT_HELPER_ARGLIST0 = 11,
  JIT_HELPER_ARGLIST1 = 12,
  JIT_HELPER_ARGLIST2 = 13,
  JIT_HELPER_ARGLIST3 = 14,
  JIT_HELPER_ARGLIST4 = 15,
  JIT_HELPER_ARGLIST5 = 16,
  JIT_HELPER_CALL     = 17,
  JIT_HELPER_CALLIR   = 18,
  JIT_HELPER_CALLT    = 19,
  JIT_HELPER_CALLTIR  = 20,
  JIT_HELPER_MCALL    = 21,
  JIT_HELPER_MCALLIR  = 22,
  JIT_HELPER_MOVETX   = 23,
  /* Step 7b: RT-9 fused list allocators.  LIST1 = LIST+ST4_0,
   * LIST2 = LIST+ST4_0+ST4_1.  Single trampoline call replaces
   * the 2- or 3-call sequence the JIT would otherwise emit. */
  JIT_HELPER_LIST1    = 24,
  JIT_HELPER_LIST2    = 25,
  /* Step 7c: SBC_FXNSIZE = `L[dst] = FXN(LIST_SIZE(L[src]))`.
   * Pure heap-header read; no MCACHE fallback (unlike FXNLGET). */
  JIT_HELPER_FXNSIZE  = 26,
  /* Step 10: 4-arg trivial helpers (MOVEEMT / FATAL) and the
   * FXNLGET with-sbc trampoline.  FATAL longjmps; the others
   * return normally. */
  JIT_HELPER_MOVEEMT  = 27,
  JIT_HELPER_FATAL    = 28,
  JIT_HELPER_FXNLGET  = 29,
  JIT_HELPER_FXNLT    = 30,
  JIT_HELPER_FXNGT    = 31,
  JIT_HELPER_FXNLTE   = 32,
  JIT_HELPER_FXNGTE   = 33,
  JIT_HELPER_FXNTAG   = 34,
  JIT_HELPER_NOT      = 35,
  JIT_HELPER_GOT      = 36,
  JIT_HELPER_NO       = 37,
  JIT_HELPER_MOVEIM   = 38,
  JIT_HELPER_INC      = 39,
  JIT_HELPER_DEC      = 40,
  JIT_HELPER_MOVEMT   = 41,
  JIT_HELPER_IMMEQ    = 42,
  JIT_HELPER_IMMNE    = 43,
  JIT_HELPER_FXNLISTN = 44,
  JIT_HELPER_FXNLSETIR = 45,
  JIT_HELPER_NEG      = 46,
  JIT_HELPER_ABS      = 47,
  JIT_HELPER_FXNAND   = 48,
  JIT_HELPER_FXNIOR   = 49,
  JIT_HELPER_FXNXOR   = 50,
  JIT_HELPER_FXNSHL   = 51,
  JIT_HELPER_FXNSHR   = 52,
  JIT_HELPER_MAX
} jit_helper_id_t;

/* One reloc entry: where the helper-pointer imm64 lives in the
 * code buffer + which helper it should resolve to.  Eight bytes
 * total on disk (matches the directory-entry struct format). */
typedef struct {
  uint32_t offset;       /* byte offset of the imm64 in jit_buf.code */
  uint8_t  helper_id;    /* jit_helper_id_t value */
  uint8_t  pad[3];
} jit_reloc_t;

typedef struct jit_buf {
  uint8_t *code;          /* base of the executable mapping */
  size_t cap;             /* total bytes mapped */
  size_t len;             /* bytes written so far */
  /* Reloc-recording state.  When `record_relocs` is non-zero,
   * jit_emit_call_abs appends a (offset, helper_id) entry to
   * `relocs` each time it emits a helper-pointer call.  The
   * helper_id is taken from `pending_helper_id` -- the wrapper
   * functions (jit_emit_call_helper3 / jit_emit_call_with_sbc /
   * the arith family) set this right before calling abs.  This
   * keeps the call_abs signature unchanged.
   *
   * relocs is a heap-malloc'd array, not stb_ds, so the writer
   * can hand the pointer back to sif2sbc without dragging in
   * ng.h.  Caller owns the buffer until jit_buf_free. */
  int record_relocs;
  jit_helper_id_t pending_helper_id;
  jit_reloc_t *relocs;
  int relocs_count;
  int relocs_cap;
} jit_buf;

/* Allocate an executable buffer of `cap` bytes.  Returns NULL on
 * OOM or platform failure.  `cap` is rounded up to a page. */
jit_buf *jit_buf_new(size_t cap);

/* Append one byte; aborts the program if the buffer is full
 * (the buffer's small enough that overrun is a logic bug). */
void jit_emit_u8(jit_buf *b, uint8_t x);

/* Append a little-endian 32-bit integer (4 bytes). */
void jit_emit_u32(jit_buf *b, uint32_t x);

/* Append `n` raw bytes from `src`. */
void jit_emit_bytes(jit_buf *b, const uint8_t *src, size_t n);

/* Return the start of the emitted code as a function pointer.
 * On platforms that require it (W^X), this flips the mapping to
 * PROT_EXEC-only before returning.  After this call the buffer is
 * sealed -- further emit calls have undefined behaviour. */
void *jit_buf_finalize(jit_buf *b);

/* Release the mapping.  Pass NULL safely. */
void jit_buf_free(jit_buf *b);

/* ============================================================
 * Step 1: typed-int arithmetic emitters.
 *
 * Each emitter generates the x86_64 sequence equivalent to one
 * SBC_I{ADD,SUB,MUL,DIV,REM} opcode, operating on a `dyn *L`
 * locals array passed as the function's first integer argument.
 * `a` and `b` are slot indices (0..65535, matching SBC RD16).
 *
 * On Win64 the locals pointer arrives in RCX; on SysV in RDI.
 * The emitters pick the right addressing mode via #ifdef _WIN32.
 *
 * Semantics mirror the FXN* macros in runtime/symta.h:
 *   IADD/ISUB/IREM   bit-for-bit on the tagged values
 *   IMUL             UNFXN(a) * b  (one side detagged first)
 *   IDIV             FXN(a / b)    (re-tag the integer quotient)
 *
 * Use `jit_emit_ret` to close the emitted function.
 * ============================================================ */
void jit_emit_iadd(jit_buf *b, int dst, int a, int x);
void jit_emit_isub(jit_buf *b, int dst, int a, int x);
void jit_emit_imul(jit_buf *b, int dst, int a, int x);
void jit_emit_idiv(jit_buf *b, int dst, int a, int x);
void jit_emit_irem(jit_buf *b, int dst, int a, int x);

/* Bare `ret` -- 1 byte.  Useful for hand-coded test fns that
 * don't run through the standard prologue/epilogue.  Real JIT'd
 * functions use jit_emit_prologue + ... + jit_emit_epilogue
 * instead.  Don't mix the two for one function. */
void jit_emit_ret(jit_buf *b);

/* ============================================================
 * Step 2: control flow + typed-int comparison emitters.
 *
 * Comparisons (SBC_I{LT,GT,LTE,GTE}) write a FXN-tagged 0/1
 * into L[dst].  Sequence: mov rax, [a]; cmp rax, [b]; setcc al;
 * movzx rax, al; shl rax, 16; mov [dst], rax.
 *
 * Jumps use rel32 displacements.  Forward references emit a
 * placeholder displacement and return the OFFSET of that
 * 4-byte slot; once the target is known, `jit_patch_jmp_here`
 * (target = current emit position) or `jit_patch_jmp_to`
 * (explicit target offset, for backward jumps via
 * `jit_here`) writes the real displacement.
 *
 * Slot-truthy branches (SBC_B-equivalent for typed-int) emit
 * `cmp qword ptr [L + slot*8], 0` then `jnz`/`jz rel32`.
 * ============================================================ */

void jit_emit_ilt (jit_buf *b, int dst, int a, int x);
void jit_emit_igt (jit_buf *b, int dst, int a, int x);
void jit_emit_ilte(jit_buf *b, int dst, int a, int x);
void jit_emit_igte(jit_buf *b, int dst, int a, int x);

/* SBC_SAME / SBC_VARY: bit-identical pointer compare (same x86
 * shape as the I* comparisons, just with sete / setne).  Used by
 * `X >< No`, `case X No`, and the rest of the FXN-tagged identity
 * checks the macroexpander emits. */
void jit_emit_same(jit_buf *b, int dst, int a, int x);
void jit_emit_vary(jit_buf *b, int dst, int a, int x);

/* Current emit position -- used as a label target for back-jumps. */
size_t jit_here(jit_buf *b);

/* Unconditional jmp rel32 with a placeholder displacement;
 * returns the offset of the 4-byte disp field for patching. */
size_t jit_emit_jmp(jit_buf *b);

/* Conditional jump on L[slot]'s truthiness (0 => falsy).
 * Returns the displacement-field offset for patching. */
size_t jit_emit_jnz_slot(jit_buf *b, int slot);
size_t jit_emit_jz_slot (jit_buf *b, int slot);

/* Resolve a pending jump's displacement: target = current
 * emit position (forward jump completing here). */
void jit_patch_jmp_here(jit_buf *b, size_t patch_off);

/* Resolve to an explicit target offset (backward jump). */
void jit_patch_jmp_to(jit_buf *b, size_t patch_off, size_t target);

/* ============================================================
 * Step 3: prologue / epilogue / C-runtime trampoline.
 *
 * Real JIT'd functions bracket their body with prologue +
 * epilogue.  The prologue saves callee-saved RBX, loads it with
 * the platform's arg0 (the locals pointer), and allocates 32
 * bytes of shadow space so any inner C call has Win64-correct
 * stack alignment.  The epilogue tears it down and returns.
 *
 * Inside the prologue..epilogue window, all arith/jump emitters
 * address [rbx + slot*8] (LOCALS_RM is RBX).  RBX is callee-
 * saved on both Win64 and SysV, so it survives inner calls
 * without per-call save/restore.
 *
 * `jit_emit_mov_arg0_from_locals` puts RBX back into arg0
 * (RCX on Win64, RDI on SysV) just before a C call.
 *
 * `jit_emit_call_abs(target)` emits the 12-byte sequence
 *   mov rax, imm64
 *   call rax
 * which works regardless of how far `target` is from the JIT
 * region.  Caller is responsible for loading any other arg
 * registers BEFORE this call.
 * ============================================================ */

void jit_emit_prologue(jit_buf *b);
void jit_emit_epilogue(jit_buf *b);
void jit_emit_mov_arg0_from_locals(jit_buf *b);
void jit_emit_call_abs(jit_buf *b, void *target);

/* ============================================================
 * Step 5c: 4-arg call trampoline.
 *
 * Emits the calling-convention-specific argument setup for a
 * helper with C signature `void helper(int64_t *L, int dst,
 * int a, int b)` and the indirect call to `target`.  Used by
 * the JIT to defer opcodes it doesn't natively emit -- e.g.
 * the untyped FXN* arith ops, MCALL dispatch, allocators --
 * to runtime helpers that mirror the interpreter's behaviour.
 *
 * Slot indices are passed as 32-bit unsigned (mov r32, imm32
 * zero-extends to 64).  The locals pointer is sourced from
 * RBX (where the prologue stashed it) and copied into the
 * platform's arg0 register.
 *
 * After the call returns:
 *   RBX still holds L (callee-saved)
 *   RAX holds the helper's return value (ignored for void)
 *   Other caller-saved regs are clobbered
 *
 * Win64 ABI: RCX=L, RDX=dst, R8=a, R9=b.
 * SysV  ABI: RDI=L, RSI=dst, RDX=a, RCX=b.
 * ============================================================ */
void jit_emit_call_helper3(jit_buf *b, void *target,
                           uint32_t slot_dst, uint32_t slot_a, uint32_t slot_b);

/* 2-arg prologue/epilogue (saves R12 in addition to RBX,
 * loads R12 with arg1).  Used by jit_translate_with_sbc. */
void jit_emit_prologue2(jit_buf *b);
void jit_emit_epilogue2(jit_buf *b);

/* SBC-aware 3-arg helper call:
 *   arg0 = L   (from RBX)
 *   arg1 = sbc (from R12)
 *   arg2 = packed_imm
 * Helper signature: void(*)(int64_t *L, struct sbc_t *sbc, uint64_t). */
void jit_emit_call_with_sbc(jit_buf *b, void *target, uint64_t packed);

/* Forward decl so callers don't need to include runtime/sif.h. */
struct sbc_t;

/* Step 8: AOT default-on flag.  Read by the writer (sif2sbc) and
 * loader (sbc_prepare) to decide whether to embed/adopt native
 * code without each consulting env vars separately.
 *
 * Default: 1 on _WIN32 (SEH unwind works), 0 elsewhere (POSIX
 * DWARF .eh_frame is still TODO so native frames can't be
 * longjmp'd through safely).
 *
 * Overridden by CLI flags in main.c:
 *   --no-jit    -> 0  (skip both AOT writer and AOT loader)
 *   --jit       -> 1  (force on, useful for Linux testing)
 *
 * Env-var overrides apply over the CLI defaults (in order of
 * precedence: SYMTA_NO_JIT > SYMTA_JIT > CLI flag > platform
 * default).  SYMTA_AOT_IA64 / SYMTA_AOT_RUN still work as
 * fine-grained controls -- if set, they take precedence over
 * the unified flag in their respective sides. */
extern int jit_aot_enabled;

/* Diagnostics set on the most recent `jit_translate` failure.
 * Useful for figuring out which opcode is blocking translation
 * of real code so we know what to implement next. */
extern uint8_t jit_last_fail_opcode;
extern size_t  jit_last_fail_offset;

/* FXN* arith helpers -- function pointers so jit_sbc.c can swap
 * in full 3-way-dispatch impls (int-int / int-float / non-int).
 * Default (set in jit.c) is the int-only fast path, which is
 * sufficient for the standalone self-test. */
extern void (*jit_rt_fxnadd_helper)(int64_t *L, int dst, int a, int b);
extern void (*jit_rt_fxnsub_helper)(int64_t *L, int dst, int a, int b);
extern void (*jit_rt_fxnmul_helper)(int64_t *L, int dst, int a, int b);
extern void (*jit_rt_fxndiv_helper)(int64_t *L, int dst, int a, int b);
extern void (*jit_rt_fxnrem_helper)(int64_t *L, int dst, int a, int b);

/* Trampoline-helper function pointers.  Set by the runtime
 * (jit_sbc.c) at first audit; left NULL in the standalone
 * self-test build, in which case the corresponding opcodes
 * refuse to translate and the function falls back to
 * interpretation. */

/* SBC_LD4_* / SBC_LOAD / SBC_LOAD8.
 * Signature: (L, dst_slot, src_slot, struct_field_index). */
extern void (*jit_rt_ld4_helper)(int64_t *L, int dst, int src, int index);

/* SBC_ST4_* / SBC_STOR (when added).
 * Signature: (L, target_obj_slot, value_slot, struct_field_index).
 * Writes L[value] into the indexed field of the object L[target]. */
extern void (*jit_rt_st4_helper)(int64_t *L, int dst, int src, int index);

/* SBC_LIST.  Allocates an `size`-slot list, leaves the dyn in
 * L[dst].  Signature reuses the helper3 calling convention --
 * the third int is the size (treated as a literal, not a slot
 * index).  No sbc pointer needed, the LIST macro routes via
 * gc_alloc which uses thread-local heap state. */
extern void (*jit_rt_list_helper)(int64_t *L, int dst, int size, int unused);

/* SBC_LIST1 / SBC_LIST2 (RT-9 fused list literals).  LIST1
 * allocates a size-1 list and stores L[x] at slot 0; LIST2
 * allocates a size-2 list and stores L[a]/L[b] at slots 0/1. */
extern void (*jit_rt_list1_helper)(int64_t *L, int dst, int x, int unused);
extern void (*jit_rt_list2_helper)(int64_t *L, int dst, int a, int b);

/* SBC_FXNSIZE: L[dst] = FXN(LIST_SIZE(L[src])).  No MCACHE
 * fallback (unlike FXNLGET); pure heap-header field read. */
extern void (*jit_rt_fxnsize_helper)(int64_t *L, int dst, int src, int unused);

/* SBC_COPY: LSET(L[dst], dindex, LGET(L[src], sindex)).  Packs
 * the two 16-bit field indices into the third helper3 arg
 * (low 16 = sindex, high 16 = dindex).  The helper unpacks. */
extern void (*jit_rt_copy_helper)(int64_t *L, int dst, int src, int packed_indices);

/* ARGLIST family + CALL: per-call-site argument setup and
 * dispatch.  ARGLIST{0,1,2,3} all use the helper3 calling
 * convention; trailing slot args are ignored when not needed.
 * CALL/CALLIR likewise -- dst+fn for CALL, fn-only for the
 * ignore-result variant.  Stack-trace `pin` recording is
 * skipped in the JIT'd path (interpreter only). */
extern void (*jit_rt_arglist0_helper)(int64_t *L, int a, int b, int c);
extern void (*jit_rt_arglist1_helper)(int64_t *L, int a, int b, int c);
extern void (*jit_rt_arglist2_helper)(int64_t *L, int a, int b, int c);
extern void (*jit_rt_arglist3_helper)(int64_t *L, int a, int b, int c);
extern void (*jit_rt_call_helper)   (int64_t *L, int dst, int fn, int unused);
extern void (*jit_rt_callir_helper) (int64_t *L, int fn,  int u1, int u2);

/* CALL_TAGGED variants: SBC_CALLT (with dst) / SBC_CALLTIR
 * (ignore result).  These dispatch via the value's tag --
 * used when the callee might not be a closure (e.g. calling
 * a text or a list).  Same operand layout as CALL/CALLIR. */
extern void (*jit_rt_callt_helper)  (int64_t *L, int dst, int fn, int unused);
extern void (*jit_rt_calltir_helper)(int64_t *L, int fn,  int u1, int u2);

/* MCALL family: method dispatch through the per-callsite mcache.
 * Each SBC_MCALL instruction carries (dst, obj, met) operands
 * AND an mcache slot index that the MCACHE_CALL macro reads
 * after the operands.  The helper takes a single 64-bit packed
 * arg so the call fits the existing call_with_sbc layout:
 *   [63:48] = mcache_idx
 *   [47:32] = met         (method-table index)
 *   [31:16] = obj         (slot)
 *   [15: 0] = dst         (slot; ignored for MCALLIR) */
extern void (*jit_rt_mcall_helper)  (int64_t *L, struct sbc_t *sbc, uint64_t packed);
extern void (*jit_rt_mcallir_helper)(int64_t *L, struct sbc_t *sbc, uint64_t packed);

/* ARGLIST4/5: pack the 4 or 5 u8 slot indices into one int32. */
extern void (*jit_rt_arglist4_helper)(int64_t *L, int packed, int u1, int u2);
extern void (*jit_rt_arglist5_helper)(int64_t *L, int packed, int u1, int u2);

/* SBC_MOVETX / MOVETX8: L[dst] = sbc->tx[src] (text constant
 * table lookup).  Needs sbc context. */
extern void (*jit_rt_movetx_helper) (int64_t *L, struct sbc_t *sbc, uint64_t packed);

/* SBC_MOVEEMT: L[dst] = Empty (the heap-allocated empty-list
 * singleton).  Cheaper than a 3-instruction inline because Empty's
 * address depends on init_builtins layout -- a helper that
 * dereferences `api.empty_` keeps the JIT free of that
 * cross-translation-unit binding.  Signature reuses helper3:
 *   (L, dst_slot, unused, unused). */
extern void (*jit_rt_moveemt_helper)(int64_t *L, int dst, int u1, int u2);

/* SBC_FATAL: raise a fatal error with the text in L[msg].
 * Longjmps via the runtime's `fatal(char*)`.  Windows SEH
 * unwind info registered by jit_register_unwind_2arg in step 5n
 * lets the unwind walk JIT'd frames.  Signature reuses helper3:
 *   (L, msg_slot, unused, unused). */
extern void (*jit_rt_fatal_helper)(int64_t *L, int msg, int u1, int u2);

/* SBC_FXNLGET / SBC_FXNLSET: list-element get / set with the
 * per-call-site mcache slot.  The interpreter does a fast-path
 * type check (T_LIST + T_INT + in-bounds) and falls back to
 * MCACHE_CALL of `m_get` / `m_set`.  Helper mirrors that exactly.
 * Wire packs the four operands into 64 bits:
 *   [15:0]   dst
 *   [31:16]  src
 *   [47:32]  index   (for FXNLGET) / val (for FXNLSET)
 *   [63:48]  mcache_idx                (for FXNLGET)
 *
 * FXNLSET also carries an `index` field (u16) that doesn't fit
 * in 64 bits with all the other operands, so its helper takes a
 * second packed argument -- not added here yet; FXNLGET is the
 * higher-impact blocker. */
extern void (*jit_rt_fxnlget_helper)(int64_t *L, struct sbc_t *sbc,
                                     uint64_t packed);

/* SBC_FXNLT / FXNGT / FXNLTE / FXNGTE: untyped ordering ops.
 * Fast path = both T_INT, inline FXNLT macro; else MCALL into
 * m_lt / m_gt / m_lte / m_gte (NO mcache slot, plain MCALL).
 * Helper3 sig: (L, dst, a, b). */
extern void (*jit_rt_fxnlt_helper) (int64_t *L, int dst, int a, int b);
extern void (*jit_rt_fxngt_helper) (int64_t *L, int dst, int a, int b);
extern void (*jit_rt_fxnlte_helper)(int64_t *L, int dst, int a, int b);
extern void (*jit_rt_fxngte_helper)(int64_t *L, int dst, int a, int b);

/* SBC_FXNTAG: L[dst] = FXN(O_TAG(L[src])).  Trivial heap-header
 * read; no fast/slow split.  Helper3: (L, dst, src, _). */
extern void (*jit_rt_fxntag_helper)(int64_t *L, int dst, int src, int u);

/* SBC_NOT / SBC_GOT / SBC_NO: dst=u16 src=u16; each writes FXN(0)
 * or FXN(1) based on a truthy/No comparison.  Trivial inline-able
 * but cheaper to ship as helper3 trampolines for now. */
extern void (*jit_rt_not_helper) (int64_t *L, int dst, int src, int u);
extern void (*jit_rt_got_helper) (int64_t *L, int dst, int src, int u);
extern void (*jit_rt_no_helper)  (int64_t *L, int dst, int src, int u);

/* SBC_MOVEIM: L[dst] = sbc->im[src].  Needs the sbc pointer so
 * goes via the call_with_sbc trampoline.  Packed: [15:0]=dst,
 * [39:16]=src (24-bit). */
extern void (*jit_rt_moveim_helper)(int64_t *L, struct sbc_t *sbc,
                                    uint64_t packed);

/* SBC_MOVEMT / SBC_MOVEMT8: L[dst] = FXN(sbc->mt[src]).  Pulls
 * a method-id from the per-SBC method table.  Packed format
 * identical to MOVEIM. */
extern void (*jit_rt_movemt_helper)(int64_t *L, struct sbc_t *sbc,
                                    uint64_t packed);

/* SBC_IMMEQ / SBC_IMMNE: immediate equality / inequality with
 * per-call-site mcache.  Three-way dispatch (T_INT inline, text-
 * vs-text via texts_equal, fallback via MCACHE_CALL m_eq/m_ne)
 * mirrors sbc.c exactly.  Packed: [15:0]=dst, [31:16]=a,
 * [47:32]=b, [63:48]=mcache_idx. */
extern void (*jit_rt_immeq_helper)(int64_t *L, struct sbc_t *sbc,
                                   uint64_t packed);
extern void (*jit_rt_immne_helper)(int64_t *L, struct sbc_t *sbc,
                                   uint64_t packed);

/* SBC_FXNLISTN: L[dst] = LIST(UNFXN(L[src])).  Variable-size list
 * allocator -- size comes from a slot at runtime.  Helper3 sig:
 * (L, dst_slot, src_slot, unused). */
extern void (*jit_rt_fxnlistn_helper)(int64_t *L, int dst, int src, int u);

/* SBC_FXNLSETIR: list-element-set, ignore-result.  Body:
 *   FXNLSET(_, L[src], L[index], L[val])  on T_LIST+T_INT+inbounds
 *   else MCACHE_CALL(m_set, L[src]) with args (src, index, val)
 * Packed (4 u16 fields):
 *   [15:0]   = src   (slot)
 *   [31:16]  = index (slot)
 *   [47:32]  = val   (slot)
 *   [63:48]  = mcache_idx */
extern void (*jit_rt_fxnlsetir_helper)(int64_t *L, struct sbc_t *sbc,
                                       uint64_t packed);

/* SBC_NEG / SBC_ABS: unary arith with T_INT fast path and MCALL
 * fallback (m_neg / m_abs).  ABS additionally has a T_FLOAT
 * fast path (fabs).  Helper3 sig: (L, dst, a, _). */
extern void (*jit_rt_neg_helper)(int64_t *L, int dst, int a, int u);
extern void (*jit_rt_abs_helper)(int64_t *L, int dst, int a, int u);

/* SBC_FXNAND / IOR / XOR / SHL / SHR: bitwise ops with T_INT-T_INT
 * fast path and MCALL fallback (m_and / m_ior / m_xor / m_shl /
 * m_shr).  Helper3 sig: (L, dst, a, b). */
extern void (*jit_rt_fxnand_helper)(int64_t *L, int dst, int a, int b);
extern void (*jit_rt_fxnior_helper)(int64_t *L, int dst, int a, int b);
extern void (*jit_rt_fxnxor_helper)(int64_t *L, int dst, int a, int b);
extern void (*jit_rt_fxnshl_helper)(int64_t *L, int dst, int a, int b);
extern void (*jit_rt_fxnshr_helper)(int64_t *L, int dst, int a, int b);

/* SBC_INC / SBC_DEC: T_INT fast path = FXNADD/SUB(_,_,FXN(1));
 * else MCALL m_inc / m_dec.  Helper3 sig: (L, dst, a, _). */
extern void (*jit_rt_inc_helper)(int64_t *L, int dst, int a, int u);
extern void (*jit_rt_dec_helper)(int64_t *L, int dst, int a, int u);

/* SBC_CLOSURE.  Builds a closure object via the CLOSURE
 * allocator macro.  The third arg packs (dst<<32|idx<<16|size)
 * so the call fits in three integer-arg registers; the helper
 * unpacks.  Requires the 2-arg JIT'd function form (sbc in
 * R12) -- see jit_translate_with_sbc. */
extern void (*jit_rt_closure_helper)(int64_t *L, struct sbc_t *sbc,
                                     uint64_t packed);

/* ============================================================
 * Step 5d: SBC-level JIT integration.
 *
 * `sbc_jit_audit` walks every function in `sbc`, tries to
 * translate it via `jit_translate`, and returns the number that
 * succeeded.  Doesn't actually invoke any JIT'd code -- it's an
 * instrumentation hook for measuring how much of a loaded SBC
 * the JIT can handle today.  Failing translations get freed
 * before counting moves on.
 *
 * Called from sbc_prepare when the env var SYMTA_JIT_AUDIT=1.
 * Prints a `jit-audit: K/N functions translatable` line to
 * stderr.  Zero overhead when the env var is unset.
 * ============================================================ */
int sbc_jit_audit(struct sbc_t *sbc);

/* ============================================================
 * Step 5k: actually install JIT'd code into the dispatch table.
 *
 * For each function `jit_translate_with_sbc` accepts, this:
 *   1. Allocates a heap payload struct {jit_body, sbc, nvars}.
 *   2. Finalizes the jit_buf (sealing it executable).
 *   3. Rewrites the hooks_heap entry for that function to
 *      point at `jit_adapter` (instead of sbc_exec_fn) with
 *      our payload (instead of pin).
 *
 * jit_adapter has the same `dyn(uint8_t*)` signature the hook
 * system needs.  Internally it allocates the function's frame
 * via a VLA matching the SUBR macro, then calls the JIT'd body
 * with `(L, sbc)`.  CALL's outer api.frame save/restore takes
 * care of unwinding on return.
 *
 * Returns the number of functions successfully installed.
 * Functions that can't translate are left interpreter-bound;
 * mixed-mode dispatch is fully supported (a JIT'd caller can
 * invoke an interpreted callee or vice versa, the hook layer
 * is uniform). */
int sbc_jit_install(struct sbc_t *sbc);

/* ============================================================
 * Step 6c: install pre-baked native code from the SBC's IA64
 * section.  Like sbc_jit_install but consumes bytes the writer
 * already produced (step 6b) instead of re-running the JIT.
 *
 * Returns the number of functions installed.  Skips functions
 * with payload_offset=0 (writer judged untranslatable) and
 * functions whose relocs reference an unknown helper_id (would
 * mean the writer and loader disagree on the helper enum).
 *
 * Gated on SYMTA_AOT_RUN=1 in sbc_prepare; default builds leave
 * the section unused (interpreter dispatch) until the AOT path
 * is fully shaken out.
 * ============================================================ */
int sbc_install_ia64(struct sbc_t *sbc);

/* ============================================================
 * Step 4: SBC bytecode -> x86 translator.
 *
 * Walks `n` bytes of SBC bytecode starting at `bc` and emits an
 * equivalent x86_64 function (prologue + body + epilogue).  The
 * returned buffer is freshly allocated and not yet finalized --
 * the caller should `jit_buf_finalize` it and cast to the
 * appropriate function pointer.
 *
 * The translator handles a strict subset: SBC_NOP, the typed-
 * int arith family (IADD/ISUB/IMUL/IDIV/IREM), the typed-int
 * comparisons (ILT/IGT/ILTE/IGTE), and the LEAVE/LEAVE0
 * terminators.  Any other opcode -- function calls, allocation,
 * branches, MCALL, etc. -- causes the translator to free the
 * buffer and return NULL so the caller falls back to the
 * interpreter.  Step 5 will widen this via the C-runtime
 * trampoline.
 *
 * Return value: a fresh `jit_buf` on success (the caller owns
 * it), NULL if any opcode in the stream isn't supported.
 * ============================================================ */
jit_buf *jit_translate(const uint8_t *bc, size_t n);

/* Same translator, but emits a 2-arg function: L in arg0, the
 * owning `sbc_t *` in arg1.  Prologue saves R12 in addition to
 * RBX and loads R12 with arg1 -- both callee-saved on Win64
 * and SysV so they survive across inner C calls.  Opcodes that
 * need sbc access (SBC_CLOSURE) only translate in this mode;
 * the 1-arg path bails on them.
 *
 * Caller invokes the JIT'd code as `fn(L, sbc)`. */
jit_buf *jit_translate_with_sbc(const uint8_t *bc, size_t n);

/* AOT-recording variant of jit_translate_with_sbc.  Same code is
 * emitted, but each helper-pointer call site adds an entry to
 * `b->relocs`.  The writer hands these off to sif2sbc which
 * stores them in the SBC's IA64 section; the loader patches the
 * imm64s on load.
 *
 * Caller owns the returned jit_buf (and its relocs[]).  Note the
 * code is NOT finalized to PROT_EXEC -- the writer never executes
 * it, just reads b->code[0..b->len] for storage. */
jit_buf *jit_translate_with_sbc_record(const uint8_t *bc, size_t n);

/* Look up the live function pointer for a given helper_id in
 * THIS process.  Returns NULL for JIT_HELPER_NONE or unknown
 * ids.  Used by the loader to patch each relocated imm64 in
 * a JIT'd code blob.  Implemented in jit_sbc.c (the runtime
 * glue layer); jit.c's standalone self-test stubs it. */
void *jit_helper_pointer(int helper_id);

/* Ensure the helper-pointer globals point at their full-dispatch
 * impls (not the int-only fallbacks).  Idempotent.  Implemented
 * in jit_sbc.c.  Writer must call this before
 * jit_translate_with_sbc_record so the relocs collected refer to
 * the real impls; the loader must call it too before applying
 * relocs since the helper-id-to-pointer mapping depends on it. */
void jit_install_helpers_public(void);

#endif
