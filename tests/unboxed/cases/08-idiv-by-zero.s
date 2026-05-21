// TS-4.1: typed-int divide-by-zero traps to `bad` via the
// Windows SEH handler in `w64/ctx.c` (matching SBC_FXNDIV's
// behaviour).  Caught via `btrap`.
div_two X^int Y^int = X / Y
Caught btrap: => div_two 10 0
say "caught: [Caught.is_bterror]"
say "msg: [Caught.text]"
