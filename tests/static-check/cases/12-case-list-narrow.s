// TS-3.9: list-shape case narrow (0/1/2-element).
buggy Xs = case Xs
  [A B] | _the text Xs
  Else | "other"
