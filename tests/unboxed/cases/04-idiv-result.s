// TS-4.1: typed-int divide (`/`) via `_idiv`.
// C-level integer division: truncates toward zero.
div_two X^int Y^int = X / Y
say (div_two 100 7)
say (div_two -100 7)
say (div_two 6 3)
