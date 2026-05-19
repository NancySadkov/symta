// TS-3.9d: typed element pattern `X^T` in a case arm.  When T
// is a known type-name, the pattern binds X with X statically
// typed T over the arm body (composes with the existing T-as-
// constructor runtime check from `T Key`).  Same surface
// vocabulary as TS-1.1 ascription `E^T` and TS-1.2 constructor
// `T E` -- the typename-is-a-function ontology stays consistent.
foo Xs = case Xs
  [X^int @Rest] | _the text X
  Else | "other"
