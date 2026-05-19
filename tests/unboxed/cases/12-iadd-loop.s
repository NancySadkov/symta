// TS-4.1: tight inner loop exercising typed-int arithmetic.
// Every iteration of the body should run on the IADD path
// (X is the typed-int parameter; Acc is reassigned each step
// and is therefore not statically typed, BUT `Acc + X` has
// only X proved int -- and the strict checker has no way to
// propagate Acc's int-ness through the reassignment yet).
//
// This case documents the LIMIT of TS-4.1: only the operands
// whose types `infer_type` can prove get the unboxed path.
// TS-4.3 (unboxed locals) will extend the proof through
// reassignments.
loop_sum N^int =
  Acc 0
  for I [:N]: Acc = Acc + I
  Acc
say (loop_sum 10)        // 1+2+...+10 = 55
say (loop_sum 100)       // 1+2+...+100 = 5050
