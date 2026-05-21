#!/usr/bin/env bash
# TS-4 unboxed-arithmetic regression runner.
#
# Each `cases/NN-name.s` is a self-contained Symta snippet
# exercising one of the unboxed-int (or, later, unboxed-float)
# code paths.  The runner compiles + runs each via
# `./symta.exe -f`, captures stdout+stderr, normalises platform
# bits, and compares against `expected/NN-name.out`.
#
# What we test today (behavioural):
#   * typed-int arith (`X^int + Y^int`) produces correct results
#   * untyped arith still works (FXNADD slow path)
#   * mixed-type arith promotes to float
#   * runtime traps (division by zero) still hit `bad`
#   * constant-folding for both-literal cases
#   * static-checker propagates typed-arith result type
#
# What we plan to test (once we have a CLI disassembler):
#   * IADD/ISUB/IMUL/IDIV/IREM opcodes are actually emitted
#     when the static checker proves int operands
#   * untyped operands stay on the FXNADD path
#
# Usage:
#   ./run.sh             # run all
#   ./run.sh 03          # run cases starting with 03
#   ./run.sh --update    # rewrite goldens from current output

set -u
cd "$(dirname "$0")/../.."

SYMTA=./symta.exe
[ -x "$SYMTA" ] || SYMTA=./symta
CASES=tests/unboxed/cases
EXPECT=tests/unboxed/expected
GLOB="${1:-}"
UPDATE=0
case "$GLOB" in
  --update) UPDATE=1; GLOB="" ;;
esac

passed=0
failed=0
new=0
fail_names=""

normalize() {
  # Strip path prefixes and platform-specific bits so goldens
  # compare equal across machines.
  tr -d '\r' \
    | sed -e 's|[A-Z]:/[Uu]sers/[^/]*/[^,]*/symta/\{1,\}|REPO/|g' \
          -e 's|/home/[^/]*/[^,]*/symta/\{1,\}|REPO/|g' \
          -e 's|/Users/[^/]*/[^,]*/symta/\{1,\}|REPO/|g' \
          -e 's|C:/Users/[^/]*/AppData/Local/Temp/|TMP/|g' \
          -e 's|/tmp/|TMP/|g'
}

for f in "$CASES"/*.s; do
  base=$(basename "$f" .s)
  case "$GLOB" in
    "") ;;
    *) case "$base" in "$GLOB"*) ;; *) continue ;; esac ;;
  esac
  expected="$EXPECT/$base.out"

  raw=$(timeout 5 "$SYMTA" -f "$f" 2>&1)
  actual=$(printf '%s' "$raw" | normalize)

  if [ ! -f "$expected" ]; then
    if [ $UPDATE -eq 1 ]; then
      printf '%s\n' "$actual" > "$expected"
      echo "  NEW    $base"
      new=$((new+1))
    else
      echo "  ?      $base   (no golden; run with --update to capture)"
      new=$((new+1))
    fi
    continue
  fi

  golden=$(cat "$expected" | normalize)
  if [ "$actual" = "$golden" ]; then
    passed=$((passed+1))
  else
    if [ $UPDATE -eq 1 ]; then
      printf '%s\n' "$actual" > "$expected"
      echo "  UPDATE $base"
    else
      failed=$((failed+1))
      fail_names="$fail_names $base"
      echo "  FAIL   $base"
      tmp_g=".unboxed-golden-$$"
      tmp_a=".unboxed-actual-$$"
      printf '%s\n' "$golden" > "$tmp_g"
      printf '%s\n' "$actual" > "$tmp_a"
      diff "$tmp_g" "$tmp_a" | head -10 | sed 's/^/         /'
      rm -f "$tmp_g" "$tmp_a"
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
