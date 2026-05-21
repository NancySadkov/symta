// TS-4.1: when the static checker can't prove operand types,
// the macro falls back to the generic `_add` / `_sub` / ...
// forms, which compile to SBC_FXNADD (with the tag-check +
// MCALL fallback path).  That keeps untyped code paths working
// and lets the optimised typed paths exist alongside.
//
// Verifies: an untyped function still does the right thing
// for int operands at runtime (FXNADD's int+int fast path).
add_dyn X Y = X + Y
say (add_dyn 10 32)         // 42
say (add_dyn 1.5 2.5)       // 4.0  (float+float via FXNADD float path)
say (add_dyn "hi" " mom")   // hi mom (text.+ mcall fallback)
