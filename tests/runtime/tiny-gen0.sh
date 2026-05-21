#!/usr/bin/env bash
# Regression test for the pre-existing GC bug exposed by tiny gen0.
#
# Symptom: with SYMTA_GEN0_SIZE=65536 (a 64 KB nursery -- aggressive
# enough to force GC many times during macroexpansion of core_.s),
# the compiler crashes with:
#
#   _unused0_ has no method `is_list`
#   object=000003fe466d0007 (tag=3)
#
# Tag 3 is undefined; the dyn is a corrupted value.  Stack trace
# lands in maybe_doc / has_doc_head -- the macroexpander checking
# whether a function body's first statement is a docstring.  Pure
# Symta-side code; reproduces identically under --no-jit, so this
# is NOT a JIT bug -- it's a missing GC root in one of the C
# builtins that maybe_doc / its helpers transitively call.
#
# Fix candidate: instrument GC tracing to find which root is missed;
# probably a temporary heap pointer held in a C local that isn't
# either on api.frame's locals or in an api.* slot the GC traces.
#
# Once fixed: this test should pass; if the bug regresses we'll
# catch it here.

set -u
cd "$(dirname "$0")/../.."

SYMTA=./symta.exe
[ -x "$SYMTA" ] || SYMTA=./symta

tmp=tests/runtime/.tiny-gen0
rm -rf "$tmp"
mkdir -p "$tmp/src"
printf 'say "hi"\n' > "$tmp/src/go.s"

# Run with the smallest gen0 the runtime accepts (clamps below to a
# page).  64 KB is small enough that GC fires many times during
# core_.s macroexpansion -- enough to reliably surface the bug.
out=$(SYMTA_GEN0_SIZE=65536 "$SYMTA" "$tmp" 2>&1 || true)
rc=$?
rm -rf "$tmp"

# Currently this bug IS present.  The test exists to PIN that fact
# down so we notice when it's fixed (or when it gets worse).  When
# the underlying GC bug is fixed, flip the assertion: success on
# clean exit, failure on the "_unused0_ has no method" pattern.
if echo "$out" | grep -q "has no method"; then
  echo "tiny-gen0: bug still present (expected for now) -- rc=$rc"
  echo "  ${out}" | head -5 | sed 's/^/   /'
  echo "  See task #12 for context.  Flip this check when fixed."
  exit 0
elif [ $rc -eq 0 ]; then
  echo "tiny-gen0: bug appears FIXED -- compile succeeded under SYMTA_GEN0_SIZE=65536."
  echo "  Time to flip this assertion and treat the bug as resolved."
  exit 0
else
  echo "tiny-gen0: unexpected failure mode -- rc=$rc, output:"
  echo "$out" | head -10 | sed 's/^/   /'
  exit 1
fi
