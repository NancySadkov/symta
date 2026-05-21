// Inside `static:` block-form (via static followed by block),
// bare-literal init propagates -- A 1 makes A int for subsequent
// uses, even though outside static the conservative
// `infer_declared_type` wouldn't fire.
foo X^int = static:
  A 1
  A + X
say (foo 10)               // 11

// Loop accumulator with bare-int init.
sum N^int = static:
  Acc 0
  for I [:N]:
    I2 I^int
    Acc = Acc + I2
  Acc
say (sum 10)               // sum 0..10 = 55
