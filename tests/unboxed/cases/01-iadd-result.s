// TS-4.1: typed-int add (`+`) produces correct results.
//   bin_op proves both operands int -> emits `_iadd A B`
//   -> compiler.s SsaFormCases hits `[_iadd A B]` arm
//   -> SIF `iadd dst a b` -> SBC_IADD at runtime
//   -> C-side FXNADD on the tagged-int representation.
//
// The wire shape and bit semantics are byte-for-byte identical
// to SBC_FXNADD's int+int fast path; the difference is that
// SBC_IADD has no tag check or fallback, so a future x86 backend
// can lower it to a literal native ADD without runtime
// dispatch.
add_two X^int Y^int = X + Y
say (add_two 10 32)
say (add_two -5 5)
say (add_two 0 0)
