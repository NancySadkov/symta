// TS-4.1: both-literal arithmetic gets constant-folded at
// compile time by `ssa_fixed2`'s int+int shortcut.  The output
// SBC contains a plain ldfxn, not an IADD/IMUL.  Behavioural
// check: results are correct and the program runs.
say (10 + 32)        // 42
say (100 - 7)        // 93
say (6 * 7)          // 42
say (100 / 7)        // 14
say (100 % 7)        // 2
