// 37-the.s -- TS-1 + TS-1.1 + TS-1.2 + TS-1.3: types-as-functions.
//
// Three semantically distinct forms cover the type-system surface:
//
//   _the T X / X^T      ASSERT: runtime tag check, no conversion.
//                       Fails fast if X isn't already T.
//   T X                 CONSTRUCTOR: convert via `.T` method,
//                       then `_the T`-check.  Identity if X is
//                       already T.
//   as_T X              PURE CONVERTER: call `.T` method, no
//                       runtime check.  Just the conversion.
//
// `_unsafe T X` -- C-style trust cast; UB if X isn't T.
//
// Core types (int, float, text, fixtext) are primitives with
// runtime tag.  Struct types (introduced by `type Foo: ...`,
// often cls / ECS component interfaces) follow the same triple.
//
// `list` is a UNION type (T_LIST | T_VIEW | T_CONS | T_BYTES).
// For "is X a list" use `_the list X` / `X^list`; for "convert
// X to a list" use `as_list X` (delegates to the universal `.l`
// method).  Construction is via the native `[X Y Z]` literal
// (or `` `[]` X Y Z `` -- the underlying form).  Concrete
// subtypes built via method forms: `N.bytes`, `Xs.pre H`,
// `Xs[S:E]`.
//
// Run:  symta -f examples/37-the.s


// --- _the / ^: assertion (no conversion) ---------------------

A 5^int
say "A = [A]"                          // A = 5

S "hello"^text
say "S = [S]"                          // S = hello

// TS-3.1: the static checker catches `42^text` at MEX TIME -- a
// compile error, not a runtime trap.  To demonstrate the runtime
// `^T` path that btrap CAN catch, the value has to come from
// somewhere the checker can't statically prove the type of:
F (=> 42)                              // returns dyn
Caught btrap: => F()^text              // F() type is dyn -> static
                                       // check passes; runtime
                                       // check fails; bad fires
say "caught: [Caught.is_bterror]"      // caught: 1


// --- T X: type-constructor (convert via .T, then check) ------

X int 3.5                              // 3.5.int = 3
say "X = [X]"                          // X = 3

Y int "42"                             // "42".int = 42
say "Y = [Y]"                          // Y = 42

W (3 + 4)^int                          // parens to apply ^
say "W = [W]"                          // W = 7


// --- as_T X: pure converter (no check) -----------------------

C as_int 3.5                           // 3.5.int = 3 (no _the check)
say "C = [C]"                          // C = 3

D as_list "abc"                        // text -> char-list via .l
say "D = [D]"                          // D = (a b c)


// --- list union: assertion across all 4 concrete tags --------

L _the list [1 2 3]                    // T_LIST flat
say "L.is_list = [L.is_list]"          // 1

M [1 2 3]                              // native list literal
say "M = [M]"                          // M = (1 2 3)


// --- _unsafe: trust-me cast ----------------------------------

U _unsafe int 9                        // skip the runtime check
say "U = [U]"                          // U = 9


// --- Reassignment ---------------------------------------------

B 0
B = 42^int
say "B = [B]"                          // B = 42


// --- ^ still apply-on-left for non-type RHS ------------------

5^say                                  // 5
"world"^say                            // world
