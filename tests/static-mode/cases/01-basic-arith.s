// `static Expr` succeeds when both operands of every numeric
// op have a known type.  Pure literals trivially typed.
say (static (5 + 3))      // 8
say (static (5 * 3))      // 15
say (static (1.5 + 2.5))  // 4.0
