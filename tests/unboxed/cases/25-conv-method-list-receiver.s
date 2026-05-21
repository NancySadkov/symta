// REGRESSION TEST: static inference must NOT claim that
// conversion methods (`.int`, `.float`, `.text`, `.bytes`) return
// the named type when the receiver is list-shaped.  `list.int`
// MAPS the conversion element-wise -- returns a list, not an int.
//
// The bug this guards against:
//   1. TS-3.4 claims `.int` returns int regardless of receiver.
//   2. TS-4.5 Phase 2 adds a pre-mex `[`.` Recv M]` arm in
//      infer_type so bin_op can see `Xs.int / K` as int/int.
//   3. bin_op emits _IDIV on what's actually a list-tagged value
//      at runtime -- producing garbage that crashes downstream
//      (`game/src/view.s:141` got `int has no method `.`` from
//      a `list.- Ys` call where Ys was the garbage int).
//
// Every line below would have CRASHED (or produced wrong values)
// under the broken inference.  Watch for:
//   - silent garbage at the `/` site
//   - "int has no method `.`" if downstream list arith runs
//   - any change in the printed output
//
// If a future inference rule re-introduces this class of bug
// (claiming a type for `Recv.M` that doesn't match runtime
// behaviour), one of these assertions will trip.

// --- list-of-float -> list-of-int, then arith on the result.
A [3.5 7.2 9.9]
say (A.int)                   // (3 7 9)
say (A.int / 2)               // (1 3 4)   <- maps int-div
say (A.int - [1 2 3])         // (2 5 6)   <- list - list

// --- list-of-int -> list-of-float, then arith.
B [10 20 30]
say (B.float)                 // (10.0 20.0 30.0)
say (B.float * 0.5)           // (5.0 10.0 15.0)

// --- The game-crash pattern: chained conversion + division +
// list subtraction.  This is the exact `view.view_origin = ...`
// shape that broke when `_IDIV` fired on a list operand.
demo Src Mult =
  R Src.int - Mult*2
  R
say (demo [10 20] [3 4])      // (4 12)
say (demo [100.5 200.5] [10 20])  // (80 160)

// --- Through a fn wrapper -- even untyped fn args must not have
// `.int / K` mis-typed.  Without the fix, this crashed inside
// the runtime once the value flowed into another list op.
mapped_int_div Xs K = Xs.int / K
say (mapped_int_div [4.5 8.5 12.5] 2)   // (2 4 6)
