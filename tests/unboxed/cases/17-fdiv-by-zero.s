// TS-4.2: typed-float divide-by-zero substitutes FLT_MIN (matching
// SBC_FXNDIV's float-fallback semantics; avoids IEEE inf/nan).
// The result is a very small positive float, not inf/nan.
fdiv_two X^float Y^float = X / Y
Result fdiv_two 1.0 0.0
say "no trap: [Result.is_float]"   // 1
say "positive: [Result > 0.0]"     // 1
