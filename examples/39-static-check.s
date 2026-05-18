// 39-static-check.s -- TS-3.1: compile-time type checking.
//
// The strict static checker rejects obvious type mismatches at
// MEX time, before any runtime check fires.  Today it covers the
// substrate cases below; broader inference (TS-3.2+) follows.
//
//   _the int 5            // compiles -- 5 is int
//   _the int 1.5          // COMPILE ERROR -- 1.5 is float
//   _the int "abc"        // COMPILE ERROR -- "abc" is text
//   _the text 42          // COMPILE ERROR
//
// The check is conservative: it only fires when the RHS type is
// statically known.  Anything the checker can't prove falls
// through to the runtime check (the TS-1 substrate).  `_unsafe`
// bypasses both checks.
//
// To trigger the static failures below, uncomment them one at a
// time.  As shipped, this file compiles + runs to demonstrate
// the passing cases.
//
// Run:  symta -f examples/39-static-check.s


// --- statically valid (compiles) -----------------------------

A _the int 5                           // int <- int
say "A = [A]"                          // A = 5

B _the float 1.5                       // float <- float
say "B = [B]"                          // B = 1.5

C _the text "hi"                       // text <- text
say "C = [C]"                          // C = hi


// --- dynamic RHS: check passes statically, runs at runtime ---

F (=> 42)
G F()^int                              // dyn RHS -> runtime check
say "G = [G]"                          // G = 42

H btrap: => F()^text                   // dyn RHS, runtime fails
say "H caught: [H.is_bterror]"         // 1


// --- _unsafe: skips both static and runtime checks -----------

U _unsafe int 9                        // no checks at all
say "U = [U]"                          // U = 9


// --- TS-3.2: variable-flow propagation -----------------------
//
// A typed declaration / assignment records the variable's type
// in GVarsTypes.  Subsequent uses inside a `_the` slot get
// checked against that type:

I _the int 5
J _the int I                           // OK: I is known int
say "J = [J]"                          // J = 5

T _the text "hello"
// _the int T                          // would COMPILE-ERROR: T is text

// Reassignment via `=` also propagates:
N _the int 0
N = _the int 100
say "N = [N]"                          // N = 100


// --- statically invalid: uncomment to see the compile error --

// X _the int 1.5
//    error: type mismatch: expected `int`, got `float`
//
// Y _the text 42
//    error: type mismatch: expected `text`, got `int`
//
// Z _the int "no"
//    error: type mismatch: expected `int`, got `text`
//
// // Variable-flow mismatch:
// T _the text "hi"
// W _the int T
//    error: type mismatch: expected `int`, got `text`
