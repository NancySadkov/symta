// TS-4.1: typed-int return type propagates to a static
// mismatch when the caller asserts a different type.  The fn
// body is wrapped with `_the int` to make the declared return
// type explicit -- otherwise infer_declared_type can't see
// past the bare `+` form to the typed-arith result.  TS-4
// follow-up will lift this restriction (teach
// infer_declared_type about `+`/`-`/... when operands carry
// typed-param annotations).
adder X^int Y^int = _the int (X + Y)
result = _the text (adder 10 32)    // text mismatches inferred int
