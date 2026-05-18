// TS-3.5: predicate-arm case narrow.
buggy X = case X
  int? | _the text X
  Else | "other"
