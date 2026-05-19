// TS-4.2: typed-float arithmetic via _fadd / _fmul.
//   X^float + Y^float -> bin_op routes to _fadd
//   -> compiler.s emits SIF `fadd` -> SBC_FADD at runtime.
//
// Runtime path uses STFLT/LDFLT directly -- no tag check, no
// MCALL.  An x86 backend can lower these to ADDSS / SUBSS /
// MULSS / DIVSS on xmm regs.
fadd_two X^float Y^float = X + Y
fmul_two X^float Y^float = X * Y
say (fadd_two 1.5 2.5)        // 4.0
say (fadd_two -1.0 1.0)       // 0.0
say (fmul_two 2.0 3.0)        // 6.0
say (fmul_two 0.5 0.5)        // 0.25
