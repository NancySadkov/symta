// Phase 3 conditional narrowing: `if X.is_T: body` narrows X to T
// inside the Then branch.  Without narrowing, `static (X + 1)`
// fails because X is dyn; with narrowing it routes to `_iadd` and
// returns the int result.

// 1. `if X.is_int: ...` -- 2-arg if with implicit Else=No.
demo_if X = if X.is_int: static (X + 1)
say (demo_if 41)         // 42 (narrowed, _iadd routes inline)
say (demo_if "hi")       // No (Else branch fires)

// 2. `when X.is_int: ...` -- expands to _if with wrapped Cond.
demo_when X = when X.is_int: static (X * 2)
say (demo_when 21)       // 42
say (demo_when "hi")     // No (when's implicit Else)

// 3. Explicit if/then/else with narrowing.
demo_full X = if X.is_int then static (X - 1) else 0
say (demo_full 43)       // 42
say (demo_full 0.5)      // 0 (Else)

// 4. Float narrowing too -- same shape, different known type.
demo_float X = if X.is_float: static (X * 2.0)
say (demo_float 1.5)     // 3.0
say (demo_float 7)       // No (Else)
