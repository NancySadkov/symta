// TS-4.5 Phase 3: methods can declare typed params via `^T`.
// `expand_block_item_method` now runs `strip_typed_params` so
// `list.foo Ys^list = ...` strips the `^list` and wraps the
// body with both a runtime `_the list Ys` boundary check and
// a static `_type list Ys ...` scope.  Combined with Phase 1
// (Me auto-typed list) and Phase 2 (`.n -> int` shortcut),
// every `Me.n + Ys.n`-shaped body now routes to _iadd.

list.zip_sizes Ys^list = Me.n + Ys.n
say ([1 2 3].zip_sizes [10 20])        // 3 + 2 = 5
say ([].zip_sizes [10 20 30 40])       // 0 + 4 = 4

// Multiple typed params.
list.dot_size Ys^list Zs^list = Me.n * Ys.n + Zs.n
say ([1 2].dot_size [3 4 5] [6])        // 2*3 + 1 = 7

// Mixed typed + untyped params (untyped stays dyn).
list.scale Factor^int = Me.n * Factor
say ([1 2 3].scale 4)                   // 3 * 4 = 12

// Runtime boundary check fires when caller passes wrong type.
list.requires_list Ys^list = Ys.n
say ([].requires_list 42)               // -> "value not list"
