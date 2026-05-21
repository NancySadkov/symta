# LGET-as-store audit (bug #13 / #14 class)

**Status: 4 UNSAFE sites found, all FIXED in this commit.**  Audit results
documented below for reference; the proposed fixes have been applied and
verified against `tests/runtime/cross-gen-store.sh`, `tiny-gen0.sh`, the
full runtime suite (34/34), drift bootstrap (PASS 5/5), and a clean
`bash game/build.sh` under `--jit`.

## Summary

Audited every `LGET(x, i) = v` assignment (and `&LGET(...)`-derived lvalue
stores) in `reader.c`, `bltin.c`, `am.h`, `jit.c`, `jit_sbc.c`, `sbc.c`,
`common.h`, `gc.c`, `ih.h`, `dh.h`, `th.h`, `symta.h` — 192 sites total
(`jit_sbc.c`, `gc.c`, `common.h`, `ih.h`, `dh.h`, `th.h` had none of the
direct-assignment form; the bug #14 fix in `jit_sbc.c` already routes
through `STARG` + reload).

Verdict tally:

| Verdict           | Count | Notes                                                       |
| ----------------- | ----: | ----------------------------------------------------------- |
| UNSAFE            |     4 | All in `reader.c`; all the same `tok`-cache / `o.value`-mutation pattern as bug #13.  All FIXED. |
| SAFE-by-inspection|   186 | Fresh-LIST-then-fill, or under `GC_DISABLE` with all allocations in same nursery epoch, or RHS is older/immediate |
| NEEDS-HUMAN-REVIEW|     2 | `parse_suf_unary` shadowing of `LGET(o, 0)` with KW-immediates: technically safe but ergonomically the same shape as the unsafe sites and could regress if a KW_ value is ever changed to a younger heap object |

The four UNSAFE sites were NEW finds — same bug class as the original bug #13
(`reader.c:893`, fixed in commit `449414a`), but in sibling parser branches
that the original fix never touched.

> **Historical note on the original audit pass.**  The first pass listed
> five UNSAFE sites including `reader.c:893` itself.  That was a false
> positive caused by the audit agent running concurrently with the test
> agent — which had temporarily reverted line 893's fix to validate the
> new `cross-gen-store.sh` regression test.  `git diff` at HEAD confirms
> line 893 is and always was `LSET(tok, 6, pp);` since `449414a`.

## Methodology

For each site I checked: (a) where `base` was last allocated (LIST / CLOSURE / OBJECT / CONS / fresh from a returning constructor like `token()` / `mk_token()`); (b) whether any allocation, function call, or `GC_ENABLE` sat between that allocation and the store; (c) whether `value` is an immediate / static-interned `KW_*` (so a missing barrier is harmless) or a fresh young heap object; (d) whether the containing function wraps the work in `GC_DISABLE() ... GC_ENABLE()`, which makes the in-call window race-free.

Rule applied: `LGET(base, i) = value` is SAFE iff at least one of
1. `base` was just allocated (in the current nursery, current call's allocation window), OR
2. `value` is an immediate / a known-older heap object (no younger->older write), OR
3. `GC_DISABLE` is in effect for the entire window from the write through any subsequent store/use AND `value` would not need to survive a younger-gen collection after `GC_ENABLE`.

Bug #13 / #14 reproductions both involved case 1 *appearing* to hold within one function but not in fact: the **caller** had aged `base` (a token from `p_peek`/`p_pop`) into gen 4 across many prior parse calls, while `value` (a fresh `LIST` / `TEXT`) is in the current nursery. The write therefore creates an old->young pointer with no dirty-page mark; the next gen-0 collection moves `value` while `base.[i]` keeps pointing at `value`'s old (now-recycled) slot.

`GC_DISABLE`/`GC_ENABLE` only stops `gc()` from firing inside the window — it does NOT promote `value` to `base`'s generation, and does NOT add the dirty mark. So the bug fires AFTER the call returns and `GC_ENABLE` re-enables collection. Every UNSAFE site below is inside the parser's `parse_term`/`parse_suf_unary`/`binary_loop` paths, which receive long-lived tokens (originally allocated during `tokenize`, surviving across many parser sub-calls and therefore across many GCs).

## UNSAFE sites (FIXED — diff is in this commit)

### `symta/runtime/reader.c:1025` — `parse_suf_unary` (NEW)

```c
dyn ot_pair; LIST(ot_pair, 1); LGET(ot_pair, 0) = tok_type(o);
LGET(o, 6) = ot_pair;                    // <-- UNSAFE
```

Same shape as bug #13: `o` is a token parameter that has flowed through `parse_suf_op` from `parse_suf_loop`'s `p_peek`/`p_pop` — long-lived, can be aged. `ot_pair` is a fresh `LIST(1)`. The store caches `[tok_type(o)]` in slot 6 (the `parsed` slot — same slot bug #13 corrupts).

**Fix:**
```c
gc_anchor_push(&o);
dyn ot_pair; LIST(ot_pair, 1); LGET(ot_pair, 0) = tok_type(o);
LSET(o, 6, ot_pair);
gc_anchor_pop_n(1);
```

The `gc_anchor_push(&o)` is needed because the `LIST(ot_pair, 1)` allocation can move `o`'s heap location across a GC if the caller hasn't kept it rooted; mirrors the bug-#12-then-#13 layered fix.

### `symta/runtime/reader.c:992` — `parse_suf_unary` (NEW)

```c
dyn nv; TEXT(nv, buf);
LGET(o, 1) = nv;                         // <-- UNSAFE
```

`o` is the suffix-operator token, originally popped from the input stream — same aging story. `nv` is a fresh `TEXT` (could be a fixtext immediate if short, in which case the store is safe — but the `text_to_cstring(v) + "_"` builder produces strings >= 2 chars and the appended `_` plus arbitrary identifier length means `nv` is generally a bigtext heap object).

**Fix:**
```c
gc_anchor_push(&o);
dyn nv; TEXT(nv, buf);
LSET(o, 1, nv);
gc_anchor_pop_n(1);
```

### `symta/runtime/reader.c:1260` — `binary_loop` (NEW)

```c
dyn nv; TEXT(nv, buf);
LGET(o, 1) = nv;                         // <-- UNSAFE
free(buf);
LIST(ne, 2); LGET(ne, 0) = o; LGET(ne, 1) = e;
```

Exact mirror of 992: `o` is the binary operator token from `parse_op`, `nv` is a fresh appended-underscore `TEXT`. This path is the common case (every binary operator with no right operand gets its `.value` rewritten to append `_`); it runs deep inside the `for (;;)` loop of `binary_loop` so the GC pressure is high.

**Fix:**
```c
gc_anchor_push(&o);
dyn nv; TEXT(nv, buf);
LSET(o, 1, nv);
gc_anchor_pop_n(1);
```

### `symta/runtime/reader.c:1134` — `parse_suf_loop` (`.`/`^` method-name idiom) (NEW)

```c
dyn nv;
TEXT(nv, buf);
LGET(t, 1) = nv;                         // <-- UNSAFE
```

`t` is from `p_peek(p)` near line 1063 (and `(void)p_pop(p)` at 1112), so it's a popped token — same aging risk. `nv` is a fresh `TEXT` containing e.g. `"=src"` / `"=value"` (always at least 2 chars including the `=`, so generally a bigtext).

**Fix:**
```c
gc_anchor_push(&t);
dyn nv;
TEXT(nv, buf);
LSET(t, 1, nv);
gc_anchor_pop_n(1);
```

---

All five UNSAFE sites share the same shape:

```c
/* tok / o / t is a long-lived parser token (possibly gen 4) */
... TEXT(nv, ...) or LIST(pp, ...) ...        /* fresh nursery value */
LGET(tok_like, idx) = nv_or_pp;               /* OLD <- YOUNG, no barrier */
```

and the same fix recipe:

```c
gc_anchor_push(&tok_like);
... TEXT / LIST that may GC ...
LSET(tok_like, idx, fresh_value);
gc_anchor_pop_n(1);
```

## NEEDS-HUMAN-REVIEW sites

### `symta/runtime/reader.c:945, 951` — `parse_suf_unary` mutates `LGET(o, 0)` with `KW_bracketsL` / `KW_curlyL`

```c
LGET(o, 0) = KW_bracketsL;   /* line 945 */
...
LGET(o, 0) = KW_curlyL;      /* line 951 */
```

`o` is a long-lived token (same as 992/1025). The RHS is a `KW_*` static — for the two specific keywords `"["` and `"{"`, both are single-character strings and `fixtext_encode` returns an immediate (the 1-byte string is encoded inline in the tagged dyn). So no GC ref is being installed and a missing barrier is harmless — **today**.

Why review: if `KW_bracketsL` / `KW_curlyL` were ever extended to a multi-char form, or if `fixtext_encode`'s threshold changed, these would silently flip to bigtext heap objects (initialized in `kw_init`, age-0 on the very first parse call before any GC) and the same bug class would be re-introduced. The other `LGET(t, 0) = KW_symbol;` / `LGET(t, 0) = KW_colon;` at lines 1075, 1076, 1113 have the same fragility, but for already-`KW_*` short strings the immediates argument applies cleanly. Suggest a comment annotating the immediate-only invariant, or change to `LSET` defensively — `lsetm`'s `GC_OLDER(base, value)` check is a couple of branches, not worth the risk.

## SAFE-by-inspection sites (grouped by pattern)

- **Fresh-LIST + fill (no intervening alloc, no function call between LIST and the store):** `reader.c` 347, 348, 379, 477-479, 484, 531, 580, 581, 660-672, 718-727, 766-767, 816-824, 842, 863-864, 915, 954, 999, 1003, 1005, 1020, 1028-1030, 1033, 1089, 1098, 1100-1102, 1140, 1179, 1186, 1224, 1230, 1233, 1249, 1262, 1318, 1381, 1395-1396, 1537-1538, 1550-1551, 1573, 1607-1612, 1624, 1672-1675, 1740, 1796, 1806, 1812, 1814-1816, 1823-1825, 1841, 1929, 1942, 1988, 2022-2025, 2047-2048, 2115, 2121, 2124-2125; `reader.c` 435-436 (`make_meta_wrapper`'s fresh OBJECT); `sbc.c` 1057-1058, 1067; `jit_sbc.c` 654, 658-659; `bltin.c` 988, 1590-1598, 1613, 1678-1679, 1760-1762, 2099-2100, 2217-2219, 2347, 2356, 2361-2364, 2532, 3339-3341.

- **`STARG(i, v)` after `ARGLIST(n)`:** every `STARG` site I traced was either (a) a `BUILTIN_VARARGS` body that received its `api.args` from the caller, or (b) immediately preceded by an explicit `ARGLIST(n)` with no intervening allocation. The bug-#14-fixed JIT helpers (`jit_rt_immeq_impl`, `jit_rt_immne_impl`, `jit_rt_fxnlset_impl`, `jit_rt_fxnlsetir_impl`) all correctly reload from frame slots AFTER `ARGLIST(n)` — confirmed against the `STARG(0, ((dyn*)L)[a_sl]);` re-read pattern at `jit_sbc.c:774-775, 807-808, 469-471, 624-626`.

- **`LGET(R, i) = ...` inside `GC_DISABLE`/`GC_ENABLE` where `R` was LIST'd in the same block and no `GC_ENABLE` sits between the LIST and the store:** `bltin.c:1010` (`qsort` with all writes via `lsetm`), 2098-2100, 2216-2219, 2360-2364, 2342-2348, 2351-2357, 2527-2532, 3335-3341; `am.h:199-202, 210-213, 218-222, 721-728, 734-741, 746-753, 759-766, 772-779, 796-799, 805-808, 813-816, 822-825, 831-834`. The `am.h` builders also have the property that even without `GC_DISABLE`, `r` and `kv` are both freshly allocated in the same C statement window, so they're same-gen with respect to each other.

- **`&LGET(base, k)` lvalue pointer for bulk fill:** `bltin.c:778-781, 913-916, 937-940, 959-962, 1100-1102` — all write into a freshly-LIST'd `R` under `GC_DISABLE`. `bltin.c:888` is an `O_PTR(o)[i] = value` loop explicitly gated on `IMMEDIATE(value) || value == Empty || value == No` (no heap ref crossing gens). `gc_types.h` `&LGET` sites are GC-internal scanning helpers, not stores from user code.

- **RHS is an immediate or a static-interned `KW_*` text:** `reader.c:1075, 1076, 1113` (writing `KW_colon`, `t_colon`, `KW_symbol` into a popped token). Same caveat as the NEEDS-HUMAN-REVIEW entries, but for these specific strings the values are either fixtext immediates or bigtexts that promote to the oldest generation within the first few GC cycles — the runtime always sees them as "older than anything you could be writing them into."

- **Init-time-only sites under outer `GC_DISABLE`:** `bltin.c:3339-3341` (one-time builtins table population, called from `init_builtins`'s `GC_DISABLE` block during process startup before any user code runs); `am.h` GC-disabled accessor bodies.

- **JIT helpers that LIST+fill in a single body with no intervening helper that allocates:** `jit_sbc.c:651-660` (`jit_rt_list1_impl`, `jit_rt_list2_impl`). `((dyn*)L)[dst]` after `LIST` is by definition the freshly-allocated object; the source slots `((dyn*)L)[x]`, `((dyn*)L)[a]`, `((dyn*)L)[b]` are read directly into the new object with no GC trigger in between.
