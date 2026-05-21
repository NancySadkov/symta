// TS-4.3c: typed-local declaration now narrows subsequent
// references via the sideband channel in expand_block.
//
// Previously the eager-mex of `Y + X` saw Y untyped because
// the TS-3.2 `_type Y` wrap was built AFTER expand_block_item
// already mexed the use.  expand_block now pushes the declared
// type into GVarsTypes after each item, so the next item's
// eager-mex sees it.
local_sum X^int =
  Y X^int
  Y + X
say (local_sum 5)       // 5+5 = 10
say (local_sum 100)     // 100+100 = 200

// Multi-stage chain: each typed decl unblocks IADD for the
// next use.
chain X^int =
  A X^int
  B A + X
  C B^int
  C + A
say (chain 10)          // (10+10) + 10 = 30
say (chain 7)           // (7+7) + 7 = 21

// Loop with typed accumulator + typed iter var inside body.
loop_typed N^int =
  Acc 0^int
  for I [:N]:
    I2 I^int
    Acc = Acc + I2
  Acc
say (loop_typed 10)     // sum 0..10 = 55
say (loop_typed 100)    // sum 0..100 = 5050
