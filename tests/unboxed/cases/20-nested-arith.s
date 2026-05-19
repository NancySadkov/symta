// TS-4.3: pre-mex arith arms in infer_type let nested typed
// expressions get the unboxed path end-to-end.
//
//   foo X^int Y^int = (X + Y) * 2
//   -> bin_op on outer `*`: infer_type X+Y = "int" (via
//      arith_result_type), infer_type 2 = "int"
//   -> _imul.  And the inner `+` is _iadd because both X and Y
//      prove int from the typed-param wrap.
nested X^int Y^int = (X + Y) * 2
say (nested 3 5)          // (3+5)*2 = 16
say (nested 10 -2)        // (10+-2)*2 = 16
say (nested 0 100)        // (0+100)*2 = 200
