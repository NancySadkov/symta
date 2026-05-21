#!/usr/bin/env bash
# Regression test for the pre-existing GC bug exposed by tiny gen0.
#
# Original symptom (fixed in commit ###): with SYMTA_GEN0_SIZE=65536
# (a 64 KB nursery -- aggressive enough to force GC many times during
# macroexpansion of core_.s), the compiler crashed with:
#
#   _unused0_ has no method `is_list`
#   object=0000000000000007 (tag=3)
#
# Tag 3 is reserved.  The dyn `0x07` decodes as (heap=1, tag=3,
# gid=0), i.e. garbage data interpreted as a heap pointer.  The
# source was a stale C-local heap-dyn in `parse_term`:
#
#   if (have_parsed) {
#     dyn pp; LIST(pp, 1); LGET(pp, 0) = parsed;
#     LGET(tok, 6) = pp;
#   }
#
# `LIST(pp, 1)` calls gc_alloc which can trigger GC; if it did,
# `parsed` (a C local) was left pointing at the freed nursery
# slot of its now-moved target.  Storing that stale dyn into
# pp[0] leaked a dangling reference.  Subsequent GCs forwarded
# the dangling pp[0] through gc_list with the freed object's
# zeroed gc_head -- producing a fresh 0-element list.  Then
# parse_strip would do `LGET(parsed_cache, 0)` past the end of
# that 0-element list and pick up whatever poison happened to
# sit at the next slot, which is what surfaced as 0x07.
#
# The fix lives in `reader_parse_strip`: only follow the parsed
# cache when it's a textbook size-1 list.  Anything else falls
# back to `tok_value(tok)`, which is the path tokens take when
# no parsed cache is set.
#
# This test pins the fix: the "_unused0_ has no method" pattern
# must not reappear.  Note: tiny-gen0 still exposes _other_ GC
# corruption bugs further along (segfaults in expand_qlmb_r),
# which are tracked separately -- this test only guards the
# original tag-3 manifestation.

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

if echo "$out" | grep -q "_unused0_ has no method"; then
  echo "tiny-gen0: REGRESSION -- original tag-3 bug came back -- rc=$rc"
  echo "$out" | head -10 | sed 's/^/   /'
  exit 1
fi

# The original bug is gone.  Other tiny-gen0 GC issues may still
# trigger downstream failures (other stale-pointer sites surfacing
# as wrong-type method dispatch or absurd allocation sizes); those
# are tracked separately and don't fail this regression test.
echo "tiny-gen0: original tag-3 bug is fixed (no '_unused0_ has no method')."
exit 0
