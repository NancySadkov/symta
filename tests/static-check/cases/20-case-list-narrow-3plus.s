// TS-3.9: list-shape case narrow for 3+ element fixed patterns.
// Previously deferred -- bisection traced the failure to a
// pre-existing `.` macro field-lookup bug in macro_ops.s (the
// `got 0 = 1` short-circuit miscomputed P for non-struct types
// like `list`).  Once that was fixed, arbitrary-length list
// patterns narrow cleanly.
foo Xs = case Xs
  [A B C D E] | _the int Xs
  Else | "other"
