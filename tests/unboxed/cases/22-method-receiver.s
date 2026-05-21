// TS-4.5 Phase 1: method-receiver `Me` is now auto-typed for
// primitive types.  Method bodies that do `Me +/-/*/% K` no
// longer need explicit `Me^int` ascription -- the macro layer
// wraps the body in `[_type int Me body]` automatically.
//
// To exercise: define a method on int with raw Me arithmetic.
// `mex` sees Me as int via the auto-wrap; `bin_op` routes to
// _iadd / _isub / _imul / _idiv / _irem.

// Smoke test: builtin float.`++` / float.`--` already exercise
// the auto-wrap on `float` -- they're `Me + 1.0` / `Me - 1.0`.
say (4.5.`++`)             // 5.5
say (2.5.`--`)              // 1.5

// User-defined methods on primitives also get the wrap.  Inside
// `int.double`, Me is now statically int -- `Me * 2` routes to
// _imul, not the generic _mul.
int.double = Me * 2
say (5.double)              // 10
say (-3.double)             // -6

// Chained method dispatch -- each method's Me is typed by its
// receiver, so the arithmetic each does is unboxed.
int.cube = Me * Me * Me
say (3.cube)                // 27
say (5.cube)                // 125
