// TS-4.5 Phase 2: `Xs.n + K` where Xs is statically a list/text
// now routes the `+` to _iadd via the KISS `.n -> int` shortcut.
//
// Before Phase 2: `Xs.n` was unconditionally dyn at infer_type
// time, so `+ Xs.n 1` couldn't prove both operands int, fell to
// FXNADD.  Phase 2 adds a hardcoded shortcut: any `Recv.n` where
// Recv has a known sequence-shaped type returns int.

list_size_plus_one Xs^list = Xs.n + 1
say (list_size_plus_one [10 20 30])     // 4
say (list_size_plus_one [])             // 1
say (list_size_plus_one [1])            // 2

text_size_double S^text = S.n * 2
say (text_size_double "hello")          // 10
say (text_size_double "")               // 0

// `.hash` -> int (universal: any object hashes to an int).
hash_plus_offset X = X.hash + 7
say (hash_plus_offset "key")            // some int

// `.code` -> int for text/fixtext.
char_offset C^fixtext = C.code - 'a'.code
say (char_offset \c)                    // 2
say (char_offset \a)                    // 0
