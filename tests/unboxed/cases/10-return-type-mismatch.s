// TS-4.1: typed-int return type propagates to a static
// mismatch when the caller asserts a different type.
// TS-4.3a lifted the earlier `_the int (X + Y)` workaround --
// infer_declared_type now peeks through pre-mex `+` when both
// operands are declaration-typed (here via the typed params).
adder X^int Y^int = X + Y
result = _the text (adder 10 32)    // text mismatches inferred int
