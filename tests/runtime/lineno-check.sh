#!/usr/bin/env bash
# Line-number regression test.
#
# Before the row-init fix in tokenize.c, the bare `-f` path produced
# error messages and stack traces with line numbers off by one (row was
# 0-indexed internally but printed verbatim, so foo on physical line N
# was tagged as `N-1`). This test pins the 1-indexed behavior so we
# notice any future drift.
#
# Exit non-zero if any case fails.

set -u
cd "$(dirname "$0")/../.."

SYMTA=./symta.exe
[ -x "$SYMTA" ] || SYMTA=./symta

tmp=tests/runtime/.lineno-check.s
pass=0
fail=0

check() {
  src="$1"
  want_pat="$2"
  label="$3"
  printf '%s\n' "$src" > "$tmp"
  # Step 8: pass --no-jit so the AOT default doesn't elide the
  # per-instruction pin writes the interpreter uses to recover
  # accurate per-statement source-line info.  JIT'd frames carry
  # pin=0 (the "zero-cost pin tracking" trade in step 5n) and
  # would report the function-header line instead of the body
  # position this test pins down.  Drop the flag once basic-block
  # pin granularity lands in the JIT.
  out=$("$SYMTA" --no-jit -f "$tmp" 2>&1)
  if echo "$out" | grep -q "$want_pat"; then
    pass=$((pass+1))
  else
    fail=$((fail+1))
    echo "  FAIL $label"
    echo "    expected pattern: $want_pat"
    echo "    actual first line: $(echo "$out" | head -1)"
  fi
}

# Case 1: undef on physical line 1 -> row should be 1, not 0.
check '| foo_undefined' ':1,2: undefined variable .foo_undefined.' \
      'line-1 error tagged as line 1'

# Case 2: undef on physical line 3 (skipping a comment + blank).
check '// comment

| foo_undefined' ':3,2: undefined variable .foo_undefined.' \
      'line-3 error tagged as line 3'

# Case 3: undef on physical line 5 in a body with leading blanks.
check '



| foo_undefined' ':5,2: undefined variable .foo_undefined.' \
      'line-5 error tagged as line 5'

# Case 4: CORE-1 -- stack-trace frame for a function in flight
# should track its progress through the body via `lsrc` markers,
# not the function-definition row from fn_meta_t.  We compile the
# trace request via the runtime builtin `print_stack_trace_`
# (CORE-3 suppressed traces from caught `bad`s, so the only
# always-visible trace path goes through that builtin).
#
# Granularity today is per-top-level-statement of a function body;
# nested let-bindings / if-branches inherit the position of the
# most recent statement-level lsrc.  big_fn's body is three
# statements -- Y=, Z=, if-expr -- emitted at lines 8,9,10.  By
# the time the `if` branch fires print_stack_trace_, lsrc(10,4)
# has run, so the frame reports line 10.
check '// pad 1
// pad 2
// pad 3
// pad 4
// pad 5
//
big_fn X =
  Y X + 1
  Z X + 2
  if X > 100
    then say "big"
    else print_stack_trace_

big_fn 0' 'big_fn:[789][0-9]*,' \
      'big_fn frame reports a body-line position, not the function header'

# Case 5: classify uses a `case` dispatch.  The case-dispatch IR
# is a single top-level statement (line 5), so the frame reports
# line 5 -- not the function header (line 4), and not the
# matched-clause line (line 7).  Future work could push lsrc into
# case branches, but the current per-statement coverage already
# beats "always show the `=` line".
check '// pad 1
// pad 2
//
classify V =
  case V:
    1 = "one"
    Else = print_stack_trace_

classify 2' 'classify:[5-9],' \
      'classify frame reports a body-line position, not the function header'

rm -f "$tmp"

echo "Summary: $pass passed, $fail failed"
[ $fail -eq 0 ]
