// idioms.s -- compact list-processing patterns from sbe.md.
//
// One short example per documented idiom, each followed by its
// expected value. Together this is the regression net for the
// `{}` / `(...)` macro family that distinguishes Symta from
// other Lisps.
//
// Skipped (currently broken in the macroexpander, not in this
// test's scope):
//   L(:@_ -P&=0)   -- bad match case `&`
//   L(/~ P @_)     -- "invalid `~` expr"
//   L(@_ ~<P @_)   -- "invalid `~` expr"
//   10{&1*2:}      -- "implement `&Var` (1)"
//
// Run with:  symta -f tests-macros/cases/idioms.s


// ----------------------------------------------------------------
// {} -- map / filter / replace / group
// ----------------------------------------------------------------
say "filter & replace:"
say [1 2 3 4 5]{:?%2}              // keep odds        -> (1 3 5)
say [1 2 3 4 5]{?%2=}              // skip odds        -> (2 4)
say [1 2 3 4 5]{?%2=\odd}          // tag odds         -> (odd 2 odd 4 odd)
say [1 2 3 4 5 6 7 8 9]{~/3}       // group of 3       -> ((1 2 3) (4 5 6) (7 8 9))


// ----------------------------------------------------------------
// (...) -- list-shape lambdas
// ----------------------------------------------------------------
say "list-shape (...):"
say [1 2 3 4 5](:@_ ?>3 @_)        // has > 3?         -> 1
say [1 2 3 4 5](:@_ ?>9 @_)        //                  -> 0
say [1 2 3 4 5](/3 @~)             // drop first 3     -> (4 5)
say [1 2 3 4 5](@~ /2)             // drop last 2      -> (1 2 3)
say [1 2 3 4 5](/1 @~ /1)          // drop both ends   -> (2 3 4)
say [1 2 3 4 5](~ @_)              // first elem       -> 1
say [1 2 3 4 5](_ @~)              // rest             -> (2 3 4 5)


// ----------------------------------------------------------------
// Color triple ops (RGB triples flattened in a single list).
// ----------------------------------------------------------------
say "color triples:"
L: 10 20 30 40 50 60
say L{/3=@Q.f}                     // BGR              -> (30 20 10 60 50 40)
say L{/3=Q.z/3}                    // grayscale 8bit   -> (20 50)
say L{/3&=Q.z/3..3}                // grayscale 24bit  -> (20 20 20 50 50 50)


// ----------------------------------------------------------------
// Text -- {} on a string acts char-by-char.
// ----------------------------------------------------------------
say "text manipulation:"
say "/usr/bin/bash"{'/' = '*'}     // char replace     -> *usr*bin*bash
say "/usr/bin/bash"{@'usr' = @'USR'}  // substring     -> /USR/bin/bash


// ----------------------------------------------------------------
// Splitting / projecting via ~name auto-closures.
// ----------------------------------------------------------------
say "split into planes:"
Pix: 100 50 25 200 150 75 30 60 90
[Rs Gs Bs] Pix{R G B = ~r.R ~g.G ~b.B}
say "  Rs=[Rs]"
say "  Gs=[Gs]"
say "  Bs=[Bs]"


// ----------------------------------------------------------------
// Folds, sums, counters.
// ----------------------------------------------------------------
say "folds / counters:"
say [1 2 3 4 5]{&~s+?}             // sum              -> 15
say [1 2 3 4 5]{?%2=~i+}           // count odds       -> 3
say [1 2 3 4 5]{~j+:?%2=~i+}       // count (odd even) -> (2 3)
say [1 2 3 4 5 6]{~j.Q:?%2=~i.Q}   // split odd/even   -> ((2 4 6) (1 3 5))


// ----------------------------------------------------------------
// First / last matching element and their indices.
// ----------------------------------------------------------------
say "first/last match:"
say [2 4 5 6 7]{?%2=Q%}            // first odd        -> 5
say [2 4 5 6 7]{?%2=~_=Q}          // last odd         -> 7
say [2 4 5 6 7]{?%2=~~%}           // index 1st odd    -> 2
say [2 4 5 6 7]{?%2=~_=~~}         // index last odd   -> 4


// ----------------------------------------------------------------
// Max via auto-closure.
// ----------------------------------------------------------------
say "max:"
say [3 1 4 1 5 9 2 6]{(max &~x ?)}  // max element     -> 9


// ----------------------------------------------------------------
// Diagonal / aggregate on a matrix (list of rows).
// ----------------------------------------------------------------
say "matrix diagonals:"
Matrix: 1,2,3 4,5,6 7,8,9
say Matrix{?.~~}                   // diagonal         -> (1 5 9)
say Matrix{&~s+?.~~}               // sum of diagonal  -> 15


// ----------------------------------------------------------------
// Split list of pairs into two parallel lists.
// ----------------------------------------------------------------
say "unzip and re-index:"
LP: [1 a] [2 b] [3 c]
say LP{:~a._,~b._}                 // split             -> ((1 2 3) (a b c))
say LP{:~a._,~b.(i~+)}             // with 0..n index   -> ((1 2 3) (0 1 2))


// ----------------------------------------------------------------
// Pair each element with a synthetic value.
// ----------------------------------------------------------------
say "pair with auto-counter:"
say [\a \b \c \d \e]{?,5+}         // -> ((a 5) (b 6) (c 7) (d 8) (e 9))


// ----------------------------------------------------------------
// Matrix transpose (.zip equivalent via {}).
// ----------------------------------------------------------------
say "transpose via map:"
Xs: [1 a] [2 b] [3 c]
say Xs{A,B = ~i.A ~j.B}            // -> ((1 2 3) (a b c))


// ----------------------------------------------------------------
// Three-element row matchers from the trailing example block in
// sbe.md's "Advanced List-processing constructs":
//   L: [1 1 1] [2 3 4] [5 6]
// ----------------------------------------------------------------
say "three-elem map:"
L3: [1 1 1] [2 3 4] [5 6]
say L3{_ ~ _}                      // -> (1 3 (5 6))
say L3{:_ ~ _}                     // -> (1 3)
say L3{_ ~ _;~ _}                  // -> (1 3 5)
say L3{_ ~ ~}                      // -> ((1 1) (3 4) (5 6))


// ----------------------------------------------------------------
// B10 regression: `[@list]` inside string splice used to crash
// the compiler with `unknown symbol _insert`.  The fix lowers
// `"prefix [@list]"` to the same shape as `"prefix [list.text]"`
// (concatenate element texts) -- see expand_text_splice in
// macro_ops.s.
// ----------------------------------------------------------------
say "splat in string splice:"
Words [\a \b \c]
say "x [@Words]"                   // -> x abc
say "y [Words]"                    // -> y (a b c)
say "z [Words.text]"               // -> z abc
