<img src="logo.webp" alt="Symta logo" width="96" align="right">

# Symta — Roadmap

Symta is a working, self-hosted, AOT-compiled language with a small
generational-GC runtime, an x86_64 JIT, a real ECS, an FFI, a stdlib,
an examples tree, and Windows + Linux binaries you can run today.

This page is about **what comes next**.  Each milestone below is
scoped, costed, and has a concrete plan in the engine room.  If you
want to help Symta get there, see *How to support* at the bottom.

---

## Where Symta is today

- Self-hosted compiler, ~28 k LOC of Symta + ~10 k LOC of C runtime.
- Bytecode interpreter **and** an x86_64 JIT translator that covers
  ~99 % of opcodes inline; native code is AOT-cached inside each
  `.sbc`, installed at SBC load on Windows and re-baked on Linux.
- Generational GC (15 generations + dirty-page write barrier) with
  pinned regression tests for the cross-gen pointer-store class.
- Static binary: one `symta.exe`, no DLL soup, no Python, no LLVM.
- Pattern matcher, macros, `eval`, quasiquote, gensyms — built in.
- ECS (`cls`, `dsm`, IPS) integrated into the language, not bolted on.
- FFI plugins for SDL2, FreeType, SVG, a small voxel renderer.
- Cold compile of a 21 k-LOC game: **~14 seconds** with JIT
  (down from ~51 s a few months ago).  Runtime FPS "noticeably
  faster" than the interpreter on the same hardware.
- Runs **Windows x64 + Linux x64** with native JIT today; macOS
  builds from source (no AOT path verified there yet).
- Static type-system tooling in `tests/static-check/`,
  `tests/static-mode/`, `tests/unboxed/` — 50+ regression cases
  exercising the gradual-type extensions.

---

## Milestone 1 — Sub-10-second cold compile *(weeks, not months)*

Symta already shaved cold-compile time from 51 s → 20 s by moving
hot dispatch chains from Symta into the C runtime, and from 20 s
→ ~14 s by shipping the x86_64 JIT.  The next 4-5 seconds come
from a cross-opcode register allocator (Phase 2 in
[`jit-todo.md`](jit-todo.md)) plus tiny-list allocation inlined
into the JIT.

**You get:**

- A 21 k-LOC project that compiles in **under 10 s, cold**.
- Edit-compile-run cycles measured in single seconds.
- No change to the source language — your existing code just gets
  faster.

---

## Milestone 2 — Static type system *(one quarter, in progress)*

A gradual, inference-driven type system that **augments** Symta
rather than replacing it.  Optional annotations, full backward
compatibility, no Hindley-Milner-flavored ceremony.

**Already shipped (v0.1.0+):**

- 50+ regression cases under `tests/static-check/`,
  `tests/static-mode/`, `tests/unboxed/`.
- `the` introspection, `typeof`, and the static-check tool
  (examples `37`-`39`).
- Compile-time tag inference threaded into the macroexpander so
  `times I N` loops emit typed `_ilt` / `_iadd` at the loop edge.

**Still to land:**

- Optional type annotations as first-class syntax: `Int X`,
  `[Int] Xs`, `Text:Int Table`.
- Whole-program inference where you skip the annotations.
- Compile-time errors for the mistakes that today are runtime
  `bterror`s.
- A type registry available at runtime, so reflection / debug /
  pattern-match introspection still work.

Borrows from Common Lisp's `declare`, TypeScript's structural
types, and ML's inference — without their bad parts.  See
[`architecture.md`](architecture.md) for where it slots in.

---

## Milestone 3 — Unboxed numerics & SoA layouts *(one quarter)*

Today every `Int` is a tagged pointer.  Loop-heavy numeric code
pays the boxing tax.  With Milestone 2 in place, the compiler can
prove a `for I in :N` runs over machine ints and emit raw integer
opcodes.

**Already shipped:**

- Typed-int arithmetic opcodes (`_iadd`, `_isub`, `_imul`,
  `_idiv`, `_irem`, `_ilt`, `_igt`, ...) with both interpreter
  and JIT support.  The JIT inlines them as raw x86 add/sub/mul
  with no boxing.
- Typed-float arithmetic on the same shape (`_fadd`, ...).
- Macro-layer propagation: `times` / `dup` / `for` loops over
  statically-known integer ranges emit the typed variants.

**Still to land:**

- `[Int]` and `[Flt]` arrays as actual flat memory, not pointer
  soups.  ECS components and tensor workloads need cache-friendly
  layouts.
- True 64-bit ints (today's typed ints are 60-bit via tag bits).
- Auto-vectorisation on tight typed loops.

---

## Milestone 4 — Native code generation *(largely shipped, x86_64; ARM64 pending)*

Bytecode→native-x86_64 has shipped.  The JIT translator emits
native code at SBC load (AOT) or compile (runtime), with full
unwind / pin tracking and a `lsetm` write barrier.

**Already shipped (v0.1.0 + v0.1.1):**

- x86_64 JIT covers ~99 % of opcodes the runtime emits inline:
  arith, comparison, immediate load, MOVE family, load/store
  family, list array access, immediate compare.  Only
  `SBC_CTX` (try/finally setjmp/longjmp) stays on the interpreter
  in 11 error-handling functions.
- AOT-cached native code embedded in each `.sbc` (the `IA64`
  section).  Loader patches relocs against the live runtime
  helper table; SBC load is essentially zero-cost.
- Mixed-mode dispatch: JIT'd caller can invoke interpreter callee
  and vice versa.
- Per-platform encoding: Win64 + SysV-x64 share one translator
  via `#ifdef _WIN32` branches; the `IA64` section is stamped
  with its originating ABI so a Win64-baked SBC on Linux falls
  back gracefully to runtime translation.
- Cross-gen GC barrier inline for the store family; bypass on
  immediate values.

**Still to land:**

- **ARM64 backend** — Apple Silicon, Graviton, modern Android.
  Half the integration is already shared (the lowering IR);
  only the per-opcode emit tables differ.
- **Cross-opcode register allocator** — pins hot locals to
  callee-saved regs, spills only at block exits and helper
  calls.  Roadmap's high-leverage next step; 2-3× projected on
  counted loops.  Tracked in [`jit-todo.md`](jit-todo.md)
  Phase 2.
- **POSIX SEH equivalent** — DWARF `.eh_frame` registration via
  `__register_frame` so `longjmp` walks through JIT'd frames
  cleanly on Linux + macOS.  Today the Linux fallback uses the
  runtime translator without an unwind table, which works as
  long as no Symta-side `bterror` fires from a JIT'd frame.

See [`jit-todo.md`](jit-todo.md) for the full JIT phase
breakdown (Phases 1-12) and per-step status.

---

## Milestone 5 — Tiered + profile-guided JIT *(speculative; ship-if-funded)*

The shipped JIT is a *baseline* JIT: every function gets the same
translation strategy, no profile-driven specialization.  For
long-running services and games, a second tier on top:

**You get:**

- Hot functions recompile with inlining + guard-protected
  monomorphic dispatch.
- On-stack replacement (OSR) for hot loops that started cold.
- Deoptimization when a guard fails — fall back to the baseline
  JIT or the interpreter cleanly.
- Closes the last gap to hand-written C on irregular workloads.

Tracked in [`jit-todo.md`](jit-todo.md) Phases 6-7.

---

## Always-on workstreams

These don't sit on the critical path.  They land in parallel as
funding allows.

| Workstream | What lands |
|---|---|
| **Float-precision round-tripping** | Compiler preserves `1e-12` literals exactly through SIF / SBC. (Today they round to `0.0`.) |
| **Reader / FFI polish** | Better error messages for FFI mismatches; scientific-notation float literals; faster parsing of large source files. |
| **GC tuning** | Larger generations, write-barrier specialization, pacing knobs for game-loop friendly pause budgets. |
| **Stdlib expansion** | More batteries: HTTP client, JSON, `sqlite3`, regex (real engine, not toy). |
| **Editor support** | VS Code grammar, Notepad++ syntax file, indentation rules.  An LSP eventually. |
| **Docs** | A book.  Tutorials by example.  A short paper on the pattern-matcher / `{}` operator design. |

---

## What's already done *(receipts)*

- Self-hosted compiler, with a one-screen bootstrap.
- AOT bytecode + generational GC, statically linked.
- **x86_64 JIT** translator (~99 % opcode coverage) with AOT cache
  embedded in each `.sbc`.  Released as
  [v0.1.0](https://github.com/NancySadkov/symta/releases/tag/v0.1.0)
  in May 2026.
- **Linux JIT support** with cross-platform ABI handling and
  runtime-translation fallback for Win64-baked SBCs loaded on
  SysV.  Released as
  [v0.1.1](https://github.com/NancySadkov/symta/releases/tag/v0.1.1).
- Typed-arith opcodes (`_iadd`/`_isub`/`_imul`/`_idiv`/`_irem`/
  `_ilt`/`_igt`/`_ilte`/`_igte` + float variants) emitted by the
  macroexpander for statically-known integer/float ranges,
  inlined as raw x86 by the JIT.
- 40 example programs covering FizzBuzz, quicksort, Prolog-style
  inference, n-body, voxel renderer, SDL game runtime, static
  type checking, type introspection, double-entry bank.
- Pattern matcher unified with map / filter / reduce / replace /
  parse / destructure — one syntax for all of them.
- Cold-compile time cut from 51 s to ~14 s through runtime
  consolidation + Phase-1 JIT inline campaign.  See
  [`architecture.md`](architecture.md) and the per-step roadmap
  in [`jit-todo.md`](jit-todo.md).
- Five GC-corruption bugs pinned and fixed (the cross-gen
  direct-store class + the JIT helper "C-local stale across
  ARGLIST" class).  Methodology documented in
  [`runtime-debug.md`](runtime-debug.md); two pinned
  regression tests in `tests/runtime/`.

---

## How to support

Symta is dual-licensed MIT OR Apache-2.0.  It is, and will remain,
free to use and embed.  Development is currently funded by one
human writing the code in her spare time.  If you'd like to see the
roadmap above move faster, the following channels exist:

- **GitHub sponsorship** *(coming soon)* — recurring monthly tiers
  that fund weekly hours on the core compiler.
- **One-shot work-for-hire** — if your team needs a Symta feature
  on a schedule (a backend, a stdlib module, an FFI binding), I
  take milestone contracts.  Open an issue tagged `sponsor` to
  start the conversation.
- **Contribute code** — every milestone above breaks down into
  weekend-scoped pull requests.  See [the contributing
  section](README.md#license) and open an issue describing what
  you'd like to take on.
- **Use it in production** and tell me what broke.  Real-world
  pressure shapes the roadmap.

---

*Roadmap last updated: 2026.  Symta is built by Nancy Sadkov.  See
[LICENSE](LICENSE) for copying conditions.*
