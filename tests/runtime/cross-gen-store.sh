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

# -- Part A -----------------------------------------------------------
# Bracket-literal cache write (reader.c:1031 parse_suf_unary,
# storing a fresh LIST into a possibly-promoted token's slot 6).
# 150 X-vars empirically reliably crashes the bug-#13-reverted build
# under gen0=65536 across all tested runs; below 100 the parse
# finishes before enough gen-0 collections have promoted any token
# across generations.
triggerA="$tmp/bracket.s"
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
} > "$triggerA"

outA=$(SYMTA_GEN0_SIZE=65536 "$SYMTA" -f "$triggerA" 2>&1)
rcA=$?

if [ "$rcA" -ne 0 ]; then
  echo "cross-gen-store: REGRESSION (part A / bracket cache) -- rc=$rcA"
  echo "$outA" | tail -10 | sed 's/^/   /'
  rm -rf "$tmp"
  exit 1
fi
if ! echo "$outA" | grep -q 'total=111750'; then
  echo "cross-gen-store: REGRESSION (part A) -- expected 'total=111750'"
  echo "$outA" | tail -10 | sed 's/^/   /'
  rm -rf "$tmp"
  exit 1
fi

# -- Part B -----------------------------------------------------------
# `.=longname` setter combiner (reader.c:1144 parse_suf_loop),
# storing a fresh bigtext (">6 chars" -- "=somename" with a name long
# enough to spill out of fixtext) into a possibly-promoted token's
# slot 1.  Reverting THAT LSET in isolation reliably crashes a full
# `bash game/build.sh` under tiny gen0 at the `main_data` stage;
# this synthetic trigger drives the same write rate so the test
# catches it without needing the game tree.
#
# We deliberately produce a program whose RUNTIME would error (no
# `=longfieldname_N` setter exists on int), but the SEGV (if the
# barrier is missing) happens during PARSING -- before the bad
# runtime call.  We accept any rc but reject 139 / 134 (SIGSEGV /
# SIGABRT) and "segfault" / "stack is too big" in stderr.
triggerB="$tmp/dotsetter.s"
{
  echo "Junk 0"
  m=400
  i=1
  while [ "$i" -le "$m" ]; do
    echo "Junk.=longfieldname_$i 0"
    i=$((i+1))
  done
} > "$triggerB"

outB=$(SYMTA_GEN0_SIZE=65536 "$SYMTA" -f "$triggerB" 2>&1)
rcB=$?
rm -rf "$tmp"

# Crash signatures (any of these is a regression):
#   rc == 139         POSIX SIGSEGV
#   rc == 134         SIGABRT (rare, but seen on some failure paths)
#   "segfault at"     in stderr
#   "stack is too big" in stderr  (cascading stack-overflow in error path)
if [ "$rcB" -eq 139 ] || [ "$rcB" -eq 134 ] \
   || echo "$outB" | grep -qE 'segfault|stack is too big'; then
  echo "cross-gen-store: REGRESSION (part B / .=name cache) -- rc=$rcB"
  echo "$outB" | tail -10 | sed 's/^/   /'
  exit 1
fi
# rc != 0 with no crash signature == runtime error from the synthetic
# input (expected); part B's contract is "no SEGV during parse," not
# "the program runs cleanly to completion."

echo "cross-gen-store: bracket-cache + .=name setter barriers both hold."
exit 0
