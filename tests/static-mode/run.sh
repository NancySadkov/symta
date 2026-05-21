#!/usr/bin/env bash
# `static`-macro regression runner.  Same shape as static-check/.
# Each cases/NN-name.s exercises one path of strict-static mode.
# Goldens are either the run's stdout (success), or a one-line
# mex_error message ("static: `+` operands not provably numeric: ...").

set -u
cd "$(dirname "$0")/../.."

SYMTA=./symta.exe
[ -x "$SYMTA" ] || SYMTA=./symta
CASES=tests/static-mode/cases
EXPECT=tests/static-mode/expected
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
  tr -d '\r' \
    | sed -e 's|[A-Z]:/[Uu]sers/[^/]*/[^,]*/symta/\{1,\}|REPO/|g' \
          -e 's|/home/[^/]*/[^,]*/symta/\{1,\}|REPO/|g' \
          -e 's|/Users/[^/]*/[^,]*/symta/\{1,\}|REPO/|g'
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
      diff <(printf '%s\n' "$golden") <(printf '%s\n' "$actual") | head -8 | sed 's/^/         /'
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
