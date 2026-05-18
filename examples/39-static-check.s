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


// --- TS-3.3: typed-shape propagation through any form --------
//
// The infer_type pass also recognises pre-mex typed shapes
// inside the declaration RHS, so all three TS-1 forms make
// the var's type visible to later refs:

K 5^int                                // ^-ascription
say "K = [K]"                          // K = 5
// L _the text K                       // would COMPILE-ERROR

P int 5                                // T-constructor
say "P = [P]"                          // P = 5
// Q _the text P                       // would COMPILE-ERROR

R text "hi"
say "R = [R]"                          // R = hi
// S _the int R                        // would COMPILE-ERROR


// --- TS-3.4: method-return inference -------------------------
//
// Conversion methods (`.int`, `.float`, `.text`, ...) always
// return a value of the named type, regardless of receiver.
// `infer_type` recognises `[_mcall E [_quote M]]` (the post-mex
// shape) and returns M when M is a known type name.

V _the float (5.float)                 // OK: .float returns float
say "V = [V]"                          // V = 5.0

// _the int (5.float)
//   error: type mismatch: expected `int`, got `float`


// --- TS-3.8: function-call return-type inference -------------
//
// When a function's body has a statically inferable return
// type (single typed expression like `_the T E` or `T E`),
// the function gets registered with that return type.  Call
// sites then propagate the type, so `_the U (f X)` catches
// when f's return doesn't match U.

mkint X = _the int 5                   // returns int
FRes _the int mkint(0)                 // OK: int <- mkint -> int
say "FRes = [FRes]"                    // FRes = 5

// To see the static catch, uncomment:
// _the text mkint(0)
//   error: type mismatch: expected `text`, got `int`


// --- TS-3.10: multi-stmt body inference ---------------------
//
// `infer_type` recurses into pre-mex `|` blocks and post-mex
// `_progn` blocks to find the LAST statement's type.  Lets a
// fn with setup statements before the return-expr still
// register a typed return.

mkint2 X =
  Setup 100                            // setup stmt -- ignored
  _the int X                           // last expr -- propagates

FRes2 _the int mkint2(7)               // OK: int <- mkint2 -> int
say "FRes2 = [FRes2]"                  // FRes2 = 7

// To see the static catch, uncomment:
// _the text mkint2(0)
//   error: type mismatch: expected `text`, got `int`


// --- TS-3.7: if-branch type unification ----------------------
//
// `if cond E1 else E2` -- if both branches have the same
// inferable type, the if-expression has that type.  Composes
// with TS-3.1's mex check so `_the U (if ...)` catches when
// the branches' shared type doesn't match U.

IfRes _the int (if 1: 10 else 20)      // both int -- OK
say "IfRes = [IfRes]"                  // IfRes = 10

WhenRes _the text (when 1: "yes")      // text + No (dyn) -- OK
say "WhenRes = [WhenRes]"              // WhenRes = yes

// To see the static catch, uncomment:
// _the int (if 1: "a" else "b")
//   error: type mismatch: expected `int`, got `text`


// --- TS-3.6: reassignment type-check -------------------------
//
// Once a variable is typed (via `_the T`, `^T`, or `T E`
// declaration), reassigning it to a value of incompatible
// type is a compile error.  Caught via the prior-type info
// pushed into GVarsTypes by the declaration's `_type` wrap.

Z _the int 5
Z = 10                                 // OK: int <- int
say "Z = [Z]"                          // Z = 10

// To see the static catch, uncomment:
// Z = "hi"
//   error: type mismatch on reassign: `Z` is `int`, got `text`

// Untyped vars stay dyn -- no reassign check:
W 0                                    // declaration, NOT typed
W = "hello"                            // OK -- W is dyn
say "W = [W]"                          // W = hello


// --- TS-3.5: case-arm narrowing -----------------------------
//
// Inside a `case X T?:` arm, X is statically known to be T --
// the runtime predicate test that selects the arm doubles as a
// type assertion for the body.  Composes with the static check:
// any `_the U X` inside the arm is verified against T.

classify X =
  case X
    int?   | _the int X        // X is int here -- OK
    float? | _the float X      // X is float here -- OK
    text?  | _the text X       // X is text here -- OK
    Else   | "other"

say "classify 5 = [classify 5]"
say "classify 1.5 = [classify 1.5]"
say "classify 'hi' = [classify 'hi']"

// To see the static catch, uncomment any of these:
// buggy X = case X
//   int? | _the text X        // COMPILE-ERROR: X is int, not text
//   Else | "?"
//
// buggy2 X = case X
//   list? | _the int X        // COMPILE-ERROR: X is list, not int
//   Else | "?"


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
