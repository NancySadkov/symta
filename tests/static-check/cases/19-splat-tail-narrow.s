// TS-3.9: splat-tail narrow.  In `case L [X@Xs]: <body>`, Xs is
// known to be `list` inside the body (it's bound to the list
// tail produced by `Key.tail`).  Emitted via `_let1` at the
// binding site of the pattern, not as an outer `_type` wrap.
foo Xs = case Xs
  [A @Rest] | _the int Rest
  Else | 0
