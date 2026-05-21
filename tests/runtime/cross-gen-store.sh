#!/usr/bin/env bash
# Regression test for the cross-gen direct-write GC bug class.
#
# Class: a heap-object field store written via `LGET(base, off) = v`
# (i.e. raw `((void**)O_PTR(base))[off] = v`) instead of `LSET(base,
# off, v)` skips the generational write barrier.  That's correct
# only when `base` is a freshly-allocated nursery object -- if
# `base` has been promoted to an older generation and `v` is in
# the nursery, the cross-gen reference is never recorded as a
# dirty page.  The next gen-0 collection then doesn't see the slot
# as a root, the nursery copy of `v` gets reclaimed, and `base.off`
# is left pointing at recycled memory.  The crash surfaces much
# later -- usually a SEGV inside gc_list / gc_custom on the next
# older-gen collection that tries to trace the dangling slot, or a
# wrong-tag method-dispatch error when the stale slot reads as a
# nonsense dyn.
#
# Two real bugs of this class hit the runtime in May 2026:
#   #13  reader.c:parse_term cached `pp` into `tok.6` (the parsed-
#        slot) with LGET instead of LSET.  Fixed in 449414a by
#        switching to LSET (the anchor pushes added in the same
#        commit are part of bug #12's fix, not bug #13's).
#   #14  jit.c trampolines (jit_rt_immeq_impl etc.) cached frame-
#        slot dyns into C locals before ARGLIST allocated, then
#        STAR'd those stale locals.  Fixed in 9f3ae76.
#
# Both bugs originally surfaced ONLY on the full game compile
# (~120 .s files); smaller workloads -- including the existing
# tiny-gen0.sh test -- ran clean.  This test produces a synthetic
# input that's large enough in tokens-per-process to flush the
# reader's parsed-cache writes across multiple gen-0 collections
# under SYMTA_GEN0_SIZE=65536, deterministically reproducing the
# crash when the LSET barrier is missing.
#
# To verify this catches bug #13 specifically: change `LSET(tok, 6,
# pp)` back to `LGET(tok, 6) = pp;` in runtime/reader.c (~line
# 893) and rebuild.  This script will SEGV (rc=139) with no stderr
# output -- the bare segfault is the diagnostic.
#
# Confidence (1-10) that this test would have caught bug #13: 9.
# It directly exercises the canonical trigger -- many integer/
# bracket literal tokens with parsed-cache writes under aggressive
# gen-0 churn.  Reverting the LSET in reader.c reliably segfaults
# this input across 5/5 runs.
#
# Confidence that it would have caught bug #14: 3.  Bug #14 is
# the same CLASS (stale-after-alloc / missing barrier) but lives
# in the JIT trampolines, not the reader.  This test runs Symta
# code through both -- it does call ARGLIST-allocating builtins
# inside a tiny-gen0 environment -- so there's a plausible path,
# but no specific JIT-trampoline coverage is forced.  Bug #14's
# original symptom (qlmb_supply_cvars sees No instead of list)
# is JIT-specific and would more reliably be caught by exercising
# the immeq/fxnlset families directly.

set -u
cd "$(dirname "$0")/../.."

SYMTA=./symta.exe
[ -x "$SYMTA" ] || SYMTA=./symta

tmp=tests/runtime/.cross-gen-store
rm -rf "$tmp"
mkdir -p "$tmp"

# Generate the trigger: many bracket-literal tokens (each going
# through parse_term's `KW_brackets` cache-write path) followed by
# many references back into them.  150 X-vars empirically reliably
# crashes the bug-#13-reverted build under gen0=65536 across all
# tested runs; below 100 the parse finishes before enough gen-0
# collections have promoted any token across generations.
trigger="$tmp/stress.s"
{
  n=150
  i=0
  while [ "$i" -lt "$n" ]; do
    echo "X$i [$((i*10)) $((i*10+1)) $((i*10+2))]"
    i=$((i+1))
  done
  echo 'Total 0'
  i=0
  while [ "$i" -lt "$n" ]; do
    echo "Total X$i.0 + Total"
    i=$((i+1))
  done
  echo 'say "total=[Total]"'
} > "$trigger"

# Tiny gen0 forces many young-gen collections during the parse,
# so by the time later X-vars are parsed the earlier `tok`s have
# already been promoted out of gen 0.  Without the LSET barrier
# their parsed-cache slots silently dangle and the next gen-0/
# gen-4 collection segfaults walking the freed nursery slot.
out=$(SYMTA_GEN0_SIZE=65536 "$SYMTA" -f "$trigger" 2>&1)
rc=$?
rm -rf "$tmp"

# Expected: rc=0 and stdout ends with "total=111750" (the closed-
# form sum of (i*10) for i in 0..149 == 10 * 149*150/2).  Anything
# else -- segfault, error message, wrong total -- is a regression.
if [ "$rc" -ne 0 ]; then
  echo "cross-gen-store: REGRESSION -- runtime exited rc=$rc (segv if 139)"
  echo "$out" | tail -10 | sed 's/^/   /'
  exit 1
fi

if ! echo "$out" | grep -q 'total=111750'; then
  echo "cross-gen-store: REGRESSION -- expected 'total=111750' in output"
  echo "$out" | tail -10 | sed 's/^/   /'
  exit 1
fi

echo "cross-gen-store: parsed-cache LSET barrier holds (no segv, total=111750)."
exit 0
