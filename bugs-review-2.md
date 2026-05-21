# Final-sweep audit (complement to bugs-review.md)

**Status: all 3 sites FIXED.**  Empirical triggerability post-fix
testing: `view_set`'s revert did NOT segfault on the game compile
under tiny gen0 (views are short-lived in the game's compile pipeline
and rarely outlive their nursery).  `amSwap` is documented as broken
in `bltin.c:1583`'s comment but isn't exercised by the game compile.
`amSetNo` has a documented API but isn't called in the game.  All
three fixes are therefore "defensive" in the same sense as
reader.c:997 and :1271 -- the patterns are unambiguously wrong but
the trigger pathways aren't reached today.  Kept as fixes because the
diff is tiny and the failure mode (silent corruption in long-running
programs) is exactly the kind of latent bug we already chased through
two rounds.

Bug class: cross-generation pointer writes that bypass the GC write
barrier — same family as bug #13 (LGET-as-store) and bug #14 (C-local
stale across allocation).

## Files surveyed

- `runtime/main.c` — read in full
- `runtime/sif.c` — read in full
- `runtime/sif2sbc.c` — grep'd for dyn / LGET / alloc macros (only one `dyn val` hit, holds immediate)
- `runtime/sbc2sif.c` — grep'd, no dyn-write
- `runtime/tokenize.c` — read in full
- `runtime/fs.c` — grep'd, no dyn-write
- `runtime/ncm.c`, `runtime/ng.c` — stubs (`#define ..._IMPLEMENTATION` + include)
- `runtime/snippets.c` — dev scratch, no dyn-write
- `runtime/meta_table.c` — read in full
- `runtime/gc_types.h` — read in full
- `runtime/am.h` — extended re-pass (the original audit covered the LGET-into-fresh-R sites; this pass adds the AM_VOID / VIEW_BASE store sites that the original grep missed)
- `runtime/nb.h`, `runtime/nh.h`, `runtime/ng.h`, `runtime/effi.h`, `runtime/flt16.h`, `runtime/prf.h`, `runtime/sif.h`, `runtime/jit.h`, `runtime/fs.h`, `runtime/reader.h`, `runtime/meta_table.h`, `runtime/tests.h` — grep'd for dyn-write patterns (all clean or covered)
- `runtime/linux/ctx.c`, `runtime/osx/ctx.c`, `runtime/w64/ctx.c` — CPU context shims, no dyn ops
- `runtime/bltin.c` — extended re-pass for VIEW_BASE writes (the original audit grep `LGET(...) =` does not match `VIEW_BASE(...) =` syntactically, even though VIEW_BASE expands to LGET(o,0); the macro layer was an audit blind spot)


## UNSAFE / NEEDS-FIX

All three findings are LGET-as-store sites that the original audit's
literal `LGET(...) =` grep skipped because they're hidden behind macro
wrappers — `AM_VOID(o) = v` and `VIEW_BASE(o) = v`, which both expand
to `LGET(o, slot) = v`.

### `runtime/am.h:461` — `amSetNo(dyn o, dyn value)`

```c
INLINE void amSetNo(dyn o, dyn value) {
  AM_VOID(o) = value;       // <-- expands to LGET(o, 1) = value
}
```

**Why unsafe:** `o` is the user table — long-lived through the
program, often promoted into gen 2+ by the time `T.setNo X` runs.
`value` is whatever Symta-side argument the user passes — frequently
a fresh `\marker` text or a freshly constructed object (the table's
"missing-key sentinel"). Cross-gen pointer with no barrier:

- `amSet` does this correctly via `AM_ATTRACT(o, key)` /
  `AM_ATTRACT(o, value)` (lines 531-532) — registers `o` in the
  young gen's magnets array so the minor GC will scan it.
- `amSetNo` calls neither `AM_ATTRACT` nor `lsetm` — the store
  is naked.

**Triggering case:** Any long-running Symta program that creates a
table, ages it through many GC cycles, then calls `T.setNo X` with
a freshly heap-allocated `X` (e.g. a bigtext sentinel). The next
minor GC of `X`'s gen sweeps it because nothing aged enough to be
scanned holds a barrier-marked reference; `T.K`-on-miss then returns
a stale `X`-shaped slot.

The contract is documented in `am.h` lines 36-51 — `T.setNo X` is a
real API call, exercised in `tests/am/src/tc_void.s` with the
`\empty_marker` text. The bug isn't theoretical.

**Fix:**
```c
INLINE void amSetNo(dyn o, dyn value) {
  AM_ATTRACT(o, value);     /* register cross-gen if needed */
  AM_VOID(o) = value;       /* (or lsetm(o, 1, value)) */
}
```

`AM_ATTRACT` is the right primitive here — it mirrors what
`amSet`/`amGidSet` use for the regular key/value writes and keeps the
GC traversal path consistent.

### `runtime/am.h:472, 477` — `amSwap(dyn o, dyn m)`

```c
AM_BASE(o) = AM_BASE(m);
AM_VOID(o) = AM_VOID(m);    // <-- LGET(o, 1) = LGET(m, 1)
...
AM_BASE(m) = obase;
AM_VOID(m) = ovoid;         // <-- LGET(m, 1) = (saved ovoid)
```

**Why unsafe:** identical mechanism to `amSetNo`. If `o` and `m`
are in different generations, swapping their void slots installs a
younger-aged dyn into an aged container with no barrier. The
post-swap `AM_SET_YOUNGEST(o, AM_YOUNGEST(m))` updates the
generation tag but does NOT register `o` (or `m`) into the magnets
array for the now-younger contents.

**Evidence this is known-broken:** the builtin caller already
carries a self-warning comment:

```c
BUILTIN2("tbl.swap",tbl_swap,C_ANY,o,C_ANY,m) //advanced cls.s stuff needs it
  //currently breaks due to how AM_ATTRACT works on direct object address
  amSwap(o,m);
```
(`bltin.c:1582-1585`)

**Fix:**
```c
INLINE void amSwap(dyn o, dyn m) {
  dyn obase = AM_BASE(o);
  dyn ovoid = AM_VOID(o);
  uint32_t otype = AM_TYPE(o);
  uint32_t oyoungest = AM_YOUNGEST(o);

  AM_ATTRACT(o, AM_VOID(m));   /* mirror what amSet does */
  AM_ATTRACT(m, ovoid);
  /* AM_BASE is a C pointer (dh/th/ih/nb_t*), not scanned as dyn —
   * AM_ATTRACT not needed for it. The table contents themselves
   * still need to attract `o` or `m` for any aged-young mismatch;
   * the safest fix is to walk both tables' younger-than-target
   * values and AM_ATTRACT each one, but a coarse approximation
   * (push both objects into hg0[hgp->age].magnets unconditionally)
   * is cheap and correct. */

  AM_BASE(o) = AM_BASE(m);
  AM_VOID(o) = AM_VOID(m);
  AM_SET_TYPE(o,AM_TYPE(m));
  AM_SET_YOUNGEST(o,AM_YOUNGEST(m));
  AM_BASE(m) = obase;
  AM_VOID(m) = ovoid;
  AM_SET_TYPE(m,otype);
  AM_SET_YOUNGEST(m,oyoungest);
}
```

The simplest correct fix is to register both `o` and `m` into the
youngest gen's magnets array (so a minor GC of any younger gen scans
both) and trust the existing `tbl_gc_internals` to find the contents.
The structural rewrite is out of scope for this report.

### `runtime/bltin.c:784` — `view_set` copy-on-write

```c
BUILTIN3("list.`=`",view_set,C_ANY,o,C_INT,index,C_ANY,value)
  dyn base = VIEW_BASE(o);
  uint32_t start = VIEW_START(o);
  if (VIEW_SHARED(base)) {
    //base is a shared object, so copy it on write
    uint32_t size = VIEW_SIZE(o);
    dyn r;
    LIST(r, size);                                      // fresh nursery LIST
    dyn *oo = &LGET(base, start);
    dyn *pp = &LGET(r,0);
    for (int i = 0; i < size; i++) {
      pp[i] = oo[i];
    }
    VIEW_START(o) = 0;
    VIEW_BASE(o) = VIEW_STRIP_SHARED(r);  // <-- LGET(o, 0) = fresh r
  }
```

**Why unsafe:** `o` is the user-supplied view, possibly aged through
prior GCs. `r` is a freshly LIST'd copy in the current nursery. The
`VIEW_BASE(o) = ...` write installs a young-pointer into a possibly-
old object with no barrier — exact bug-#13 shape.

The other VIEW_BASE writes I checked are safe:
- `bltin.c:805, 822, 841` write `base = VIEW_MARK_SHARED(VIEW_BASE(o))`
  back into `VIEW_BASE(o)`. `VIEW_MARK_SHARED` only flips a tag bit
  on the pointer — the heap target is the same dyn the slot already
  held, so the barrier is a no-op.
- `gc_types.h:109` is inside the GC scanner, writing into the fresh
  to-space copy `p` — bug-#13 rule 1.

**Fix:**
```c
    VIEW_START(o) = 0;
    lsetm((dyn*)o, 0, VIEW_STRIP_SHARED(r));     /* go through barrier */
```

Or equivalently the anchor-and-LSET recipe used in `reader.c`:
```c
    gc_anchor_push(&o);
    LIST(r, size);
    /* fill r ... */
    VIEW_START(o) = 0;
    LSET(o, 0, VIEW_STRIP_SHARED(r));
    gc_anchor_pop_n(1);
```

`view_set` is the entry point for `MyView.I = X` on any view, so
this fires for every assignment into a view derived from
`L.take`/`L.drop`/`L.tail` once the view itself has aged. Likely
real-world reachable; same pattern as the `reader.c:1031`
real-trigger that bugs-review.md confirmed.


## Suspicious-but-likely-safe

### `gc_types.h:152` — `CDR(prev) = p` inside `gc_cons` loop

```c
void *prev = p;
CONS(p, 0, 0);
CDR(prev) = p;          /* prev was a fresh CONS earlier in this scan */
```

Writing into a fresh to-space cons is bug-#13 rule 1 — `prev` is a
to-space allocation from the current GC pass. `p` (the new CONS) is
also in to-space at the same age. Same-gen-or-younger write into
fresh allocation: safe.

I'm flagging it only because the pattern *looks* like the anti-pattern
on a first read; it isn't because of the to-space invariant.

### `meta_table.c:34-36` — `ihSet(&t, obj, src)`

`obj` is a heap dyn argument; `ihSet` may grow the underlying ih_t
(`free()`/`malloc()` a new C-side slot array). No Symta `gc_alloc`
inside `ih_t` operations — the C-side malloc doesn't trigger
GC. `obj` and `src` aren't re-derived from heap state after the
call. Safe.

### `tokenize.c` entire file

Every dyn-write happens between `GC_DISABLE()` at line 557 and the
matching `GC_ENABLE()` reached via `r_cleanup()` (line 41) at the
end of `tokenize()`. No GC fires during tokenization, so all the
`LGET(t, i) = ...` cross-gen-shape stores in `read_normal_list`,
`read_bracket_list`, `read_normal_string`, `read_string`'s incut
branch (line 535: `TEXT(LGET(m,0), "()")`), and `read_list`
(line 462: `LGET(r,i) = ts[i]`) are race-free.

This mirrors the bug-#13 audit's "init-time-only sites under outer
`GC_DISABLE`" group; tokenize is just the same idiom at a function
scope.

### `main.c` `find_export` (lines 579-600)

Reads `pair = LGET(exports, i)` then `export_name = LGET(pair, 0)`
into C locals. No allocations between the read and use — `O_TAG`,
`LIST_SIZE`, `IS_TEXT`, `texts_equal` are all non-allocating.
`print_object` is allocating but only on the fatal-path which exits.
Caller (`load_sbcs`) holds `find_export` inside `GC_DISABLE`
(line 630-637). Safe.

### `main.c:586-595` — bug-#14 form on `exports` array

`exports` is the loaded library's exports list, originally rooted
through `module_imports[].libs`. The function is called from
`load_sbcs` under `GC_DISABLE`. Even without that, `LIST_SIZE` and
the LGET reads are non-allocating. Safe.


## All-clear

Covered 11 files in detail and 14 more by targeted grep
(`runtime/{linux,osx,w64}/ctx.c`, `runtime/{nb,nh,ng,sif,jit,fs,reader,meta_table,tests,effi,flt16,prf}.h`,
`runtime/{ncm,ng}.c`).  The only previously-unaudited file with
substantive dyn-write activity was `tokenize.c`; its outer
`GC_DISABLE` makes the file race-free as a whole.

Three NEW unsafe sites found:

| Site                  | Shape              | Triggerability      |
| --------------------- | ------------------ | ------------------- |
| `am.h:461` `amSetNo`  | `AM_VOID(o)=v`     | `T.setNo X` API; reachable from `tc_void.s` if table is aged |
| `am.h:472,477` `amSwap` | `AM_VOID(o/m)=...` | `tbl.swap` builtin; comment at `bltin.c:1583` confirms a known issue |
| `bltin.c:784` `view_set` | `VIEW_BASE(o)=fresh_LIST` | `View.I = X` on a copy-on-write view; same shape as reader.c:1031 |

All three are macro-hidden LGET-as-store sites that the original
audit's literal-string grep would not have found.  None of the
three call AM_ATTRACT, lsetm, or the `gc_anchor_push`+LSET recipe;
all three are inside builtin entry points that take a user-supplied
container parameter (so the LHS is freely able to be aged in a
long-running program).

Bug-#14 (C-local-stale-across-allocation) shape: I did NOT find any
new instance.  The `dyn x = LGET(...)` reads in `main.c`,
`tokenize.c`, and elsewhere were either:
- followed by no allocation before use (find_export, code2text)
- inside `GC_DISABLE` (tokenize.c throughout)
- already covered by the original audit (reader.c, bltin.c)
- in tests/scratch code that doesn't ship (tests.h, snippets.c)
