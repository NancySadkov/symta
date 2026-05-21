// TS-4.1: typed-int arithmetic counts as "explicitly typed"
// in infer_declared_type, so a fn whose body is `X + Y` with
// X^int Y^int gets a TS-3.8 fn-return registration of int.
// Downstream `_the U (foo X)` mismatch detection then fires.
//
// This case: passes the static check (int == int).
adder X^int Y^int = X + Y
result = _the int (adder 10 32)
say "result: [result]"
