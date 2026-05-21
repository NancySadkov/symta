// TS-3.10: multi-stmt body inference.  Fn body has setup + typed return.
mkint2 X =
  Setup 100
  _the int X
Y _the text mkint2(7)
