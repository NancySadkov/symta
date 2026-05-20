# JIT roadmap — toward a serious-language JIT

> **Premise.** Symta is a language for writing real software (games, tools,
> services), not a teaching toy. Its JIT therefore needs to deliver
> production-grade performance: within 2-3× of C on numeric code,
> sub-millisecond GC pauses, and predictable behavior under load — the
> bar HotSpot and SBCL set. The current x86_64 translator (steps 0-12 in
> `symta-TODO.md` history) is the prototype; this document is the
> roadmap from that prototype to a JIT we'd ship behind a paid product.

## Where we are (May 2026)

What works today:
- x86_64 translator translates ~60-75 % of the runtime's functions per
  SBC (compiler.sbc 75 %, macro.sbc 67 %, core_.sbc 56 %).
- AOT mode (default on Windows): each `.sbc` carries an `IA64` native
  section; the loader installs native code on SBC load, replacing the
  interpreter hook per function. Mixed-mode dispatch — JIT'd caller can
  invoke interpreter callee and vice versa.
- Inline x86 for the typed-arith family (`IADD`/`ISUB`/`IMUL`/`IDIV`/
  `IREM`, `ILT`/`IGT`/`ILTE`/`IGTE`, `SAME`/`VARY`) and the FXN* slow-
  path-with-helper for the untyped variants.
- Windows SEH unwind registered per JIT'd function so `bad` /
  `longjmp` walks through native frames cleanly. Zero-cost pin tracking
  via the existing frame chain.

What it costs us today, measured on the `times I 10^9: X += I` micro
and the `./game/` cold compile:

| workload | interpreter | current JIT | C reference |
|---|---:|---:|---:|
| `1 B add-loop` | 13.6 s | 9.2 s | 1.85 s |
| `./game/` cold compile (~21 kLOC) | ~17.0 s | ~16.0 s | n/a |

The arithmetic micro is the canonical "JIT competence" test. We're at
**5× C** there. HotSpot and SBCL hit 1.5-2× on the same shape. The gap
is structural — memory round-trips per opcode, no cross-opcode register
allocation, no inlining. Closing it is what this roadmap is about.

## The reference points

The competition we're measuring ourselves against:

- **HotSpot (OpenJDK):** tiered C1+C2, profile-driven inlining, escape
  analysis, deopt. Within 2× of C on numeric code, often within 1.2× on
  ECMA-style benchmarks. Pause times sub-10 ms with G1.
- **SBCL (Common Lisp):** typed-AOT (with `declare (type ...)`). Within
  ~1.5× of C on typed numeric loops, sometimes faster than gcc -O2 on
  hand-laid-out integer code. Doesn't do speculative optimization — its
  win is "if you tell me the types, I'll generate tight code."
- **LuaJIT:** trace-based JIT, dynamic types. Within 2-3× of C on tight
  loops without type annotations.
- **V8 (Node.js):** Ignition + TurboFan, profile-driven specialization
  + deopt. The "JIT magic" tier.

Our target lane: **SBCL-style AOT with tasteful JIT fast paths**. Symta
has compile-time type inference (TS-1..TS-4); we should lean on it. The
JIT's job is to take typed code and emit tight x86. Untyped paths get a
slower but correct fallback. We're not chasing V8's profile-driven
specialization (too much complexity for a one-person codebase).

## The phases

Each phase delivers a measurable, shippable win. Don't skip ahead.

### Phase 1 — opcode coverage + correctness *(in progress)*

Every opcode the runtime emits must translate. Steps 5-12 in
`symta-TODO.md` cover this; remaining gaps:

- [ ] `SBC_FXNLGET` / `SBC_FXNLSET`: array-style fast path (`T_LIST` +
      `T_INT` index, in bounds) → inline heap-slot read/write. Two prior
      attempts hit a non-obvious bug in the inline encoding even after
      fixing the SIB scale=8 issue. Re-attempt with a byte-level
      disassembly comparison harness.
- [ ] `SBC_LD4_*` / `SBC_LOAD` / `SBC_LOAD8`: same heap-deref shape.
      Same bug class as `FXNLGET`. Once one works the others fall out.
- [ ] `SBC_STOR` / `SBC_STOR8` / `SBC_ST4_*`: store family. Adds the
      `lsetm` GC write barrier on cross-generation stores — inline the
      common case, branch to helper on barrier fire.
- [ ] `SBC_FXNLISTN` / `SBC_LIST` / `SBC_LIST1` / `SBC_LIST2`:
      tiny-list allocation. RT-9 measurement: 480 M tiny-pair allocs
      per game compile. Inline `gc_alloc(T_LIST, n)` + slot stores for
      sizes 1-4; fall back to helper for variable / large.
- [ ] `SBC_IMMEQ` / `SBC_IMMNE`: immediate-equality with mcache. The
      mcache-fast-path is already there in C; mirror it inline.

**Target on completion:** > 95 % of compiler.sbc + macro.sbc functions
translate. No interpreter fallback for hot paths.

### Phase 2 — cross-opcode register allocation *(the lever)*

The current JIT loads every operand from `L[slot]` per opcode and
stores the result back. A tight loop pays 3-4 memory round-trips per
iteration that gcc -O2 would put entirely in registers.

Build a **basic-block register allocator** that:
- Identifies hot locals (referenced > N times in one block).
- Pins them to callee-saved registers (`R13`-`R15` are free; `RBX`,
  `R12` reserved for L and `sbc`).
- Spills to the slot only at block exits (and at every helper call).
- Treats typed-int locals as the priority — those have the most to
  gain from register residence.

**Prerequisite:** Phase 3 (typed-arith opcodes everywhere a typed loop
appears). The allocator needs to know `L[I]` is provably int across an
entire block.

**Expected lever:** 2-3× speedup on counted loops. Brings the `1 B
add-loop` micro from 9 s into the 2-3 s range — comparable to SBCL.

Estimated effort: 2-3 weeks. The hard part is correctness across
prologue/epilogue and helper-call boundaries (caller-saved registers
get clobbered, so spill before every helper call). Reference design:
LuaJIT's "fast register allocator" — a stack of free registers, LRU
eviction, single-pass.

### Phase 3 — typed-arith propagation *(macro layer)*

Today the `times`, `dup`, `for` macros emit untyped `_add` / `_lt` /
`_inc` etc. even when the loop bounds are statically int. The runtime
INC has a fast path for ints, but its helper-call overhead dominates
the per-iteration cost; the JIT inlines the fast path (step 11), still
~4-5 ns/iter vs C's 1.85.

The right fix is at the macro layer:

- [ ] `times`, `dup`, `for`, `while`: emit `_iadd` / `_ilt` /
      `_iadd I 1` when the loop counter is statically int. (The `_tag`
      assertion in `times` already establishes this — the macro
      currently just doesn't act on it.)
- [ ] Type-narrowing in `case` arms: when one branch establishes
      `X :int`, the rest of that arm's `+`, `<`, `.n` etc. uses typed
      opcodes.
- [ ] Method-return inference (TS-4.5) needs to cover the common
      stdlib methods consistently (`list.n`, `text.l`, etc. already
      return int; ensure `int.*` operators consistently propagate).

**Win:** typed loops become indistinguishable in bytecode from
hand-written `_ilt` / `_iadd` code. Combined with phase 2, they reach
SBCL parity.

### Phase 4 — optimization passes on the SBC IR

Before native emission, run a small set of classical passes on the
SBC stream. None require SSA conversion (a benefit of our flat
register-based IR over Java bytecode's stack-based):

- [ ] **Dead store elimination.** SSA-register stores that no one
      reads. Already filed under CORE-8.
- [ ] **Copy propagation.** `MOVE r1 r0; <use r1>` → `<use r0>`.
- [ ] **Constant folding.** `IADD r0 const1 const2` → `LDFXN r0
      (const1 + const2)`. Compiler already does some of this; extend
      to the runtime's pre-JIT pass for opcodes that survive
      macroexpansion.
- [ ] **Hoisting loop-invariant code.** `LDFXN K 1` inside a loop
      body lifts out.
- [ ] **Strength reduction.** `IMUL r0 r1 (FXN 2)` → `SHL r0 r1 1`
      (we already have `FXNSHL`).
- [ ] **Branch threading.** `JMP L1; L1: JMP L2;` → `JMP L2`.

Estimated effort per pass: 1-3 days. None individually huge, but the
sum is 2-3× the inner-loop perf on real code. Pre-condition: the
register allocator (Phase 2) so the passes can see usage counts.

### Phase 5 — function inlining

Hot leaf calls cost a `CALL` + frame setup. Inline them:

- [ ] **Small leaf functions** (< 32 bytes of bytecode, no recursion):
      paste body into caller.
- [ ] **Monomorphic method calls**: when the mcache shows one tag
      hit on every call after warmup, inline the body of THAT method
      directly (no dispatch). The mcache slot becomes a guard
      (deoptimization point).
- [ ] **Method-call open-coding** for primitive-receiver methods
      (`int.+`, `int.<`, `list.l`, etc.). Compiler already does some of
      this at SSA time (`_iadd` etc.); cover the rest.

This is where Symta starts to beat the interpreter by ORDERS of
magnitude on dispatch-heavy code (the game's per-frame ECS update is
99 % method dispatch).

Estimated effort: 4-6 weeks total, can be staged.

### Phase 6 — deoptimization + speculative guards

Phase 5's "monomorphic inline" creates a problem: what if a later call
hits a different type? Today the mcache catches that. Inline code
needs explicit guards + a way to bail out.

- [ ] Guard format: tag check + `jne deopt_label`. Deopt label exits
      the inlined region and falls through to the generic dispatch
      path.
- [ ] Recompile-on-deopt: track deopt counts per call site; if a site
      exceeds a threshold, recompile that function without the
      speculative inline.
- [ ] On-stack replacement (OSR): if deopt fires mid-loop, bail to the
      interpreter at the SAME bytecode position. Needs the JIT to
      maintain a bytecode-PC ↔ x86-PC map per safepoint.

This is the boundary where Symta's JIT crosses from "AOT with tasteful
inline" to "actual JIT". Estimated effort: 3-4 weeks for the basic
deopt+OSR.

### Phase 7 — tiered compilation

Three tiers:
1. **Interpreter** (current). The fallback. Always correct.
2. **Baseline JIT** (current). Per-opcode inline, no optimization.
   Fast to emit (μs per function), modest perf.
3. **Optimizing JIT** (new). Runs passes from Phase 4 + inlining from
   Phase 5 + deopt from Phase 6. Slow to emit (~ms per function), C-
   competitive perf.

Promotion criteria: a function exceeds N execution counts, recompile at
tier 3. Hot loop OSR: a back-edge fires > M times, OSR-recompile the
loop.

Estimated effort: 2 weeks for the tier-3 driver + perf counters.

### Phase 8 — escape analysis + stack allocation

The single biggest GC pressure source is short-lived list/closure
allocations (RT-9 numbers: 530 M T_LIST per game compile, ~90 %
size-1 or size-2). Escape analysis can prove "this allocation never
outlives this frame" and stack-allocate it:

- [ ] Intraprocedural EA: track each allocation's lifetime through
      the function; if it never reaches a return, escape, or store-to-
      heap, allocate on the stack.
- [ ] Interprocedural EA (with inlining from Phase 5): same analysis
      across inlined boundaries. Closures over loop bodies become
      stack-allocated.

**Expected lever:** 30-50 % of GC pressure on the game's hot loop
vanishes. Cold compile drops by 1-3 s.

Estimated effort: 4-6 weeks. EA correctness is subtle (GC-collected
references vs raw pointers, finalizers, etc.).

### Phase 9 — GC for the JIT era

Today's GC is a stop-the-world copying collector. Acceptable when the
JIT is slow because GC time is dwarfed by interpreter time, untenable
once JIT'd code spends 50 ms in a 100-ms frame:

- [ ] **Generational nursery**: most allocations die young (RT-9
      measurement). Add a nursery: small (~256 KB) sub-arena, scan
      only on minor GC. Promote survivors to the main heap.
- [ ] **Card marking**: dirty cards in old-generation slots that
      reference young-generation objects. Avoids whole-heap scan on
      minor GC.
- [ ] **Concurrent marking**: parallel-mark the old generation while
      mutator runs. Reduces pause time from O(heap size) to O(young
      gen size).
- [ ] **Write barrier integration**: JIT emits barrier inline at every
      `lsetm` site. Today we call into the C barrier; inline drops the
      per-store cost.
- [ ] **Read barriers** *(optional, deferred)*: needed only if we go
      to a fully concurrent (Shenandoah-style) collector. Brings pause
      times below 1 ms but adds load overhead. Defer until specific
      use case demands it.

Estimated effort: 6-8 weeks. The current GC works, this is "make it
not stall the JIT'd code."

### Phase 10 — SIMD / vectorization

Modern x86 has AVX2 / AVX-512. Tight typed loops can use them:

- [ ] **Auto-vectorize** counted loops with no cross-iteration deps.
      `times I N: X[I] = A[I] + B[I]` → 8-wide SIMD add per iter.
- [ ] **SIMD intrinsics** in the language. `vec4 X = vec4_load p; X = X
      * V;`. Maps directly to `MOVDQA`/`MULPD`.
- [ ] **GPU-via-FFI**: `array<float> X` already heap-layout-compatible
      with C; pass to OpenCL / CUDA kernels via the existing FFI.

Estimated effort: 6-10 weeks for auto-vectorize, less for intrinsics.
This is where Symta becomes a serious option for numerical/graphics
work — competing with Julia and Numpy on typed-array code.

### Phase 11 — POSIX support

Currently Windows-only AOT. Linux/macOS need:

- [ ] **DWARF `.eh_frame`** registration so `longjmp` walks through
      JIT'd frames. `__register_frame` API; emit hand-crafted CIE/FDE
      bytes per function.
- [ ] **POSIX `mmap` with `MAP_JIT`** (macOS) or `PROT_EXEC` (Linux).
      Already partially there.
- [ ] **ARM64 backend** (NATIVE-2 from `symta-TODO.md`): same SBC →
      native pipeline but emitting AArch64. Apple Silicon, Graviton,
      Windows-on-ARM. Half the integration code is shared with x86_64;
      only the opcode-emit tables differ.

Estimated effort: 2-3 weeks for POSIX DWARF; 4-6 weeks for ARM64
backend (concurrent with the x86_64 work if we share the lowering IR).

### Phase 12 — production polish

What makes the JIT a SHIPPABLE product:

- [ ] **Stable bytecode + native ABI** (NATIVE-PRE from
      `symta-TODO.md`). Freeze the SBC layout and helper signatures.
      Bump `SBC_REVISION` only on incompatible change; old SBCs get a
      clean "recompile" error.
- [ ] **Cross-platform fat binaries**. Single `.sbc` with native
      sections for x86_64 + ARM64 + Windows + Linux + macOS.
- [ ] **JIT debugging integration**. GDB / LLDB JIT interface — they
      need callbacks to symbolicate JIT'd code. Standard `__jit_debug_
      register_code` protocol.
- [ ] **Perf profiling**. Linux `perf_jit_dump` + Windows ETW so the
      profiler sees JIT'd functions, not opaque `[anon]` regions.
- [ ] **Crash-with-stack-trace** for JIT'd code. Today's SEH/setjmp
      works; need it to symbolicate to source lines via the
      lineno side table.

Estimated effort: 4-6 weeks of polish work, mostly platform integration.

## Calibration vs the competition

Once Phase 1-7 land, the comparison shifts:

| workload | symta now | symta phase 7 | HotSpot | SBCL | C |
|---|---:|---:|---:|---:|---:|
| `1 B add-loop` | 9.2 s | 2-3 s | 2 s | 2 s | 1.85 s |
| `./game/` cold | 16 s | 6-8 s | n/a | n/a | n/a |
| game runtime FPS | acceptable | C-competitive | comparable | comparable | reference |

That's the bar: numeric code within 2× of C, runtime FPS not bottlenecked
by the language, cold compile fast enough that the inner edit-test loop
feels instant.

## What this is NOT

A note on what we're explicitly choosing NOT to chase:

- **V8-style PGO + speculative specialization.** Yields more peak perf
  but at huge complexity (one-person codebase). Symta's compile-time
  type system can deliver 80 % of the win at 20 % of the cost.
- **Tree-shaking IR like Graal/Truffle.** Beautiful design but
  multi-person engineering effort.
- **Multi-threading the JIT compiler.** Symta compiles fast enough
  serially; not worth the complexity.
- **A full HotSpot/V8 clone.** We're 100k LOC, not 10 M.

## Sequencing and risk

The risky path is Phase 8 (escape analysis) — subtle correctness bugs
have very long bisection times. The high-leverage path is Phase 2
(register allocator) → Phase 3 (typed propagation) → Phase 7 (tiered).
That sequence delivers measurable wins at every commit and unblocks
later phases.

**Rough timeline at sustained focus:**
- Phase 1 completion: 2-3 weeks
- Phase 2 + 3: 4-6 weeks
- Phase 4 + 5: 8-10 weeks
- Phase 6 + 7: 6-8 weeks
- Phase 8 + 9: 10-12 weeks
- Phase 10: 6-10 weeks (concurrent with 11)
- Phase 11: 4-6 weeks
- Phase 12: 4-6 weeks

Total to "production-grade JIT": 9-12 months at 1 person, focused.
That's the order of magnitude for the SBCL parity claim.

## Closing

Symta's bet is: **the language's compile-time type system + a
SBCL-style typed-AOT JIT delivers 90 % of the perf of HotSpot at 10 %
of the engineering cost.** This roadmap is what that bet looks like in
phases.

A serious instrument doesn't have a slow tier. By the end of Phase 7
we shouldn't have to apologize for any workload — typed numeric code
is C-competitive, dispatch-heavy code is comparable to Java, GC pauses
don't show up on a frame budget. That's the bar. Anything less is a
toy.
