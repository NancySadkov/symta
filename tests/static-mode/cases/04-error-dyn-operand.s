// `static` refuses to fall back to FXNADD when an operand's
// type isn't statically proven.  The whole point of opting in
// is to surface the missing annotation as a compile-time error.
buggy X = static (X + 5)
