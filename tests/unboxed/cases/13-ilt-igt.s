// TS-4.2: typed-int comparisons via _ilt / _igt.
//   `X < Y` with X^int Y^int -> bin_op emits `_ilt X Y`
//   -> compiler.s `[_ilt A B] | ssa_fixed2 K ilt A B`
//   -> SIF `ilt dst a b` -> SBC_ILT at runtime.
//
// Result is FXN-tagged 0 or 1 (same as fxnlt's int+int path).
lt X^int Y^int = X < Y
gt X^int Y^int = X > Y
say (lt 3 5)         // 1
say (lt 5 3)         // 0
say (lt 7 7)         // 0
say (gt 5 3)         // 1
say (gt 3 5)         // 0
