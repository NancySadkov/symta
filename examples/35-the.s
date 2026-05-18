// 35-the.s -- TS-1 + TS-1.1: types-as-functions via overloaded `^`.
//
// Substrate (TS-1, from runtime/sbc.c via mex):
//
//   _the T E      DYN -> typed boundary.  Runtime-checks E is
//                 of type T; propagates T statically for fast-
//                 paths.  Mismatch raises an error via `bad`.
//   _unsafe T E   C-style trust-me cast.  Same propagation
//                 minus the runtime check.  UB if E isn't T.
//
// Surface sugar (TS-1.1):
//
//   X^T           When T is a known type (primitive recognised
//                 by tag_for_predicate / tags_for_predicate, or
//                 user-defined via `type Foo:...`), the `^`
//                 macro emits `_the T X` instead of the regular
//                 apply-on-left `T(X)`.  `^` keeps its existing
//                 semantics for non-type RHS.
//   X Expr^T      Declaration with typed value.  The RHS goes
//                 through the overloaded `^`; downstream sees
//                 X as typed.
//   X = Expr^T    Reassignment with typed value (X must be
//                 declared first).
//
// `X^T = E` and `X^T 5` (with `^` on the LHS) do NOT work --
// they'd collide with the `\`^\` A B = body` operator-def form,
// which has the same AST shape.  Use the `X Expr^T` /
// `X = Expr^T` forms instead.
//
// Run:  symta -f examples/35-the.s


// --- ^ overload: typed ascription (expression position) ------

A 5^int                                // _the int 5 = 5
say "A = [A]"                          // A = 5

S "hello"^text                         // text is multi-tag (FIXTEXT|TEXT)
say "S = [S]"                          // S = hello

// Chained: parens to apply ^ before assignment
Z (3 + 4)^int
say "Z = [Z]"                          // Z = 7


// --- Reassignment with typed RHS -----------------------------

B 0                                    // declare B
B = 42^int                             // reassign via typed expr
say "B = [B]"                          // B = 42


// --- ^ STILL apply-on-left for non-type RHS ------------------

5^say                                  // 5
"world"^say                            // world


// --- Bare _the and _unsafe still work ------------------------

Y _the int 7
say "Y = [Y]"                          // Y = 7

U _unsafe int 9                        // skip the runtime check
say "U = [U]"                          // U = 9


// --- Runtime check fires on mismatch -------------------------

Caught btrap: => 42^text               // 42.is_text = 0, bad fires
say "caught: [Caught.is_bterror]"      // caught: 1
