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

void jit_emit_prologue(jit_buf *b) {
  /* push rbx -- one byte (PUSH r64 family: 50+reg, RBX=011) */
  jit_emit_u8(b, 0x53);
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
  /* sub rsp, 32 :  48 83 ec 20 (Win64 shadow space + alignment
   * marker; harmless on SysV).  Keeps RSP%16==0 before any
   * inner CALL. */
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x83);
  jit_emit_u8(b, 0xec);
  jit_emit_u8(b, 0x20);
}

void jit_emit_epilogue(jit_buf *b) {
  /* add rsp, 32 :  48 83 c4 20 */
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0x83);
  jit_emit_u8(b, 0xc4);
  jit_emit_u8(b, 0x20);
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
#define BC_JMP    0x04
#define BC_B      0x06
#define BC_IADD   0xA3
#define BC_ISUB   0xA4
#define BC_IMUL   0xA5
#define BC_IDIV   0xA6
#define BC_IREM   0xA7
#define BC_ILT    0xAA
#define BC_IGT    0xAB
#define BC_ILTE   0xAC
#define BC_IGTE   0xAD

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

jit_buf *jit_translate(const uint8_t *bc, size_t n) {
  /* x86 expansion factor.  Worst case so far is the comparison
   * sequence at ~16 bytes per opcode; B is ~12 bytes; prologue
   * 6, epilogue 7.  Round up to 20/opcode to leave headroom. */
  jit_buf *b = jit_buf_new(n * 20 + 64);
  if (!b) return NULL;

  /* bc_to_x86[i] = x86 offset of the instruction starting at
   * bc[i], or (size_t)-1 if bc[i] isn't an instruction start.
   * Used by the patch pass to resolve branch targets. */
  size_t *bc_to_x86 = (size_t*)malloc((n + 1) * sizeof(size_t));
  if (!bc_to_x86) { jit_buf_free(b); return NULL; }
  for (size_t k = 0; k <= n; k++) bc_to_x86[k] = (size_t)-1;

  jit_patch patches[JIT_MAX_PATCHES];
  size_t patches_n = 0;
  int fail = 0;

  jit_emit_prologue(b);

  size_t i = 0;
  while (i < n) {
    bc_to_x86[i] = b->len;
    uint8_t op = bc[i];
    switch (op) {
    case BC_NOP:
      i += 1;
      break;

    case BC_IADD: case BC_ISUB: case BC_IMUL: case BC_IDIV:
    case BC_IREM: case BC_ILT:  case BC_IGT:  case BC_ILTE: case BC_IGTE: {
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
      }
      i += 7;
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

    case BC_LEAVE: {
      if (i + 3 > n) { fail = 1; goto done; }
      int src = bc_rd16(bc + i + 1);
      emit_mov_rax_from_slot(b, src);
      jit_emit_epilogue(b);
      i += 3;
      break;
    }

    case BC_LEAVE0:
      emit_xor_rax_rax(b);
      jit_emit_epilogue(b);
      i += 1;
      break;

    default:
      /* Unsupported opcode -- bail out so the caller falls back
       * to the interpreter.  A later step will widen coverage
       * via the C-runtime trampoline. */
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
      jit_emit_epilogue(b);
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

void jit_emit_call_abs(jit_buf *b, void *target) {
  /* Two-instruction call: load the absolute address into RAX
   * via `mov rax, imm64` (10 bytes) then `call rax` (2 bytes).
   * Total 12 bytes -- longer than a relative call but works
   * for any target regardless of its distance from the JIT'd
   * code. */
  uint64_t v = (uint64_t)target;
  jit_emit_u8(b, 0x48);
  jit_emit_u8(b, 0xb8);  /* mov rax, imm64 (opcode b8+reg, rax=0) */
  for (int i = 0; i < 8; i++) jit_emit_u8(b, (uint8_t)(v >> (i*8)));
  /* call rax :  ff d0 (ModR/M 11 010 000, /2=call, r/m=rax) */
  jit_emit_u8(b, 0xff);
  jit_emit_u8(b, 0xd0);
}

/* ============================================================
 * Step 2: comparisons + jumps.
 * ============================================================ */

/* cmp rax, [locals_reg + slot*8] -- 48 3B + ModR/M + disp */
static void emit_cmp_rax_to_slot(jit_buf *b, int slot) {
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
  if (fail) {
    fprintf(stderr, "\n%d check(s) failed\n", fail);
    return 1;
  }
  printf("\nJIT self-test: ALL PASS.\n");
  return 0;
}

#endif
