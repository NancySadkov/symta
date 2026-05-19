// TS-3.13: case-result type unification.  When every arm of a
// case form has the same statically-declared type, the whole
// case expression takes that type -- so a function whose body
// is a case dispatch (any arm count, mixed predicate / list /
// Else arms) gets a proper TS-3.8 fn-return registration.
//
// Conservative: uses `infer_declared_type`, so bare literals
// (`5`, `"hi"`) don't propagate -- arms have to opt in via
// `_the` / `^` / type-constructor / typed-fn call.  Matches
// TS-3.6's "bare init doesn't auto-type a var" rule.
foo X = case X
  int? | _the int 5
  Else | _the int 10
result = _the text (foo 3)
