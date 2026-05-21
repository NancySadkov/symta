// TS-4.1: chained typed-int arithmetic.  Each intermediate
// result is typed int via the `_iadd`/`_isub`/`_imul` recognisers
// in infer_type, so subsequent ops keep using the unboxed path.
//
//   X + Y * 2 - 1
//     becomes (after operator precedence parses it):
//   (X) + ((Y) * 2) - 1
//     -> _isub (_iadd X (_imul Y 2)) 1
chain X^int Y^int = X + Y * 2 - 1
say (chain 10 3)        // 10 + 3*2 - 1 = 15
say (chain 0 0)         // 0 + 0 - 1 = -1
say (chain 100 -10)     // 100 + -20 - 1 = 79
