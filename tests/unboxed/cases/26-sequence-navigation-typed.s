// TS-4.5 Phase 4: hardcoded universal shortcuts for sequence-
// navigation methods.  `.tail`/`.lead`/`.take`/`.drop` on a
// list-shaped receiver preserve shape; on text return text;
// `.l`/`.f` always return list; `.keep`/`.skip`/`.map`/`.rmap`
// always return list; `is_*` predicates return int.
//
// These cover the most common method-chain patterns without
// requiring 80+ per-method annotations in core_.s.  Each line
// below should route an arithmetic op through the unboxed path
// because the chain's final type proves int.

// `.tail.n + 1` -- list -> list -> int -> _iadd
chain_tail Xs^list = Xs.tail.n + 1
say (chain_tail [10 20 30 40 50])     // 5

// `.take(K).n + 1` -- list -> list -> int -> _iadd
chain_take Xs^list = Xs.take(3).n + 1
say (chain_take [10 20 30 40 50])     // 4

// `.drop(K).n + 1` -- list -> list -> int -> _iadd
chain_drop Xs^list = Xs.drop(2).n + 1
say (chain_drop [10 20 30 40 50])     // 4

// `.f.n + 1` -- list-shaped -> list -> int -> _iadd
chain_force Xs^list = Xs.f.n + 1
say (chain_force [10 20 30 40 50])    // 6

// text navigation: text.take returns text, then `.n + 1`
// requires another KISS shortcut (n on text -> int).
chain_text S^text = S.take(3).n + 1
say (chain_text "hello world")        // 4

// `.l.n + 1` -- text -> list -> int -> _iadd
chain_text_l S^text = S.l.n + 1
say (chain_text_l "abcde")            // 6

// `.keep` returns list -- chained arithmetic.
chain_keep Xs^list = Xs.keep(? > 2).n + 1
say (chain_keep [1 2 3 4 5])          // 4 (kept 3,4,5)

// `.map` returns list.
chain_map Xs^list = Xs.map(? * 2).n + 1
say (chain_map [10 20 30])            // 4

// `is_*` predicates -- return int 0/1, can feed arith.
add_is_int X^int = X.is_int + 5
say (add_is_int 42)                   // 6
