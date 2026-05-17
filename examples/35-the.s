// 35-the.s -- TS-1 substrate: `_the` and `_unsafe`.
//
// `_the T E` is the DYN -> typed boundary: runtime-checks that E
// is of type T and propagates T statically for downstream
// type-aware fast-paths.  Raises a runtime error on mismatch.
//
// `_unsafe T E` is the C-style trust-me cast: skips the runtime
// check, propagates T statically.  Undefined behaviour if the
// value isn't really of type T.
//
// Surface sugar (`X^T = E`, `f^Ret A^T1 ... = body`) will be
// layered on top in a later phase that introduces a non-`^`
// operator for typed declarations (`^` already binds the
// apply-on-left operator -- see `\`^\` A B = ...` in macro.s).
//
// Run:  symta -f examples/35-the.s


// --- _the: inline ascription ---------------------------------

X _the int 5
say "X = [X]"                          // X = 5

S _the text "hello"
say "S = [S]"                          // S = hello


// --- _unsafe: trust-me cast (skip the check) -----------------
// Here 7 really IS an int, so no UB.

U _unsafe int 7
say "U = [U]"                          // U = 7


// --- _the catches mismatches at runtime ----------------------
// Wrap in btrap so the test driver doesn't see the bterror as
// an unhandled error.

Caught btrap: => _the int "not an int"
say "caught: [Caught.is_bterror]"      // caught: 1
