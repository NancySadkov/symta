# Debugging the Symta runtime

This document captures the debugging methodology that worked on
the GC corruption bugs (task #12 family, June 2026).  The same
approach generalises to any bug whose surface symptom is far
removed from its root cause -- typically because data flows from
the buggy write site, through several allocations and a GC pass
or two, and only then gets read back in a context where the
corruption becomes visible.

The shape of the work is **two phases**:

1. **Bisect to a minimal trigger.**  A bug that reproduces only
   under a 20-minute compile of the entire game is useless for
   investigation; you need a reproducer that runs in under a few
   seconds and either crashes or doesn't.

2. **Trace back from the surface to the source with write traps.**
   Once the bug is small and deterministic, instrument the heap
   so that the moment the bad value lands in a slot, the writing
   instruction's RIP gets logged.  From there it's a short walk
   to the C source line.

This method has handled every Symta-runtime bug we've seen so
far **except heisenbugs** -- failures that happen once every
several days, can't be reliably triggered, and may be caused by
faulty hardware or OS scheduling rather than the program itself.
See the *Heisenbugs* section at the end for how to recognise
them and why this methodology doesn't apply.

---

## Phase 1: Bisect to a minimal trigger

Most observed bugs reproduce under a narrow envelope.  The art
is finding that envelope so each subsequent run takes seconds,
not minutes.

### Input-bisect

The bug is triggered by *some* input.  Start with the largest
input that reproduces (usually `bash game/build.sh` or `symta
/path/to/project`) and shrink it.  Useful shrink operations:

- Replace the source file's contents with a one-liner like
  `say "hi"`.  Most compile-time bugs fire during `core_.s`
  macroexpansion before they ever see the user code, so this
  shrinks the input without changing the failure.
- Remove project dependencies (`use foo`) one at a time.
- For runtime bugs, find the single user-source-file that
  triggers and shrink that file with binary search.

### Heap-pressure bisect

Some bugs depend on GC running often.  `SYMTA_GEN0_SIZE=<bytes>`
forces a smaller nursery and a higher GC rate.  The original
task #12 bug only triggered below ~96 KB.  A useful sweep:

```sh
for sz in 65536 131072 262144 524288 1048576; do
  echo "=== gen0=$sz ==="
  SYMTA_GEN0_SIZE=$sz ./symta.exe "$tmp" 2>&1 | grep -E 'ERROR|has no method'
done
```

The smallest size that still reproduces is what you want.  Below
that the runtime clamps to a page; above that the bug stops
firing.  Pick a size right in the trigger band.

### JIT vs interpreter bisect

If the bug fires under `--jit` but not `--no-jit`, you have a
JIT-correctness regression.  This is task #14 territory.  The
shape of the fix is different (audit recent JIT translations
and helpers, not heap writes), but the bisect is the same:
shrink the input + flip the flag.

### Commit-bisect

Once you have a minimal input, `git bisect` works normally as
long as each candidate commit can be built.  The Windows build
needs `TMPDIR=$LOCALAPPDATA/Temp PATH=/c/soft/w64devkit/bin:...
make -f Makefile.w64`; bake that into the bisect script.  See
the comment in `feedback_symta_toolchain.md` -- using `mingw64`
instead of `w64devkit` produces binaries that won't start, which
will throw bisect off if you don't notice the build is broken.

### Don't trust the cache

`./game/sbc/` and `./game/cache/` hold previously compiled SBCs
and intermediate state.  If a bug only fires "sometimes" check
whether you're inadvertently reusing those.  `rm -rf cache sbc`
before each reproducer run is cheap insurance.

---

## Phase 2: Trace from surface to source

Once the bug is small and deterministic, you know roughly
*where* it fires (the stack trace from the rterr / segfault
gives you the Symta function and source line).  What you need
next is **where the bad value was written**.

### Step 2.1: Identify the bad value

Find one consistent fact about the value:

- Its raw bit pattern (e.g. `0x07`, `0x000001fe7fe60013`).
- The slot or register it ends up in (printed by the sink
  builtin's "has no method" message or by a manual probe).
- A type-mismatch property -- "tag should be T_LIST=9 but is 3",
  "size field reads back as 2.1 GB", etc.

For task #12 the consistent fact was: the slot holding the
poison always had `O_TAG = 3` (`_unused0_`), a reserved tag.
Whenever you can phrase the bug as "this dyn shouldn't ever
exist", the rest of the hunt is mechanical.

### Step 2.2: Localise the holding slot

The surface symptom tells you what the value *looks like* but
not which heap slot is holding it.  A scan over the live heap
finds the slot.  See `gc_types.h` GCDEF(gc_list)'s
`SYMTA_DBG_LISTSCAN` block for the template -- on every list
trace, check whether any slot matches your bad-value signature,
and print `(gid, size, slot_index)` on the first hit.

Once you have the gid and slot, the slot's heap-byte-offset is
`api.heap0 + gid*8 + slot*8`.  Note that gid is **deterministic
across runs** (alloc order is a function of the input program),
so you can use the gid from the first run as a fixed key for
the second run.

### Step 2.3: Set a write trap on the slot

This is where the runtime's purpose-built infrastructure earns
its keep.  We have two implementations:

- **`ctx_set_page_watchpoint(void *addr)`** (in
  `runtime/w64/ctx.c`) -- the workhorse.  `VirtualProtect`'s
  the 4 KB page containing the slot to `PAGE_READONLY`, installs
  a vectored exception handler, and traps every write to that
  page.  On the slot-match write it logs full GPR state + RIP
  and re-arms via single-step.  Slower than DR0 (every page
  write traps, not just the watched address) but works without
  elevated privileges or a debug session.
- **`ctx_set_write_watchpoint(void *addr, uint64_t value)`** --
  hardware DR0 via a suspending helper thread.  Faster but
  Windows silently drops the register write if the session
  doesn't have sufficient integrity (we've seen GetThreadContext
  read back DR0=0 immediately after SetThreadContext "succeeds").
  Enable via `SYMTA_WATCH_HW=1`.

Both are wired up from `runtime/main.c` behind env vars:

| env var               | meaning                                          |
|-----------------------|--------------------------------------------------|
| `SYMTA_WATCH_ADDR`    | raw heap address (hex, full 64-bit) to watch     |
| `SYMTA_WATCH_GID`     | alternative: gid in heap0; slot = `api.heap0[gid]` |
| `SYMTA_WATCH_SLOT`    | additional slot offset (default 0)               |
| `SYMTA_WATCH_VALUE`   | only log+abort writes that land this value       |
| `SYMTA_WATCH_HW`      | use DR0 instead of page protect                  |
| `SYMTA_DUMP_SEGV`     | dump RIP+GPRs on any access violation            |
| `SYMTA_DBG_LISTSCAN`  | gc_list emits a forensic dump for absurd sizes   |
| `SYMTA_SCAN_AST`      | recursively scan reader.c outputs for poison    |
| `SYMTA_FATAL_BAD_TAG` | abort immediately when a tag-3..5/7 dyn appears  |

Typical session:

```sh
# Run 1: find the holder gid via the LISTSCAN diagnostic.
SYMTA_GEN0_SIZE=65536 SYMTA_DBG_LISTSCAN=1 ./symta.exe "$tmp" 2>&1 \
  | grep LISTSCAN

# Run 2: trap writes to the slot that contained the poison.
SYMTA_GEN0_SIZE=65536 \
  SYMTA_WATCH_GID=33451165 SYMTA_WATCH_SLOT=1 SYMTA_WATCH_VALUE=7 \
  ./symta.exe "$tmp" 2>&1 | grep -E 'PAGE|WATCH'
```

The watchpoint handler prints the writing instruction's RIP and
every GPR.

### Step 2.4: Resolve RIP to a source line

`GetModuleHandleA(NULL)` is logged alongside the RIP so you can
compute the file offset: `RIP - ImageBase = file VA`.  Add the
preferred image base (`0x140000000` on Windows w64devkit builds)
and grep the disasm:

```sh
objdump -d ./symta.exe > /tmp/symta-disasm.txt
awk '/^[0-9a-f]+ <[^>]+>:/ {sym=$0; addr=strtonum("0x"$1)}
     addr <= TARGET {last_sym=sym; last_addr=addr}
     END {print last_sym; printf "offset = 0x%x\n", TARGET - last_addr}' \
    /tmp/symta-disasm.txt
```

This drops you on a specific instruction inside a named C
function with a byte offset.  For task #12 it landed at
`reader_parse_strip + 0x155`, which the disassembly identified
as the store of the recursive-call return into `LGET(out, i)`.

### Step 2.5: Fix the source

Once you know the C site the fix is usually one of two patterns:

- **A defensive guard at the read site.**  When the bad value
  is structurally invalid -- a reserved tag, a list size in the
  gigabyte range, a parsed-cache list with size != 1 -- check
  for it before consuming.  Task #12's first fix was a one-line
  guard in `reader_parse_strip` that falls back to `tok_value(x)`
  whenever the parsed cache isn't a textbook 1-element list.
- **Anchoring stale C locals across an allocating call.**  When
  the writer is a C function that holds a heap dyn in a local
  across `LIST` / `OBJECT` / `mk_token`, anchor the local with
  `gc_anchor_push(&x); ...; gc_anchor_pop()`.  The GC then
  forwards the local on every collection so the next dereference
  reads the moved object instead of the freed-then-reused slot.

The anchor API lives in `runtime/common.h` (declarations) and
`runtime/gc.c` (the `gc_anchors` stb_ds stack, walked by
`gc_builtins`).

---

## Heisenbugs (when this methodology breaks down)

The phase-1 bisect assumes the bug **reproduces on demand**.  A
heisenbug -- something that fires once every few days, never on
the same input twice, sometimes only on one specific machine --
defeats every step:

- You can't shrink the input because each shrink might happen to
  hide the bug for the wrong reason.
- You can't bisect commits because a "good" commit might just be
  one where you got lucky.
- You can't set a watchpoint because you don't know which run
  will be the bad one.

Symptoms that suggest a heisenbug rather than a determinism bug:

- The fault address varies between runs by more than ASLR slide.
- The same binary on the same input fails sometimes and succeeds
  sometimes.
- The fault disappears under `--no-jit`, then comes back, then
  disappears again on rebuild, with no source changes.
- The reproducer involves wall-clock time, sockets, or other
  external state.
- The fault is only seen on one machine in the wild and never on
  CI or developer hardware.

When you see those, the next step is **not** to apply more
instrumentation but to **exclude environmental causes**:

- Run a memory test (MemTest86 or equivalent) overnight.  Bad
  RAM produces bit flips that look exactly like GC corruption.
- Check the kernel log for ECC corrections, disk errors,
  thermal throttling.
- Check `wmic memorychip` (Windows) / `dmidecode` (Linux) -- a
  recent module swap can introduce bad sticks.
- If the bug is in a JIT-compiled function, check that nothing
  is mapping the JIT arena with `PAGE_EXECUTE_READWRITE` and
  letting an unrelated process scribble on it (some antivirus
  inject DLLs do this).
- If the bug only shows in the wild, suspect the user's machine
  before suspecting the code.

Only after those are ruled out is it worth setting up
long-running statistical instrumentation (a continuously-armed
watchpoint that logs to disk, a corrupted-heap detector that
runs once per GC and crashes with full context on the first
mismatch).  Even then, plan for the investigation to take days
of clock time rather than minutes.

---

## Quick reference: tools we've added

All env-gated, inert when unset:

- `SYMTA_DUMP_SEGV` -- access-violation handler logs full GPRs.
- `SYMTA_WATCH_GID` / `SYMTA_WATCH_SLOT` / `SYMTA_WATCH_VALUE` /
  `SYMTA_WATCH_HW` -- arms the page or DR0 watchpoint at startup.
- `SYMTA_DBG_LISTSCAN` -- gc_list dumps source list + neighbours
  on absurd size.
- `SYMTA_SCAN_AST` -- reader.c emits `[ASTSCAN]` lines at parser
  function boundaries when poison appears in the AST.
- `SYMTA_DBG_LIST2` -- SBC_LIST2 traps on building a list with
  poison in slot b.
- `SYMTA_FATAL_BAD_TAG` -- the gc_custom defensive guard and the
  SBC_LIST2 watchpoint abort immediately on first detection so
  the stack trace shows the exact context.

All can be combined.  None has measurable cost when not set.

## Quick reference: APIs we've added

- `gc_anchor_push(dyn *p)` / `gc_anchor_pop(void)` /
  `gc_anchor_pop_n(int)` -- register stack-local dyn slots as GC
  roots.  See `runtime/common.h`.
- `ctx_set_page_watchpoint(void *addr)` /
  `ctx_set_write_watchpoint(void *addr, uint64_t value)` /
  `ctx_set_watch_target_value(uint64_t v)` -- watchpoint
  primitives.  See `runtime/w64/ctx.h`.

## Worked examples

- `tests/runtime/tiny-gen0.sh` -- pinned regression for the
  original tag-3 / 0x07 poison bug.  The fix in commit `3c7ec19`
  exercises the full methodology: minimal reproducer (`say "hi"`
  under tiny gen0), heap scan via LISTSCAN, page watchpoint via
  WATCH_GID/SLOT/VALUE, RIP-to-source via objdump, defensive
  guard in `reader_parse_strip` plus root-cause anchor pattern
  in `parse_term`.

---

## Currently open runtime bugs

This list is kept in sync with the in-flight tracker tasks and
should be the first stop when a regression surfaces.  Each entry
records the minimal trigger, the surface symptom, and what we
already know about the source.

### Bug #14 (FIXED) -- JIT helpers leaked stale C-local dyns past ARGLIST

- **Trigger.**  `rm -rf game/sbc; bash game/build.sh` (with
  `--jit`).  Crashed during macroexpansion with a "has no
  method" error on a `No` (tag-14) or `int 0` object.  Surface
  varied between runs (`qlmb_supply_cvars:204` calling `.n` on
  `No`, `ssa_mark` receiving `Name = No`, etc.) because the
  bug was allocation-pattern-sensitive.
- **Root cause.**  The JIT trampolines `jit_rt_immeq_impl`,
  `jit_rt_immne_impl`, `jit_rt_fxnlset_impl`, and
  `jit_rt_fxnlsetir_impl` cached frame-slot reads in C locals
  *before* calling `ARGLIST2` / `ARGLIST3`, then passed the
  stale C locals into `STARG` after the allocation:

    ```c
    dyn aa = ((dyn*)L)[a_sl];       // <-- snapshot
    dyn bb = ((dyn*)L)[b_sl];
    ...
    ARGLIST2(aa, bb);               // GC may run here
    // STARG(0, aa); STARG(1, bb);  // stale pointers leaked
    ```

  `ARGLIST*` calls `LIST(api.args, n)` which can trigger GC.
  Frame slots are GC roots (scanned via `api.frame`) so the
  slot value is updated to the new heap address, but the C
  local `aa` still holds the *old* pointer.  When `STARG`
  wrote `aa` into the newly-allocated args list, the args
  list ended up with a dangling pointer to recycled memory --
  which read back as `No` (or whatever else was now at that
  address).  The first method dispatch through that args
  list then saw the wrong receiver and crashed.
- **Why bisect was so painful.**  The trigger depends on the
  binary's link layout because that shifts heap addresses and
  GC timing.  The disabled-opcode bisect under
  `SYMTA_NO_FXNLSET` / `SYMTA_NO_OBJECT` / etc. didn't catch
  it because those flags gated the *JIT translator's*
  opcode-emission path, not the *trampoline helper bodies*
  the AOT-installed code calls into.  The buggy helpers are
  reached through pre-existing translations (IMMEQ / IMMNE
  from commit `237c89f`; FXNLSET / FXNLSETIR from `0b2d433`).
- **Tool that found it.**  Same methodology as bugs #12/#13:
  pin a deterministic reproducer (`bash build.sh` always
  failed on the qlmb path), bisect with `SYMTA_JIT_FILTER` +
  `SYMTA_JIT_MAX_FN` / `SYMTA_JIT_SKIP_FN` to a single
  function (`fnmeta.name` at compiler.sbc fn-index 207),
  decode that function's bytecode (`SYMTA_JIT_PRINT_FNS=1`),
  read the opcode trace -- IMMEQ stood out -- then audit the
  matching helper for the C-local-before-allocation pattern.
- **Fix.**  Commit `<TBD>`.  Rewrote the affected helpers so
  every `STARG` reads from the frame slot post-`ARGLIST`:

    ```c
    ARGLIST(2);
    STARG(0, ((dyn*)L)[a_sl]);    // fresh read; post-GC
    STARG(1, ((dyn*)L)[b_sl]);
    ```

  Also reload for the `O_TAG`-driven mcache key.  The pre-
  `ARGLIST` C locals are kept for the fast-path tag checks
  (tag bits are GC-stable), but never re-used after the
  allocation point.
- **Verified.**  `rm -rf game/sbc; bash game/build.sh` now
  succeeds three runs in a row.  Drift PASS 5/5.  Runtime
  tests 34/34 PASS.  `tests/runtime/tiny-gen0.sh` still
  pinned.  Game `build.sh` restored to `--jit` (commit
  `1b2c8e4`'s `--no-jit` workaround reverted).

### Bug #13 (FIXED) -- tiny-gen0 secondary GC corruption

- **Trigger.**  Was `SYMTA_GEN0_SIZE=65536 ./symta.exe ...`
  (clean reproducer; default heap sizes never hit it).
- **Surface.**  Was a segfault below heap base, `gc_alloc + 0x79`
  writing the new object's header after `hgp->top - size`
  underflowed.  Caller was `gc_list + 0x49` invoked via
  `gc_custom`'s slot iteration, asking for `~2.1 GB` because
  the source list's `gc_head_t` read back as adjacent dyn data
  instead of a `{size, code}` pair.
- **Root cause.**  Cross-gen write without barrier.  Same
  source line as bug #12 (`parse_term`'s parsed-cache write),
  different failure mode.  After my fix to bug #12, `tok` was
  anchored across `LIST(pp, 1)`, so the C local stayed valid.
  But the actual write `LGET(tok, 6) = pp` was a *direct*
  store -- no `lsetm` write barrier.  Under tiny-gen0 `tok`
  often promotes to gen 4 by the time the cache is written
  (many GCs have happened by then) while `pp` is a fresh
  nursery allocation in gen 0.  A cross-gen pointer without
  the dirty-marking write barrier doesn't get scanned during
  younger-gen collections, so when `pp`'s gen-0 location is
  recycled, `tok.6` is left pointing into the recycled memory.
  When gen-4 eventually collects, `gc_custom` traces `tok`,
  calls `gc_list` on the stale `tok.6`, and reads a corrupted
  `gc_head_t` -> 2.1 GB alloc -> heap underrun -> segfault.
- **Tool that found it.**  Exactly the methodology in this
  file.  `SYMTA_DUMP_SEGV=1` caught the absurd gc_alloc size
  at the source.  The gc_list scan reported the holder slot
  (heap[33519473] in gen 4).  `SYMTA_WATCH_GID=33519473
  SYMTA_WATCH_VALUE=0x000001fe7fe60013` armed a page
  watchpoint that fired on the exact `LGET(tok, 6) = pp`
  instruction (RIP -> objdump -> `parse_term + 0x1e2`).
- **Fix.**  Commit `7f4dd7b` follow-up: changed
  `LGET(tok, 6) = pp` to `LSET(tok, 6, pp)`.  `LSET` calls
  `lsetm` which checks `GC_OLDER(tok, pp)` and marks the
  containing page dirty when the write crosses gen boundaries.
- **Verified.**  `tests/runtime/tiny-gen0.sh` no longer
  surfaces a segfault.  Other tiny-gen0 bugs (#14 under
  `--jit`, an "undefined variable `.`" symptom under
  `--no-jit` that's clearly downstream) still fire; those are
  separate stale-pointer sites tracked separately.

### Bug #11 (P2) -- JIT phase 1d (inline setjmp for SBC_CTX_BTLAND)

- **Trigger.**  None on its own; this is a pending JIT
  improvement, not a correctness bug.
- **Surface.**  N/A.
- **What we know.**  SBC_CTX_BTLAND currently always falls
  through to the interpreter for the setjmp call, costing a
  helper-trampoline jump per `try` block.  An inline emission
  would let JIT'd code stay on the hot path through try / catch.
- **Blocked by.**  Was bug #14; now unblocked.

### Resolved (kept for context)

- **Bug #15** -- four NEW direct-`LGET`-store sites in
  `reader.c` that shared bug #13's shape (`parse_suf_unary`
  parsed-cache + value-mutation, `parse_suf_loop` `=method`-
  name combiner, `binary_loop` no-right-operand append-`_`).
  Found by sweeping every `LGET-as-store` across the runtime
  (audit in `bugs-review.md`).  Fixed in commit `<TBD>` with
  the same anchor + `LSET` recipe.  Pinned class-wide by the
  new `tests/runtime/cross-gen-store.sh` regression test
  (150 bracket-literal tokens under tiny gen0; reverting any
  of the four LSETs back to a raw LGET store reproduces a
  deterministic SEGV).
- **Bug #14** -- the JIT api.args corruption crash under
  `--jit` on game compile.  Fixed in commit `9f3ae76` by
  reordering `STARG` reads to happen *after* `ARGLIST` in
  `jit_rt_immeq_impl` / `jit_rt_immne_impl` /
  `jit_rt_fxnlset_impl` / `jit_rt_fxnlsetir_impl`.  Pre-fix
  the helpers snapshotted L-slot dyns into C locals, then
  called `ARGLIST*` (which can GC), then passed the stale
  C locals to `STARG` -- leaking dangling pointers into
  api.args.  Three consecutive clean game compiles confirm
  the fix.
- **Bug #13** -- the tiny-gen0 expand_qlmb_r segfault.  Same
  source line as bug #12 (`parse_term`'s parsed-cache write)
  but a different failure mode: cross-gen write without
  barrier.  Fixed in commit `449414a` by changing
  `LGET(tok, 6) = pp` to `LSET(tok, 6, pp)` so `lsetm` marks
  the page dirty for `gc_older_gens`.  Found via the
  methodology in this file: SYMTA_DUMP_SEGV -> gc_list scan ->
  page watchpoint -> RIP -> objdump -> source line.
- **Bug #12** -- the original tag-3 / 0x07 poison crash under
  tiny gen0.  Fixed in commit `3c7ec19` via the
  `reader_parse_strip` parsed-cache guard.  Pinned regression in
  `tests/runtime/tiny-gen0.sh`.
- **Bug #10** -- game's `show_fps` overlay was ignoring
  `ui.cfg`.  Fixed in commit `2db44fb`, also added `show_perf`
  for per-frame ms.
