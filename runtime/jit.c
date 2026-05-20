/* runtime/jit.c -- x86_64 JIT scaffold (step 0).
 *
 * See jit.h for the surface API and rationale.  This file is the
 * platform-portable allocator + raw-bytes appender, with a
 * self-test in #ifdef JIT_SELF_TEST that proves we can emit a
 * 7-byte `add(a,b)=a+b` x86_64 function and invoke it.
 *
 * Build (self-test):
 *   gcc -O0 -g -Wall -DJIT_SELF_TEST runtime/jit.c -o jit_test
 *   ./jit_test
 *
 * Normal build: included via Makefile, linked into symta.exe.
 */

#include "jit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
#endif

/* Round `n` up to the nearest multiple of `page`. */
static size_t round_up(size_t n, size_t page) {
  return (n + page - 1) & ~(page - 1);
}

static size_t page_size(void) {
#ifdef _WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (size_t)si.dwPageSize;
#else
  return (size_t)sysconf(_SC_PAGESIZE);
#endif
}

jit_buf *jit_buf_new(size_t cap) {
  jit_buf *b = (jit_buf*)calloc(1, sizeof(*b));
  if (!b) return NULL;
  size_t ps = page_size();
  b->cap = round_up(cap, ps);
#ifdef _WIN32
  b->code = (uint8_t*)VirtualAlloc(NULL, b->cap, MEM_RESERVE|MEM_COMMIT,
                                   PAGE_EXECUTE_READWRITE);
#else
  b->code = (uint8_t*)mmap(NULL, b->cap, PROT_READ|PROT_WRITE|PROT_EXEC,
                           MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (b->code == MAP_FAILED) b->code = NULL;
#endif
  if (!b->code) { free(b); return NULL; }
  return b;
}

void jit_emit_u8(jit_buf *b, uint8_t x) {
  if (b->len >= b->cap) {
    fprintf(stderr, "jit_emit_u8: buffer overflow (cap=%zu)\n", b->cap);
    abort();
  }
  b->code[b->len++] = x;
}

void jit_emit_u32(jit_buf *b, uint32_t x) {
  jit_emit_u8(b, (uint8_t)(x & 0xff));
  jit_emit_u8(b, (uint8_t)((x >> 8) & 0xff));
  jit_emit_u8(b, (uint8_t)((x >> 16) & 0xff));
  jit_emit_u8(b, (uint8_t)((x >> 24) & 0xff));
}

void jit_emit_bytes(jit_buf *b, const uint8_t *src, size_t n) {
  if (b->len + n > b->cap) {
    fprintf(stderr, "jit_emit_bytes: would overflow (cap=%zu len=%zu n=%zu)\n",
            b->cap, b->len, n);
    abort();
  }
  memcpy(b->code + b->len, src, n);
  b->len += n;
}

void *jit_buf_finalize(jit_buf *b) {
  /* W^X tightening: on POSIX, drop write permission so a buggy
   * write past `len` can't silently land in live code.  On
   * Windows we leave RWX -- VirtualProtect is cheap but not
   * strictly needed for the proof-of-concept.  Both platforms
   * need an icache flush on architectures that have a split
   * icache; x86_64 is coherent so nothing to do there. */
#ifndef _WIN32
  mprotect(b->code, b->cap, PROT_READ|PROT_EXEC);
#endif
  return (void*)b->code;
}

void jit_buf_free(jit_buf *b) {
  if (!b) return;
  if (b->code) {
#ifdef _WIN32
    VirtualFree(b->code, 0, MEM_RELEASE);
#else
    munmap(b->code, b->cap);
#endif
  }
  free(b);
}

/* ============================================================
 * Step 1 helpers: x86_64 modr/m + addressing-mode emission.
 *
 * The locals-array pointer is passed via the platform's first
 * integer-arg register: RCX on Win64, RDI on SysV.  Both happen
 * to share the same encoding lane on the index 1 / 7 axis of
 * the ModR/M byte, so the emitters parameterise only on that
 * register's r/m field via the LOCALS_RM constant below.
 *
 * ModR/M layout (mod[7:6] reg[5:3] r/m[2:0]):
 *   mod=01  ->  [base + disp8]
 *   mod=10  ->  [base + disp32]
 * We choose disp8 when the slot's byte offset fits in int8_t
 * (slots 0..15), else disp32.  The 16-bit SBC slot indices
 * make disp32 the worst case (8*65535 = 524280, well under
 * int32_t max).
 * ============================================================ */

#ifdef _WIN32
  /* Win64: arg0 = RCX, r/m code 001. */
  #define LOCALS_RM 1
#else
  /* SysV:  arg0 = RDI, r/m code 111. */
  #define LOCALS_RM 7
#endif

/* Emit ModR/M + displacement for `[locals_reg + slot*8]` with
 * the given /reg (or /opcode-ext) field set.  Picks disp8 vs
 * disp32 automatically. */
static void emit_mem_op(jit_buf *b, uint8_t reg_field, int slot) {
  int32_t disp = (int32_t)slot * 8;
  if (disp >= -128 && disp <= 127) {
    jit_emit_u8(b, 0x40 | (uint8_t)((reg_field & 7) << 3) | LOCALS_RM);
    jit_emit_u8(b, (uint8_t)(int8_t)disp);
  } else {
    jit_emit_u8(b, 0x80 | (uint8_t)((reg_field & 7) << 3) | LOCALS_RM);
    jit_emit_u32(b, (uint32_t)disp);
  }
}

/* mov rax, [locals_reg + slot*8] */
static void emit_mov_rax_from_slot(jit_buf *b, int slot) {
  jit_emit_u8(b, 0x48);  /* REX.W */
  jit_emit_u8(b, 0x8b);  /* opcode: mov r64, r/m64 */
  emit_mem_op(b, 0, slot);  /* reg=RAX (0) */
}

/* mov [locals_reg + slot*8], rax */
static void emit_mov_slot_from_rax(jit_buf *b, int slot) {
  jit_emit_u8(b, 0x48);  /* REX.W */
  jit_emit_u8(b, 0x89);  /* opcode: mov r/m64, r64 */
  emit_mem_op(b, 0, slot);  /* reg=RAX (0) */
}

/* mov [locals_reg + slot*8], rdx (for IREM remainder) */
static void emit_mov_slot_from_rdx(jit_buf *b, int slot) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x89);
  emit_mem_op(b, 2, slot);  /* reg=RDX (2) */
}

/* add rax, [locals_reg + slot*8] */
static void emit_add_rax_from_slot(jit_buf *b, int slot) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x03);  /* opcode: add r64, r/m64 */
  emit_mem_op(b, 0, slot);
}

/* sub rax, [locals_reg + slot*8] */
static void emit_sub_rax_from_slot(jit_buf *b, int slot) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x2b);  /* opcode: sub r64, r/m64 */
  emit_mem_op(b, 0, slot);
}

/* imul rax, [locals_reg + slot*8] (2-operand form, low 64 bits) */
static void emit_imul_rax_from_slot(jit_buf *b, int slot) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x0f);
  jit_emit_u8(b, 0xaf);  /* opcode: imul r64, r/m64 */
  emit_mem_op(b, 0, slot);
}

/* idiv qword [locals_reg + slot*8] (signed, RDX:RAX / mem -> RAX, RDX) */
static void emit_idiv_from_slot(jit_buf *b, int slot) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0xf7);
  emit_mem_op(b, 7, slot);  /* /7 = idiv */
}

/* sar rax, imm8 (arithmetic shift right -- preserves sign) */
static void emit_sar_rax(jit_buf *b, uint8_t imm) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0xc1);
  jit_emit_u8(b, 0xf8);  /* mod=11 reg=/7=sar r/m=000=RAX */
  jit_emit_u8(b, imm);
}

/* shl rax, imm8 */
static void emit_shl_rax(jit_buf *b, uint8_t imm) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0xc1);
  jit_emit_u8(b, 0xe0);  /* mod=11 reg=/4=shl r/m=000=RAX */
  jit_emit_u8(b, imm);
}

/* cqo : sign-extend RAX into RDX:RAX (prelude to idiv) */
static void emit_cqo(jit_buf *b) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x99);
}

void jit_emit_iadd(jit_buf *b, int dst, int a, int x) {
  emit_mov_rax_from_slot(b, a);
  emit_add_rax_from_slot(b, x);
  emit_mov_slot_from_rax(b, dst);
}

void jit_emit_isub(jit_buf *b, int dst, int a, int x) {
  emit_mov_rax_from_slot(b, a);
  emit_sub_rax_from_slot(b, x);
  emit_mov_slot_from_rax(b, dst);
}

void jit_emit_imul(jit_buf *b, int dst, int a, int x) {
  /* FXNMUL: dst = UNFXN(a) * b -- detag the first operand by
   * arithmetic shift right (preserves negative numbers), then
   * multiply by the still-tagged b.  The shift effectively
   * cancels one of the two tags so the product has exactly one
   * tag's worth of low-zero bits, matching FXN(X_a*X_b). */
  emit_mov_rax_from_slot(b, a);
  emit_sar_rax(b, 16);  /* GID_SHFT = TAG_BITS = 16 */
  emit_imul_rax_from_slot(b, x);
  emit_mov_slot_from_rax(b, dst);
}

void jit_emit_idiv(jit_buf *b, int dst, int a, int x) {
  /* FXNDIV: dst = FXN(a/b).  Integer division of two tagged
   * values cancels the tags (both low-16 are zero so the
   * quotient is the untagged ratio); shift left 16 to re-tag.
   * Division by zero traps via the existing SEH/SIGFPE handler
   * the same as the interpreter's FXNDIV. */
  emit_mov_rax_from_slot(b, a);
  emit_cqo(b);
  emit_idiv_from_slot(b, x);
  emit_shl_rax(b, 16);
  emit_mov_slot_from_rax(b, dst);
}

void jit_emit_irem(jit_buf *b, int dst, int a, int x) {
  /* FXNREM: dst = a % b -- bit-for-bit modulus on tagged
   * values.  IDIV produces remainder in RDX, which is already
   * tagged correctly because the low 16 bits of both operands
   * were zero so the modulus's low bits are zero too. */
  emit_mov_rax_from_slot(b, a);
  emit_cqo(b);
  emit_idiv_from_slot(b, x);
  emit_mov_slot_from_rdx(b, dst);
}

void jit_emit_ret(jit_buf *b) {
  jit_emit_u8(b, 0xc3);
}

#ifdef JIT_SELF_TEST

/* Mirror the FXN tag-encoding from runtime/symta.h so the test
 * file is self-contained (avoids dragging in all of symta.h
 * transitively).  TAG_BITS = 16. */
#define TEST_FXN(x)   ((int64_t)(x) << 16)
#define TEST_UNFXN(x) ((int64_t)(x) >> 16)

static int check(const char *label, int64_t got, int64_t want) {
  int ok = got == want;
  printf("%s  %s  got=0x%llx (%lld)  want=0x%llx (%lld)\n",
         ok ? "OK " : "FAIL", label,
         (unsigned long long)got, (long long)TEST_UNFXN(got),
         (unsigned long long)want, (long long)TEST_UNFXN(want));
  return !ok;
}

/* Step 0 proof: a hand-coded `add(a,b) = a + b` -- one int64
 * return value, two int64 args, no locals array. */
static int step0_add(void) {
  jit_buf *b = jit_buf_new(64);
  if (!b) return 1;

#ifdef _WIN32
  static const uint8_t add_a_b[] = {
    0x48, 0x89, 0xc8,  /* mov rax, rcx */
    0x48, 0x01, 0xd0,  /* add rax, rdx */
    0xc3,              /* ret          */
  };
#else
  static const uint8_t add_a_b[] = {
    0x48, 0x89, 0xf8,  /* mov rax, rdi */
    0x48, 0x01, 0xf0,  /* add rax, rsi */
    0xc3,              /* ret          */
  };
#endif
  jit_emit_bytes(b, add_a_b, sizeof(add_a_b));
  int64_t (*fn)(int64_t, int64_t) =
    (int64_t(*)(int64_t,int64_t))jit_buf_finalize(b);

  int fail = 0;
  fail += check("add(3,5)",  fn(3, 5),  8);
  fail += check("add(0,0)",  fn(0, 0),  0);
  fail += check("add(-1,1)", fn(-1, 1), 0);
  jit_buf_free(b);
  return fail;
}

/* Step 1 proof: each typed-int opcode emits the right
 * x86 and produces the same result as the FXN* macros.
 *
 * Test program:
 *   L[2] = L[0] + L[1]
 *   L[3] = L[0] - L[1]
 *   L[4] = L[0] * L[1]
 *   L[5] = L[0] / L[1]
 *   L[6] = L[0] % L[1]
 *   return
 *
 * Inputs: L[0] = FXN(20), L[1] = FXN(7).
 * Expected: FXN(27), FXN(13), FXN(140), FXN(2), FXN(6). */
static int step1_arith(void) {
  jit_buf *b = jit_buf_new(256);
  if (!b) return 1;

  jit_emit_iadd(b, 2, 0, 1);
  jit_emit_isub(b, 3, 0, 1);
  jit_emit_imul(b, 4, 0, 1);
  jit_emit_idiv(b, 5, 0, 1);
  jit_emit_irem(b, 6, 0, 1);
  jit_emit_ret(b);

  void (*fn)(int64_t*) = (void(*)(int64_t*))jit_buf_finalize(b);

  int64_t L[10] = {0};
  L[0] = TEST_FXN(20);
  L[1] = TEST_FXN(7);
  fn(L);

  int fail = 0;
  fail += check("IADD  20+7",   L[2], TEST_FXN(27));
  fail += check("ISUB  20-7",   L[3], TEST_FXN(13));
  fail += check("IMUL  20*7",   L[4], TEST_FXN(140));
  fail += check("IDIV  20/7",   L[5], TEST_FXN(2));
  fail += check("IREM  20%7",   L[6], TEST_FXN(6));

  /* Negative-number coverage for the sign-sensitive ops
   * (IMUL via sar; IDIV/IREM via cqo + idiv). */
  L[0] = TEST_FXN(-20);
  L[1] = TEST_FXN(7);
  fn(L);
  fail += check("IADD -20+7",   L[2], TEST_FXN(-13));
  fail += check("ISUB -20-7",   L[3], TEST_FXN(-27));
  fail += check("IMUL -20*7",   L[4], TEST_FXN(-140));
  fail += check("IDIV -20/7",   L[5], TEST_FXN(-2));
  fail += check("IREM -20%7",   L[6], TEST_FXN(-6));

  /* Slot index large enough to force disp32 encoding (>15). */
  jit_buf *bb = jit_buf_new(256);
  jit_emit_iadd(bb, 32, 0, 1);   /* L[32] = L[0] + L[1] */
  jit_emit_ret(bb);
  void (*fn2)(int64_t*) = (void(*)(int64_t*))jit_buf_finalize(bb);

  int64_t L2[64] = {0};
  L2[0] = TEST_FXN(100);
  L2[1] = TEST_FXN(50);
  fn2(L2);
  fail += check("IADD disp32 L[32]", L2[32], TEST_FXN(150));

  jit_buf_free(b);
  jit_buf_free(bb);
  return fail;
}

int main(void) {
  int fail = 0;
  printf("=== step 0: hand-coded add ===\n");
  fail += step0_add();
  printf("\n=== step 1: typed-int arith emitters ===\n");
  fail += step1_arith();
  if (fail) {
    fprintf(stderr, "\n%d check(s) failed\n", fail);
    return 1;
  }
  printf("\nJIT self-test: ALL PASS.\n");
  return 0;
}

#endif
