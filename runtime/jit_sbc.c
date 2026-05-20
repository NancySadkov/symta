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

    jit_buf *jit = jit_translate(body, body_len);
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
