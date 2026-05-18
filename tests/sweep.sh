#!/bin/bash
# Run every Symta test suite and print one Summary line per suite.
#
# We call /usr/bin/bash (or `bash` from $PATH if absolute path is
# unavailable, e.g. on macOS) instead of plain `bash` because on
# Windows w64devkit a bare `bash` in a subshell context can fall
# back to a stripped-down shell that doesn't support arrays --
# which then dies on `CASES=( ... )` in tests/{ffi,uim}/run.sh.
SH="/usr/bin/bash"
[ -x "$SH" ] || SH=bash

cd "$(dirname "$0")/.."

for s in tokenizer reader macros runtime compiler static-check gfx ffi am uim repl wh; do
  echo -n "$s: "
  "$SH" tests/$s/run.sh 2>&1 | grep "^Summary" || echo "(no summary)"
done

echo -n "ncm: "
"$SH" ncm/tests/run.sh 2>&1 | grep "^Summary" || echo "(no summary)"

echo -n "lineno: "
"$SH" tests/runtime/lineno-check.sh 2>&1 | grep "^Summary"

echo -n "drift: "
"$SH" tests/bootstrap/drift.sh 3 2>&1 | tail -1
