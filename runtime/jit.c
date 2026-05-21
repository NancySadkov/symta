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
  if (b->relocs) free(b->relocs);
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

/* Locals-pointer register.  We use RBX (callee-saved on both
 * Win64 and SysV) so the pointer survives across inner C calls
 * without explicit save/restore.  The prologue moves the
 * platform's arg0 (RCX on Win64, RDI on SysV) into RBX once;
 * every memory emitter addresses [rbx + slot*8] thereafter.
 * RBX's r/m field is 011 (3). */
#define LOCALS_RM 3

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

/* ============================================================
 * Phase 2a register allocation primitives.
 *
 * The slot-access primitives below (emit_mov_rax_from_slot, etc.)
 * all consult b->pinned[] via `jit_slot_reg(b, slot)`.  If the
 * requested slot is mapped to a callee-saved register (R13, R14,
 * or R15), the primitive emits a register-to-register move via
 * the `_from_reg` / `_from_reg` helpers below instead of the
 * memory load/store.  Helper callers (jit_emit_call_helper3 et
 * al.) wrap their inner CALL with `emit_spill_pinned` /
 * `emit_reload_pinned` so the L[] memory image is consistent
 * with the in-register copies across the helper boundary.
 *
 * Invariant: between any two opcode emissions (and on entry to
 * every helper call after the wrapper's spill), R<reg> holds
 * the current value of L[slot] for each (slot, reg) in
 * b->pinned[].
 * ============================================================ */

/* Returns the x86 register number (13..15) that the given SBC
 * slot is pinned to, or -1 if the slot lives in memory. */
static int jit_slot_reg(jit_buf *b, int slot) {
  for (int i = 0; i < b->pinned_count; i++) {
    if (b->pinned[i].slot == slot) return (int)b->pinned[i].reg;
  }
  return -1;
}

/* All register-to-register helpers below assume `reg` is 13, 14,
 * or 15.  Encoding builds on REX.B / REX.R prefixes to extend
 * the 3-bit ModR/M.reg or ModR/M.r/m field to 4 bits. */

/* mov rax, R<reg>   --   4C 89 (C0 | ((reg-8)<<3))   3 bytes */
static void emit_mov_rax_from_reg(jit_buf *b, int reg) {
  jit_emit_u8(b, 0x4c);
  jit_emit_u8(b, 0x89);
  jit_emit_u8(b, (uint8_t)(0xc0 | ((reg - 8) << 3)));
}

/* mov R<reg>, rax   --   49 89 (C0 | (reg-8))        3 bytes */
static void emit_mov_reg_from_rax(jit_buf *b, int reg) {
  jit_emit_u8(b, 0x49);
  jit_emit_u8(b, 0x89);
  jit_emit_u8(b, (uint8_t)(0xc0 | (reg - 8)));
}

/* mov R<reg>, rdx   --   49 89 (D0 | (reg-8))        3 bytes */
static void emit_mov_reg_from_rdx(jit_buf *b, int reg) {
  jit_emit_u8(b, 0x49);
  jit_emit_u8(b, 0x89);
  jit_emit_u8(b, (uint8_t)(0xd0 | (reg - 8)));
}

/* mov rdx, R<reg>   --   4C 89 (C2 | ((reg-8)<<3))   3 bytes */
static void emit_mov_rdx_from_reg(jit_buf *b, int reg) {
  jit_emit_u8(b, 0x4c);
  jit_emit_u8(b, 0x89);
  jit_emit_u8(b, (uint8_t)(0xc2 | ((reg - 8) << 3)));
}

/* add rax, R<reg>   --   49 03 (C0 | (reg-8))        3 bytes */
static void emit_add_rax_from_reg(jit_buf *b, int reg) {
  jit_emit_u8(b, 0x49);
  jit_emit_u8(b, 0x03);
  jit_emit_u8(b, (uint8_t)(0xc0 | (reg - 8)));
}

/* sub rax, R<reg>   --   49 2B (C0 | (reg-8))        3 bytes */
static void emit_sub_rax_from_reg(jit_buf *b, int reg) {
  jit_emit_u8(b, 0x49);
  jit_emit_u8(b, 0x2b);
  jit_emit_u8(b, (uint8_t)(0xc0 | (reg - 8)));
}

/* or  rax, R<reg>   --   49 0B (C0 | (reg-8))        3 bytes */
static void emit_or_rax_from_reg(jit_buf *b, int reg) {
  jit_emit_u8(b, 0x49);
  jit_emit_u8(b, 0x0b);
  jit_emit_u8(b, (uint8_t)(0xc0 | (reg - 8)));
}

/* cmp rax, R<reg>   --   49 3B (C0 | (reg-8))        3 bytes */
static void emit_cmp_rax_from_reg(jit_buf *b, int reg) {
  jit_emit_u8(b, 0x49);
  jit_emit_u8(b, 0x3b);
  jit_emit_u8(b, (uint8_t)(0xc0 | (reg - 8)));
}

/* imul rax, R<reg>  --   49 0F AF (C0 | (reg-8))    4 bytes */
static void emit_imul_rax_from_reg(jit_buf *b, int reg) {
  jit_emit_u8(b, 0x49);
  jit_emit_u8(b, 0x0f);
  jit_emit_u8(b, 0xaf);
  jit_emit_u8(b, (uint8_t)(0xc0 | (reg - 8)));
}

/* idiv R<reg>       --   49 F7 (F8 | (reg-8))        3 bytes
 *   ModR/M: mod=11, reg=/7 (idiv), r/m=R<reg>[0:2]. */
static void emit_idiv_from_reg(jit_buf *b, int reg) {
  jit_emit_u8(b, 0x49);
  jit_emit_u8(b, 0xf7);
  jit_emit_u8(b, (uint8_t)(0xf8 | (reg - 8)));
}

/* cmp R<reg>, 0     --   49 83 (F8 | (reg-8)) 00    4 bytes
 *   ModR/M: mod=11, reg=/7 (cmp imm), r/m=R<reg>[0:2]. */
static void emit_cmp_reg_zero(jit_buf *b, int reg) {
  jit_emit_u8(b, 0x49);
  jit_emit_u8(b, 0x83);
  jit_emit_u8(b, (uint8_t)(0xf8 | (reg - 8)));
  jit_emit_u8(b, 0x00);
}

/* Spill every pinned register back to its L[slot] memory cell.
 * Called immediately before a helper invocation so the helper
 * reads the up-to-date value of every pinned slot when it
 * dereferences L[slot] directly.
 *
 * 7 bytes per pinned slot (disp8 form):
 *   REX.B + opcode 89 + ModR/M(SIB?) + disp
 *   `mov [rbx + slot*8], R<reg>` -- since reg is R13..R15 we
 *   need REX.R=1 and the SIB-less form for low slot indices.
 * Larger disp32 form is 10 bytes per slot. */
static void emit_spill_pinned(jit_buf *b) {
  for (int i = 0; i < b->pinned_count; i++) {
    int reg = b->pinned[i].reg;
    int slot = b->pinned[i].slot;
    /* mov [rbx + slot*8], R<reg>:
     *   REX = 0x4C (W=1, R=1 for R13..R15 as the reg field)
     *   opcode = 0x89 (mov r/m64, r64)
     *   ModR/M: mod=01/10, reg=R<reg>[0:2], r/m=011(RBX)
     */
    int32_t disp = (int32_t)slot * 8;
    jit_emit_u8(b, 0x4c);
    jit_emit_u8(b, 0x89);
    uint8_t reg_field = (uint8_t)((reg - 8) & 7);
    if (disp >= -128 && disp <= 127) {
      jit_emit_u8(b, (uint8_t)(0x40 | (reg_field << 3) | 3));  /* mod=01 r/m=011 */
      jit_emit_u8(b, (uint8_t)(int8_t)disp);
    } else {
      jit_emit_u8(b, (uint8_t)(0x80 | (reg_field << 3) | 3));  /* mod=10 r/m=011 */
      jit_emit_u32(b, (uint32_t)disp);
    }
  }
}

/* Mirror of emit_spill_pinned: reload each pinned register from
 * its L[slot] memory cell.  Called immediately after a helper
 * returns so any L[] updates the helper made (including GC moves
 * of heap refs in pinned slots) are reflected in the registers.
 *
 * 7 bytes per pinned slot (disp8): same shape as spill but
 * opcode 0x8B (mov r64, r/m64). */
static void emit_reload_pinned(jit_buf *b) {
  for (int i = 0; i < b->pinned_count; i++) {
    int reg = b->pinned[i].reg;
    int slot = b->pinned[i].slot;
    int32_t disp = (int32_t)slot * 8;
    jit_emit_u8(b, 0x4c);
    jit_emit_u8(b, 0x8b);
    uint8_t reg_field = (uint8_t)((reg - 8) & 7);
    if (disp >= -128 && disp <= 127) {
      jit_emit_u8(b, (uint8_t)(0x40 | (reg_field << 3) | 3));
      jit_emit_u8(b, (uint8_t)(int8_t)disp);
    } else {
      jit_emit_u8(b, (uint8_t)(0x80 | (reg_field << 3) | 3));
      jit_emit_u32(b, (uint32_t)disp);
    }
  }
}

/* mov rax, [locals_reg + slot*8]   (or "mov rax, R<reg>" if pinned) */
static void emit_mov_rax_from_slot(jit_buf *b, int slot) {
  int r = jit_slot_reg(b, slot);
  if (r >= 0) { emit_mov_rax_from_reg(b, r); return; }
  jit_emit_u8(b, 0x48);  /* REX.W */
  jit_emit_u8(b, 0x8b);  /* opcode: mov r64, r/m64 */
  emit_mem_op(b, 0, slot);  /* reg=RAX (0) */
}

/* mov [locals_reg + slot*8], rax   (or "mov R<reg>, rax" if pinned) */
static void emit_mov_slot_from_rax(jit_buf *b, int slot) {
  int r = jit_slot_reg(b, slot);
  if (r >= 0) { emit_mov_reg_from_rax(b, r); return; }
  jit_emit_u8(b, 0x48);  /* REX.W */
  jit_emit_u8(b, 0x89);  /* opcode: mov r/m64, r64 */
  emit_mem_op(b, 0, slot);  /* reg=RAX (0) */
}

/* mov [locals_reg + slot*8], rdx (for IREM remainder)
 * (or "mov R<reg>, rdx" if pinned). */
static void emit_mov_slot_from_rdx(jit_buf *b, int slot) {
  int r = jit_slot_reg(b, slot);
  if (r >= 0) { emit_mov_reg_from_rdx(b, r); return; }
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x89);
  emit_mem_op(b, 2, slot);  /* reg=RDX (2) */
}

/* add rax, [locals_reg + slot*8]   (or "add rax, R<reg>" if pinned) */
static void emit_add_rax_from_slot(jit_buf *b, int slot) {
  int r = jit_slot_reg(b, slot);
  if (r >= 0) { emit_add_rax_from_reg(b, r); return; }
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x03);  /* opcode: add r64, r/m64 */
  emit_mem_op(b, 0, slot);
}

/* or rax, [locals_reg + slot*8] -- combines tag bits across two
 * operands so a single tag check covers both.  Used for the
 * inline fast path of FXN-arith / FXN-cmp: if (aa|bb) has any
 * tag bits set, fall through to the helper.
 * (Or "or rax, R<reg>" if pinned.) */
static void emit_or_rax_from_slot(jit_buf *b, int slot) {
  int r = jit_slot_reg(b, slot);
  if (r >= 0) { emit_or_rax_from_reg(b, r); return; }
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x0b);  /* opcode: or r64, r/m64 */
  emit_mem_op(b, 0, slot);
}

/* test ax, ax -- 3 bytes (66 85 C0).  Sets ZF iff low 16 bits
 * of RAX are zero, i.e. the value is FXN-tagged int (T_INT=0).
 * For any other tag, at least one of the low 15 tag bits is set
 * (FLG_BITS=1, TAG_BITS=16, so the tag lives in bits 1..15). */
static void emit_test_ax_ax(jit_buf *b) {
  jit_emit_u8(b, 0x66);  /* operand-size override -> 16-bit */
  jit_emit_u8(b, 0x85);  /* opcode: test r16, r16 */
  jit_emit_u8(b, 0xc0);  /* ModR/M: AX, AX */
}

/* add rax, imm32 -- sign-extended.  6 bytes (48 05 imm32).
 * Uses the RAX-specific short form. */
static void emit_add_rax_imm32(jit_buf *b, int32_t imm) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x05);
  jit_emit_u32(b, (uint32_t)imm);
}

/* jnz rel32 -- 6 bytes (0F 85 + 4-byte rel).  Returns the
 * offset of the 4-byte displacement field for later patching
 * via jit_patch_jmp_here.  Mirrors jit_emit_jmp's contract. */
static size_t emit_jnz_rel32(jit_buf *b) {
  jit_emit_u8(b, 0x0f);
  jit_emit_u8(b, 0x85);
  size_t patch = b->len;
  jit_emit_u32(b, 0);
  return patch;
}

/* sub rax, [locals_reg + slot*8]   (or "sub rax, R<reg>" if pinned) */
static void emit_sub_rax_from_slot(jit_buf *b, int slot) {
  int r = jit_slot_reg(b, slot);
  if (r >= 0) { emit_sub_rax_from_reg(b, r); return; }
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x2b);  /* opcode: sub r64, r/m64 */
  emit_mem_op(b, 0, slot);
}

/* imul rax, [locals_reg + slot*8] (2-operand form, low 64 bits)
 * (or "imul rax, R<reg>" if pinned). */
static void emit_imul_rax_from_slot(jit_buf *b, int slot) {
  int r = jit_slot_reg(b, slot);
  if (r >= 0) { emit_imul_rax_from_reg(b, r); return; }
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x0f);
  jit_emit_u8(b, 0xaf);  /* opcode: imul r64, r/m64 */
  emit_mem_op(b, 0, slot);
}

/* idiv qword [locals_reg + slot*8] (signed, RDX:RAX / mem -> RAX, RDX)
 * (or "idiv R<reg>" if pinned). */
static void emit_idiv_from_slot(jit_buf *b, int slot) {
  int r = jit_slot_reg(b, slot);
  if (r >= 0) { emit_idiv_from_reg(b, r); return; }
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

/* ============================================================
 * Step 3: prologue / epilogue / C-call trampoline.
 *
 * Stack layout while inside a JIT'd function (the `[]` marks
 * grow-down direction):
 *
 *   [ caller's frame                 ] <- on entry RSP%16==8
 *   [ return address (8 bytes)       ] <- caller's CALL pushed
 *   [ saved RBX (8 bytes)            ] <- prologue: push rbx
 *   [ shadow space (32 bytes)        ] <- prologue: sub rsp, 32
 *                                          (Win64 requires 32
 *                                          home bytes for any
 *                                          inner CALL; SysV
 *                                          ignores them but
 *                                          allocating is harmless)
 *                                       <- RSP%16==0; inner CALL
 *                                          will push 8, callee
 *                                          sees %16==8 as
 *                                          expected.
 *
 * RBX holds the locals pointer the entire time.  Inner C calls
 * preserve RBX by ABI; we never have to save/restore it
 * mid-function.
 * ============================================================ */

/* push R<n> for n in 8..15: 41 50+(n-8).  Used by the prologue
 * extensions when any slots are pinned to R13..R15. */
static void emit_push_r13_15(jit_buf *b, int reg) {
  jit_emit_u8(b, 0x41);  /* REX.B */
  jit_emit_u8(b, (uint8_t)(0x50 | (reg - 8)));
}
static void emit_pop_r13_15(jit_buf *b, int reg) {
  jit_emit_u8(b, 0x41);  /* REX.B */
  jit_emit_u8(b, (uint8_t)(0x58 | (reg - 8)));
}

/* Always reserve all three Phase-2a callee-saved registers when
 * pinning is active so the stack-alignment math stays uniform
 * regardless of how many slots actually get pinned (1, 2, or 3).
 * The unused pushes cost 6 bytes prologue + 6 bytes epilogue --
 * negligible vs the memory-op savings on the hot path. */
#define JIT_PHASE2A_HAS_PINS(b) ((b)->pinned_count > 0)

void jit_emit_prologue(jit_buf *b) {
  /* push rbx -- one byte (PUSH r64 family: 50+reg, RBX=011) */
  jit_emit_u8(b, 0x53);
  if (JIT_PHASE2A_HAS_PINS(b)) {
    /* push r13, r14, r15 -- three more callee-saved slots. */
    emit_push_r13_15(b, 13);
    emit_push_r13_15(b, 14);
    emit_push_r13_15(b, 15);
  }
  /* mov rbx, <arg0>  --  rbx is the new home of the locals ptr */
#ifdef _WIN32
  /* mov rbx, rcx :  48 89 cb (ModR/M 11 001 011, r=rcx, r/m=rbx) */
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x89);
  jit_emit_u8(b, 0xcb);
#else
  /* mov rbx, rdi :  48 89 fb (ModR/M 11 111 011, r=rdi, r/m=rbx) */
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x89);
  jit_emit_u8(b, 0xfb);
#endif
  if (JIT_PHASE2A_HAS_PINS(b)) {
    /* Initial-load: each pinned register snapshots its slot's
     * current memory value.  Same encoding as emit_reload_pinned. */
    emit_reload_pinned(b);
  }
  /* sub rsp, K -- Win64 shadow space + alignment marker, harmless
   * on SysV.  Stack-alignment math (entry RSP%16 == 8):
   *   no pins:  push rbx (-8 -> %16=0)            +  sub 32  -> %16=0 ✓
   *   3 pins:   push rbx + 3 more (-32 -> %16=8)  +  sub 40  -> %16=0 ✓
   */
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x83);
  jit_emit_u8(b, 0xec);
  jit_emit_u8(b, JIT_PHASE2A_HAS_PINS(b) ? 0x28 : 0x20);
}

void jit_emit_epilogue(jit_buf *b) {
  if (JIT_PHASE2A_HAS_PINS(b)) {
    /* Final-store: each pinned register's current value back to
     * its L[slot] memory cell so the caller observes the right
     * state. */
    emit_spill_pinned(b);
  }
  /* add rsp, K -- mirrors prologue's sub rsp choice. */
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x83);
  jit_emit_u8(b, 0xc4);
  jit_emit_u8(b, JIT_PHASE2A_HAS_PINS(b) ? 0x28 : 0x20);
  if (JIT_PHASE2A_HAS_PINS(b)) {
    emit_pop_r13_15(b, 15);
    emit_pop_r13_15(b, 14);
    emit_pop_r13_15(b, 13);
  }
  /* pop rbx :  5b */
  jit_emit_u8(b, 0x5b);
  /* ret :  c3 */
  jit_emit_u8(b, 0xc3);
}

void jit_emit_mov_arg0_from_locals(jit_buf *b) {
#ifdef _WIN32
  /* mov rcx, rbx :  48 89 d9 (ModR/M 11 011 001) */
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x89);
  jit_emit_u8(b, 0xd9);
#else
  /* mov rdi, rbx :  48 89 df (ModR/M 11 011 111) */
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x89);
  jit_emit_u8(b, 0xdf);
#endif
}

/* ============================================================
 * Step 4: SBC opcode constants (subset).
 *
 * Locally `#define`d rather than `#include`d from runtime/sif.h
 * so the self-test build stays independent of the rest of the
 * runtime headers.  These numeric values are the on-disk format
 * and shouldn't change; if they ever do, the divergence will
 * surface as a self-test failure here.
 * ============================================================ */
#define BC_NOP    0x00
#define BC_LEAVE  0x02
#define BC_LEAVE0 0x03
#define BC_CNAS   0x14    /* function-prologue nargs check */
#define BC_MOVE    0x19    /* dst=u16 src=u16; L[dst]=L[src] */
#define BC_MOVE8   0x1A    /* dst=u8  src=u8;  L[dst]=L[src] */
#define BC_MOVEEMT 0x1B    /* dst=u16; L[dst]=Empty (the empty-list singleton) */
#define BC_MOVENO  0x1C    /* dst=u16; L[dst]=No (a fixed immediate) */
#define BC_FATAL   0x5C    /* msg=u16; longjmp via fatal((char*)L[msg]) */
#define BC_FXNLGET 0x34    /* dst=u16 src=u16 index=u16 mcache=u16 */
#define BC_MOVEIM  0x21    /* dst=u16 src=u24; L[dst]=sbc->im[src] */
#define BC_MOVEMT  0x1F    /* dst=u16 src=u24; L[dst]=FXN(sbc->mt[src]) */
#define BC_MOVEMT8 0x20    /* dst=u8  src=u8;  L[dst]=FXN(sbc->mt[src]) */
#define BC_IMMEQ   0x3E    /* dst,a,b=u16; mcache=u16; L[dst]=FXN(a==b)  */
#define BC_IMMNE   0x3F    /* dst,a,b=u16; mcache=u16; L[dst]=FXN(a!=b)  */
#define BC_FXNLISTN  0x32  /* dst=u16 src=u16; L[dst]=LIST(UNFXN(L[src])) */
#define BC_FXNLSETIR 0x36  /* src,index,val=u16 + mcache=u16; LSET ignored */
#define BC_ABS     0x33    /* dst,a=u16; abs(L[a]) */
#define BC_NEG     0x38    /* dst,a=u16; -L[a] */
#define BC_FXNAND  0x44    /* dst,a,b=u16; L[a] & L[b] */
#define BC_FXNIOR  0x45
#define BC_FXNXOR  0x46
#define BC_FXNSHL  0x47
#define BC_FXNSHR  0x48
#define BC_FXNLSET 0x35    /* dst,src,index,val=u16 + mcache=u16 */
#define BC_OBJECT  0x11    /* dst=u16 tid=u16 size=u16 */
#define BC_ARGLIST  0x15   /* size=u16 + size*u16 src indices */
#define BC_ARGLIST8 0x16   /* size=u8  + size*u8  src indices */
#define BC_DMET    0x58    /* tyidx=u16 mtidx=u24 handler=u16 */
#define BC_TINIT   0x55    /* type=u16 size=u16 name=u24 */
#define BC_SUBTYPE 0x57    /* super=u16 sub=u16 */
#define BC_MNAME   0x5B    /* dst=u16 src=u16 */
#define BC_TINITI  0x56    /* tag=u16 size=u16 name=u64 */
#define BC_CURMET  0x5A    /* dst=u16 */
#define BC_FADD    0xAE
#define BC_FSUB    0xAF
#define BC_FMUL    0xB0
#define BC_FDIV    0xB1
#define BC_INC     0x9E    /* dst=u16 a=u16; INC(L[dst], L[a]) */
#define BC_DEC     0x9F    /* dst=u16 a=u16; DEC(L[dst], L[a]) */
#define BC_LOAD   0x24    /* dst=u16 src=u16 index=u16; L[dst]=O_PTR(L[src])[index] */
#define BC_LOAD8  0x25    /* dst=u8 src=u8 index=u8; same body */
#define BC_MOVE4  0x97    /* opr=u8; dst=opr&0xF src=opr>>4; L[dst]=L[src] */
#define BC_CLOSURE 0x10   /* dst=u16 idx=u16 size=u8; L[dst]=CLOSURE(sbc->hooks[idx],size) */
#define BC_LIST    0x12   /* dst=u16 size=u16; L[dst]=LIST(size) */
#define BC_COPY    0x26   /* dst=u16 src=u16 dindex=u16 sindex=u16; COPY */
#define BC_ARGLIST0 0x8A  /* opcode only; ARGLIST(0) */
#define BC_ARGLIST1 0x8B  /* opcode + a(u8); ARGLIST(1) + STARG(0,L[a]) */
#define BC_ARGLIST2 0x8C  /* opcode + a(u8) + b(u8) */
#define BC_ARGLIST3 0x8D  /* opcode + a + b + c (u8 each) */
#define BC_CALL     0x09  /* opcode + dst(u16) + fn(u16) */
#define BC_CALLIR   0x0A  /* opcode + fn(u16); ignore return value */
#define BC_CALLT    0x0B  /* opcode + dst(u16) + fn(u16); CALL_TAGGED */
#define BC_CALLTIR  0x0C  /* opcode + fn(u16); CALL_TAGGED ignore return */
#define BC_MCALL    0x0D  /* opcode + dst(u16) + obj(u16) + met(u16) + mcache_idx(u16) */
#define BC_MCALLIR  0x0E  /* opcode + obj(u16) + met(u16) + mcache_idx(u16) */
#define BC_MCALL8   0x0F  /* opcode + dst(u8) + obj(u8) + met(u8) + mcache_idx(u16) */
#define BC_IFFXN    0x13  /* opcode + cnd(u8) + diff(int16); branch if O_TAG(L[cnd])==0 */
#define BC_MOVETX   0x1D  /* opcode + dst(u16) + src(u24); L[dst] = sbc->tx[src] */
#define BC_MOVETX8  0x1E  /* opcode + dst(u8)  + src(u8);  L[dst] = sbc->tx[src] */
#define BC_FXT8     0x90  /* opcode + dst(u8) + imm(u8);  L[dst] = FIXTEXT(imm) */
#define BC_FXT16    0x91  /* opcode + dst(u8) + imm(u16) */
#define BC_FXT24    0x92  /* opcode + dst(u8) + imm(u24) */
#define BC_FXT32    0x93  /* opcode + dst(u8) + imm(u32) */
#define BC_FXT40    0x94  /* opcode + dst(u8) + imm(u40) */
#define BC_FXT48    0x95  /* opcode + dst(u8) + imm(u48) */
#define BC_FXT56    0x96  /* opcode + dst(u8) + imm(u56) */
#define BC_ARGLIST4 0x8E  /* opcode + a..d (u8 each) */
#define BC_ARGLIST5 0x8F  /* opcode + a..e (u8 each) */
#define BC_JMP    0x04
#define BC_JMP16  0x05    /* opcode + int16 PC-relative diff */
#define BC_B      0x06
#define BC_B8     0x07    /* opcode + uint8 cnd + int16 PC-relative diff */
#define BC_FXNB0  0x27    /* dst=RD8; L[dst]=0 */
#define BC_FXNB8  0x28    /* dst=RD8; L[dst]=FXN(int8) */
#define BC_FXNB16 0x29    /* dst=RD8; L[dst]=FXN(int16) */
#define BC_FXNB32 0x2A    /* dst=RD8; L[dst]=FXN(int32) */
#define BC_FXN0   0x2B    /* dst=RD16; L[dst]=0 */
#define BC_FXN8   0x2C    /* dst=RD16; L[dst]=FXN(int8) */
#define BC_FXN16  0x2D    /* dst=RD16; L[dst]=FXN(int16) */
#define BC_FXN32  0x2E    /* dst=RD16; L[dst]=FXN(int32) */
/* Untyped (dyn) tagged-arith family -- bit-for-bit math on
 * tagged values.  These were the pre-TS-4 default; the JIT
 * dispatches them via the C-runtime trampoline because the
 * inline encoding without static type info is non-trivial
 * (FXNMUL needs a runtime untag depending on operand shape;
 * cleanest to defer to the existing macros). */
#define BC_FXNADD 0x39
#define BC_FXNSUB 0x3A
#define BC_FXNMUL 0x3B
#define BC_FXNDIV 0x3C
#define BC_FXNREM 0x3D
#define BC_FXNLT  0x40
#define BC_FXNGT  0x41
#define BC_FXNLTE 0x42
#define BC_FXNGTE 0x43
#define BC_FXNTAG 0x31
#define BC_NOT    0x9B
#define BC_GOT    0x9C
#define BC_NO     0x9D
#define BC_IADD   0xA3
#define BC_ISUB   0xA4
#define BC_IMUL   0xA5
#define BC_IDIV   0xA6
#define BC_IREM   0xA7
#define BC_ILT    0xAA
#define BC_IGT    0xAB
#define BC_ILTE   0xAC
#define BC_IGTE   0xAD
#define BC_SAME   0xA8
#define BC_VARY   0xA9
#define BC_LIST1  0xA2
#define BC_LIST2  0xA1
#define BC_FXNSIZE 0x37

/* Little-endian uint16_t read.  SBC operands are encoded LE; the
 * RD16 macro in sbc.c does the same thing inline. */
static uint16_t bc_rd16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* Zero RAX -- emitted as the return value for SBC_LEAVE0.
 * Encoding: xor eax, eax (0x31 0xc0).  Zeroes the full RAX
 * because writes to 32-bit subregs implicitly clear the upper
 * 32 bits.  Shorter than `mov rax, 0`. */
static void emit_xor_rax_rax(jit_buf *b) {
  jit_emit_u8(b, 0x31);
  jit_emit_u8(b, 0xc0);
}

/* mov rax, imm64 -- 10 bytes (REX.W + opcode b8+rax + 8-byte imm).
 * Always full 64-bit form; the cost is uniform regardless of
 * the immediate's magnitude.  Smaller immediates could use
 * `mov rax, imm32` (sign-extended, 7 bytes) but the savings
 * aren't worth a special path at this stage. */
static void emit_mov_rax_imm64(jit_buf *b, uint64_t imm) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0xb8);
  for (int k = 0; k < 8; k++) jit_emit_u8(b, (uint8_t)(imm >> (k * 8)));
}

/* ============================================================
 * Step 5c/5l runtime helpers (called from JIT'd code).
 *
 * Each FXN* opcode's full semantics is a 3-way dispatch:
 *   (a is int, b is int)  -> bit-arithmetic via the FXN* macro
 *   (a is int, b is float)-> convert to float, compute
 *   (a is not int)        -> ARGLIST2 + MCALL via m_<op>
 *
 * The runtime side (jit_sbc.c) installs full impls that cover
 * all three paths.  The fallback impls below cover only the
 * int-int case, sufficient for the standalone self-test that
 * doesn't link jit_sbc.c.  Production code MUST go through
 * sbc_jit_audit or sbc_jit_install first so the full helpers
 * are installed before any JIT'd code calls them. */
static void fallback_fxnadd(int64_t *L, int dst, int a, int b) {
  L[dst] = L[a] + L[b];
}
static void fallback_fxnsub(int64_t *L, int dst, int a, int b) {
  L[dst] = L[a] - L[b];
}
static void fallback_fxnmul(int64_t *L, int dst, int a, int b) {
  L[dst] = (L[a] >> 16) * L[b];
}
static void fallback_fxndiv(int64_t *L, int dst, int a, int b) {
  L[dst] = (L[a] / L[b]) << 16;
}
static void fallback_fxnrem(int64_t *L, int dst, int a, int b) {
  L[dst] = L[a] % L[b];
}

void (*jit_rt_fxnadd_helper)(int64_t*, int, int, int) = fallback_fxnadd;
void (*jit_rt_fxnsub_helper)(int64_t*, int, int, int) = fallback_fxnsub;
void (*jit_rt_fxnmul_helper)(int64_t*, int, int, int) = fallback_fxnmul;
void (*jit_rt_fxndiv_helper)(int64_t*, int, int, int) = fallback_fxndiv;
void (*jit_rt_fxnrem_helper)(int64_t*, int, int, int) = fallback_fxnrem;

/* L[dst] = FXN(imm).  Tagged-int store via mov rax, imm64; mov [rbx+dst*8], rax.
 * The caller passes the un-tagged signed value; we shift left
 * by TAG_BITS (16) here.  Handles negative imm correctly via
 * arithmetic shift semantics (cast to int64_t before shifting). */
static void emit_fxn_imm(jit_buf *b, int dst, int64_t imm) {
  uint64_t tagged = (uint64_t)((int64_t)imm << 16);
  emit_mov_rax_imm64(b, tagged);
  emit_mov_slot_from_rax(b, dst);
}

/* Read a 24-bit little-endian unsigned int from bc[off..off+2]. */
static uint32_t bc_rd24(const uint8_t *p) {
  return (uint32_t)p[0]
       | ((uint32_t)p[1] << 8)
       | ((uint32_t)p[2] << 16);
}

#define JIT_MAX_PATCHES 256

typedef struct {
  size_t jit_off;     /* offset of the 4-byte disp32 placeholder */
  size_t bc_target;   /* target byte offset within the bc buffer */
} jit_patch;

/* Diagnostics: set whenever jit_translate aborts.  Read by the
 * audit code in jit_sbc.c to tally which opcodes are blocking
 * coverage of real SBC files. */
uint8_t jit_last_fail_opcode = 0;
size_t  jit_last_fail_offset = 0;

/* Step 12e: runtime address of `api_g.heap0` (the FIELD address,
 * not its value).  Set by jit_install_helpers_once on first use.
 * The inline LD4 emitter bakes this into the JIT'd code as imm64;
 * the runtime then dereferences it to read the heap base pointer.
 * Stays NULL in the standalone JIT_SELF_TEST build (the LD4 cases
 * fall back to the helper-call path or to "bail out" if neither
 * is installed). */
void *jit_rt_heap0_addr = NULL;

/* Trampoline helpers set by the runtime side.  See jit.h. */
void (*jit_rt_ld4_helper)(int64_t *L, int dst, int src, int index) = NULL;
void (*jit_rt_st4_helper)(int64_t *L, int dst, int src, int index) = NULL;
void (*jit_rt_list_helper)(int64_t *L, int dst, int size, int unused) = NULL;
void (*jit_rt_copy_helper)(int64_t *L, int dst, int src, int packed_indices) = NULL;
void (*jit_rt_arglist0_helper)(int64_t *L, int a, int b, int c) = NULL;
void (*jit_rt_arglist1_helper)(int64_t *L, int a, int b, int c) = NULL;
void (*jit_rt_arglist2_helper)(int64_t *L, int a, int b, int c) = NULL;
void (*jit_rt_arglist3_helper)(int64_t *L, int a, int b, int c) = NULL;
void (*jit_rt_call_helper)   (int64_t *L, int dst, int fn, int u) = NULL;
void (*jit_rt_callir_helper) (int64_t *L, int fn,  int u1, int u2) = NULL;
void (*jit_rt_callt_helper)  (int64_t *L, int dst, int fn, int u) = NULL;
void (*jit_rt_calltir_helper)(int64_t *L, int fn,  int u1, int u2) = NULL;
void (*jit_rt_mcall_helper)  (int64_t *L, struct sbc_t *sbc, uint64_t packed) = NULL;
void (*jit_rt_mcallir_helper)(int64_t *L, struct sbc_t *sbc, uint64_t packed) = NULL;
void (*jit_rt_arglist4_helper)(int64_t *L, int packed, int u1, int u2) = NULL;
void (*jit_rt_arglist5_helper)(int64_t *L, int packed, int u1, int u2) = NULL;
void (*jit_rt_movetx_helper) (int64_t *L, struct sbc_t *sbc, uint64_t packed) = NULL;
void (*jit_rt_closure_helper)(int64_t *L, struct sbc_t *sbc,
                              uint64_t packed) = NULL;
void (*jit_rt_list1_helper)(int64_t *L, int dst, int x, int unused) = NULL;
void (*jit_rt_list2_helper)(int64_t *L, int dst, int a, int b) = NULL;
void (*jit_rt_fxnsize_helper)(int64_t *L, int dst, int src, int unused) = NULL;
void (*jit_rt_moveemt_helper)(int64_t *L, int dst, int u1, int u2) = NULL;
void (*jit_rt_fatal_helper)  (int64_t *L, int msg, int u1, int u2) = NULL;
void (*jit_rt_fxnlget_helper)(int64_t *L, struct sbc_t *sbc,
                              uint64_t packed) = NULL;
void (*jit_rt_fxnlt_helper) (int64_t *L, int dst, int a, int b) = NULL;
void (*jit_rt_fxngt_helper) (int64_t *L, int dst, int a, int b) = NULL;
void (*jit_rt_fxnlte_helper)(int64_t *L, int dst, int a, int b) = NULL;
void (*jit_rt_fxngte_helper)(int64_t *L, int dst, int a, int b) = NULL;
void (*jit_rt_fxntag_helper)(int64_t *L, int dst, int src, int u) = NULL;
void (*jit_rt_not_helper)  (int64_t *L, int dst, int src, int u) = NULL;
void (*jit_rt_got_helper)  (int64_t *L, int dst, int src, int u) = NULL;
void (*jit_rt_no_helper)   (int64_t *L, int dst, int src, int u) = NULL;
void (*jit_rt_moveim_helper)(int64_t *L, struct sbc_t *sbc,
                             uint64_t packed) = NULL;
void (*jit_rt_inc_helper)(int64_t *L, int dst, int a, int u) = NULL;
void (*jit_rt_dec_helper)(int64_t *L, int dst, int a, int u) = NULL;
void (*jit_rt_movemt_helper)(int64_t *L, struct sbc_t *sbc,
                             uint64_t packed) = NULL;
void (*jit_rt_immeq_helper)(int64_t *L, struct sbc_t *sbc,
                            uint64_t packed) = NULL;
void (*jit_rt_immne_helper)(int64_t *L, struct sbc_t *sbc,
                            uint64_t packed) = NULL;
void (*jit_rt_fxnlistn_helper)(int64_t *L, int dst, int src, int u) = NULL;
void (*jit_rt_fxnlsetir_helper)(int64_t *L, struct sbc_t *sbc,
                                uint64_t packed) = NULL;
void (*jit_rt_neg_helper)(int64_t *L, int dst, int a, int u) = NULL;
void (*jit_rt_abs_helper)(int64_t *L, int dst, int a, int u) = NULL;
void (*jit_rt_fxnand_helper)(int64_t *L, int dst, int a, int b) = NULL;
void (*jit_rt_fxnior_helper)(int64_t *L, int dst, int a, int b) = NULL;
void (*jit_rt_fxnxor_helper)(int64_t *L, int dst, int a, int b) = NULL;
void (*jit_rt_fxnshl_helper)(int64_t *L, int dst, int a, int b) = NULL;
void (*jit_rt_fxnshr_helper)(int64_t *L, int dst, int a, int b) = NULL;
void (*jit_rt_fxnlset_helper)(int64_t *L, struct sbc_t *sbc,
                              uint64_t packed1, uint64_t packed2) = NULL;
void (*jit_rt_object_helper)(int64_t *L, struct sbc_t *sbc,
                             uint64_t packed) = NULL;
void (*jit_rt_arglist_n_helper)(int64_t *L, struct sbc_t *sbc,
                                uint64_t packed1, uint64_t packed2) = NULL;
void (*jit_rt_arglist8_n_helper)(int64_t *L, struct sbc_t *sbc,
                                 uint64_t packed1, uint64_t packed2) = NULL;
void (*jit_rt_dmet_helper)(int64_t *L, struct sbc_t *sbc,
                           uint64_t packed) = NULL;
void (*jit_rt_tiniti_helper)(int64_t *L, struct sbc_t *sbc,
                             uint64_t packed1, uint64_t packed2) = NULL;
void (*jit_rt_curmet_helper)(int64_t *L, int dst, int u1, int u2) = NULL;
void (*jit_rt_fadd_helper)(int64_t *L, int dst, int a, int b) = NULL;
void (*jit_rt_fsub_helper)(int64_t *L, int dst, int a, int b) = NULL;
void (*jit_rt_fmul_helper)(int64_t *L, int dst, int a, int b) = NULL;
void (*jit_rt_fdiv_helper)(int64_t *L, int dst, int a, int b) = NULL;
void (*jit_rt_tinit_helper)(int64_t *L, struct sbc_t *sbc,
                            uint64_t packed) = NULL;
void (*jit_rt_subtype_helper)(int64_t *L, struct sbc_t *sbc,
                              uint64_t packed) = NULL;
void (*jit_rt_mname_helper)(int64_t *L, int dst, int src, int u) = NULL;

/* Step 8: platform-aware default for the AOT pipeline.  Windows
 * has SEH unwind registered (step 5n) so longjmp through native
 * frames works.  POSIX still needs DWARF .eh_frame -- defaulting
 * to OFF there until that lands. */
#ifdef _WIN32
int jit_aot_enabled = 1;
#else
int jit_aot_enabled = 0;
#endif

static jit_buf *jit_translate_core(const uint8_t *bc, size_t n,
                                   int have_sbc, int record_relocs);

jit_buf *jit_translate(const uint8_t *bc, size_t n) {
  return jit_translate_core(bc, n, 0, 0);
}
jit_buf *jit_translate_with_sbc(const uint8_t *bc, size_t n) {
  return jit_translate_core(bc, n, 1, 0);
}
jit_buf *jit_translate_with_sbc_record(const uint8_t *bc, size_t n) {
  /* Step 6b: AOT path.  Same code shape as the runtime translator
   * but turns on the reloc recorder so the writer can persist the
   * helper-pointer call sites.  The returned buffer is owned by
   * the caller; sif2sbc reads code+relocs out and then jit_buf_free's. */
  return jit_translate_core(bc, n, 1, 1);
}

/* ============================================================
 * Phase 2a pre-scan: select the slots to pin to R13..R15.
 *
 * Walks the bytecode opcode-by-opcode using a length table that
 * mirrors the translator's `i += N` increments.  For each opcode
 * whose operands are slot indices, increments a per-slot
 * counter.  After the walk, picks the top JIT_MAX_PINNED slots
 * by count and writes them into b->pinned[].
 *
 * Bailing on unknown opcodes is critical for correctness:
 * mis-skipping an opcode would shift the walk out of phase and
 * cause us to read random bytes as slot indices.  Those bogus
 * slot indices might be valid-looking (small ints) and we'd
 * pin a slot the function never actually uses -- which then
 * gets garbage-read in the prologue and garbage-written in the
 * epilogue, corrupting the C stack.  Conservative bail-out
 * keeps the system safe.
 *
 * Disable with SYMTA_NO_REGALLOC. */
static void jit_select_pinned_slots(jit_buf *b, const uint8_t *bc, size_t n) {
  b->pinned_count = 0;
  for (int i = 0; i < JIT_MAX_PINNED; i++) {
    b->pinned[i].slot = -1;
    b->pinned[i].reg = 0;
  }
  if (getenv("SYMTA_NO_REGALLOC")) return;

  enum { JIT_SCAN_MAX_SLOT = 4096 };
  uint32_t counts[JIT_SCAN_MAX_SLOT];
  memset(counts, 0, sizeof(counts));

#define BUMP(slot_expr) do { \
  uint32_t _s = (uint32_t)(slot_expr); \
  if (_s < JIT_SCAN_MAX_SLOT && counts[_s] != UINT32_MAX) counts[_s]++; \
} while (0)

  /* Opcode-length-aware walk.  Lengths cribbed from each `case
   * BC_*: { ...; i += N; break; }` in the translator's switch.
   * For opcodes that don't appear in the translator (i.e. we
   * couldn't translate them) we bail out and pin nothing. */
  size_t i = 0;
  while (i < n) {
    uint8_t op = bc[i];
    switch (op) {
    case BC_NOP:                       i += 1; break;
    case BC_LEAVE:                     i += 1; break;
    case BC_LEAVE0:                    i += 1; break;
    case BC_CNAS:                      i += 3; break;
    case BC_IFFXN:                     i += 4; break;
    case BC_JMP:                       i += 4; break;
    case BC_FATAL:                     i += 3; break;

    /* 5-byte slot pairs: opcode + dst(u16) + src(u16) */
    case BC_MOVE:
    case BC_INC: case BC_DEC: {
      if (i + 5 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      BUMP(bc_rd16(bc + i + 3));
      i += 5; break;
    }
    /* 3-byte slot pairs: opcode + dst(u8) + src(u8) */
    case BC_MOVE8: case BC_MOVEMT8: {
      if (i + 3 > n) return;
      BUMP(bc[i + 1]);
      BUMP(bc[i + 2]);
      i += 3; break;
    }
    /* 3-byte dst-only u16 */
    case BC_MOVEEMT: case BC_MOVENO: {
      if (i + 3 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      i += 3; break;
    }
    /* 6-byte: dst(u16) + src(u24) -- MOVEIM, MOVEMT */
    case BC_MOVEIM: case BC_MOVEMT: {
      if (i + 6 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      i += 6; break;
    }
    /* 7-byte slot triples: opcode + dst(u16) + a(u16) + b(u16).
     * Covers typed arith / cmp, FXN-arith / cmp, FXN-bitops,
     * OBJECT (dst tid size -- last is size, count anyway). */
    case BC_IADD: case BC_ISUB: case BC_IMUL: case BC_IDIV: case BC_IREM:
    case BC_ILT:  case BC_IGT:  case BC_ILTE: case BC_IGTE:
    case BC_SAME: case BC_VARY:
    case BC_FXNADD: case BC_FXNSUB: case BC_FXNMUL: case BC_FXNDIV: case BC_FXNREM:
    case BC_FXNLT:  case BC_FXNGT:  case BC_FXNLTE: case BC_FXNGTE:
    case BC_FXNAND: case BC_FXNIOR: case BC_FXNXOR:
    case BC_FXNSHL: case BC_FXNSHR: {
      if (i + 7 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      BUMP(bc_rd16(bc + i + 3));
      BUMP(bc_rd16(bc + i + 5));
      i += 7; break;
    }
    case BC_OBJECT: {  /* dst + tid + size; only dst is a slot */
      if (i + 7 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      i += 7; break;
    }
    /* 9-byte: IMMEQ/IMMNE (dst, a, b, mcache), FXNLGET (dst, src,
     * index, mcache), FXNLSETIR (src, index, val, mcache),
     * MCALL (dst, obj, met, mcache). */
    case BC_IMMEQ: case BC_IMMNE: case BC_FXNLGET:
    case BC_FXNLSETIR: case BC_MCALL: {
      if (i + 9 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      BUMP(bc_rd16(bc + i + 3));
      BUMP(bc_rd16(bc + i + 5));
      i += 9; break;
    }
    case BC_FXNLSET: {  /* 11 bytes: dst, src, idx, val, mcache */
      if (i + 11 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      BUMP(bc_rd16(bc + i + 3));
      BUMP(bc_rd16(bc + i + 5));
      BUMP(bc_rd16(bc + i + 7));
      i += 11; break;
    }
    case BC_LIST: case BC_LIST1:
    case BC_FXNLISTN: case BC_FXNSIZE: {  /* 5 bytes: dst, x or src */
      if (i + 5 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      BUMP(bc_rd16(bc + i + 3));
      i += 5; break;
    }
    case BC_LIST2: {  /* 7 bytes: dst, a, b */
      if (i + 7 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      BUMP(bc_rd16(bc + i + 3));
      BUMP(bc_rd16(bc + i + 5));
      i += 7; break;
    }
    case BC_CALL: case BC_CALLT: {  /* 5 bytes: dst, fn */
      if (i + 5 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      BUMP(bc_rd16(bc + i + 3));
      i += 5; break;
    }
    case BC_CALLIR: case BC_CALLTIR: {  /* 3 bytes: fn */
      if (i + 3 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      i += 3; break;
    }
    case BC_MCALLIR: {  /* 7 bytes: obj, met, mcache */
      if (i + 7 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      i += 7; break;
    }
    case BC_MCALL8: {  /* 6 bytes: dst u8, obj u8, met u8, mcache u16 */
      if (i + 6 > n) return;
      BUMP(bc[i + 1]);
      BUMP(bc[i + 2]);
      i += 6; break;
    }
    case BC_ARGLIST0: i += 1; break;
    case BC_ARGLIST1: {  /* 3 bytes: src(u16) */
      if (i + 3 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      i += 3; break;
    }
    case BC_ARGLIST2: {  /* 5 bytes: a(u16), b(u16) */
      if (i + 5 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      BUMP(bc_rd16(bc + i + 3));
      i += 5; break;
    }
    case BC_ARGLIST3: {  /* 7 bytes */
      if (i + 7 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      BUMP(bc_rd16(bc + i + 3));
      BUMP(bc_rd16(bc + i + 5));
      i += 7; break;
    }
    case BC_ARGLIST4: {  /* 9 bytes */
      if (i + 9 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      BUMP(bc_rd16(bc + i + 3));
      BUMP(bc_rd16(bc + i + 5));
      BUMP(bc_rd16(bc + i + 7));
      i += 9; break;
    }
    case BC_ARGLIST5: {  /* 11 bytes */
      if (i + 11 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      BUMP(bc_rd16(bc + i + 3));
      BUMP(bc_rd16(bc + i + 5));
      BUMP(bc_rd16(bc + i + 7));
      BUMP(bc_rd16(bc + i + 9));
      i += 11; break;
    }
    case BC_ARGLIST: case BC_ARGLIST8: {  /* variable: opcode + size(u16) + size*u16 srcs */
      if (i + 3 > n) return;
      uint32_t sz = bc_rd16(bc + i + 1);
      if (i + 3 + sz * 2u > n) return;
      for (uint32_t k = 0; k < sz; k++) BUMP(bc_rd16(bc + i + 3 + k*2));
      i += 3 + sz * 2u; break;
    }
    /* Heap-deref family.  LD4/ST4_N use 1+1 byte; LOAD/STOR use
     * 1+u16*3; LOAD8/STOR8 use 1+u8*3. */
    case 0x6A: case 0x6B: case 0x6C: case 0x6D:
    case 0x6E: case 0x6F: case 0x70: case 0x71:
    case 0x72: case 0x73: case 0x74: case 0x75:
    case 0x76: case 0x77: case 0x78: case 0x79: {  /* LD4_0..LD4_F */
      if (i + 2 > n) return;
      uint8_t opr = bc[i + 1];
      BUMP(opr & 0xF);  /* dst */
      BUMP(opr >> 4);   /* src */
      i += 2; break;
    }
    case 0x7A: case 0x7B: case 0x7C: case 0x7D:
    case 0x7E: case 0x7F: case 0x80: case 0x81:
    case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x89: {  /* ST4_0..ST4_F */
      if (i + 2 > n) return;
      uint8_t opr = bc[i + 1];
      BUMP(opr & 0xF);
      BUMP(opr >> 4);
      i += 2; break;
    }
    /* STOR (0x22) wide, STOR8 (0x23) narrow */
    case 0x22: {  /* 7 bytes: dst u16, src u16, index u16 */
      if (i + 7 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      BUMP(bc_rd16(bc + i + 3));
      i += 7; break;
    }
    case 0x23: {  /* 4 bytes: dst u8, src u8, index u8 */
      if (i + 4 > n) return;
      BUMP(bc[i + 1]);
      BUMP(bc[i + 2]);
      i += 4; break;
    }
    case BC_LOAD: {  /* 7 bytes: dst u16, src u16, index u16 */
      if (i + 7 > n) return;
      BUMP(bc_rd16(bc + i + 1));
      BUMP(bc_rd16(bc + i + 3));
      i += 7; break;
    }
    case BC_LOAD8: {  /* 4 bytes: dst u8, src u8, index u8 */
      if (i + 4 > n) return;
      BUMP(bc[i + 1]);
      BUMP(bc[i + 2]);
      i += 4; break;
    }
    /* Default: unknown opcode -- bail.  Function gets no pinning. */
    default:
      return;
    }
  }

#undef BUMP

  /* Pick top-3 by count.  Skip slot 0 and slot 1 (P and E --
   * the parent/args slots; PROLOGUE populates them, so pinning
   * is wasted). */
  uint32_t best_counts[JIT_MAX_PINNED] = {0};
  int      best_slots[JIT_MAX_PINNED]  = {-1, -1, -1};
  for (int s = 2; s < JIT_SCAN_MAX_SLOT; s++) {
    uint32_t c = counts[s];
    if (c == 0) continue;
    for (int k = 0; k < JIT_MAX_PINNED; k++) {
      if (c > best_counts[k]) {
        for (int j = JIT_MAX_PINNED - 1; j > k; j--) {
          best_counts[j] = best_counts[j-1];
          best_slots[j]  = best_slots[j-1];
        }
        best_counts[k] = c;
        best_slots[k]  = s;
        break;
      }
    }
  }
  int n_pinned = 0;
  for (int k = 0; k < JIT_MAX_PINNED; k++) {
    if (best_counts[k] < 2) break;
    b->pinned[n_pinned].slot = (int32_t)best_slots[k];
    b->pinned[n_pinned].reg  = (uint8_t)(13 + n_pinned);
    n_pinned++;
  }
  b->pinned_count = n_pinned;
}

static jit_buf *jit_translate_core(const uint8_t *bc, size_t n,
                                   int have_sbc, int record_relocs) {
  /* x86 expansion factor.  Worst case so far is the comparison
   * sequence at ~16 bytes per opcode; B is ~12 bytes; prologue
   * 6, epilogue 7.  Round up to 20/opcode to leave headroom. */
  jit_buf *b = jit_buf_new(n * 20 + 64);
  if (!b) return NULL;
  /* Step 6b: turn on reloc recording for the AOT writer path.
   * Each helper-pointer call site appends (offset, helper_id)
   * to b->relocs; the runtime translator path (record_relocs=0)
   * skips the bookkeeping. */
  b->record_relocs = record_relocs;
  b->pending_helper_id = JIT_HELPER_NONE;
  b->relocs = NULL;
  b->relocs_count = 0;
  b->relocs_cap = 0;

  /* bc_to_x86[i] = x86 offset of the instruction starting at
   * bc[i], or (size_t)-1 if bc[i] isn't an instruction start.
   * Used by the patch pass to resolve branch targets. */
  size_t *bc_to_x86 = (size_t*)malloc((n + 1) * sizeof(size_t));
  if (!bc_to_x86) { jit_buf_free(b); return NULL; }
  for (size_t k = 0; k <= n; k++) bc_to_x86[k] = (size_t)-1;

  jit_patch patches[JIT_MAX_PATCHES];
  size_t patches_n = 0;
  int fail = 0;

  /* Phase 2a register allocation: pick up to JIT_MAX_PINNED slots
   * to pin to R13..R15.  The prologue + epilogue and all
   * slot-access primitives consult b->pinned[] thereafter. */
  jit_select_pinned_slots(b, bc, n);

  if (have_sbc) jit_emit_prologue2(b);
  else          jit_emit_prologue(b);

  size_t i = 0;
  while (i < n) {
    bc_to_x86[i] = b->len;
    uint8_t op = bc[i];
    switch (op) {
    case BC_NOP:
      i += 1;
      break;

    case BC_CNAS:
      /* Function-prologue arg-count check.  Compiler always
       * emits matching counts; the runtime check is a defensive
       * assertion against bytecode corruption.  Skip in JIT for
       * now -- the worst case is we lose a redundant validation.
       * If a corrupted SBC ever needs the check, we'll route
       * through a trampoline helper instead.  Format: opcode
       * + 16-bit expected nargs. */
      if (i + 3 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      i += 3;
      break;

    case BC_MOVE: {
      /* SBC_MOVE: opcode + dst(u16) + src(u16).
       * L[dst] = L[src]  via  mov rax, [src]; mov [dst], rax. */
      if (i + 5 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      int dst = bc_rd16(bc + i + 1);
      int src = bc_rd16(bc + i + 3);
      emit_mov_rax_from_slot(b, src);
      emit_mov_slot_from_rax(b, dst);
      i += 5;
      break;
    }
    case BC_MOVE8: {
      /* SBC_MOVE8: opcode + dst(u8) + src(u8). */
      if (i + 3 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      int dst = bc[i + 1];
      int src = bc[i + 2];
      emit_mov_rax_from_slot(b, src);
      emit_mov_slot_from_rax(b, dst);
      i += 3;
      break;
    }
    case 0x7A: case 0x7B: case 0x7C: case 0x7D:
    case 0x7E: case 0x7F: case 0x80: case 0x81:
    case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x89: {
      /* SBC_ST4_0 .. SBC_ST4_F (struct field store family).
       * Same encoding as LD4 but stores instead of loads:
       *   STOR(L[dst], index, L[src])
       *
       * Step 12g: emit inline x86 (jit_emit_st4) instead of the
       * helper trampoline.  Fast path: when the value is an
       * immediate (low bit clear), inline store with no barrier.
       * Slow path (heap value): falls through to the existing
       * st4 helper which handles lsetm's cross-gen barrier.
       * SYMTA_NO_ST4_INLINE bypasses for bisecting. */
      if (i + 2 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      int index = op - 0x7A;
      uint8_t opr = bc[i + 1];
      uint32_t dst = (uint32_t)(opr & 0xF);
      uint32_t src = (uint32_t)(opr >> 4);
      if (jit_rt_heap0_addr && jit_rt_st4_helper
          && !getenv("SYMTA_NO_ST4_INLINE")) {
        jit_emit_st4(b, dst, src, (uint32_t)index,
                     jit_rt_heap0_addr, (void*)jit_rt_st4_helper);
      } else if (jit_rt_st4_helper) {
        b->pending_helper_id = JIT_HELPER_ST4;
        jit_emit_call_helper3(b, (void*)jit_rt_st4_helper,
                              dst, src, (uint32_t)index);
      } else {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      i += 2;
      break;
    }

    case 0x22: case 0x23: {
      /* SBC_STOR (0x22) -- opcode + dst(u16) + src(u16) + index(u16) = 7 bytes.
       * SBC_STOR8 (0x23) -- opcode + dst(u8)  + src(u8)  + index(u8)  = 4 bytes.
       * Body: STOR(L[dst], index, L[src]).  Reuses jit_rt_st4_helper
       * (same shape -- helper takes index as 32-bit, ST4 just
       * happened to be the family that exercised it first).
       * Step 12g: inline via jit_emit_st4 (same fast/slow split as
       * ST4_*).  SYMTA_NO_ST4_INLINE bypasses for bisecting. */
      int wide = (op == 0x22);
      int op_len = wide ? 7 : 4;
      if (i + op_len > n) { jit_last_fail_opcode = op;
                            jit_last_fail_offset = i;
                            fail = 1; goto done; }
      uint32_t dst, src, index;
      if (wide) {
        dst   = (uint32_t)bc_rd16(bc + i + 1);
        src   = (uint32_t)bc_rd16(bc + i + 3);
        index = (uint32_t)bc_rd16(bc + i + 5);
      } else {
        dst   = bc[i + 1];
        src   = bc[i + 2];
        index = bc[i + 3];
      }
      if (jit_rt_heap0_addr && jit_rt_st4_helper
          && !getenv("SYMTA_NO_ST4_INLINE")) {
        jit_emit_st4(b, dst, src, index,
                     jit_rt_heap0_addr, (void*)jit_rt_st4_helper);
      } else if (jit_rt_st4_helper) {
        b->pending_helper_id = JIT_HELPER_ST4;
        jit_emit_call_helper3(b, (void*)jit_rt_st4_helper, dst, src, index);
      } else {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      i += op_len;
      break;
    }

    case 0x6A: case 0x6B: case 0x6C: case 0x6D:
    case 0x6E: case 0x6F: case 0x70: case 0x71:
    case 0x72: case 0x73: case 0x74: case 0x75:
    case 0x76: case 0x77: case 0x78: case 0x79: {
      /* SBC_LD4_0 .. SBC_LD4_F (struct field load family).
       * Encoding: opcode byte (carries the field index in its
       * low nibble of `op - 0x6A`) + 1 operand byte where
       * dst = opr & 0xF, src = opr >> 4 -- both 4-bit slot
       * indices into L[0..15].  Body:
       *   L[dst] = ((void**)O_PTR(L[src]))[index]
       *
       * Step 12e: emit inline x86 (jit_emit_ld4) instead of the
       * helper trampoline.  Falls back to the helper if
       * SYMTA_NO_LD4_INLINE is set (env-gate for bisecting) or
       * if jit_rt_heap0_addr isn't available (standalone
       * self-test). */
      if (i + 2 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      int index = op - 0x6A;
      uint8_t opr = bc[i + 1];
      uint32_t dst = (uint32_t)(opr & 0xF);
      uint32_t src = (uint32_t)(opr >> 4);
      if (jit_rt_heap0_addr && !getenv("SYMTA_NO_LD4_INLINE")) {
        jit_emit_ld4(b, dst, src, (uint32_t)index, jit_rt_heap0_addr);
      } else if (jit_rt_ld4_helper) {
        b->pending_helper_id = JIT_HELPER_LD4;
        jit_emit_call_helper3(b, (void*)jit_rt_ld4_helper,
                              dst, src, (uint32_t)index);
      } else {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      i += 2;
      break;
    }

    case BC_LOAD: {
      /* SBC_LOAD: same body as LD4 but with u16 operands. */
      if (i + 7 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      uint32_t dst   = (uint32_t)bc_rd16(bc + i + 1);
      uint32_t src   = (uint32_t)bc_rd16(bc + i + 3);
      uint32_t index = (uint32_t)bc_rd16(bc + i + 5);
      if (jit_rt_heap0_addr && !getenv("SYMTA_NO_LD4_INLINE")) {
        jit_emit_ld4(b, dst, src, index, jit_rt_heap0_addr);
      } else if (jit_rt_ld4_helper) {
        b->pending_helper_id = JIT_HELPER_LD4;
        jit_emit_call_helper3(b, (void*)jit_rt_ld4_helper, dst, src, index);
      } else {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      i += 7;
      break;
    }
    case BC_LOAD8: {
      if (i + 4 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      uint32_t dst   = bc[i + 1];
      uint32_t src   = bc[i + 2];
      uint32_t index = bc[i + 3];
      if (jit_rt_heap0_addr && !getenv("SYMTA_NO_LD4_INLINE")) {
        jit_emit_ld4(b, dst, src, index, jit_rt_heap0_addr);
      } else if (jit_rt_ld4_helper) {
        b->pending_helper_id = JIT_HELPER_LD4;
        jit_emit_call_helper3(b, (void*)jit_rt_ld4_helper, dst, src, index);
      } else {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      i += 4;
      break;
    }
    case BC_LIST: {
      /* SBC_LIST: opcode + dst(u16) + size(u16).  Allocates a
       * size-slot list via gc_alloc; no sbc context required.
       * Helper3 calling convention: pass `size` as the third
       * "slot" arg -- the helper interprets it as a literal int. */
      if (i + 5 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_list_helper) { jit_last_fail_opcode = op;
                                 jit_last_fail_offset = i;
                                 fail = 1; goto done; }
      uint32_t dst  = (uint32_t)bc_rd16(bc + i + 1);
      uint32_t size = (uint32_t)bc_rd16(bc + i + 3);
      b->pending_helper_id = JIT_HELPER_LIST;
      jit_emit_call_helper3(b, (void*)jit_rt_list_helper, dst, size, 0);
      i += 5;
      break;
    }
    case BC_LIST1: {
      /* SBC_LIST1: opcode + dst(u16) + x(u16) = 5 bytes.  Fused
       * LIST(L[dst], 1) + LGET(L[dst], 0) = L[x].  Trampolined
       * to jit_rt_list1_helper which does both stores. */
      if (i + 5 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_list1_helper) { jit_last_fail_opcode = op;
                                  jit_last_fail_offset = i;
                                  fail = 1; goto done; }
      uint32_t dst = (uint32_t)bc_rd16(bc + i + 1);
      uint32_t x   = (uint32_t)bc_rd16(bc + i + 3);
      b->pending_helper_id = JIT_HELPER_LIST1;
      jit_emit_call_helper3(b, (void*)jit_rt_list1_helper, dst, x, 0);
      i += 5;
      break;
    }
    case BC_LIST2: {
      /* SBC_LIST2: opcode + dst(u16) + a(u16) + b(u16) = 7 bytes.
       * Fused LIST(L[dst], 2) + LGET(L[dst], 0) = L[a] + LGET(L[dst], 1) = L[b]. */
      if (i + 7 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_list2_helper) { jit_last_fail_opcode = op;
                                  jit_last_fail_offset = i;
                                  fail = 1; goto done; }
      uint32_t dst = (uint32_t)bc_rd16(bc + i + 1);
      uint32_t a   = (uint32_t)bc_rd16(bc + i + 3);
      uint32_t bb  = (uint32_t)bc_rd16(bc + i + 5);
      b->pending_helper_id = JIT_HELPER_LIST2;
      jit_emit_call_helper3(b, (void*)jit_rt_list2_helper, dst, a, bb);
      i += 7;
      break;
    }
    case BC_FXNSIZE: {
      /* SBC_FXNSIZE: opcode + dst(u16) + src(u16) = 5 bytes.
       * L[dst] = FXN(LIST_SIZE(L[src])).  No MCACHE fallback;
       * the heap-header read is unconditional. */
      if (i + 5 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_fxnsize_helper) { jit_last_fail_opcode = op;
                                    jit_last_fail_offset = i;
                                    fail = 1; goto done; }
      uint32_t dst = (uint32_t)bc_rd16(bc + i + 1);
      uint32_t src = (uint32_t)bc_rd16(bc + i + 3);
      b->pending_helper_id = JIT_HELPER_FXNSIZE;
      jit_emit_call_helper3(b, (void*)jit_rt_fxnsize_helper, dst, src, 0);
      i += 5;
      break;
    }

    case BC_ARGLIST0: {
      if (!jit_rt_arglist0_helper) { jit_last_fail_opcode = op;
                                     jit_last_fail_offset = i;
                                     fail = 1; goto done; }
      b->pending_helper_id = JIT_HELPER_ARGLIST0;
      jit_emit_call_helper3(b, (void*)jit_rt_arglist0_helper, 0, 0, 0);
      i += 1;
      break;
    }
    case BC_ARGLIST1: {
      if (i + 2 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_arglist1_helper) { jit_last_fail_opcode = op;
                                     jit_last_fail_offset = i;
                                     fail = 1; goto done; }
      uint32_t a = bc[i + 1];
      b->pending_helper_id = JIT_HELPER_ARGLIST1;
      jit_emit_call_helper3(b, (void*)jit_rt_arglist1_helper, a, 0, 0);
      i += 2;
      break;
    }
    case BC_ARGLIST2: {
      if (i + 3 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_arglist2_helper) { jit_last_fail_opcode = op;
                                     jit_last_fail_offset = i;
                                     fail = 1; goto done; }
      uint32_t a = bc[i + 1];
      uint32_t bb = bc[i + 2];
      b->pending_helper_id = JIT_HELPER_ARGLIST2;
      jit_emit_call_helper3(b, (void*)jit_rt_arglist2_helper, a, bb, 0);
      i += 3;
      break;
    }
    case BC_ARGLIST3: {
      if (i + 4 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_arglist3_helper) { jit_last_fail_opcode = op;
                                     jit_last_fail_offset = i;
                                     fail = 1; goto done; }
      uint32_t a = bc[i + 1];
      uint32_t bb = bc[i + 2];
      uint32_t c = bc[i + 3];
      b->pending_helper_id = JIT_HELPER_ARGLIST3;
      jit_emit_call_helper3(b, (void*)jit_rt_arglist3_helper, a, bb, c);
      i += 4;
      break;
    }
    case BC_ARGLIST4: {
      /* opcode + 4 u8 slot indices (5 bytes total).  Pack the
       * four 8-bit indices into one 32-bit immediate. */
      if (i + 5 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_arglist4_helper) { jit_last_fail_opcode = op;
                                     jit_last_fail_offset = i;
                                     fail = 1; goto done; }
      uint32_t packed = (uint32_t)bc[i + 1]
                      | ((uint32_t)bc[i + 2] << 8)
                      | ((uint32_t)bc[i + 3] << 16)
                      | ((uint32_t)bc[i + 4] << 24);
      b->pending_helper_id = JIT_HELPER_ARGLIST4;
      jit_emit_call_helper3(b, (void*)jit_rt_arglist4_helper, packed, 0, 0);
      i += 5;
      break;
    }
    case BC_ARGLIST5: {
      /* opcode + 5 u8 slot indices (6 bytes).  Pack 4 into one
       * int, the 5th into the second helper3 arg. */
      if (i + 6 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_arglist5_helper) { jit_last_fail_opcode = op;
                                     jit_last_fail_offset = i;
                                     fail = 1; goto done; }
      uint32_t packed4 = (uint32_t)bc[i + 1]
                       | ((uint32_t)bc[i + 2] << 8)
                       | ((uint32_t)bc[i + 3] << 16)
                       | ((uint32_t)bc[i + 4] << 24);
      uint32_t a4 = bc[i + 5];
      b->pending_helper_id = JIT_HELPER_ARGLIST5;
      jit_emit_call_helper3(b, (void*)jit_rt_arglist5_helper, packed4, a4, 0);
      i += 6;
      break;
    }
    case BC_CALL: {
      if (getenv("SYMTA_NO_CALL")) { jit_last_fail_opcode = op;
                                       jit_last_fail_offset = i;
                                       fail = 1; goto done; }
      /* SBC_CALL: opcode + dst(u16) + fn(u16).  The interpreter
       * also writes `api.frame->pin = pin` for stack traces --
       * we skip that here.  JIT'd code doesn't have a `pin`;
       * trace frames will be missing row/col info but the
       * function-frame chain itself is still walked correctly. */
      if (i + 5 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_call_helper) { jit_last_fail_opcode = op;
                                 jit_last_fail_offset = i;
                                 fail = 1; goto done; }
      uint32_t dst = (uint32_t)bc_rd16(bc + i + 1);
      uint32_t fn  = (uint32_t)bc_rd16(bc + i + 3);
      b->pending_helper_id = JIT_HELPER_CALL;
      jit_emit_call_helper3(b, (void*)jit_rt_call_helper, dst, fn, 0);
      i += 5;
      break;
    }
    case BC_CALLIR: {
      if (i + 3 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_callir_helper) { jit_last_fail_opcode = op;
                                   jit_last_fail_offset = i;
                                   fail = 1; goto done; }
      uint32_t fn  = (uint32_t)bc_rd16(bc + i + 1);
      b->pending_helper_id = JIT_HELPER_CALLIR;
      jit_emit_call_helper3(b, (void*)jit_rt_callir_helper, fn, 0, 0);
      i += 3;
      break;
    }

    case BC_CALLT: {
      if (i + 5 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_callt_helper) { jit_last_fail_opcode = op;
                                  jit_last_fail_offset = i;
                                  fail = 1; goto done; }
      uint32_t dst = (uint32_t)bc_rd16(bc + i + 1);
      uint32_t fn  = (uint32_t)bc_rd16(bc + i + 3);
      b->pending_helper_id = JIT_HELPER_CALLT;
      jit_emit_call_helper3(b, (void*)jit_rt_callt_helper, dst, fn, 0);
      i += 5;
      break;
    }
    case BC_CALLTIR: {
      if (i + 3 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_calltir_helper) { jit_last_fail_opcode = op;
                                    jit_last_fail_offset = i;
                                    fail = 1; goto done; }
      uint32_t fn  = (uint32_t)bc_rd16(bc + i + 1);
      b->pending_helper_id = JIT_HELPER_CALLTIR;
      jit_emit_call_helper3(b, (void*)jit_rt_calltir_helper, fn, 0, 0);
      i += 3;
      break;
    }

    case BC_MCALL: {
      if (getenv("SYMTA_NO_MCALL")) { jit_last_fail_opcode = op;
                                       jit_last_fail_offset = i;
                                       fail = 1; goto done; }
      /* SBC_MCALL: opcode + dst(u16) + obj(u16) + met(u16) +
       * mcache_idx(u16, read inside MCACHE_CALL).
       * Total: 9 bytes.  Needs the sbc pointer (mt[met], mcaches[idx]). */
      if (i + 9 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!have_sbc || !jit_rt_mcall_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t dst    = (uint64_t)bc_rd16(bc + i + 1);
      uint64_t obj    = (uint64_t)bc_rd16(bc + i + 3);
      uint64_t met    = (uint64_t)bc_rd16(bc + i + 5);
      uint64_t mcidx  = (uint64_t)bc_rd16(bc + i + 7);
      uint64_t packed = (mcidx << 48) | (met << 32) | (obj << 16) | dst;
      b->pending_helper_id = JIT_HELPER_MCALL;
      jit_emit_call_with_sbc(b, (void*)jit_rt_mcall_helper, packed);
      i += 9;
      break;
    }

    case BC_FXNLGET: {
      /* SBC_FXNLGET: opcode + dst(u16) + src(u16) + index(u16) +
       * mcache_idx(u16) = 9 bytes.  Body has a T_LIST+T_INT
       * fast path (direct LGET) and a MCACHE_CALL(m_get) slow
       * path.
       *
       * Step 12h: inline the T_LIST + T_INT + in-bounds fast path
       * (jit_emit_fxnlget) instead of routing through the helper.
       * Fall back to the helper on tag-miss or out-of-bounds.
       * SYMTA_NO_FXNLGET_INLINE env-gate for bisecting. */
      if (i + 9 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!have_sbc || !jit_rt_fxnlget_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t dst    = (uint64_t)bc_rd16(bc + i + 1);
      uint64_t src    = (uint64_t)bc_rd16(bc + i + 3);
      uint64_t index  = (uint64_t)bc_rd16(bc + i + 5);
      uint64_t mcidx  = (uint64_t)bc_rd16(bc + i + 7);
      uint64_t packed = (mcidx << 48) | (index << 32) | (src << 16) | dst;
      if (jit_rt_heap0_addr && !getenv("SYMTA_NO_FXNLGET_INLINE")) {
        jit_emit_fxnlget(b, (uint32_t)dst, (uint32_t)src, (uint32_t)index,
                         jit_rt_heap0_addr,
                         (void*)jit_rt_fxnlget_helper, packed);
      } else {
        b->pending_helper_id = JIT_HELPER_FXNLGET;
        jit_emit_call_with_sbc(b, (void*)jit_rt_fxnlget_helper, packed);
      }
      i += 9;
      break;
    }

    case BC_FXNLISTN: {
      /* SBC_FXNLISTN: opcode + dst(u16) + src(u16) = 5 bytes.
       * L[dst] = LIST(UNFXN(L[src])) -- variable-size list alloc
       * with size from a runtime slot.  Helper3 trampoline. */
      if (i + 5 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_fxnlistn_helper) { jit_last_fail_opcode = op;
                                     jit_last_fail_offset = i;
                                     fail = 1; goto done; }
      uint32_t dst = (uint32_t)bc_rd16(bc + i + 1);
      uint32_t src = (uint32_t)bc_rd16(bc + i + 3);
      b->pending_helper_id = JIT_HELPER_FXNLISTN;
      jit_emit_call_helper3(b, (void*)jit_rt_fxnlistn_helper, dst, src, 0);
      i += 5;
      break;
    }

    case BC_FXNLSETIR: {
      /* SBC_FXNLSETIR: opcode + src(u16) + index(u16) + val(u16) +
       * mcache_idx(u16) = 9 bytes.  List-element set with ignored
       * result.  Same 3-way dispatch as FXNLGET (T_LIST+T_INT+
       * in-bounds direct LSET; else MCACHE_CALL m_set).
       *   Packed: [15:0]=src, [31:16]=index, [47:32]=val,
       *           [63:48]=mcache_idx.
       *
       * Step 12i: inline the T_LIST+T_INT+in-bounds+immediate-value
       * fast path via jit_emit_fxnlset.  Falls back to the helper
       * on any check failure (heap value, tag miss, out of bounds).
       * SYMTA_NO_FXNLSET_INLINE env-gate for bisecting. */
      if (i + 9 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!have_sbc || !jit_rt_fxnlsetir_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t src    = (uint64_t)bc_rd16(bc + i + 1);
      uint64_t index  = (uint64_t)bc_rd16(bc + i + 3);
      uint64_t val    = (uint64_t)bc_rd16(bc + i + 5);
      uint64_t mcidx  = (uint64_t)bc_rd16(bc + i + 7);
      uint64_t packed = (mcidx << 48) | (val << 32) | (index << 16) | src;
      if (jit_rt_heap0_addr && !getenv("SYMTA_NO_FXNLSET_INLINE")) {
        jit_emit_fxnlset(b, /*dst=*/0, (uint32_t)src, (uint32_t)index,
                         (uint32_t)val, jit_rt_heap0_addr,
                         (void*)jit_rt_fxnlsetir_helper,
                         packed, /*packed2=*/0, /*with_result=*/0);
      } else {
        b->pending_helper_id = JIT_HELPER_FXNLSETIR;
        jit_emit_call_with_sbc(b, (void*)jit_rt_fxnlsetir_helper, packed);
      }
      i += 9;
      break;
    }

    case BC_FXNLSET: {
      if (getenv("SYMTA_NO_FXNLSET")) { jit_last_fail_opcode = op;
                                         jit_last_fail_offset = i;
                                         fail = 1; goto done; }
      /* SBC_FXNLSET: opcode + dst(u16) + src(u16) + index(u16) +
       * val(u16) + mcache_idx(u16) = 11 bytes.  5 operands don't
       * fit in one u64, so we use the 2-arg-packed trampoline.
       *   packed1: [15:0]=dst [31:16]=src [47:32]=index [63:48]=val
       *   packed2: [15:0]=mcache_idx
       *
       * Step 12i: same fast-path inline as FXNLSETIR, but uses the
       * `with_result` shape so the helper bail-out routes to
       * call_with_sbc2.  Note: the fast path ignores `dst`
       * (matches FXNLSET macro semantics); only the slow path's
       * MCACHE_CALL m_set writes L[dst]. */
      if (i + 11 > n) { jit_last_fail_opcode = op;
                        jit_last_fail_offset = i;
                        fail = 1; goto done; }
      if (!have_sbc || !jit_rt_fxnlset_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t dst    = (uint64_t)bc_rd16(bc + i + 1);
      uint64_t src    = (uint64_t)bc_rd16(bc + i + 3);
      uint64_t index  = (uint64_t)bc_rd16(bc + i + 5);
      uint64_t val    = (uint64_t)bc_rd16(bc + i + 7);
      uint64_t mcidx  = (uint64_t)bc_rd16(bc + i + 9);
      uint64_t packed1 = (val << 48) | (index << 32) | (src << 16) | dst;
      uint64_t packed2 = mcidx;
      if (jit_rt_heap0_addr && !getenv("SYMTA_NO_FXNLSET_INLINE")) {
        jit_emit_fxnlset(b, (uint32_t)dst, (uint32_t)src, (uint32_t)index,
                         (uint32_t)val, jit_rt_heap0_addr,
                         (void*)jit_rt_fxnlset_helper,
                         packed1, packed2, /*with_result=*/1);
      } else {
        b->pending_helper_id = JIT_HELPER_FXNLSET;
        jit_emit_call_with_sbc2(b, (void*)jit_rt_fxnlset_helper, packed1, packed2);
      }
      i += 11;
      break;
    }

    case BC_OBJECT: {
      if (getenv("SYMTA_NO_OBJECT")) { jit_last_fail_opcode = op;
                                       jit_last_fail_offset = i;
                                       fail = 1; goto done; }
      /* SBC_OBJECT: opcode + dst(u16) + tid(u16) + size(u16) = 7
       * bytes.  Allocates a `size`-slot heap object with type
       * sbc->ty[tid], or MKIMM for size==0.  Needs sbc context. */
      if (i + 7 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!have_sbc || !jit_rt_object_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t dst  = (uint64_t)bc_rd16(bc + i + 1);
      uint64_t tid  = (uint64_t)bc_rd16(bc + i + 3);
      uint64_t size = (uint64_t)bc_rd16(bc + i + 5);
      uint64_t packed = (size << 32) | (tid << 16) | dst;
      b->pending_helper_id = JIT_HELPER_OBJECT;
      jit_emit_call_with_sbc(b, (void*)jit_rt_object_helper, packed);
      i += 7;
      break;
    }

    case BC_ARGLIST: case BC_ARGLIST8: {
      if (getenv("SYMTA_NO_ARGLIST")) { jit_last_fail_opcode = op;
                                         jit_last_fail_offset = i;
                                         fail = 1; goto done; }
      /* SBC_ARGLIST  (0x15): opcode + size(u16) + size*u16 src
       * SBC_ARGLIST8 (0x16): opcode + size(u8)  + size*u8  src
       *
       * Pack at translate time into TWO u64s (via call_with_sbc2):
       *   packed1 = (size << 56) | src0 | (src1 << 8) | ... | (src6 << 48)
       *   packed2 = src7 | (src8 << 8) | ... | (src14 << 56)
       * = 1 size byte + up to 15 u8 slot indices.  Slot indices
       * > 255 fail at translate time (the slot field IS u16 in
       * the SBC but the translated helper currently uses u8 --
       * tight frames in practice).  For size > 15, fail and let
       * the interpreter handle the call site. */
      if (i + (op == BC_ARGLIST ? 3 : 2) > n) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      void *helper; jit_helper_id_t hid; size_t advance;
      uint32_t size;
      if (op == BC_ARGLIST) {
        size = (uint32_t)bc_rd16(bc + i + 1);
        advance = 1 + 2 + 2 * size;
        helper = (void*)jit_rt_arglist_n_helper;
        hid = JIT_HELPER_ARGLIST_N;
      } else {
        size = (uint32_t)bc[i + 1];
        advance = 1 + 1 + size;
        helper = (void*)jit_rt_arglist8_n_helper;
        hid = JIT_HELPER_ARGLIST8_N;
      }
      if (size > 15 || !have_sbc || !helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      if (i + advance > n) { jit_last_fail_opcode = op;
                             jit_last_fail_offset = i;
                             fail = 1; goto done; }
      uint64_t packed1 = (uint64_t)size << 56;
      uint64_t packed2 = 0;
      for (uint32_t j = 0; j < size; j++) {
        uint32_t src;
        if (op == BC_ARGLIST) {
          src = (uint32_t)bc_rd16(bc + i + 3 + j*2);
          if (src > 0xFF) { jit_last_fail_opcode = op;
                            jit_last_fail_offset = i;
                            fail = 1; goto done; }
        } else {
          src = (uint32_t)bc[i + 2 + j];
        }
        if (j < 7) {
          packed1 |= (uint64_t)(src & 0xFF) << (j * 8);
        } else {
          packed2 |= (uint64_t)(src & 0xFF) << ((j - 7) * 8);
        }
      }
      b->pending_helper_id = hid;
      jit_emit_call_with_sbc2(b, helper, packed1, packed2);
      i += advance;
      break;
    }

    case BC_SUBTYPE: {
      if (getenv("SYMTA_NO_SUBTYPE")) { jit_last_fail_opcode = op;
                                         jit_last_fail_offset = i;
                                         fail = 1; goto done; }
      /* SBC_SUBTYPE: opcode + super(u16) + sub(u16) = 5 bytes.
       * Calls add_subtype((int)sbc->ty[super], (int)sbc->ty[sub]). */
      if (i + 5 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!have_sbc || !jit_rt_subtype_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t super = (uint64_t)bc_rd16(bc + i + 1);
      uint64_t sub   = (uint64_t)bc_rd16(bc + i + 3);
      uint64_t packed = (sub << 16) | super;
      b->pending_helper_id = JIT_HELPER_SUBTYPE;
      jit_emit_call_with_sbc(b, (void*)jit_rt_subtype_helper, packed);
      i += 5;
      break;
    }

    case BC_MNAME: {
      if (getenv("SYMTA_NO_MNAME")) { jit_last_fail_opcode = op;
                                       jit_last_fail_offset = i;
                                       fail = 1; goto done; }
      /* SBC_MNAME: opcode + dst(u16) + src(u16) = 5 bytes.
       * L[dst] = get_method_name(UNFXN(L[src])). */
      if (i + 5 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_mname_helper) { jit_last_fail_opcode = op;
                                  jit_last_fail_offset = i;
                                  fail = 1; goto done; }
      uint32_t dst = (uint32_t)bc_rd16(bc + i + 1);
      uint32_t src = (uint32_t)bc_rd16(bc + i + 3);
      b->pending_helper_id = JIT_HELPER_MNAME;
      jit_emit_call_helper3(b, (void*)jit_rt_mname_helper, dst, src, 0);
      i += 5;
      break;
    }

    case BC_TINIT: {
      if (getenv("SYMTA_NO_TINIT")) { jit_last_fail_opcode = op;
                                       jit_last_fail_offset = i;
                                       fail = 1; goto done; }
      /* SBC_TINIT: opcode + type(u16) + size(u16) + name(u24) =
       * 8 bytes.  Body: set_type_size_and_name(sbc->ty[type],
       * size, sbc->tx[name]).  All fields fit in one u64. */
      if (i + 8 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!have_sbc || !jit_rt_tinit_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t type = (uint64_t)bc_rd16(bc + i + 1);
      uint64_t size = (uint64_t)bc_rd16(bc + i + 3);
      uint64_t name = (uint64_t)bc_rd24(bc + i + 5);
      uint64_t packed = (name << 32) | (size << 16) | type;
      b->pending_helper_id = JIT_HELPER_TINIT;
      jit_emit_call_with_sbc(b, (void*)jit_rt_tinit_helper, packed);
      i += 8;
      break;
    }

    case BC_TINITI: {
      if (getenv("SYMTA_NO_TINIT")) { jit_last_fail_opcode = op;
                                       jit_last_fail_offset = i;
                                       fail = 1; goto done; }
      /* SBC_TINITI: opcode + tag(u16) + size(u16) + name(u64) =
       * 13 bytes.  Runtime calls set_type_size_and_name(
       * sbc->ty[tag], size, name).  Two packed u64s. */
      if (i + 13 > n) { jit_last_fail_opcode = op;
                        jit_last_fail_offset = i;
                        fail = 1; goto done; }
      if (!have_sbc || !jit_rt_tiniti_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t tag  = (uint64_t)bc_rd16(bc + i + 1);
      uint64_t size = (uint64_t)bc_rd16(bc + i + 3);
      uint64_t name = 0;
      for (int k = 0; k < 8; k++) name |= (uint64_t)bc[i + 5 + k] << (k * 8);
      uint64_t packed1 = (size << 16) | tag;
      b->pending_helper_id = JIT_HELPER_TINITI;
      jit_emit_call_with_sbc2(b, (void*)jit_rt_tiniti_helper, packed1, name);
      i += 13;
      break;
    }

    case BC_CURMET: {
      if (getenv("SYMTA_NO_CURMET")) { jit_last_fail_opcode = op;
                                         jit_last_fail_offset = i;
                                         fail = 1; goto done; }
      /* SBC_CURMET: opcode + dst(u16) = 3 bytes.  L[dst] =
       * currently executing method (api.curmet). */
      if (i + 3 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_curmet_helper) { jit_last_fail_opcode = op;
                                   jit_last_fail_offset = i;
                                   fail = 1; goto done; }
      uint32_t dst = (uint32_t)bc_rd16(bc + i + 1);
      b->pending_helper_id = JIT_HELPER_CURMET;
      jit_emit_call_helper3(b, (void*)jit_rt_curmet_helper, dst, 0, 0);
      i += 3;
      break;
    }

    case BC_FADD: case BC_FSUB: case BC_FMUL: case BC_FDIV: {
      if (getenv("SYMTA_NO_FARITH")) { jit_last_fail_opcode = op;
                                        jit_last_fail_offset = i;
                                        fail = 1; goto done; }
      /* Typed-float arith.  Wire: opcode + dst(u16) + a(u16) +
       * b(u16) = 7 bytes.  Operands are statically known T_FLOAT;
       * no tag check or MCALL fallback.  Helper3 trampoline.
       * An x86 backend should lower these to ADDSS/SUBSS/MULSS/
       * DIVSS on xmm regs, but for now the helper unbox/rebox is
       * fine -- typed-float is rare in measured workloads. */
      if (i + 7 > n) { fail = 1; goto done; }
      void *helper; jit_helper_id_t hid;
      switch (op) {
        case BC_FADD: helper = (void*)jit_rt_fadd_helper; hid = JIT_HELPER_FADD; break;
        case BC_FSUB: helper = (void*)jit_rt_fsub_helper; hid = JIT_HELPER_FSUB; break;
        case BC_FMUL: helper = (void*)jit_rt_fmul_helper; hid = JIT_HELPER_FMUL; break;
        case BC_FDIV: helper = (void*)jit_rt_fdiv_helper; hid = JIT_HELPER_FDIV; break;
        default: helper = NULL; hid = JIT_HELPER_NONE;
      }
      if (!helper) { jit_last_fail_opcode = op;
                     jit_last_fail_offset = i;
                     fail = 1; goto done; }
      uint32_t dst = (uint32_t)bc_rd16(bc + i + 1);
      uint32_t a   = (uint32_t)bc_rd16(bc + i + 3);
      uint32_t x   = (uint32_t)bc_rd16(bc + i + 5);
      b->pending_helper_id = hid;
      jit_emit_call_helper3(b, helper, dst, a, x);
      i += 7;
      break;
    }

    case BC_DMET: {
      if (getenv("SYMTA_NO_DMET")) { jit_last_fail_opcode = op;
                                       jit_last_fail_offset = i;
                                       fail = 1; goto done; }
      /* SBC_DMET: opcode + tyidx(u16) + mtidx(u24) + handler(u16)
       * = 8 bytes.  Runtime add_method using sbc-side type + mt
       * tables.  Packed: [15:0]=tyidx [39:16]=mtidx [55:40]=handler. */
      if (i + 8 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!have_sbc || !jit_rt_dmet_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t tyidx   = (uint64_t)bc_rd16(bc + i + 1);
      uint64_t mtidx   = (uint64_t)bc_rd24(bc + i + 3);
      uint64_t handler = (uint64_t)bc_rd16(bc + i + 6);
      uint64_t packed = (handler << 40) | (mtidx << 16) | tyidx;
      b->pending_helper_id = JIT_HELPER_DMET;
      jit_emit_call_with_sbc(b, (void*)jit_rt_dmet_helper, packed);
      i += 8;
      break;
    }

    case BC_IMMEQ: case BC_IMMNE: {
      /* SBC_IMMEQ / SBC_IMMNE: opcode + dst(u16) + a(u16) + b(u16) +
       * mcache_idx(u16) = 9 bytes.  3-way dispatch in the helper:
       *   T_INT-T_INT      -> bitwise IMMEQ/IMMNE
       *   text-vs-text     -> texts_equal direct call
       *   otherwise        -> MCACHE_CALL m_eq / m_ne
       * Packed: [15:0]=dst, [31:16]=a, [47:32]=b, [63:48]=mcache_idx.
       *
       * Step 12f: inline the T_INT fast path -- TAGIS(T_INT, L[a])
       * is sufficient because if L[a] is T_INT and L[b] is not,
       * the bitwise compare correctly yields !equal (the tag bits
       * differ).  Matches the helper's first branch exactly.
       * SYMTA_NO_IMMEQ_INLINE bypasses the inline for bisecting. */
      if (i + 9 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      void *helper; jit_helper_id_t hid;
      if (op == BC_IMMEQ) { helper = (void*)jit_rt_immeq_helper; hid = JIT_HELPER_IMMEQ; }
      else                { helper = (void*)jit_rt_immne_helper; hid = JIT_HELPER_IMMNE; }
      if (!have_sbc || !helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t dst    = (uint64_t)bc_rd16(bc + i + 1);
      uint64_t a      = (uint64_t)bc_rd16(bc + i + 3);
      uint64_t bb     = (uint64_t)bc_rd16(bc + i + 5);
      uint64_t mcidx  = (uint64_t)bc_rd16(bc + i + 7);
      uint64_t packed = (mcidx << 48) | (bb << 32) | (a << 16) | dst;

      int inline_ok = !getenv("SYMTA_NO_IMMEQ_INLINE");
      if (inline_ok) {
        /* Fast path: check L[a] is T_INT.  Low 16 bits zero ==
         * FXN-tagged int.  If L[a] is T_INT we delegate to
         * jit_emit_same / jit_emit_vary, which run the same
         * bitwise-compare-and-FXN-tag sequence the helper takes
         * on the T_INT-fast branch. */
        emit_mov_rax_from_slot(b, (int)a);   /* rax = L[a] */
        emit_test_ax_ax(b);                   /* check T_INT (low 16 bits) */
        size_t to_slow = emit_jnz_rel32(b);

        if (op == BC_IMMEQ) jit_emit_same(b, (int)dst, (int)a, (int)bb);
        else                jit_emit_vary(b, (int)dst, (int)a, (int)bb);
        size_t to_done = jit_emit_jmp(b);

        /* Slow path: full helper call. */
        jit_patch_jmp_here(b, to_slow);
        b->pending_helper_id = hid;
        jit_emit_call_with_sbc(b, helper, packed);

        jit_patch_jmp_here(b, to_done);
      } else {
        b->pending_helper_id = hid;
        jit_emit_call_with_sbc(b, helper, packed);
      }
      i += 9;
      break;
    }
    case BC_MCALLIR: {
      /* opcode + obj(u16) + met(u16) + mcache_idx(u16) = 7 bytes */
      if (i + 7 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!have_sbc || !jit_rt_mcallir_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t obj    = (uint64_t)bc_rd16(bc + i + 1);
      uint64_t met    = (uint64_t)bc_rd16(bc + i + 3);
      uint64_t mcidx  = (uint64_t)bc_rd16(bc + i + 5);
      uint64_t packed = (mcidx << 48) | (met << 32) | (obj << 16) | 0;
      b->pending_helper_id = JIT_HELPER_MCALLIR;
      jit_emit_call_with_sbc(b, (void*)jit_rt_mcallir_helper, packed);
      i += 7;
      break;
    }
    case BC_MCALL8: {
      /* opcode + dst(u8) + obj(u8) + met(u8) + mcache_idx(u16) = 6 bytes
       * (note: mcache_idx stays u16 even though the others are u8). */
      if (i + 6 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!have_sbc || !jit_rt_mcall_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t dst    = (uint64_t)bc[i + 1];
      uint64_t obj    = (uint64_t)bc[i + 2];
      uint64_t met    = (uint64_t)bc[i + 3];
      uint64_t mcidx  = (uint64_t)bc_rd16(bc + i + 4);
      uint64_t packed = (mcidx << 48) | (met << 32) | (obj << 16) | dst;
      b->pending_helper_id = JIT_HELPER_MCALL;
      jit_emit_call_with_sbc(b, (void*)jit_rt_mcall_helper, packed);
      i += 6;
      break;
    }

    case BC_COPY: {
      /* SBC_COPY: opcode + dst(u16) + src(u16) + dindex(u16) +
       * sindex(u16).  Copies one slot from L[src]'s sindex-th
       * field to L[dst]'s dindex-th field via LSET+LGET (lsetm
       * write barrier).  Pack the two 16-bit field indices into
       * one 32-bit arg so the call fits helper3.  Layout:
       *   [31:16] = dindex
       *   [15: 0] = sindex
       */
      if (i + 9 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_copy_helper) { jit_last_fail_opcode = op;
                                 jit_last_fail_offset = i;
                                 fail = 1; goto done; }
      uint32_t dst    = (uint32_t)bc_rd16(bc + i + 1);
      uint32_t src    = (uint32_t)bc_rd16(bc + i + 3);
      uint32_t dindex = (uint32_t)bc_rd16(bc + i + 5);
      uint32_t sindex = (uint32_t)bc_rd16(bc + i + 7);
      uint32_t packed = (dindex << 16) | sindex;
      b->pending_helper_id = JIT_HELPER_COPY;
      jit_emit_call_helper3(b, (void*)jit_rt_copy_helper, dst, src, packed);
      i += 9;
      break;
    }

    case BC_CLOSURE: {
      /* SBC_CLOSURE: opcode + dst(u16) + idx(u16) + size(u8).
       * Allocates a closure object referencing sbc->hooks[idx].
       * Only available when the JIT'd function carries the sbc
       * pointer (have_sbc) -- the 1-arg path can't reach hooks[]. */
      if (i + 6 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!have_sbc || !jit_rt_closure_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t dst  = (uint64_t)bc_rd16(bc + i + 1);
      uint64_t idx  = (uint64_t)bc_rd16(bc + i + 3);
      uint64_t size = (uint64_t)bc[i + 5];
      /* Pack into one 64-bit immediate so the call fits in three
       * integer-arg registers (L, sbc, packed) without needing
       * stack args on Win64.  Layout:
       *   [63:32] = dst   (16 bits used, zero-extended)
       *   [31:16] = idx
       *   [15: 0] = size
       */
      uint64_t packed = (dst << 32) | (idx << 16) | (size & 0xFF);
      b->pending_helper_id = JIT_HELPER_CLOSURE;
      jit_emit_call_with_sbc(b, (void*)jit_rt_closure_helper, packed);
      i += 6;
      break;
    }

    case BC_MOVE4: {
      /* SBC_MOVE4: 1 operand byte holds dst (low 4 bits) and
       * src (high 4 bits) -- both 4-bit slot indices into
       * L[0..15], the hot range that gets register-allocated
       * to register slots in tight loops. */
      if (i + 2 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      uint8_t opr = bc[i + 1];
      int dst = opr & 0xF;
      int src = opr >> 4;
      emit_mov_rax_from_slot(b, src);
      emit_mov_slot_from_rax(b, dst);
      i += 2;
      break;
    }

    case BC_MOVENO: {
      /* SBC_MOVENO: opcode + dst(u16); L[dst] = No.
       * No = MKIMM(T_NO=14, 0) = (0 << GID_SHFT=16) | (14 << FLG_BITS=1) = 28
       * (i.e. the constant 0x1C as a tagged dyn). */
      if (i + 3 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      int dst = bc_rd16(bc + i + 1);
      emit_mov_rax_imm64(b, 0x1C);
      emit_mov_slot_from_rax(b, dst);
      i += 3;
      break;
    }

    case BC_MOVEEMT: {
      /* SBC_MOVEEMT: opcode + dst(u16); L[dst] = Empty (heap
       * singleton).  Helper avoids binding `&api.empty_` into
       * the JIT module. */
      if (i + 3 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_moveemt_helper) { jit_last_fail_opcode = op;
                                    jit_last_fail_offset = i;
                                    fail = 1; goto done; }
      uint32_t dst = (uint32_t)bc_rd16(bc + i + 1);
      b->pending_helper_id = JIT_HELPER_MOVEEMT;
      jit_emit_call_helper3(b, (void*)jit_rt_moveemt_helper, dst, 0, 0);
      i += 3;
      break;
    }

    case BC_FATAL: {
      /* SBC_FATAL: opcode + msg(u16); calls fatal((char*)L[msg]),
       * which longjmps.  Windows SEH unwind on the JIT'd frame is
       * registered (step 5n) so the unwind walks through. */
      if (i + 3 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!jit_rt_fatal_helper) { jit_last_fail_opcode = op;
                                  jit_last_fail_offset = i;
                                  fail = 1; goto done; }
      uint32_t msg = (uint32_t)bc_rd16(bc + i + 1);
      b->pending_helper_id = JIT_HELPER_FATAL;
      jit_emit_call_helper3(b, (void*)jit_rt_fatal_helper, msg, 0, 0);
      i += 3;
      break;
    }

    case BC_MOVETX: {
      /* SBC_MOVETX: opcode + dst(u16) + src(u24); 6 bytes.
       * L[dst] = sbc->tx[src] -- text-constant table lookup.
       * Pack dst (16) + src (24) into one 40-bit imm. */
      if (i + 6 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!have_sbc || !jit_rt_movetx_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t dst = (uint64_t)bc_rd16(bc + i + 1);
      uint64_t src = (uint64_t)bc_rd24(bc + i + 3);
      uint64_t packed = (dst << 32) | src;
      b->pending_helper_id = JIT_HELPER_MOVETX;
      jit_emit_call_with_sbc(b, (void*)jit_rt_movetx_helper, packed);
      i += 6;
      break;
    }
    case BC_MOVETX8: {
      /* opcode + dst(u8) + src(u8) = 3 bytes. */
      if (i + 3 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!have_sbc || !jit_rt_movetx_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t dst = bc[i + 1];
      uint64_t src = bc[i + 2];
      uint64_t packed = (dst << 32) | src;
      b->pending_helper_id = JIT_HELPER_MOVETX;
      jit_emit_call_with_sbc(b, (void*)jit_rt_movetx_helper, packed);
      i += 3;
      break;
    }

    case BC_MOVEIM: {
      /* SBC_MOVEIM: opcode + dst(u16) + src(u24) = 6 bytes.
       * L[dst] = sbc->im[src] -- per-SBC imported-symbol lookup.
       * Pack dst (16) + src (24). */
      if (i + 6 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      if (!have_sbc || !jit_rt_moveim_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t dst = (uint64_t)bc_rd16(bc + i + 1);
      uint64_t src = (uint64_t)bc_rd24(bc + i + 3);
      uint64_t packed = (src << 16) | dst;
      b->pending_helper_id = JIT_HELPER_MOVEIM;
      jit_emit_call_with_sbc(b, (void*)jit_rt_moveim_helper, packed);
      i += 6;
      break;
    }

    case BC_MOVEMT: case BC_MOVEMT8: {
      /* SBC_MOVEMT (0x1F): opcode + dst(u16) + src(u24) = 6 bytes.
       * SBC_MOVEMT8 (0x20): opcode + dst(u8) + src(u8)  = 3 bytes.
       * Both: L[dst] = FXN(sbc->mt[src]).  Same packed layout as
       * MOVEIM ([15:0]=dst, [39:16]=src); a single shared helper
       * handles either wire width. */
      int wide = (op == BC_MOVEMT);
      int op_len = wide ? 6 : 3;
      if (i + op_len > n) { jit_last_fail_opcode = op;
                            jit_last_fail_offset = i;
                            fail = 1; goto done; }
      if (!have_sbc || !jit_rt_movemt_helper) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      uint64_t dst, src;
      if (wide) {
        dst = (uint64_t)bc_rd16(bc + i + 1);
        src = (uint64_t)bc_rd24(bc + i + 3);
      } else {
        dst = bc[i + 1];
        src = bc[i + 2];
      }
      uint64_t packed = (src << 16) | dst;
      b->pending_helper_id = JIT_HELPER_MOVEMT;
      jit_emit_call_with_sbc(b, (void*)jit_rt_movemt_helper, packed);
      i += op_len;
      break;
    }

    case BC_FXT8: case BC_FXT16: case BC_FXT24:
    case BC_FXT32: case BC_FXT40: case BC_FXT48: case BC_FXT56: {
      /* SBC_FXT*: load FIXTEXT-tagged immediate into L[dst].
       *   FIXTEXT(x) = MKIMM(T_FIXTEXT=2, x) = (x << 16) | (2 << 1)
       *              = (x << 16) | 4
       * Inline as 10-byte mov rax, imm64 + slot store. */
      int width;  /* immediate width in bytes */
      switch (op) {
        case BC_FXT8:  width = 1; break;
        case BC_FXT16: width = 2; break;
        case BC_FXT24: width = 3; break;
        case BC_FXT32: width = 4; break;
        case BC_FXT40: width = 5; break;
        case BC_FXT48: width = 6; break;
        default:       width = 7; break;  /* FXT56 */
      }
      if (i + 2 + width > n) { jit_last_fail_opcode = op;
                               jit_last_fail_offset = i;
                               fail = 1; goto done; }
      int dst = bc[i + 1];
      uint64_t imm = 0;
      for (int k = 0; k < width; k++)
        imm |= (uint64_t)bc[i + 2 + k] << (k * 8);
      uint64_t tagged = (imm << 16) | 4;
      emit_mov_rax_imm64(b, tagged);
      emit_mov_slot_from_rax(b, dst);
      i += 2 + width;
      break;
    }

    case BC_IADD: case BC_ISUB: case BC_IMUL: case BC_IDIV:
    case BC_IREM: case BC_ILT:  case BC_IGT:  case BC_ILTE: case BC_IGTE:
    case BC_SAME: case BC_VARY: {
      /* SAME/VARY join the I* block because they share the
       * 7-byte (opcode + 3*u16) wire shape and inline as a
       * single cmp + setcc + movzx + shl + store sequence --
       * no runtime helper needed. */
      if (i + 7 > n) { fail = 1; goto done; }
      int dst = bc_rd16(bc + i + 1);
      int a   = bc_rd16(bc + i + 3);
      int x   = bc_rd16(bc + i + 5);
      switch (op) {
        case BC_IADD: jit_emit_iadd(b, dst, a, x); break;
        case BC_ISUB: jit_emit_isub(b, dst, a, x); break;
        case BC_IMUL: jit_emit_imul(b, dst, a, x); break;
        case BC_IDIV: jit_emit_idiv(b, dst, a, x); break;
        case BC_IREM: jit_emit_irem(b, dst, a, x); break;
        case BC_ILT:  jit_emit_ilt (b, dst, a, x); break;
        case BC_IGT:  jit_emit_igt (b, dst, a, x); break;
        case BC_ILTE: jit_emit_ilte(b, dst, a, x); break;
        case BC_IGTE: jit_emit_igte(b, dst, a, x); break;
        case BC_SAME: jit_emit_same(b, dst, a, x); break;
        case BC_VARY: jit_emit_vary(b, dst, a, x); break;
      }
      i += 7;
      break;
    }

    case BC_FXNADD: case BC_FXNSUB: case BC_FXNMUL:
    case BC_FXNDIV: case BC_FXNREM:
    case BC_FXNLT:  case BC_FXNGT:  case BC_FXNLTE: case BC_FXNGTE: {
      /* Same wire shape as the I* family: opcode + dst(u16) +
       * a(u16) + b(u16) = 7 bytes.  The interpreter's body for
       * each of these is:
       *   if (TAGIS(T_INT, L[a]) && TAGIS(T_INT, L[b]))
       *     <typed-arith>(L[dst], L[a], L[b]);
       *   else
       *     ARGLIST2(L[a],L[b]); MCALL(L[dst], L[a], m_*);
       *
       * The JIT inlines the fast path (typed arith using the
       * existing jit_emit_iadd/isub/imul/idiv/irem/ilt/...
       * primitives, same code IADD/ILT emit) and only falls
       * through to a helper call on the non-int slow path.
       * For a tight counted loop the per-iter savings are
       * roughly one helper call (~3-5 ns) per arith op. */
      if (i + 7 > n) { fail = 1; goto done; }
      uint32_t dst = (uint32_t)bc_rd16(bc + i + 1);
      uint32_t a   = (uint32_t)bc_rd16(bc + i + 3);
      uint32_t x   = (uint32_t)bc_rd16(bc + i + 5);
      void *helper; jit_helper_id_t hid;
      switch (op) {
        case BC_FXNADD: helper = (void*)jit_rt_fxnadd_helper; hid = JIT_HELPER_FXNADD; break;
        case BC_FXNSUB: helper = (void*)jit_rt_fxnsub_helper; hid = JIT_HELPER_FXNSUB; break;
        case BC_FXNMUL: helper = (void*)jit_rt_fxnmul_helper; hid = JIT_HELPER_FXNMUL; break;
        case BC_FXNDIV: helper = (void*)jit_rt_fxndiv_helper; hid = JIT_HELPER_FXNDIV; break;
        case BC_FXNREM: helper = (void*)jit_rt_fxnrem_helper; hid = JIT_HELPER_FXNREM; break;
        case BC_FXNLT:  helper = (void*)jit_rt_fxnlt_helper;  hid = JIT_HELPER_FXNLT;  break;
        case BC_FXNGT:  helper = (void*)jit_rt_fxngt_helper;  hid = JIT_HELPER_FXNGT;  break;
        case BC_FXNLTE: helper = (void*)jit_rt_fxnlte_helper; hid = JIT_HELPER_FXNLTE; break;
        case BC_FXNGTE: helper = (void*)jit_rt_fxngte_helper; hid = JIT_HELPER_FXNGTE; break;
        default: helper = NULL; hid = JIT_HELPER_NONE;  /* unreachable */
      }
      if (!helper) { jit_last_fail_opcode = op;
                     jit_last_fail_offset = i;
                     fail = 1; goto done; }

      /* Tag-check: if (L[a] | L[b]) has any tag bits set, take
       * the slow path.  Combining via OR means one test covers
       * both operands -- low-16 bits stay zero iff both are
       * T_INT (FXN(x) = x<<16). */
      emit_mov_rax_from_slot(b, a);
      emit_or_rax_from_slot(b, x);
      emit_test_ax_ax(b);
      size_t to_slow = emit_jnz_rel32(b);

      /* Fast path: emit the typed arith / cmp.  jit_emit_iadd
       * etc. reload from slots so the OR-clobbered rax isn't a
       * problem. */
      switch (op) {
        case BC_FXNADD: jit_emit_iadd(b, dst, a, x); break;
        case BC_FXNSUB: jit_emit_isub(b, dst, a, x); break;
        case BC_FXNMUL: jit_emit_imul(b, dst, a, x); break;
        case BC_FXNDIV: jit_emit_idiv(b, dst, a, x); break;
        case BC_FXNREM: jit_emit_irem(b, dst, a, x); break;
        case BC_FXNLT:  jit_emit_ilt (b, dst, a, x); break;
        case BC_FXNGT:  jit_emit_igt (b, dst, a, x); break;
        case BC_FXNLTE: jit_emit_ilte(b, dst, a, x); break;
        case BC_FXNGTE: jit_emit_igte(b, dst, a, x); break;
      }
      size_t to_done = jit_emit_jmp(b);

      /* Slow path. */
      jit_patch_jmp_here(b, to_slow);
      b->pending_helper_id = hid;
      jit_emit_call_helper3(b, helper, dst, a, x);
      jit_patch_jmp_here(b, to_done);
      i += 7;
      break;
    }

    case BC_FXNAND: case BC_FXNIOR: case BC_FXNXOR:
    case BC_FXNSHL: case BC_FXNSHR: {
      /* Bitwise ops -- same wire shape as the FXN-arith family
       * (7 bytes) but routed straight to a helper.  T_INT-T_INT
       * fast path is in the helper; an inline fast path is
       * possible (one x86 AND/OR/XOR/SHL/SHR after the tag check)
       * but the call cost dominates only on bitwise-heavy code,
       * which we don't currently see.  Helper for now. */
      if (i + 7 > n) { fail = 1; goto done; }
      void *helper; jit_helper_id_t hid;
      switch (op) {
        case BC_FXNAND: helper = (void*)jit_rt_fxnand_helper; hid = JIT_HELPER_FXNAND; break;
        case BC_FXNIOR: helper = (void*)jit_rt_fxnior_helper; hid = JIT_HELPER_FXNIOR; break;
        case BC_FXNXOR: helper = (void*)jit_rt_fxnxor_helper; hid = JIT_HELPER_FXNXOR; break;
        case BC_FXNSHL: helper = (void*)jit_rt_fxnshl_helper; hid = JIT_HELPER_FXNSHL; break;
        case BC_FXNSHR: helper = (void*)jit_rt_fxnshr_helper; hid = JIT_HELPER_FXNSHR; break;
        default: helper = NULL; hid = JIT_HELPER_NONE;
      }
      if (!helper) { jit_last_fail_opcode = op;
                     jit_last_fail_offset = i;
                     fail = 1; goto done; }
      uint32_t dst = (uint32_t)bc_rd16(bc + i + 1);
      uint32_t a   = (uint32_t)bc_rd16(bc + i + 3);
      uint32_t x   = (uint32_t)bc_rd16(bc + i + 5);
      b->pending_helper_id = hid;
      jit_emit_call_helper3(b, helper, dst, a, x);
      i += 7;
      break;
    }

    case BC_FXNTAG: case BC_NOT: case BC_GOT: case BC_NO:
    case BC_NEG: case BC_ABS: {
      /* All 4 share: opcode + dst(u16) + src(u16) = 5 bytes.
       * FXNTAG returns FXN(O_TAG(src)); NOT/GOT/NO return FXN(0)
       * or FXN(1) based on truthy / No comparison.  Pure
       * helper-call trampolines -- no fast-path inline. */
      if (i + 5 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      uint32_t dst = (uint32_t)bc_rd16(bc + i + 1);
      uint32_t src = (uint32_t)bc_rd16(bc + i + 3);
      void *helper; jit_helper_id_t hid;
      switch (op) {
        case BC_FXNTAG: helper = (void*)jit_rt_fxntag_helper; hid = JIT_HELPER_FXNTAG; break;
        case BC_NOT:    helper = (void*)jit_rt_not_helper;    hid = JIT_HELPER_NOT;    break;
        case BC_GOT:    helper = (void*)jit_rt_got_helper;    hid = JIT_HELPER_GOT;    break;
        case BC_NO:     helper = (void*)jit_rt_no_helper;     hid = JIT_HELPER_NO;     break;
        case BC_NEG:    helper = (void*)jit_rt_neg_helper;    hid = JIT_HELPER_NEG;    break;
        case BC_ABS:    helper = (void*)jit_rt_abs_helper;    hid = JIT_HELPER_ABS;    break;
        default: helper = NULL; hid = JIT_HELPER_NONE;
      }
      if (!helper) { jit_last_fail_opcode = op;
                     jit_last_fail_offset = i;
                     fail = 1; goto done; }
      b->pending_helper_id = hid;
      jit_emit_call_helper3(b, helper, dst, src, 0);
      i += 5;
      break;
    }

    case BC_INC: case BC_DEC: {
      /* SBC_INC / SBC_DEC: typed +/-1 with MCALL fallback.
       * Inline the T_INT fast path so per-iteration overhead in
       * counted loops is a single ADD instruction instead of a
       * call into jit_rt_inc_helper.  Bytecode wire shape:
       * opcode + dst(u16) + a(u16) = 5 bytes.
       *
       * Inline x86 (8 instructions, ~32 bytes):
       *   mov  rax, [rbx + a*8]      ; load aa
       *   test ax, ax                 ; tag check (T_INT == 0)
       *   jnz  slow                   ; non-int -> helper
       *   add  rax, +/- 0x10000       ; FXN(1) for INC, -FXN(1) for DEC
       *   mov  [rbx + dst*8], rax
       *   jmp  done
       * slow:
       *   ... helper call ...         ; ~30 bytes
       * done:
       *
       * The fast path is roughly 6 cycles vs ~5-10 ns for the
       * helper call.  In a tight counted loop (10^9 iters) the
       * difference is ~5 s of wall time. */
      if (i + 5 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      void *helper; jit_helper_id_t hid;
      int32_t delta;
      switch (op) {
        case BC_INC: helper = (void*)jit_rt_inc_helper;
                     hid = JIT_HELPER_INC;
                     delta = 0x10000;    /* FXN(1) */
                     break;
        case BC_DEC: helper = (void*)jit_rt_dec_helper;
                     hid = JIT_HELPER_DEC;
                     delta = -0x10000;   /* -FXN(1) */
                     break;
        default: helper = NULL; hid = JIT_HELPER_NONE; delta = 0;
      }
      if (!helper) { jit_last_fail_opcode = op;
                     jit_last_fail_offset = i;
                     fail = 1; goto done; }
      uint32_t dst = (uint32_t)bc_rd16(bc + i + 1);
      uint32_t a   = (uint32_t)bc_rd16(bc + i + 3);

      emit_mov_rax_from_slot(b, a);
      emit_test_ax_ax(b);
      size_t to_slow = emit_jnz_rel32(b);
      /* Fast path: aa is FXN-tagged int. */
      emit_add_rax_imm32(b, delta);
      emit_mov_slot_from_rax(b, dst);
      size_t to_done = jit_emit_jmp(b);
      /* Slow path. */
      jit_patch_jmp_here(b, to_slow);
      b->pending_helper_id = hid;
      jit_emit_call_helper3(b, helper, dst, a, 0);
      jit_patch_jmp_here(b, to_done);
      i += 5;
      break;
    }

    case BC_JMP: {
      /* SBC_JMP: opcode + 24-bit absolute bytecode offset. */
      if (i + 4 > n) { fail = 1; goto done; }
      uint32_t target = bc_rd24(bc + i + 1);
      if (target >= n) { fail = 1; goto done; }
      size_t patch_off = jit_emit_jmp(b);
      if (patches_n >= JIT_MAX_PATCHES) { fail = 1; goto done; }
      patches[patches_n].jit_off = patch_off;
      patches[patches_n].bc_target = target;
      patches_n++;
      i += 4;
      break;
    }

    case BC_JMP16: {
      /* SBC_JMP16: opcode + signed 16-bit PC-relative diff.
       * Target = (end-of-instruction in bc) + diff. */
      if (i + 3 > n) { fail = 1; goto done; }
      int16_t diff = (int16_t)(uint16_t)bc_rd16(bc + i + 1);
      int64_t target = (int64_t)i + 3 + diff;
      if (target < 0 || (uint64_t)target > n) { fail = 1; goto done; }
      size_t patch_off = jit_emit_jmp(b);
      if (patches_n >= JIT_MAX_PATCHES) { fail = 1; goto done; }
      patches[patches_n].jit_off = patch_off;
      patches[patches_n].bc_target = (size_t)target;
      patches_n++;
      i += 3;
      break;
    }

    case BC_B: {
      /* SBC_B: opcode + 16-bit cnd slot + 24-bit absolute target. */
      if (i + 6 > n) { fail = 1; goto done; }
      int cnd = bc_rd16(bc + i + 1);
      uint32_t target = bc_rd24(bc + i + 3);
      if (target >= n) { fail = 1; goto done; }
      /* SBC_B branches WHEN cnd is truthy; jnz_slot matches that. */
      size_t patch_off = jit_emit_jnz_slot(b, cnd);
      if (patches_n >= JIT_MAX_PATCHES) { fail = 1; goto done; }
      patches[patches_n].jit_off = patch_off;
      patches[patches_n].bc_target = target;
      patches_n++;
      i += 6;
      break;
    }

    case BC_B8: {
      /* SBC_B8: opcode + 8-bit cnd slot + signed 16-bit PC-relative diff. */
      if (i + 4 > n) { fail = 1; goto done; }
      int cnd = bc[i + 1];
      int16_t diff = (int16_t)(uint16_t)bc_rd16(bc + i + 2);
      int64_t target = (int64_t)i + 4 + diff;
      if (target < 0 || (uint64_t)target > n) { fail = 1; goto done; }
      size_t patch_off = jit_emit_jnz_slot(b, cnd);
      if (patches_n >= JIT_MAX_PATCHES) { fail = 1; goto done; }
      patches[patches_n].jit_off = patch_off;
      patches[patches_n].bc_target = (size_t)target;
      patches_n++;
      i += 4;
      break;
    }

    case BC_IFFXN: {
      /* SBC_IFFXN: opcode + cnd(u8) + diff(int16) = 4 bytes.
       * Branches when O_TAG(L[cnd]) == 0 -- i.e. the value is
       * an int (T_INT=0).  Test by ANDing the low 16 bits with
       * TAG_MASK (0xFFFE) and checking ZF.
       *
       * x86:  mov rax, [rbx + cnd*8]      ; load value
       *       test eax, 0xfffe            ; check tag bits
       *       jz <target>                 ; branch if int */
      if (i + 4 > n) { jit_last_fail_opcode = op;
                       jit_last_fail_offset = i;
                       fail = 1; goto done; }
      int cnd = bc[i + 1];
      int16_t diff = (int16_t)(uint16_t)bc_rd16(bc + i + 2);
      int64_t target = (int64_t)i + 4 + diff;
      if (target < 0 || (uint64_t)target > n) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      emit_mov_rax_from_slot(b, cnd);
      /* test eax, 0xfffe : a9 fe ff 00 00  (5 bytes; tests low 16
       * bits while leaving the upper 48 untouched -- they don't
       * affect the AND result). */
      jit_emit_u8(b, 0xa9);
      jit_emit_u32(b, 0x0000FFFE);
      /* jz rel32 : 0f 84 disp32, 6 bytes total. */
      jit_emit_u8(b, 0x0f);
      jit_emit_u8(b, 0x84);
      size_t patch_off = b->len;
      jit_emit_u32(b, 0);
      if (patches_n >= JIT_MAX_PATCHES) {
        jit_last_fail_opcode = op;
        jit_last_fail_offset = i;
        fail = 1; goto done;
      }
      patches[patches_n].jit_off = patch_off;
      patches[patches_n].bc_target = (size_t)target;
      patches_n++;
      i += 4;
      break;
    }

    case BC_FXNB0: {
      if (i + 2 > n) { fail = 1; goto done; }
      int dst = bc[i + 1];
      emit_fxn_imm(b, dst, 0);
      i += 2;
      break;
    }
    case BC_FXNB8: {
      if (i + 3 > n) { fail = 1; goto done; }
      int dst = bc[i + 1];
      int64_t imm = (int8_t)bc[i + 2];  /* sign-extend */
      emit_fxn_imm(b, dst, imm);
      i += 3;
      break;
    }
    case BC_FXNB16: {
      if (i + 4 > n) { fail = 1; goto done; }
      int dst = bc[i + 1];
      int64_t imm = (int16_t)(uint16_t)bc_rd16(bc + i + 2);
      emit_fxn_imm(b, dst, imm);
      i += 4;
      break;
    }
    case BC_FXNB32: {
      if (i + 6 > n) { fail = 1; goto done; }
      int dst = bc[i + 1];
      uint32_t u = (uint32_t)bc[i + 2]
                 | ((uint32_t)bc[i + 3] << 8)
                 | ((uint32_t)bc[i + 4] << 16)
                 | ((uint32_t)bc[i + 5] << 24);
      int64_t imm = (int32_t)u;
      emit_fxn_imm(b, dst, imm);
      i += 6;
      break;
    }
    case BC_FXN0: {
      if (i + 3 > n) { fail = 1; goto done; }
      int dst = bc_rd16(bc + i + 1);
      emit_fxn_imm(b, dst, 0);
      i += 3;
      break;
    }
    case BC_FXN8: {
      if (i + 4 > n) { fail = 1; goto done; }
      int dst = bc_rd16(bc + i + 1);
      int64_t imm = (int8_t)bc[i + 3];
      emit_fxn_imm(b, dst, imm);
      i += 4;
      break;
    }
    case BC_FXN16: {
      if (i + 5 > n) { fail = 1; goto done; }
      int dst = bc_rd16(bc + i + 1);
      int64_t imm = (int16_t)(uint16_t)bc_rd16(bc + i + 3);
      emit_fxn_imm(b, dst, imm);
      i += 5;
      break;
    }
    case BC_FXN32: {
      if (i + 7 > n) { fail = 1; goto done; }
      int dst = bc_rd16(bc + i + 1);
      uint32_t u = (uint32_t)bc[i + 3]
                 | ((uint32_t)bc[i + 4] << 8)
                 | ((uint32_t)bc[i + 5] << 16)
                 | ((uint32_t)bc[i + 6] << 24);
      int64_t imm = (int32_t)u;
      emit_fxn_imm(b, dst, imm);
      i += 7;
      break;
    }

    case 0x2F: case 0x30: {
      /* SBC_IMMB64 (0x2F): opcode + dst(u8) + imm(u64) = 10 bytes.
       * SBC_IMM64  (0x30): opcode + dst(u16) + imm(u64) = 11 bytes.
       * Body: L[dst] = (dyn)imm -- the imm is a pre-tagged 64-bit
       * value (the compiler bakes in the dyn-format constant).
       * Inline as `mov rax, imm64` + slot store; 17 bytes per site
       * vs ~30 bytes for a helper call, AND no call overhead. */
      int wide = (op == 0x30);
      int op_len = wide ? 11 : 10;
      if (i + op_len > n) { fail = 1; goto done; }
      int dst;
      uint64_t imm;
      if (wide) {
        dst = bc_rd16(bc + i + 1);
        imm = (uint64_t)bc[i + 3]
            | ((uint64_t)bc[i + 4]  << 8)
            | ((uint64_t)bc[i + 5]  << 16)
            | ((uint64_t)bc[i + 6]  << 24)
            | ((uint64_t)bc[i + 7]  << 32)
            | ((uint64_t)bc[i + 8]  << 40)
            | ((uint64_t)bc[i + 9]  << 48)
            | ((uint64_t)bc[i + 10] << 56);
      } else {
        dst = bc[i + 1];
        imm = (uint64_t)bc[i + 2]
            | ((uint64_t)bc[i + 3] << 8)
            | ((uint64_t)bc[i + 4] << 16)
            | ((uint64_t)bc[i + 5] << 24)
            | ((uint64_t)bc[i + 6] << 32)
            | ((uint64_t)bc[i + 7] << 40)
            | ((uint64_t)bc[i + 8] << 48)
            | ((uint64_t)bc[i + 9] << 56);
      }
      emit_mov_rax_imm64(b, imm);
      emit_mov_slot_from_rax(b, dst);
      i += op_len;
      break;
    }

    case BC_LEAVE: {
      if (i + 3 > n) { fail = 1; goto done; }
      int src = bc_rd16(bc + i + 1);
      emit_mov_rax_from_slot(b, src);
      if (have_sbc) jit_emit_epilogue2(b);
      else          jit_emit_epilogue(b);
      i += 3;
      break;
    }

    case BC_LEAVE0:
      emit_xor_rax_rax(b);
      if (have_sbc) jit_emit_epilogue2(b);
      else          jit_emit_epilogue(b);
      i += 1;
      break;

    default:
      /* Unsupported opcode -- bail out so the caller falls back
       * to the interpreter.  Diagnostics record what we hit so
       * the audit can tally which opcodes need implementing
       * next. */
      jit_last_fail_opcode = op;
      jit_last_fail_offset = i;
      fail = 1;
      goto done;
    }
  }
  /* Mark "one past the last instruction" so a JMP/B that
   * jumps to the end of the function (defensive but unusual)
   * resolves to the position right after the body. */
  bc_to_x86[n] = b->len;

  /* If the bytecode didn't end in an explicit LEAVE the function
   * still needs a terminator.  Emit an implicit LEAVE0. */
  if (i == n) {
    /* Check the last emitted instruction wasn't already a ret
     * (LEAVE/LEAVE0 just ran).  Simplest check: if the previous
     * byte is 0xc3 we know epilogue's `ret` just landed. */
    if (b->len == 0 || b->code[b->len - 1] != 0xc3) {
      emit_xor_rax_rax(b);
      if (have_sbc) jit_emit_epilogue2(b);
      else          jit_emit_epilogue(b);
    }
  }

done:
  if (fail) {
    free(bc_to_x86);
    jit_buf_free(b);
    return NULL;
  }

  /* Resolve patches.  Backward jumps already have their targets
   * recorded; forward jumps get their targets filled in now. */
  for (size_t p = 0; p < patches_n; p++) {
    size_t tgt_bc = patches[p].bc_target;
    if (tgt_bc > n) { fail = 1; break; }
    size_t tgt_x86 = bc_to_x86[tgt_bc];
    if (tgt_x86 == (size_t)-1) { fail = 1; break; }
    jit_patch_jmp_to(b, patches[p].jit_off, tgt_x86);
  }

  free(bc_to_x86);
  if (fail) { jit_buf_free(b); return NULL; }
  return b;
}

/* Append a (offset, helper_id) record to b->relocs.  Grows the
 * array on demand (doubling).  Caller has already validated
 * b->record_relocs is set. */
static void jit_record_reloc(jit_buf *b, uint32_t imm_offset,
                             jit_helper_id_t hid) {
  if (b->relocs_count >= b->relocs_cap) {
    int newcap = b->relocs_cap ? b->relocs_cap * 2 : 16;
    jit_reloc_t *neu = (jit_reloc_t*)realloc(b->relocs,
                          (size_t)newcap * sizeof(jit_reloc_t));
    if (!neu) {
      fprintf(stderr, "jit_record_reloc: realloc failed (cap=%d)\n", newcap);
      abort();
    }
    b->relocs = neu;
    b->relocs_cap = newcap;
  }
  b->relocs[b->relocs_count].offset    = imm_offset;
  b->relocs[b->relocs_count].helper_id = (uint8_t)hid;
  b->relocs[b->relocs_count].pad[0] = 0;
  b->relocs[b->relocs_count].pad[1] = 0;
  b->relocs[b->relocs_count].pad[2] = 0;
  b->relocs_count++;
}

void jit_emit_call_abs(jit_buf *b, void *target) {
  /* Two-instruction call: load the absolute address into RAX
   * via `mov rax, imm64` (10 bytes) then `call rax` (2 bytes).
   * Total 12 bytes -- longer than a relative call but works
   * for any target regardless of its distance from the JIT'd
   * code.
   *
   * Phase 2a register allocation: spill every pinned register
   * to its L[slot] memory cell BEFORE the call so the helper
   * sees the latest value if it dereferences L[slot] directly,
   * and reload AFTER the call so any helper updates to L[]
   * (including GC moves of heap refs the helper triggered) are
   * visible in the in-register copies.  R13..R15 are
   * callee-saved per Win64 + SysV so the register bytes
   * themselves survive the call; the spill/reload protects
   * against the helper inspecting L[] through its `int64_t *L`
   * argument.
   *
   * The spill/reload is a no-op when b->pinned_count == 0
   * (the loops in emit_spill_pinned / emit_reload_pinned
   * iterate over a zero-length array).
   *
   * Reloc recording: when b->record_relocs is on, we log the
   * imm64's byte offset together with b->pending_helper_id so
   * the loader can patch this site without re-translating the
   * bytecode.  The pending_helper_id is set by the wrapper
   * function (jit_emit_call_helper3 / jit_emit_call_with_sbc
   * or the arith family) right before this call.  If a caller
   * forgets to set it, we still log the offset but with
   * JIT_HELPER_NONE so the writer can flag the missing
   * mapping at build time. */
  emit_spill_pinned(b);
  uint64_t v = (uint64_t)target;
  uint32_t imm_off = (uint32_t)(b->len + 2);  /* 2 bytes of REX+opcode prefix */
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0xb8);  /* mov rax, imm64 (opcode b8+reg, rax=0) */
  for (int i = 0; i < 8; i++) jit_emit_u8(b, (uint8_t)(v >> (i*8)));
  /* call rax :  ff d0 (ModR/M 11 010 000, /2=call, r/m=rax) */
  jit_emit_u8(b, 0xff);
  jit_emit_u8(b, 0xd0);
  if (b->record_relocs) {
    jit_record_reloc(b, imm_off, b->pending_helper_id);
    b->pending_helper_id = JIT_HELPER_NONE;
  }
  emit_reload_pinned(b);
}

/* Step 5c helpers: per-platform mov r32, imm32 emitters.
 *
 * The opcode is b8+reg for the basic 8 GPRs (eax..edi).  The
 * R8..R15 variants need a REX.B prefix (0x41).  Writing to a
 * 32-bit subregister zero-extends to the full 64-bit reg, so
 * slot indices stay positive in the high half. */
#ifdef _WIN32
/* mov ecx, imm32  (arg1 register: actually arg0 on Win64).
 * Used by emit_mov_arg0_from_locals; not used here for arg
 * setup -- helpers below set RCX/RDX/R8/R9. */
static void emit_mov_arg1_imm32(jit_buf *b, uint32_t imm) {
  /* mov edx, imm32 : ba ll ll ll ll */
  jit_emit_u8(b, 0xba);
  jit_emit_u32(b, imm);
}
static void emit_mov_arg2_imm32(jit_buf *b, uint32_t imm) {
  /* mov r8d, imm32 : 41 b8 ll ll ll ll  (REX.B for R8) */
  jit_emit_u8(b, 0x41);
  jit_emit_u8(b, 0xb8);
  jit_emit_u32(b, imm);
}
static void emit_mov_arg3_imm32(jit_buf *b, uint32_t imm) {
  /* mov r9d, imm32 : 41 b9 ll ll ll ll  (REX.B for R9) */
  jit_emit_u8(b, 0x41);
  jit_emit_u8(b, 0xb9);
  jit_emit_u32(b, imm);
}
#else
/* SysV: arg1=RSI, arg2=RDX, arg3=RCX. */
static void emit_mov_arg1_imm32(jit_buf *b, uint32_t imm) {
  /* mov esi, imm32 : be ll ll ll ll */
  jit_emit_u8(b, 0xbe);
  jit_emit_u32(b, imm);
}
static void emit_mov_arg2_imm32(jit_buf *b, uint32_t imm) {
  /* mov edx, imm32 : ba ll ll ll ll */
  jit_emit_u8(b, 0xba);
  jit_emit_u32(b, imm);
}
static void emit_mov_arg3_imm32(jit_buf *b, uint32_t imm) {
  /* mov ecx, imm32 : b9 ll ll ll ll */
  jit_emit_u8(b, 0xb9);
  jit_emit_u32(b, imm);
}
#endif

/* ============================================================
 * Step 5g: 2-arg prologue/epilogue + SBC-pointer plumbing.
 *
 * The 2-arg form is identical to the 1-arg one except it also
 * saves R12 (callee-saved) and loads R12 with arg1 (the sbc_t
 * pointer).  RBX holds L, R12 holds sbc, both survive across
 * inner C calls without per-call save.
 *
 * Stack alignment math:
 *   entry:           RSP%16 == 8     (CALL pushed 8)
 *   push rbx (-8):   RSP%16 == 0
 *   push r12 (-8):   RSP%16 == 8
 *   sub rsp, 40:     RSP%16 == 0    <- ready for inner CALL
 *                                       (Win64 needs +32 shadow,
 *                                        the extra 8 is alignment)
 * ============================================================ */

void jit_emit_prologue2(jit_buf *b) {
  /* push rbx              :  53                        */
  jit_emit_u8(b, 0x53);
  /* push r12              :  41 54  (REX.B + push)     */
  jit_emit_u8(b, 0x41);
  jit_emit_u8(b, 0x54);
  if (JIT_PHASE2A_HAS_PINS(b)) {
    emit_push_r13_15(b, 13);
    emit_push_r13_15(b, 14);
    emit_push_r13_15(b, 15);
  }
  /* mov rbx, <arg0>       :  48 89 cb (Win) / fb (SysV) */
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x89);
#ifdef _WIN32
  jit_emit_u8(b, 0xcb);  /* mov rbx, rcx -- ModR/M 11 001 011 */
#else
  jit_emit_u8(b, 0xfb);  /* mov rbx, rdi -- ModR/M 11 111 011 */
#endif
  /* mov r12, <arg1>       :  49 89 d4 (Win) / f4 (SysV)
   *   REX = 0100 W R X B = 0100 1 0 0 1 = 0x49
   *   opcode 89 (mov r/m64, r64)
   *   Win64:  ModR/M 11 010 100  (reg=rdx,    r/m=r12)
   *   SysV:   ModR/M 11 110 100  (reg=rsi,    r/m=r12) */
  jit_emit_u8(b, 0x49);
  jit_emit_u8(b, 0x89);
#ifdef _WIN32
  jit_emit_u8(b, 0xd4);
#else
  jit_emit_u8(b, 0xf4);
#endif
  if (JIT_PHASE2A_HAS_PINS(b)) {
    emit_reload_pinned(b);
  }
  /* sub rsp, K -- 40 with no pins (2 pushes -> %16==8, +40 makes
   * %16==0), 32 with pins (5 pushes -> %16==0, +32 keeps %16==0). */
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x83);
  jit_emit_u8(b, 0xec);
  jit_emit_u8(b, JIT_PHASE2A_HAS_PINS(b) ? 0x20 : 0x28);
}

void jit_emit_epilogue2(jit_buf *b) {
  if (JIT_PHASE2A_HAS_PINS(b)) {
    emit_spill_pinned(b);
  }
  /* add rsp, K -- mirrors prologue2's sub rsp choice. */
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x83);
  jit_emit_u8(b, 0xc4);
  jit_emit_u8(b, JIT_PHASE2A_HAS_PINS(b) ? 0x20 : 0x28);
  if (JIT_PHASE2A_HAS_PINS(b)) {
    emit_pop_r13_15(b, 15);
    emit_pop_r13_15(b, 14);
    emit_pop_r13_15(b, 13);
  }
  /* pop r12               :  41 5c */
  jit_emit_u8(b, 0x41);
  jit_emit_u8(b, 0x5c);
  /* pop rbx               :  5b */
  jit_emit_u8(b, 0x5b);
  /* ret */
  jit_emit_u8(b, 0xc3);
}

/* mov <arg1>, r12 -- copy the saved sbc pointer into the
 * platform's second integer-arg register for a helper call. */
static void emit_mov_arg1_from_sbc(jit_buf *b) {
#ifdef _WIN32
  /* mov rdx, r12 :  4c 89 e2  (REX.R=1 for r12 as reg, r/m=rdx) */
  jit_emit_u8(b, 0x4c);
  jit_emit_u8(b, 0x89);
  jit_emit_u8(b, 0xe2);
#else
  /* mov rsi, r12 :  4c 89 e6 */
  jit_emit_u8(b, 0x4c);
  jit_emit_u8(b, 0x89);
  jit_emit_u8(b, 0xe6);
#endif
}

/* mov <arg2>, imm64 -- 10 bytes.  Used by call_with_sbc to load
 * the packed (dst<<32 | idx<<16 | size) word. */
static void emit_mov_arg2_imm64(jit_buf *b, uint64_t imm) {
#ifdef _WIN32
  /* mov r8, imm64 :  49 b8 <8 bytes>  (REX.W + REX.B for r8) */
  jit_emit_u8(b, 0x49);
  jit_emit_u8(b, 0xb8);
#else
  /* mov rdx, imm64 :  48 ba <8 bytes> */
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0xba);
#endif
  for (int k = 0; k < 8; k++) jit_emit_u8(b, (uint8_t)(imm >> (k * 8)));
}

/* mov <arg3>, imm64 -- 10 bytes.  Used by call_with_sbc2 for the
 * second packed word when 4 helper args are needed. */
static void emit_mov_arg3_imm64(jit_buf *b, uint64_t imm) {
#ifdef _WIN32
  /* mov r9, imm64 :  49 b9 <8 bytes>  (REX.W + REX.B for r9) */
  jit_emit_u8(b, 0x49);
  jit_emit_u8(b, 0xb9);
#else
  /* mov rcx, imm64 :  48 b9 <8 bytes> */
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0xb9);
#endif
  for (int k = 0; k < 8; k++) jit_emit_u8(b, (uint8_t)(imm >> (k * 8)));
}

/* Emit a 3-arg helper call:
 *   arg0 = L   (from rbx)
 *   arg1 = sbc (from r12)
 *   arg2 = packed_imm (64-bit immediate)
 * The helper signature: void(*)(int64_t*L, sbc_t*sbc, uint64_t).
 * Used by SBC_CLOSURE / SBC_LIST / etc. -- opcodes whose
 * implementation needs both L and the owning sbc. */
void jit_emit_call_with_sbc(jit_buf *b, void *target, uint64_t packed) {
  emit_mov_arg2_imm64(b, packed);     /* arg2 = packed first --
                                         frees the imm-write reg
                                         before the rbx/r12 copies */
  emit_mov_arg1_from_sbc(b);          /* arg1 = sbc  */
  jit_emit_mov_arg0_from_locals(b);   /* arg0 = L    */
  jit_emit_call_abs(b, target);
}

/* Emit a 4-arg helper call:
 *   arg0 = L   (from rbx)
 *   arg1 = sbc (from r12)
 *   arg2 = packed1 (64-bit immediate)
 *   arg3 = packed2 (64-bit immediate)
 * The helper signature: void(*)(int64_t*L, sbc_t*sbc, uint64_t, uint64_t).
 * Used by FXNLSET (with-result variant) -- 5 operands (dst, src,
 * index, val, mcache_idx) overflow a single u64. */
void jit_emit_call_with_sbc2(jit_buf *b, void *target,
                             uint64_t packed1, uint64_t packed2) {
  emit_mov_arg2_imm64(b, packed1);
  emit_mov_arg3_imm64(b, packed2);
  emit_mov_arg1_from_sbc(b);
  jit_emit_mov_arg0_from_locals(b);
  jit_emit_call_abs(b, target);
}

void jit_emit_call_helper3(jit_buf *b, void *target,
                           uint32_t slot_dst, uint32_t slot_a, uint32_t slot_b) {
  /* Arg setup order matters only if the target functions touched
   * the registers we're setting up.  Since we're loading
   * IMMEDIATES (not register-to-register copies), the order is
   * irrelevant -- pick the layout that minimises register
   * dependency reads.  We load arg0 last because it depends on
   * RBX and the imm32 movs don't touch RBX. */
  emit_mov_arg1_imm32(b, slot_dst);   /* dst */
  emit_mov_arg2_imm32(b, slot_a);     /* a   */
  emit_mov_arg3_imm32(b, slot_b);     /* b   */
  jit_emit_mov_arg0_from_locals(b);   /* L   */
  jit_emit_call_abs(b, target);
}

/* ============================================================
 * Step 2: comparisons + jumps.
 * ============================================================ */

/* cmp rax, [locals_reg + slot*8] -- 48 3B + ModR/M + disp */
static void emit_cmp_rax_to_slot(jit_buf *b, int slot) {
  int r = jit_slot_reg(b, slot);
  if (r >= 0) { emit_cmp_rax_from_reg(b, r); return; }
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x3b);
  emit_mem_op(b, 0, slot);
}

/* setcc al -- one of 0F 9C/9D/9E/9F + C0 */
static void emit_setcc_al(jit_buf *b, uint8_t cc_opcode) {
  jit_emit_u8(b, 0x0f);
  jit_emit_u8(b, cc_opcode);  /* 0x9c=setl 0x9d=setge 0x9e=setle 0x9f=setg */
  jit_emit_u8(b, 0xc0);
}

/* movzx rax, al -- 48 0F B6 C0 */
static void emit_movzx_rax_al(jit_buf *b) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x0f);
  jit_emit_u8(b, 0xb6);
  jit_emit_u8(b, 0xc0);
}

/* Compare-and-FXN-store helper shared by ILT/IGT/ILTE/IGTE. */
static void emit_cmp_op(jit_buf *b, int dst, int a, int x, uint8_t setcc_opcode) {
  emit_mov_rax_from_slot(b, a);
  emit_cmp_rax_to_slot(b, x);
  emit_setcc_al(b, setcc_opcode);
  emit_movzx_rax_al(b);
  emit_shl_rax(b, 16);  /* FXN-tag the 0/1 result */
  emit_mov_slot_from_rax(b, dst);
}

void jit_emit_ilt (jit_buf *b, int dst, int a, int x) {
  emit_cmp_op(b, dst, a, x, 0x9c);  /* setl */
}
void jit_emit_igt (jit_buf *b, int dst, int a, int x) {
  emit_cmp_op(b, dst, a, x, 0x9f);  /* setg */
}
void jit_emit_ilte(jit_buf *b, int dst, int a, int x) {
  emit_cmp_op(b, dst, a, x, 0x9e);  /* setle */
}
void jit_emit_igte(jit_buf *b, int dst, int a, int x) {
  emit_cmp_op(b, dst, a, x, 0x9d);  /* setge */
}
/* SBC_SAME / SBC_VARY: pointer-identity / pointer-vary on tagged
 * dyns.  The interpreter uses the IMMEQ/IMMNE macros, which lower
 * to `FXN((a) == (b))` -- bit-identical compare.  Same x86
 * sequence as the I*-comparison family but with sete / setne. */
void jit_emit_same(jit_buf *b, int dst, int a, int x) {
  emit_cmp_op(b, dst, a, x, 0x94);  /* sete  */
}
void jit_emit_vary(jit_buf *b, int dst, int a, int x) {
  emit_cmp_op(b, dst, a, x, 0x95);  /* setne */
}

/* ============================================================
 * Step 12e: inline SBC_LD4_* / SBC_LOAD / SBC_LOAD8.
 *
 * Body in C:
 *   L[dst] = ((void**)O_PTR(L[src]))[index]
 *
 * where O_PTR(o) = api_g.heap0 + (o >> 16).  The expansion in
 * x86_64:
 *
 *   mov  rax, [rbx + src*8]            ; rax = L[src]   (tagged)
 *   shr  rax, 16                        ; rax = gid
 *   movabs rcx, &api_g.heap0            ; rcx = field address
 *   mov  rcx, [rcx]                     ; rcx = api_g.heap0 value
 *   mov  rax, [rcx + rax*8 + index*8]   ; rax = heap0[gid + index]
 *   mov  [rbx + dst*8], rax             ; L[dst] = rax
 *
 * RAX and RCX are caller-saved on Win64 + SysV; the locals reg
 * (RBX) is unchanged.  No call boundary -- the prologue's
 * shadow space stays intact.
 * ============================================================ */

/* shr rax, imm8 (logical shift right -- zero-extends).  4 bytes.
 * We use shr (not sar) because the dyn's high bits encode the
 * GID, which is always non-negative; logical and arithmetic
 * shift give the same result here and shr's encoding is one byte
 * shorter on some assemblers.  Symmetric with emit_shl_rax. */
static void emit_shr_rax(jit_buf *b, uint8_t imm) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0xc1);
  jit_emit_u8(b, 0xe8);  /* mod=11 reg=/5=shr r/m=000=RAX */
  jit_emit_u8(b, imm);
}

/* movabs rcx, imm64 -- 10 bytes (48 B9 + 8-byte imm).  The imm
 * may be reloc-recorded (caller passes the helper_id; we record
 * the imm64's byte offset). */
static void emit_movabs_rcx_helper(jit_buf *b, uint64_t imm,
                                    jit_helper_id_t hid) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0xb9);  /* mov rcx, imm64 -- opcode b8+reg, RCX=001 */
  uint32_t imm_off = (uint32_t)b->len;
  for (int i = 0; i < 8; i++) jit_emit_u8(b, (uint8_t)(imm >> (i*8)));
  if (b->record_relocs && hid != JIT_HELPER_NONE) {
    jit_record_reloc(b, imm_off, hid);
  }
}

/* mov rcx, [rcx] -- 3 bytes (48 8B 09).  ModR/M = mod=00,
 * reg=001(rcx), r/m=001(rcx).  No SIB, no disp -- direct deref
 * of an unrelated-from-SIB-special register. */
static void emit_mov_rcx_from_rcx_deref(jit_buf *b) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x8b);
  jit_emit_u8(b, 0x09);
}

/* mov rax, [rcx + rax*8 + disp]   (5 bytes if disp fits in s8,
 * else 8 bytes).  ModR/M = mod=01|10 (disp size), reg=000(rax),
 * r/m=100 (SIB indicator).  SIB = scale=11(8x), index=000(rax),
 * base=001(rcx).
 *
 * Used to fetch heap0[gid + index] in one instruction:
 *   gid lives in RAX, heap0 base in RCX, index baked into disp. */
static void emit_mov_rax_from_rcx_indexed8(jit_buf *b, int32_t disp) {
  jit_emit_u8(b, 0x48);                    /* REX.W */
  jit_emit_u8(b, 0x8b);                    /* mov r64, r/m64 */
  if (disp >= -128 && disp <= 127) {
    jit_emit_u8(b, 0x44);                   /* mod=01 reg=000 r/m=100(SIB) */
    jit_emit_u8(b, 0xc1);                   /* SIB: scale=11 index=000 base=001 */
    jit_emit_u8(b, (uint8_t)(int8_t)disp);
  } else {
    jit_emit_u8(b, 0x84);                   /* mod=10 reg=000 r/m=100(SIB) */
    jit_emit_u8(b, 0xc1);
    jit_emit_u32(b, (uint32_t)disp);
  }
}

void jit_emit_ld4(jit_buf *b, uint32_t dst, uint32_t src, uint32_t index,
                  void *heap0_addr_imm) {
  emit_mov_rax_from_slot(b, (int)src);              /* rax = L[src]      */
  emit_shr_rax(b, 16);                              /* rax = gid         */
  emit_movabs_rcx_helper(b, (uint64_t)heap0_addr_imm,
                         JIT_HELPER_AMP_HEAP0);     /* rcx = &heap0      */
  emit_mov_rcx_from_rcx_deref(b);                   /* rcx = heap0 value */
  /* index*8 fits in uint32_t for any sane SBC: even u16 index ->
   * disp = 524280, well under int32_t max.  Cast to signed for
   * the encoder. */
  emit_mov_rax_from_rcx_indexed8(b, (int32_t)(index * 8u));
  emit_mov_slot_from_rax(b, (int)dst);              /* L[dst] = rax      */
}

/* ============================================================
 * Step 12g: inline SBC_ST4_* / SBC_STOR / SBC_STOR8.
 *
 * Body in C (lsetm):
 *   void **slot = (void**)O_PTR(L[dst]) + index;
 *   *slot = L[src];
 *   if (!IMMEDIATE(L[src]) && O_AGE(L[dst]) > O_AGE(L[src])) {
 *     <mark page dirty>;
 *   }
 *
 * Fast path: if L[src] is an immediate (low bit of value clear),
 * no barrier is ever needed -- store inline and skip the helper.
 * If L[src] is heap, fall through to the existing jit_rt_st4
 * helper which redoes the store and handles the barrier check.
 * The redundant store on the slow path is cheap; the win is
 * skipping the call frame setup on the common immediate-store
 * case (FXN ints, fixtexts, No, etc.).
 *
 *   mov   rdx, [rbx + src*8]     ; rdx = L[src] (value)
 *   test  dl, 1                   ; T_HEAP bit of value
 *   jnz   slow                    ; heap value -> helper
 *   mov   rax, [rbx + dst*8]     ; rax = L[dst] (base)
 *   shr   rax, 16                 ; rax = gid_base
 *   movabs rcx, &api_g.heap0
 *   mov   rcx, [rcx]              ; rcx = heap0
 *   mov   [rcx + rax*8 + index*8], rdx
 *   jmp   done
 * slow:
 *   <helper call: st4(L, dst, src, index)>
 * done:
 * ============================================================ */

/* mov rdx, [rbx + slot*8] -- reg=010 (RDX). */
static void emit_mov_rdx_from_slot(jit_buf *b, int slot) {
  int r = jit_slot_reg(b, slot);
  if (r >= 0) { emit_mov_rdx_from_reg(b, r); return; }
  jit_emit_u8(b, 0x48);  /* REX.W */
  jit_emit_u8(b, 0x8b);  /* opcode: mov r64, r/m64 */
  emit_mem_op(b, 2, slot);  /* reg=RDX (2) */
}

/* test dl, imm8 -- 3 bytes (F6 C2 ib).  ModR/M=11 reg=/0(test)
 * r/m=010(DL).  Used to inspect the low byte of RDX (the T_HEAP
 * bit of a value held there). */
static void emit_test_dl_imm8(jit_buf *b, uint8_t imm) {
  jit_emit_u8(b, 0xf6);  /* test r/m8, imm8 */
  jit_emit_u8(b, 0xc2);  /* ModR/M: mod=11 reg=/0 r/m=010(DL) */
  jit_emit_u8(b, imm);
}

/* mov [rcx + rax*8 + disp], rdx  (5 bytes if disp fits in s8,
 * 8 bytes for disp32).  Mirror of emit_mov_rax_from_rcx_indexed8
 * but in the store direction (mem <- reg).
 *
 *   REX.W = 0x48
 *   opcode = 0x89  (mov r/m64, r64)
 *   ModR/M = mod=01|10, reg=010(RDX), r/m=100(SIB indicator)
 *   SIB = scale=11(8x), index=000(RAX), base=001(RCX) */
static void emit_mov_to_rcx_rax_indexed8_from_rdx(jit_buf *b, int32_t disp) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x89);
  if (disp >= -128 && disp <= 127) {
    jit_emit_u8(b, 0x54);  /* mod=01 reg=010 r/m=100(SIB) */
    jit_emit_u8(b, 0xc1);  /* SIB: scale=11 index=000 base=001 */
    jit_emit_u8(b, (uint8_t)(int8_t)disp);
  } else {
    jit_emit_u8(b, 0x94);  /* mod=10 reg=010 r/m=100(SIB) */
    jit_emit_u8(b, 0xc1);
    jit_emit_u32(b, (uint32_t)disp);
  }
}

void jit_emit_st4(jit_buf *b, uint32_t dst, uint32_t src, uint32_t index,
                  void *heap0_addr_imm,
                  void *st4_helper) {
  emit_mov_rdx_from_slot(b, (int)src);              /* rdx = value */
  emit_test_dl_imm8(b, 1);                           /* T_HEAP bit? */
  size_t to_slow = emit_jnz_rel32(b);

  /* Fast path: value is immediate, no barrier needed.  Store
   * inline via the same heap-deref shape as LD4. */
  emit_mov_rax_from_slot(b, (int)dst);              /* rax = base */
  emit_shr_rax(b, 16);                               /* rax = gid_base */
  emit_movabs_rcx_helper(b, (uint64_t)heap0_addr_imm,
                         JIT_HELPER_AMP_HEAP0);      /* rcx = &heap0 */
  emit_mov_rcx_from_rcx_deref(b);                    /* rcx = heap0 */
  emit_mov_to_rcx_rax_indexed8_from_rdx(b,
                                        (int32_t)(index * 8u));
  size_t to_done = jit_emit_jmp(b);

  /* Slow path: heap value -- helper does store + barrier. */
  jit_patch_jmp_here(b, to_slow);
  b->pending_helper_id = JIT_HELPER_ST4;
  jit_emit_call_helper3(b, st4_helper, dst, src, index);

  jit_patch_jmp_here(b, to_done);
}

/* ============================================================
 * Step 12h: inline SBC_FXNLGET fast path.
 *
 * Helper logic (jit_rt_fxnlget_impl):
 *   if (TAGIS(T_LIST, L[src]) && TAGIS(T_INT, L[index_slot])
 *       && (uint64_t)L[index_slot] < (uint64_t)FXN(LIST_SIZE(L[src])))
 *     L[dst] = ((void**)O_PTR(L[src]))[UNFXN(L[index_slot])];
 *   else
 *     <MCACHE_CALL m_get>;
 *
 * Tag-bit layout (FLG_BITS=1, TAG_BITS=15):
 *   heap object's low 16 bits = (tag << 1) | T_HEAP
 *   T_LIST encoded =            (9   << 1) | 1   = 0x13
 *   T_INT immediate             =            0
 *
 * x86_64 fast-path sequence:
 *
 *   mov   rdx, [rbx + idx*8]              ; rdx = L[idx] (T_INT?)
 *   test  dx, dx                          ; low 16 == 0 ?
 *   jne   slow
 *   mov   rax, [rbx + src*8]              ; rax = L[src] (T_LIST?)
 *   cmp   ax, 0x13                        ; low 16 == 0x13 ?
 *   jne   slow
 *   shr   rax, 16                         ; rax = gid_src
 *   movabs rcx, &api_g.heap0              ; (JIT_HELPER_AMP_HEAP0 reloc)
 *   mov   rcx, [rcx]                      ; rcx = heap0
 *   mov   r8d, [rcx + rax*8 - 4]          ; r8d = LIST_SIZE (32-bit;
 *                                         ; gc_head.size is at offset
 *                                         ; -8 of obj data; size lives
 *                                         ; in the low 4 bytes of the
 *                                         ; 8-byte header)
 *   shl   r8, 16                          ; r8 = FXN(size)
 *   cmp   rdx, r8                         ; rdx (FXN-idx) < FXN(size)?
 *   jae   slow                            ; out of bounds
 *   shr   rdx, 16                         ; rdx = UNFXN(idx)
 *   add   rax, rdx                        ; rax = gid_src + idx
 *   mov   rax, [rcx + rax*8]              ; rax = heap0[gid + idx]
 *   mov   [rbx + dst*8], rax              ; L[dst] = rax
 *   jmp   done
 * slow:
 *   <helper call: fxnlget(L, sbc, packed)>
 * done:
 *
 * Clobbers: RAX, RCX, RDX, R8 (all caller-saved).  RBX (L) and
 * R12 (sbc) preserved.
 *
 * gc_head layout (runtime/symta.h):  struct gc_head_t {
 *     uint32_t size;  // size of the object (not counting this header)
 *     uint32_t code;  // closure machine code / entity id
 * } __attribute__((packed));  // sizeof = 8
 *
 * So if we have `rax = gid_src` and `rcx = heap0`, then the object
 * data lives at  [rcx + rax*8]  (8 bytes per slot).  The header lives
 * immediately before the data, at  [rcx + rax*8 - 8].  The `size`
 * field is the FIRST 4 bytes of the header, i.e. at  [rcx + rax*8 - 8].
 * So `mov r8d, [rcx + rax*8 - 8]` loads it.
 * ============================================================ */

/* cmp ax, imm16 -- 4 bytes (66 3D imm16). */
static void emit_cmp_ax_imm16(jit_buf *b, uint16_t imm) {
  jit_emit_u8(b, 0x66);
  jit_emit_u8(b, 0x3d);
  jit_emit_u8(b, (uint8_t)(imm & 0xff));
  jit_emit_u8(b, (uint8_t)((imm >> 8) & 0xff));
}

/* test dx, dx -- 3 bytes (66 85 D2).  ModR/M=11 reg=010(DX)
 * r/m=010(DX). */
static void emit_test_dx_dx(jit_buf *b) {
  jit_emit_u8(b, 0x66);
  jit_emit_u8(b, 0x85);
  jit_emit_u8(b, 0xd2);
}

/* mov r8d, [rcx + rax*8 + disp]  (5 bytes if disp fits in s8,
 * 8 bytes for disp32).  Loads 32-bit unsigned at the indexed
 * heap address into R8 (zero-extended in 64-bit mode).
 *
 *   REX = 0x44   (REX.R for R8 as destination reg)
 *   opcode = 0x8B (mov r32, r/m32)
 *   ModR/M = mod=01|10, reg=000(R8 low 3 bits), r/m=100(SIB)
 *   SIB = scale=11(8x), index=000(RAX), base=001(RCX) */
static void emit_mov_r8d_from_rcx_rax_indexed8(jit_buf *b, int32_t disp) {
  jit_emit_u8(b, 0x44);                              /* REX.R */
  jit_emit_u8(b, 0x8b);                              /* mov r32, r/m32 */
  if (disp >= -128 && disp <= 127) {
    jit_emit_u8(b, 0x44);                            /* mod=01 reg=000 r/m=100 */
    jit_emit_u8(b, 0xc1);                            /* SIB scale=8 idx=rax base=rcx */
    jit_emit_u8(b, (uint8_t)(int8_t)disp);
  } else {
    jit_emit_u8(b, 0x84);                            /* mod=10 reg=000 r/m=100 */
    jit_emit_u8(b, 0xc1);
    jit_emit_u32(b, (uint32_t)disp);
  }
}

/* shl r8, imm8 -- 4 bytes (49 C1 E0 imm8).  REX.B for R8. */
static void emit_shl_r8(jit_buf *b, uint8_t imm) {
  jit_emit_u8(b, 0x49);
  jit_emit_u8(b, 0xc1);
  jit_emit_u8(b, 0xe0);  /* mod=11 reg=/4(shl) r/m=000(R8 via REX.B) */
  jit_emit_u8(b, imm);
}

/* cmp rdx, r8 -- 3 bytes (4C 39 C2).  REX.W + REX.R; opcode 0x39
 * (cmp r/m64, r64); ModR/M=11 reg=000(R8) r/m=010(RDX). */
static void emit_cmp_rdx_r8(jit_buf *b) {
  jit_emit_u8(b, 0x4c);
  jit_emit_u8(b, 0x39);
  jit_emit_u8(b, 0xc2);
}

/* jae rel32 -- 6 bytes (0F 83 disp32).  Returns the offset of the
 * disp32 placeholder for later patching. */
static size_t emit_jae_rel32(jit_buf *b) {
  jit_emit_u8(b, 0x0f);
  jit_emit_u8(b, 0x83);
  size_t patch = b->len;
  jit_emit_u32(b, 0);
  return patch;
}

/* shr rdx, imm8 -- 4 bytes (48 C1 EA imm8). */
static void emit_shr_rdx(jit_buf *b, uint8_t imm) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0xc1);
  jit_emit_u8(b, 0xea);  /* mod=11 reg=/5(shr) r/m=010(RDX) */
  jit_emit_u8(b, imm);
}

/* add rax, rdx -- 3 bytes (48 01 D0).  REX.W; opcode 0x01;
 * ModR/M=11 reg=010(RDX) r/m=000(RAX). */
static void emit_add_rax_rdx(jit_buf *b) {
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x01);
  jit_emit_u8(b, 0xd0);
}

void jit_emit_fxnlget(jit_buf *b, uint32_t dst, uint32_t src,
                      uint32_t index_slot, void *heap0_addr_imm,
                      void *fxnlget_helper, uint64_t fxnlget_packed) {
  /* T_INT check on L[index_slot]. */
  emit_mov_rdx_from_slot(b, (int)index_slot);
  emit_test_dx_dx(b);
  size_t to_slow_int = emit_jnz_rel32(b);

  /* T_LIST check on L[src]. */
  emit_mov_rax_from_slot(b, (int)src);
  emit_cmp_ax_imm16(b, 0x13);   /* (T_LIST<<1)|T_HEAP = (9<<1)|1 */
  size_t to_slow_list = emit_jnz_rel32(b);

  /* Compute heap0 + gid_src*8 base. */
  emit_shr_rax(b, 16);                                /* rax = gid_src */
  emit_movabs_rcx_helper(b, (uint64_t)heap0_addr_imm,
                         JIT_HELPER_AMP_HEAP0);
  emit_mov_rcx_from_rcx_deref(b);                     /* rcx = heap0 */

  /* Bounds check.  gc_head.size at offset -8 of the object data. */
  emit_mov_r8d_from_rcx_rax_indexed8(b, -8);          /* r8d = LIST_SIZE */
  emit_shl_r8(b, 16);                                 /* r8 = FXN(size) */
  emit_cmp_rdx_r8(b);                                 /* rdx (FXN-idx) vs FXN(size) */
  size_t to_slow_bounds = emit_jae_rel32(b);          /* idx >= size -> slow */

  /* Fast path: rax = heap0[gid_src + idx]. */
  emit_shr_rdx(b, 16);                                /* rdx = UNFXN(idx) */
  emit_add_rax_rdx(b);                                /* rax = gid_src + idx */
  emit_mov_rax_from_rcx_indexed8(b, 0);               /* rax = [rcx + rax*8] */
  emit_mov_slot_from_rax(b, (int)dst);                /* L[dst] = rax */
  size_t to_done = jit_emit_jmp(b);

  /* Slow path: full helper call. */
  jit_patch_jmp_here(b, to_slow_int);
  jit_patch_jmp_here(b, to_slow_list);
  jit_patch_jmp_here(b, to_slow_bounds);
  b->pending_helper_id = JIT_HELPER_FXNLGET;
  jit_emit_call_with_sbc(b, fxnlget_helper, fxnlget_packed);

  jit_patch_jmp_here(b, to_done);
}

/* ============================================================
 * Step 12i: inline SBC_FXNLSET / SBC_FXNLSETIR fast path.
 *
 * Helper logic (jit_rt_fxnlset_impl / jit_rt_fxnlsetir_impl):
 *   if (TAGIS(T_LIST, L[src]) && TAGIS(T_INT, L[idx])
 *       && (uint64_t)L[idx] < (uint64_t)FXN(LIST_SIZE(L[src])))
 *     FXNLSET(L[dst], L[src], L[idx], L[val]);  // dst is ignored
 *                                               // -- lsetm into L[src]
 *   else
 *     <MCACHE_CALL m_set>;
 *
 * `FXNLSET(dst, xs, i, v) -> LSET(xs, UNFXN(i), v) -> lsetm(xs, UNFXN(i), v)`
 * -- the dst slot is NOT touched in the fast path (matches the
 * macro definition in symta.h:174).  Slow path's MCACHE_CALL does
 * write the dst slot with the method's return value.
 *
 * Fast path further requires the VALUE to be immediate so no GC
 * cross-gen barrier is needed: matches ST4's immediate-bypass
 * shape.  Heap value -> fall to helper (which redoes the store
 * with the full barrier check).
 *
 * Composed from existing primitives: T_LIST + T_INT + bounds
 * (same as FXNLGET) followed by immediate-value test + inline
 * store (same as ST4).
 * ============================================================ */

void jit_emit_fxnlset(jit_buf *b, uint32_t dst, uint32_t src,
                      uint32_t index_slot, uint32_t val,
                      void *heap0_addr_imm,
                      void *fxnlset_helper,
                      uint64_t fxnlset_packed1, uint64_t fxnlset_packed2,
                      int with_result) {
  (void)dst;  /* dst is unused on the fast path -- only the slow */
              /* path's MCACHE_CALL writes it.  Pass-through to */
              /* the helper packed args. */

  /* T_LIST check on L[src]. */
  emit_mov_rax_from_slot(b, (int)src);
  emit_cmp_ax_imm16(b, 0x13);
  size_t to_slow_list = emit_jnz_rel32(b);

  /* T_INT check on L[index_slot]. */
  emit_mov_rdx_from_slot(b, (int)index_slot);
  emit_test_dx_dx(b);
  size_t to_slow_int = emit_jnz_rel32(b);

  /* Compute heap0 + gid_src*8 base. */
  emit_shr_rax(b, 16);                                /* rax = gid_src */
  emit_movabs_rcx_helper(b, (uint64_t)heap0_addr_imm,
                         JIT_HELPER_AMP_HEAP0);
  emit_mov_rcx_from_rcx_deref(b);                     /* rcx = heap0 */

  /* Bounds check. */
  emit_mov_r8d_from_rcx_rax_indexed8(b, -8);          /* r8d = LIST_SIZE */
  emit_shl_r8(b, 16);                                 /* r8 = FXN(size) */
  emit_cmp_rdx_r8(b);
  size_t to_slow_bounds = emit_jae_rel32(b);

  /* Compute final slot offset: rax = gid_src + UNFXN(idx). */
  emit_shr_rdx(b, 16);                                /* rdx = UNFXN(idx) */
  emit_add_rax_rdx(b);                                /* rax = gid_src + idx */

  /* Load value and check it's immediate.  Clobbers rdx (we're done
   * with the index).  Heap value -> slow (helper does the barrier). */
  emit_mov_rdx_from_slot(b, (int)val);
  emit_test_dl_imm8(b, 1);
  size_t to_slow_heap_value = emit_jnz_rel32(b);

  /* Inline store: *(slot) = value.  Disp=0 because rax now holds
   * the full gid_src + idx offset. */
  emit_mov_to_rcx_rax_indexed8_from_rdx(b, 0);
  size_t to_done = jit_emit_jmp(b);

  /* Slow path: full helper call. */
  jit_patch_jmp_here(b, to_slow_list);
  jit_patch_jmp_here(b, to_slow_int);
  jit_patch_jmp_here(b, to_slow_bounds);
  jit_patch_jmp_here(b, to_slow_heap_value);
  b->pending_helper_id = with_result ? JIT_HELPER_FXNLSET
                                     : JIT_HELPER_FXNLSETIR;
  /* FXNLSET (with-result) uses call_with_sbc2 (two packed u64s
   * because 5 operands overflow a single u64); FXNLSETIR
   * (ignore-result) uses call_with_sbc (single packed). */
  if (with_result) {
    jit_emit_call_with_sbc2(b, fxnlset_helper, fxnlset_packed1,
                            fxnlset_packed2);
  } else {
    jit_emit_call_with_sbc(b, fxnlset_helper, fxnlset_packed1);
  }

  jit_patch_jmp_here(b, to_done);
}

size_t jit_here(jit_buf *b) { return b->len; }

size_t jit_emit_jmp(jit_buf *b) {
  jit_emit_u8(b, 0xe9);
  size_t patch = b->len;
  jit_emit_u32(b, 0);  /* placeholder */
  return patch;
}

/* cmp qword ptr [locals_reg + slot*8], 0 -- compare a slot
 * against zero so the following jcc tests slot truthiness.
 * Encoding: 48 83 + ModR/M(/7) + disp + imm8(0). */
static void emit_cmp_slot_zero(jit_buf *b, int slot) {
  int r = jit_slot_reg(b, slot);
  if (r >= 0) { emit_cmp_reg_zero(b, r); return; }
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x83);
  emit_mem_op(b, 7, slot);  /* /7 = cmp imm */
  jit_emit_u8(b, 0x00);
}

size_t jit_emit_jnz_slot(jit_buf *b, int slot) {
  emit_cmp_slot_zero(b, slot);
  jit_emit_u8(b, 0x0f);
  jit_emit_u8(b, 0x85);  /* jne rel32 */
  size_t patch = b->len;
  jit_emit_u32(b, 0);
  return patch;
}

size_t jit_emit_jz_slot(jit_buf *b, int slot) {
  emit_cmp_slot_zero(b, slot);
  jit_emit_u8(b, 0x0f);
  jit_emit_u8(b, 0x84);  /* je rel32 */
  size_t patch = b->len;
  jit_emit_u32(b, 0);
  return patch;
}

/* Write disp32 = target - (patch_off + 4) into the placeholder. */
void jit_patch_jmp_to(jit_buf *b, size_t patch_off, size_t target) {
  int64_t rel = (int64_t)target - (int64_t)(patch_off + 4);
  /* Programmer error if the displacement doesn't fit; we'd
   * have to emit a long-form jump or chain trampolines, which
   * is out of scope for step 2.  Aborting here is louder than
   * silently truncating. */
  if (rel < INT32_MIN || rel > INT32_MAX) {
    fprintf(stderr, "jit_patch_jmp_to: rel32 overflow (%lld bytes)\n",
            (long long)rel);
    abort();
  }
  uint32_t disp = (uint32_t)(int32_t)rel;
  b->code[patch_off + 0] = (uint8_t)(disp & 0xff);
  b->code[patch_off + 1] = (uint8_t)((disp >> 8) & 0xff);
  b->code[patch_off + 2] = (uint8_t)((disp >> 16) & 0xff);
  b->code[patch_off + 3] = (uint8_t)((disp >> 24) & 0xff);
}

void jit_patch_jmp_here(jit_buf *b, size_t patch_off) {
  jit_patch_jmp_to(b, patch_off, b->len);
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

  jit_emit_prologue(b);
  jit_emit_iadd(b, 2, 0, 1);
  jit_emit_isub(b, 3, 0, 1);
  jit_emit_imul(b, 4, 0, 1);
  jit_emit_idiv(b, 5, 0, 1);
  jit_emit_irem(b, 6, 0, 1);
  jit_emit_epilogue(b);

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
  jit_emit_prologue(bb);
  jit_emit_iadd(bb, 32, 0, 1);   /* L[32] = L[0] + L[1] */
  jit_emit_epilogue(bb);
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

/* Step 2 proof, part A: comparison emitters store FXN-tagged
 * 0/1 in the dst slot.  Tests all four predicates on positive
 * and negative operand pairs. */
static int step2_compares(void) {
  jit_buf *b = jit_buf_new(256);
  if (!b) return 1;

  jit_emit_prologue(b);
  /* L[2]=ILT(0,1) L[3]=IGT(0,1) L[4]=ILTE(0,1) L[5]=IGTE(0,1) */
  jit_emit_ilt (b, 2, 0, 1);
  jit_emit_igt (b, 3, 0, 1);
  jit_emit_ilte(b, 4, 0, 1);
  jit_emit_igte(b, 5, 0, 1);
  jit_emit_epilogue(b);

  void (*fn)(int64_t*) = (void(*)(int64_t*))jit_buf_finalize(b);

  int fail = 0;
  int64_t L[10] = {0};

  L[0] = TEST_FXN(3); L[1] = TEST_FXN(5);
  fn(L);
  fail += check("ILT  3<5",   L[2], TEST_FXN(1));
  fail += check("IGT  3>5",   L[3], TEST_FXN(0));
  fail += check("ILTE 3<=5",  L[4], TEST_FXN(1));
  fail += check("IGTE 3>=5",  L[5], TEST_FXN(0));

  L[0] = TEST_FXN(5); L[1] = TEST_FXN(5);
  fn(L);
  fail += check("ILT  5<5",   L[2], TEST_FXN(0));
  fail += check("IGT  5>5",   L[3], TEST_FXN(0));
  fail += check("ILTE 5<=5",  L[4], TEST_FXN(1));
  fail += check("IGTE 5>=5",  L[5], TEST_FXN(1));

  L[0] = TEST_FXN(-3); L[1] = TEST_FXN(-5);
  fn(L);
  fail += check("ILT  -3<-5", L[2], TEST_FXN(0));
  fail += check("IGT  -3>-5", L[3], TEST_FXN(1));

  jit_buf_free(b);
  return fail;
}

/* Step 2 proof, part B: assemble a countdown-and-sum loop
 * entirely in emitted x86 and verify the result matches the
 * closed-form formula.
 *
 *   L[0] = 0                      ; accumulator (init by caller)
 *   L[1] = N                      ; counter   (init by caller)
 *   L[2] = 1                      ; step      (init by caller)
 *  loop:
 *   L[0] = L[0] + L[1]            ; iadd
 *   L[1] = L[1] - L[2]            ; isub
 *   if L[1] != 0: goto loop       ; jnz_slot L[1]
 *   ret
 *
 * For N=10, expect L[0] = 10+9+...+1 = 55. */
static int step2_loop(void) {
  jit_buf *b = jit_buf_new(256);
  if (!b) return 1;

  jit_emit_prologue(b);
  size_t loop_start = jit_here(b);
  jit_emit_iadd(b, 0, 0, 1);
  jit_emit_isub(b, 1, 1, 2);
  size_t back = jit_emit_jnz_slot(b, 1);
  jit_patch_jmp_to(b, back, loop_start);
  jit_emit_epilogue(b);

  void (*fn)(int64_t*) = (void(*)(int64_t*))jit_buf_finalize(b);

  int64_t L[10] = {0};
  L[0] = 0;
  L[1] = TEST_FXN(10);
  L[2] = TEST_FXN(1);
  fn(L);

  int fail = 0;
  fail += check("loop sum 1..10", L[0], TEST_FXN(55));

  /* Also try N=100 -> 5050. */
  L[0] = 0;
  L[1] = TEST_FXN(100);
  L[2] = TEST_FXN(1);
  fn(L);
  fail += check("loop sum 1..100", L[0], TEST_FXN(5050));

  jit_buf_free(b);
  return fail;
}

/* Step 2 proof, part C: forward jump.  Emits an `if (L[0] > L[1])
 * L[2] = L[0]; else L[2] = L[1]` max-of-two using a comparison
 * + jump-forward sequence. */
static int step2_forward_jmp(void) {
  jit_buf *b = jit_buf_new(256);
  if (!b) return 1;

  jit_emit_prologue(b);
  /* L[3] = IGT L[0] L[1]      ; 1 if L[0] > L[1] */
  jit_emit_igt(b, 3, 0, 1);

  /* if L[3]: jump to "take_a" branch */
  size_t take_a = jit_emit_jnz_slot(b, 3);

  /* else: L[2] = L[1] (b is bigger); jump to end */
  jit_emit_iadd(b, 2, 1, 4);  /* L[2] = L[1] + L[4] where L[4]=0 */
  size_t end_jmp = jit_emit_jmp(b);

  /* take_a:  L[2] = L[0] */
  jit_patch_jmp_here(b, take_a);
  jit_emit_iadd(b, 2, 0, 4);  /* L[2] = L[0] + L[4] where L[4]=0 */

  /* end: */
  jit_patch_jmp_here(b, end_jmp);
  jit_emit_epilogue(b);

  void (*fn)(int64_t*) = (void(*)(int64_t*))jit_buf_finalize(b);

  int fail = 0;
  int64_t L[10] = {0};
  L[4] = 0;  /* zero scratch */

  L[0] = TEST_FXN(10); L[1] = TEST_FXN(3);
  fn(L);
  fail += check("max(10,3)", L[2], TEST_FXN(10));

  L[0] = TEST_FXN(-5); L[1] = TEST_FXN(7);
  fn(L);
  fail += check("max(-5,7)", L[2], TEST_FXN(7));

  L[0] = TEST_FXN(4); L[1] = TEST_FXN(4);
  fn(L);
  fail += check("max(4,4)", L[2], TEST_FXN(4));

  jit_buf_free(b);
  return fail;
}

/* Step 3 callbacks: ordinary C functions invoked from JIT'd
 * code via jit_emit_call_abs.  Their first arg (RCX on Win64,
 * RDI on SysV) gets loaded by jit_emit_mov_arg0_from_locals. */
static void step3_cb_store_magic(int64_t *L) { L[3] = TEST_FXN(42); }
static int  step3_cb_count = 0;
static void step3_cb_increment_counter(int64_t *L) {
  (void)L;
  step3_cb_count++;
}

/* Step 3 proof: emit a function that does arithmetic, calls
 * a C function, does more arithmetic, calls another C function.
 * Verifies (a) the trampoline works, (b) RBX survives across
 * the call so post-call emitters can still access L, and
 * (c) multiple calls compose. */
static int step3_call(void) {
  jit_buf *b = jit_buf_new(512);
  if (!b) return 1;

  jit_emit_prologue(b);

  /* L[2] = L[0] + L[1]   (pre-call arith) */
  jit_emit_iadd(b, 2, 0, 1);

  /* Call step3_cb_store_magic(L) -- writes FXN(42) into L[3]. */
  jit_emit_mov_arg0_from_locals(b);
  jit_emit_call_abs(b, (void*)step3_cb_store_magic);

  /* Post-call arith: L[4] = L[2] + L[3] = 15 + 42 = 57.  This
   * tests that L is still accessible via RBX after the call. */
  jit_emit_iadd(b, 4, 2, 3);

  /* Call step3_cb_increment_counter(L) -- bumps a global. */
  jit_emit_mov_arg0_from_locals(b);
  jit_emit_call_abs(b, (void*)step3_cb_increment_counter);

  /* L[5] = L[4] * L[1] = 57 * 5 = 285 */
  jit_emit_imul(b, 5, 4, 1);

  jit_emit_epilogue(b);

  void (*fn)(int64_t*) = (void(*)(int64_t*))jit_buf_finalize(b);

  int fail = 0;
  int64_t L[10] = {0};
  L[0] = TEST_FXN(10);
  L[1] = TEST_FXN(5);

  step3_cb_count = 0;
  fn(L);
  fail += check("L[2] = 10+5",        L[2], TEST_FXN(15));
  fail += check("L[3] = 42 (cb)",     L[3], TEST_FXN(42));
  fail += check("L[4] = 15+42 (post)",L[4], TEST_FXN(57));
  fail += check("L[5] = 57*5",        L[5], TEST_FXN(285));
  if (step3_cb_count != 1) {
    printf("FAIL  counter cb fired %d times (want 1)\n", step3_cb_count);
    fail++;
  } else {
    printf("OK   counter cb fired once\n");
  }

  /* Call twice -- counter should reach 2. */
  fn(L);
  if (step3_cb_count != 2) {
    printf("FAIL  counter cb fired %d times (want 2)\n", step3_cb_count);
    fail++;
  } else {
    printf("OK   counter cb fired twice across two invocations\n");
  }

  jit_buf_free(b);
  return fail;
}

/* Step 4 proof: feed a hand-crafted SBC bytecode stream
 * through jit_translate and verify the resulting x86 function
 * computes the same thing the interpreter would.
 *
 * Program:
 *   L[2] = L[0] + L[1]         IADD
 *   L[3] = L[0] * L[1]         IMUL
 *   L[4] = L[3] - L[2]         ISUB
 *   L[5] = L[0] < L[1]         ILT  (0 or 1, FXN-tagged)
 *   return L[4]                LEAVE
 *
 * Inputs: L[0]=FXN(10), L[1]=FXN(3).
 * Expected:
 *   L[2] = FXN(13)
 *   L[3] = FXN(30)
 *   L[4] = FXN(17)
 *   L[5] = FXN(0)  (10 < 3 is false)
 *   return value = L[4] = FXN(17)
 */
static int step4_translate(void) {
  static const uint8_t bytecode[] = {
    BC_IADD, 2,0, 0,0, 1,0,   /* L[2] = L[0] + L[1] */
    BC_IMUL, 3,0, 0,0, 1,0,   /* L[3] = L[0] * L[1] */
    BC_ISUB, 4,0, 3,0, 2,0,   /* L[4] = L[3] - L[2] */
    BC_ILT,  5,0, 0,0, 1,0,   /* L[5] = L[0] < L[1] */
    BC_LEAVE, 4,0,            /* return L[4]        */
  };

  jit_buf *b = jit_translate(bytecode, sizeof(bytecode));
  if (!b) {
    printf("FAIL  jit_translate returned NULL\n");
    return 1;
  }
  int64_t (*fn)(int64_t*) = (int64_t(*)(int64_t*))jit_buf_finalize(b);

  int fail = 0;
  int64_t L[10] = {0};
  L[0] = TEST_FXN(10);
  L[1] = TEST_FXN(3);
  int64_t ret = fn(L);
  fail += check("IADD L[2] = 10+3",   L[2], TEST_FXN(13));
  fail += check("IMUL L[3] = 10*3",   L[3], TEST_FXN(30));
  fail += check("ISUB L[4] = 30-13",  L[4], TEST_FXN(17));
  fail += check("ILT  L[5] = 10<3",   L[5], TEST_FXN(0));
  fail += check("LEAVE returns L[4]", ret,  TEST_FXN(17));

  jit_buf_free(b);

  /* Negative test: unsupported opcode -- jit_translate must
   * return NULL so the caller knows to fall back. */
  static const uint8_t bad_bytecode[] = {
    BC_IADD, 2,0, 0,0, 1,0,
    0xFF,                     /* not a real opcode */
    BC_LEAVE0,
  };
  jit_buf *b2 = jit_translate(bad_bytecode, sizeof(bad_bytecode));
  if (b2 != NULL) {
    printf("FAIL  jit_translate accepted unsupported opcode 0xFF\n");
    jit_buf_free(b2);
    fail++;
  } else {
    printf("OK   jit_translate rejected unsupported opcode 0xFF\n");
  }

  /* Negative test: truncated operand stream. */
  static const uint8_t truncated[] = {
    BC_IADD, 2,0, 0,0,        /* missing two bytes of `x` operand */
  };
  jit_buf *b3 = jit_translate(truncated, sizeof(truncated));
  if (b3 != NULL) {
    printf("FAIL  jit_translate accepted truncated stream\n");
    jit_buf_free(b3);
    fail++;
  } else {
    printf("OK   jit_translate rejected truncated stream\n");
  }

  return fail;
}

/* Step 5a proof: jit_translate handles SBC_JMP and SBC_B
 * (conditional branch).  Verifies backward AND forward
 * branch resolution via the patch list.
 *
 * Loop program (countdown-sum, same shape as step 2b but
 * driven entirely from SBC bytecode):
 *
 *   bc[ 0..6 ]: IADD L[0] = L[0] + L[1]       7 bytes
 *   bc[ 7..13]: ISUB L[1] = L[1] - L[2]       7 bytes
 *   bc[14..19]: B    L[1], target=0           6 bytes
 *   bc[20..22]: LEAVE L[0]                    3 bytes
 *
 * Forward-branch program (max-of-two via JMP):
 *
 *   bc[ 0..6 ]: IGT  L[3] = L[0] > L[1]
 *   bc[ 7..12]: B    L[3], target=20         (skip else arm)
 *   bc[13..19]: IADD L[2] = L[1] + L[4=0]    (else: L[2]=L[1])
 *   bc[20..  ]: ...wait that's the target -> we need a JMP
 *
 * Actually rewrite cleaner:
 *
 *   bc[ 0..6 ]: IGT  L[3] = L[0] > L[1]      ; 7
 *   bc[ 7..12]: B    L[3], target=20         ; 6  -> skip to "take_a"
 *   bc[13..19]: IADD L[2] = L[1] + L[4]      ; 7  (else branch)
 *   bc[20..23]: JMP  target=30               ; 4  -> end
 *   bc[24..30]: IADD L[2] = L[0] + L[4]      ; 7  (take_a branch)
 *   bc[31..33]: LEAVE L[2]                   ; 3
 *
 * (L[4] is initialized to 0 by the caller, used as identity
 *  for the IADD-copy.)
 */
static int step5_branches(void) {
  int fail = 0;

  /* --- Backward branch: countdown loop. --- */
  {
    static const uint8_t bc[] = {
      BC_IADD, 0,0, 0,0, 1,0,
      BC_ISUB, 1,0, 1,0, 2,0,
      BC_B,    1,0, 0,0,0,
      BC_LEAVE, 0,0,
    };
    jit_buf *b = jit_translate(bc, sizeof(bc));
    if (!b) { printf("FAIL  jit_translate (loop) returned NULL\n"); return 1; }
    int64_t (*fn)(int64_t*) = (int64_t(*)(int64_t*))jit_buf_finalize(b);

    int64_t L[10] = {0};
    L[1] = TEST_FXN(10);
    L[2] = TEST_FXN(1);
    int64_t ret = fn(L);
    fail += check("loop sum 1..10",  ret,  TEST_FXN(55));
    fail += check("loop accumulator", L[0], TEST_FXN(55));

    L[0] = 0; L[1] = TEST_FXN(100); L[2] = TEST_FXN(1);
    ret = fn(L);
    fail += check("loop sum 1..100", ret,  TEST_FXN(5050));

    jit_buf_free(b);
  }

  /* --- Forward branch + unconditional jmp: max-of-two. --- */
  {
    /*  0..6   IGT  L[3] = L[0] > L[1]
     *  7..12  B    L[3], target=24    (jump fwd to take_a)
     * 13..19  IADD L[2] = L[1] + L[4]  (else: L[2] = L[1])
     * 20..23  JMP  target=31           (skip take_a)
     * 24..30  IADD L[2] = L[0] + L[4]  (take_a: L[2] = L[0])
     * 31..33  LEAVE L[2]
     */
    static const uint8_t bc[] = {
      BC_IGT,  3,0, 0,0, 1,0,             /*  0..6  */
      BC_B,    3,0, 24,0,0,               /*  7..12 */
      BC_IADD, 2,0, 1,0, 4,0,             /* 13..19 */
      BC_JMP,  31,0,0,                    /* 20..23 */
      BC_IADD, 2,0, 0,0, 4,0,             /* 24..30 */
      BC_LEAVE, 2,0,                      /* 31..33 */
    };
    jit_buf *b = jit_translate(bc, sizeof(bc));
    if (!b) { printf("FAIL  jit_translate (max) returned NULL\n"); return 1; }
    int64_t (*fn)(int64_t*) = (int64_t(*)(int64_t*))jit_buf_finalize(b);

    int64_t L[10] = {0};  /* L[4] = 0 (identity for IADD-copy) */

    L[0] = TEST_FXN(10); L[1] = TEST_FXN(3);
    int64_t ret = fn(L);
    fail += check("max(10,3) via JIT bc", ret, TEST_FXN(10));

    L[0] = TEST_FXN(-5); L[1] = TEST_FXN(7);
    ret = fn(L);
    fail += check("max(-5,7) via JIT bc", ret, TEST_FXN(7));

    L[0] = TEST_FXN(4); L[1] = TEST_FXN(4);
    ret = fn(L);
    fail += check("max(4,4) via JIT bc", ret, TEST_FXN(4));

    jit_buf_free(b);
  }

  return fail;
}

/* Step 5b proof: immediate-load opcodes and PC-relative
 * branches.  Test program is a self-contained factorial that
 * initializes its accumulators in-band:
 *
 *   bc[ 0..3 ]: FXN8 dst=0, imm=1        ; L[0] = FXN(1)  (acc)
 *   bc[ 4..7 ]: FXN8 dst=2, imm=1        ; L[2] = FXN(1)  (step)
 *  loop:
 *   bc[ 8..14]: IMUL L[0] = L[0] * L[1]
 *   bc[15..21]: ISUB L[1] = L[1] - L[2]
 *   bc[22..27]: B    L[1], 8             ; (absolute target back to loop)
 *   bc[28..30]: LEAVE L[0]
 *
 * Inputs: L[1] = FXN(N) (the value to factorial-ize).
 * Expected: L[0] = N! (FXN-tagged).
 */
static int step5b_immediates(void) {
  int fail = 0;

  /* Factorial via FXN8 loads + IMUL/ISUB + absolute B + LEAVE. */
  {
    static const uint8_t bc[] = {
      BC_FXN8, 0,0, 1,            /*  0..3 : L[0] = FXN(1) */
      BC_FXN8, 2,0, 1,            /*  4..7 : L[2] = FXN(1) */
      BC_IMUL, 0,0, 0,0, 1,0,     /*  8..14: L[0] *= L[1]  */
      BC_ISUB, 1,0, 1,0, 2,0,     /* 15..21: L[1] -= L[2]  */
      BC_B,    1,0, 8,0,0,        /* 22..27: if L[1] -> 8  */
      BC_LEAVE, 0,0,              /* 28..30                */
    };
    jit_buf *b = jit_translate(bc, sizeof(bc));
    if (!b) { printf("FAIL  factorial returned NULL\n"); return 1; }
    int64_t (*fn)(int64_t*) = (int64_t(*)(int64_t*))jit_buf_finalize(b);

    int64_t L[10] = {0};
    L[1] = TEST_FXN(5);
    int64_t ret = fn(L);
    fail += check("factorial(5)", ret, TEST_FXN(120));

    L[1] = TEST_FXN(7);
    ret = fn(L);
    fail += check("factorial(7)", ret, TEST_FXN(5040));

    L[1] = TEST_FXN(10);
    ret = fn(L);
    fail += check("factorial(10)", ret, TEST_FXN(3628800));

    jit_buf_free(b);
  }

  /* FXNB16 / FXN32 -- larger immediate paths.  Verify both
   * the 8-bit-dst-slot family and the wider immediate
   * encodings produce identical results. */
  {
    static const uint8_t bc[] = {
      BC_FXNB16, 0, 0xE8,0x03,   /* 0..3:  L[0] = FXN(1000)         */
      BC_FXN32,  1,0, 0x40,0x42,0x0F,0x00,  /* 4..10: L[1] = FXN(1000000) */
      BC_IADD, 2,0, 0,0, 1,0,    /* 11..17: L[2] = L[0] + L[1] = 1001000 */
      BC_LEAVE, 2,0,             /* 18..20 */
    };
    jit_buf *b = jit_translate(bc, sizeof(bc));
    if (!b) { printf("FAIL  larger-imm returned NULL\n"); return 1; }
    int64_t (*fn)(int64_t*) = (int64_t(*)(int64_t*))jit_buf_finalize(b);

    int64_t L[10] = {0};
    int64_t ret = fn(L);
    fail += check("L[0] = FXN(1000)", L[0], TEST_FXN(1000));
    fail += check("L[1] = FXN(1000000)", L[1], TEST_FXN(1000000));
    fail += check("sum = FXN(1001000)", ret, TEST_FXN(1001000));

    jit_buf_free(b);
  }

  /* Negative immediate path -- FXN8 with -5. */
  {
    static const uint8_t bc[] = {
      BC_FXN8, 0,0, (uint8_t)(int8_t)-5,  /* L[0] = FXN(-5) */
      BC_FXN8, 1,0, 3,                    /* L[1] = FXN(3)  */
      BC_IADD, 2,0, 0,0, 1,0,             /* L[2] = -5 + 3 = -2 */
      BC_LEAVE, 2,0,
    };
    jit_buf *b = jit_translate(bc, sizeof(bc));
    if (!b) { printf("FAIL  negative-imm returned NULL\n"); return 1; }
    int64_t (*fn)(int64_t*) = (int64_t(*)(int64_t*))jit_buf_finalize(b);

    int64_t L[10] = {0};
    int64_t ret = fn(L);
    fail += check("L[0] = FXN(-5)", L[0], TEST_FXN(-5));
    fail += check("L[2] = -5+3",     ret,  TEST_FXN(-2));

    jit_buf_free(b);
  }

  /* PC-relative jump (JMP16): unconditional skip-forward.
   *   bc[0..3]:  FXN8 dst=0, imm=10   (L[0] = 10)
   *   bc[4..6]:  JMP16 diff=+7        (skip past the next FXN8)
   *   bc[7..10]: FXN8 dst=0, imm=99   (would overwrite L[0])
   *   bc[11..13]: LEAVE L[0]
   * After JMP16 at bc[4]: pin = bc[7]; target = 4 + 3 + diff = 7 + diff.
   * For diff=4 the target lands at bc[11] (the LEAVE), bypassing
   * the second FXN8.  Expected: L[0] stays 10.
   */
  {
    static const uint8_t bc[] = {
      BC_FXN8, 0,0, 10,            /* 0..3 */
      BC_JMP16, 4,0,               /* 4..6  diff=+4 -> target bc[11] */
      BC_FXN8, 0,0, 99,            /* 7..10 (skipped) */
      BC_LEAVE, 0,0,               /* 11..13 */
    };
    jit_buf *b = jit_translate(bc, sizeof(bc));
    if (!b) { printf("FAIL  JMP16 returned NULL\n"); return 1; }
    int64_t (*fn)(int64_t*) = (int64_t(*)(int64_t*))jit_buf_finalize(b);

    int64_t L[10] = {0};
    int64_t ret = fn(L);
    fail += check("JMP16 skipped overwrite", ret, TEST_FXN(10));

    jit_buf_free(b);
  }

  /* PC-relative conditional branch (B8): back-jump loop.
   *   bc[ 0..3]: FXN8 dst=0, imm=0       (acc = 0)
   *   bc[ 4..7]: FXN8 dst=2, imm=1       (step = 1)
   *  loop (bc[8]):
   *   bc[ 8..14]: IADD L[0] = L[0] + L[1]
   *   bc[15..21]: ISUB L[1] = L[1] - L[2]
   *   bc[22..25]: B8   cnd=1, diff=-18    (back to loop = bc[8])
   *               target = 22 + 4 + (-18) = 8 ✓
   *   bc[26..28]: LEAVE L[0]
   */
  {
    static const uint8_t bc[] = {
      BC_FXN8, 0,0, 0,
      BC_FXN8, 2,0, 1,
      BC_IADD, 0,0, 0,0, 1,0,
      BC_ISUB, 1,0, 1,0, 2,0,
      BC_B8,   1, (uint8_t)(int16_t)-18, (uint8_t)(((int16_t)-18) >> 8),
      BC_LEAVE, 0,0,
    };
    jit_buf *b = jit_translate(bc, sizeof(bc));
    if (!b) { printf("FAIL  B8 loop returned NULL\n"); return 1; }
    int64_t (*fn)(int64_t*) = (int64_t(*)(int64_t*))jit_buf_finalize(b);

    int64_t L[10] = {0};
    L[1] = TEST_FXN(10);
    int64_t ret = fn(L);
    fail += check("B8 backward loop sum 1..10", ret, TEST_FXN(55));

    jit_buf_free(b);
  }

  return fail;
}

/* Step 5c proof: untyped FXN* opcodes dispatch through C
 * runtime helpers via the step-3 trampoline.  Verifies (a)
 * the 4-arg call sequence sets up registers correctly,
 * (b) the helpers mutate L through the L pointer, (c) RBX
 * survives across multiple back-to-back helper calls, and
 * (d) the helper-routed semantics match the inline-emitted
 * I* family bit-for-bit. */
static int step5c_fxn_trampoline(void) {
  int fail = 0;

  /* All-FXN* program: do every untyped arith op once.
   *   bc[ 0..3 ]: FXN8 dst=0, imm=20
   *   bc[ 4..7 ]: FXN8 dst=1, imm=7
   *   bc[ 8..14]: FXNADD L[2] = L[0] + L[1]
   *   bc[15..21]: FXNSUB L[3] = L[0] - L[1]
   *   bc[22..28]: FXNMUL L[4] = L[0] * L[1]
   *   bc[29..35]: FXNDIV L[5] = L[0] / L[1]
   *   bc[36..42]: FXNREM L[6] = L[0] % L[1]
   *   bc[43..45]: LEAVE L[2]
   */
  static const uint8_t bc_all_fxn[] = {
    BC_FXN8, 0,0, 20,
    BC_FXN8, 1,0, 7,
    BC_FXNADD, 2,0, 0,0, 1,0,
    BC_FXNSUB, 3,0, 0,0, 1,0,
    BC_FXNMUL, 4,0, 0,0, 1,0,
    BC_FXNDIV, 5,0, 0,0, 1,0,
    BC_FXNREM, 6,0, 0,0, 1,0,
    BC_LEAVE, 2,0,
  };
  jit_buf *b = jit_translate(bc_all_fxn, sizeof(bc_all_fxn));
  if (!b) { printf("FAIL  step5c all-FXN returned NULL\n"); return 1; }
  int64_t (*fn)(int64_t*) = (int64_t(*)(int64_t*))jit_buf_finalize(b);

  int64_t L[10] = {0};
  int64_t ret = fn(L);
  fail += check("FXNADD 20+7",  L[2], TEST_FXN(27));
  fail += check("FXNSUB 20-7",  L[3], TEST_FXN(13));
  fail += check("FXNMUL 20*7",  L[4], TEST_FXN(140));
  fail += check("FXNDIV 20/7",  L[5], TEST_FXN(2));
  fail += check("FXNREM 20%7",  L[6], TEST_FXN(6));
  fail += check("LEAVE -> L[2]", ret,  TEST_FXN(27));
  jit_buf_free(b);

  /* Mixed I* + FXN* program: prove typed and trampolined paths
   * compose cleanly within the same function -- L stays
   * accessible across helper calls via RBX. */
  static const uint8_t bc_mixed[] = {
    BC_FXN8, 0,0, 10,
    BC_FXN8, 1,0, 3,
    BC_IADD,   2,0, 0,0, 1,0,    /* typed   L[2] = L[0] + L[1] = 13 */
    BC_FXNMUL, 3,0, 0,0, 1,0,    /* untyped L[3] = L[0] * L[1] = 30 */
    BC_IADD,   4,0, 2,0, 3,0,    /* typed   L[4] = L[2] + L[3] = 43 */
    BC_LEAVE, 4,0,
  };
  jit_buf *b2 = jit_translate(bc_mixed, sizeof(bc_mixed));
  if (!b2) { printf("FAIL  step5c mixed returned NULL\n"); return 1; }
  int64_t (*fn2)(int64_t*) = (int64_t(*)(int64_t*))jit_buf_finalize(b2);

  int64_t L2[10] = {0};
  int64_t ret2 = fn2(L2);
  fail += check("mixed I+F: L[2] = 10+3 (I)",   L2[2], TEST_FXN(13));
  fail += check("mixed I+F: L[3] = 10*3 (F)",   L2[3], TEST_FXN(30));
  fail += check("mixed I+F: L[4] = 13+30 (I)",  L2[4], TEST_FXN(43));
  fail += check("mixed I+F: ret = L[4]",        ret2,  TEST_FXN(43));
  jit_buf_free(b2);

  /* Negative-operand FXNMUL via trampoline -- exercises the
   * sign-handling >> 16 inside jit_rt_fxnmul. */
  static const uint8_t bc_neg[] = {
    BC_FXN8, 0,0, (uint8_t)(int8_t)-6,
    BC_FXN8, 1,0, 4,
    BC_FXNMUL, 2,0, 0,0, 1,0,
    BC_LEAVE, 2,0,
  };
  jit_buf *b3 = jit_translate(bc_neg, sizeof(bc_neg));
  if (!b3) { printf("FAIL  step5c neg returned NULL\n"); return 1; }
  int64_t (*fn3)(int64_t*) = (int64_t(*)(int64_t*))jit_buf_finalize(b3);
  int64_t L3[10] = {0};
  int64_t ret3 = fn3(L3);
  fail += check("FXNMUL -6*4", ret3, TEST_FXN(-24));
  jit_buf_free(b3);

  return fail;
}

int main(void) {
  int fail = 0;
  printf("=== step 0: hand-coded add ===\n");
  fail += step0_add();
  printf("\n=== step 1: typed-int arith emitters ===\n");
  fail += step1_arith();
  printf("\n=== step 2a: comparison emitters ===\n");
  fail += step2_compares();
  printf("\n=== step 2b: backward-jump loop ===\n");
  fail += step2_loop();
  printf("\n=== step 2c: forward-jump branch ===\n");
  fail += step2_forward_jmp();
  printf("\n=== step 3: C-runtime trampoline ===\n");
  fail += step3_call();
  printf("\n=== step 4: SBC -> x86 translator ===\n");
  fail += step4_translate();
  printf("\n=== step 5a: branch resolution (loop + max) ===\n");
  fail += step5_branches();
  printf("\n=== step 5b: immediate-loads + relative branches ===\n");
  fail += step5b_immediates();
  printf("\n=== step 5c: FXN* trampolines (untyped arith) ===\n");
  fail += step5c_fxn_trampoline();
  if (fail) {
    fprintf(stderr, "\n%d check(s) failed\n", fail);
    return 1;
  }
  printf("\nJIT self-test: ALL PASS.\n");
  return 0;
}

#endif
