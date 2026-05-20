#!/usr/bin/env bash
# Compiler-output regression tests.
#
# For every Symta single-file example, run a clean compile via
#   symta.exe <build-dir>
# and compare the produced sbc/go.sbc against a golden in
# tests-compiler/expected/. Symta's compiler is byte-deterministic, so
# any drift in tokens, macroexpansion, SSA, line/col attachments, or
# bytecode emission will trip a test.
#
# Usage:
#   ./tests-compiler/run.sh            # run all
#   ./tests-compiler/run.sh 18         # run examples starting "18"
#   ./tests-compiler/run.sh --update   # rewrite goldens

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SYMTA="$ROOT/symta.exe"
[ -x "$SYMTA" ] || SYMTA="$ROOT/symta"
EXAMPLES="$ROOT/examples"
EXPECT="$HERE/expected"
BUILD="$HERE/build"

GLOB="${1:-}"
UPDATE=0
case "$GLOB" in
  --update) UPDATE=1; GLOB="" ;;
esac

passed=0
failed=0
new=0
fail_names=""

mkdir -p "$EXPECT" "$BUILD"

for f in "$EXAMPLES"/*.s; do
  base=$(basename "$f" .s)
  case "$GLOB" in
    "") ;;
    *) case "$base" in "$GLOB"*) ;; *) continue ;; esac ;;
  esac

  case_dir="$BUILD/$base"
  expected="$EXPECT/$base.sbc"

  # Clean rebuild each time so cached sbc doesn't mask compiler drift.
  rm -rf "$case_dir"
  mkdir -p "$case_dir/src"
  cp "$f" "$case_dir/src/go.s"

  # Force --no-jit so the AOT-default state on Windows doesn't
  # add a native section to the produced go.sbc -- compiler-output
  # goldens are bytecode-only by design (the AOT section's bytes
  # are deterministic but huge; doubling every golden file is not
  # what this test exists to measure).
  ( cd "$case_dir" && "$SYMTA" . --no-jit ) >"$case_dir/build.log" 2>&1
  rc=$?
  actual="$case_dir/sbc/go.sbc"

  if [ $rc -ne 0 ] || [ ! -f "$actual" ]; then
    failed=$((failed+1))
    fail_names="$fail_names $base"
    echo "  BUILD-FAIL $base   (see $case_dir/build.log)"
    tail -5 "$case_dir/build.log" | sed 's/^/         /'
    continue
  fi

  if [ ! -f "$expected" ]; then
    if [ $UPDATE -eq 1 ]; then
      cp "$actual" "$expected"
      echo "  NEW    $base"
      new=$((new+1))
    else
      echo "  ?      $base   (no golden; run with --update to capture)"
      new=$((new+1))
    fi
    continue
  fi

  if cmp -s "$actual" "$expected"; then
    passed=$((passed+1))
  else
    if [ $UPDATE -eq 1 ]; then
      cp "$actual" "$expected"
      echo "  UPDATE $base"
    else
      failed=$((failed+1))
      fail_names="$fail_names $base"
      sa=$(wc -c < "$actual" | tr -d ' ')
      se=$(wc -c < "$expected" | tr -d ' ')
      echo "  FAIL   $base   (actual=${sa}B golden=${se}B)"
      # Show first byte offset that differs (and a few hex bytes around it).
      cmp "$actual" "$expected" 2>/dev/null | head -1 | sed 's/^/         /'
    fi
  fi
done

echo
echo "Summary: $passed passed, $failed failed, $new new"
if [ $failed -gt 0 ]; then
  echo "Failed:$fail_names"
  exit 1
fi
exit 0
