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

#ifdef JIT_SELF_TEST

/* Step-0 proof: emit
 *
 *   int64_t add(int64_t a, int64_t b) { return a + b; }
 *
 * In Win64 ABI: arg0=RCX, arg1=RDX, return value=RAX.
 *   mov rax, rcx     ; 48 89 c8
 *   add rax, rdx     ; 48 01 d0
 *   ret              ; c3
 *
 * In SysV  ABI: arg0=RDI, arg1=RSI, return value=RAX.
 *   mov rax, rdi     ; 48 89 f8
 *   add rax, rsi     ; 48 01 f0
 *   ret              ; c3
 */
int main(void) {
  jit_buf *b = jit_buf_new(64);
  if (!b) { fprintf(stderr, "alloc failed\n"); return 1; }

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

  struct { int64_t a, b, want; } cases[] = {
    {3, 5, 8}, {10, -2, 8}, {0, 0, 0}, {INT32_MAX, 1, (int64_t)INT32_MAX + 1},
    {-100, 100, 0},
  };
  int fail = 0;
  for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
    int64_t got = fn(cases[i].a, cases[i].b);
    int ok = got == cases[i].want;
    printf("%s  add(%lld, %lld) = %lld (want %lld)\n",
           ok ? "OK " : "FAIL",
           (long long)cases[i].a, (long long)cases[i].b,
           (long long)got, (long long)cases[i].want);
    if (!ok) fail++;
  }
  jit_buf_free(b);
  if (fail) { fprintf(stderr, "%d case(s) failed\n", fail); return 1; }
  printf("\nJIT self-test passed: emitted %zu bytes, all cases match.\n",
         sizeof(add_a_b));
  return 0;
}

#endif
