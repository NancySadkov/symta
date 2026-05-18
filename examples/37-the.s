// 37-the.s -- TS-1 + TS-1.1 + TS-1.2: types-as-functions.
//
// Substrate (TS-1):
//
//   _the T E      DYN -> typed boundary.  Runtime-checks E is
//                 of type T; propagates T statically for fast-
//                 paths.  Mismatch raises an error via `bad`.
//   _unsafe T E   C-style trust-me cast.  Same propagation
//                 minus the runtime check.  UB if E isn't T.
//
// Type-constructor macros (TS-1.2):
//
//   int X       coerces X to int via X.int method (which is
//               identity for int, b_float_int for float,
//               text.int parse for text), then `_the int`
//               runtime-checks the result.  Same form for
//               float, text, fixtext.
//
//     int 3      -> 3.int = 3 (identity)         -> 3
//     int 3.5    -> 3.5.int = 3 (float.int)      -> 3
//     int "42"   -> "42".int = 42 (text.int)     -> 42
//
// Surface sugar (TS-1.1):
//
//   X^T          When T is a known type, the `^` macro emits
//                `_the T X` -- assertion form (no conversion).
//                Falls through to apply-on-left for non-types.
//
//                Note: `X^int` is the ASSERTION form (E must
//                already be int).  For coercion, write `int X`.
//
// `list` and `fn` are NOT registered as type-constructors --
// `list` is the list-builder (`list 1 2 3` -> [1 2 3]) and
// `fn` collides with user-code local-fn definitions.  For
// "is X a fn/list" use `_the fn X` / `_the list X` or
// `X^fn` / `X^list`.
//
// Run:  symta -f examples/37-the.s


// --- Type-constructor: coercion via .T method ----------------

X int 123                              // identity: 123.int = 123
say "X = [X]"                          // X = 123

Y int 3.5                              // coerce: 3.5.int = 3
say "Y = [Y]"                          // Y = 3

Z int "42"                             // coerce: "42".int = 42
say "Z = [Z]"                          // Z = 42


// --- ^ assertion: no coercion --------------------------------

A 5^int                                // _the int 5 = 5 (assertion)
say "A = [A]"                          // A = 5

S "hello"^text                         // _the text "hello" = "hello"
say "S = [S]"                          // S = hello

// Chained: parens to apply ^ before assignment
W (3 + 4)^int
say "W = [W]"                          // W = 7


// --- Reassignment with typed RHS -----------------------------

B 0                                    // declare B
B = 42^int                             // reassign via typed expr
say "B = [B]"                          // B = 42


// --- ^ STILL apply-on-left for non-type RHS ------------------

5^say                                  // 5
"world"^say                            // world


// --- Bare _the and _unsafe still work ------------------------

P _the int 7
say "P = [P]"                          // P = 7

U _unsafe int 9                        // skip the runtime check
say "U = [U]"                          // U = 9


// --- Assertion check fires on mismatch -----------------------

Caught btrap: => 42^text               // 42.is_text = 0, bad fires
say "caught: [Caught.is_bterror]"      // caught: 1
