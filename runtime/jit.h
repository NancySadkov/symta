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
void jit_emit_ret(jit_buf *b);

#endif
