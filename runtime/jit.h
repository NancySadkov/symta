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

typedef struct jit_buf {
  uint8_t *code;     /* base of the executable mapping */
  size_t cap;        /* total bytes mapped */
  size_t len;        /* bytes written so far */
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

#endif
