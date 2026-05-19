// TS-3.13: case-result unification, success path.  When every
// arm and the consumer agree on int, the static check stays
// silent (no compile error; runtime executes normally).
foo X = case X
  int? | _the int 5
  Else | _the int 10
result = _the int (foo 3)
say "got [result]"
