<img src="logo.webp" alt="Symta logo" width="128" align="right">

# Symta

> A list-processing Lisp dialect where one terse line replaces fifty
> verbose ones — and stays readable.

Inspired by **REFAL**, **POP-11**, and **MIT PLANNER**, Symta is built
around one idea: the everyday operations that bloat real programs —
filtering, mapping, parsing, replacing, destructuring, classifying —
should each be one short, composable construct. Not a library call.
Not a regex. Not a 30-line for-loop. **One construct.**

```symta
// FizzBuzz, in full:
[:100]{~?%15=\FizzBuzz; ~?%3=\Fizz; ~?%5=\Buzz}

// Quicksort:
qsort@r H,@T = @T{:?<H}^r, H, @T{?<H=}^r

// Substitute %vars% in a template from a table -- in 18 characters:
Form{@"%[V]%"=Data.V}
```

The same `{}` operator does map, filter, group, reduce, replace, parse
and pattern-match — because in real programs those are the same idea
with different bodies.

---

## Why you might care

- **You write Lisp** and miss having concise list-processing on tap.
  Symta has macros, `eval`, quasiquote, gensyms — *and* a pattern
  matcher fused into the syntax.
- **You like APL or J** but wish your read-only colleagues didn't.
  Symta beats J in brevity on most non-linear-algebra tasks while
  staying alphabetic and grep-friendly.
- **You wrangle data, logs, or text.** Replacing "every word matching
  X" or "every digit grouped by line" is a one-liner, not a sprint.
- **You build games or simulations** and have hit the wall where OOP
  starts pretending it scales. Symta ships a real ECS (`cls`, `dsm`,
  IPS) integrated into the language, not bolted on as a library.
- **You implement languages.** Symta is self-hosted, AOT-compiled to
  bytecode, runs on a small generational-GC C runtime (~28k lines of
  Symta + ~10k lines of C), and the compiler fits in your head.
  See [architecture.md](architecture.md).

---

## A 60-second taste

```symta
// Forward and backward through the same shape:
Data name!\Nancy age!37 city!\Amsterdam
Form "Hi! My name is %name%, I'm %age% years old and I live in %city%."
say Form{@"%[V]%"=Data.V}
//   Hi! My name is Nancy, I'm 37 years old and I live in Amsterdam.

say Form("Hi! My name is [Name], I'm [Age] years old and I live in [City]." =: Name Age City)
//   binds Name="Nancy", Age=37, City="Amsterdam"


// Frequency table, sorted by count -- 1 line:
S{~D.?+}.s | ?1 > ??1


// Tree walk: collect ints > 100 anywhere inside a nested structure:
Tree(:_^r@_^r; &~r._<{?>100}<int?)


// 11-line Prolog-style inference engine, two-way:
fact Sbj Verb Obj =
  new Sbj Verb,Obj
  new Obj "[Verb]_by",Sbj

infer Sbj Verb =
  Rs Sbj^each{Verb}.skip^No
  Rs: Rs @Rs{|infer ? Verb}
  Rs.j

who  Verb Sbj = say infer(Sbj "[Verb]_by").text^" " Verb Sbj
what Sbj Verb = say Sbj Verb: infer(Sbj Verb).text^", "
```

Every example above is real, runnable Symta — not pseudocode.

---

## Quickstart

A pre-built `symta.exe` (Windows x64) ships in the repo.  Pre-built
binaries for **Windows and Linux** are also attached to each
[release](https://github.com/NancySadkov/symta/releases).  On macOS,
build from source:

```sh
make -f Makefile.linux   # Linux  (gcc, libsdl2-dev, libpng-dev)
make -f Makefile.osx     # macOS
make -f Makefile.w64     # Windows (w64devkit)
```

Hello, World:

```sh
echo 'say "Hello, World!"' > hello.s
./symta -f hello.s       # Linux / macOS
./symta.exe -f hello.s   # Windows
```

Or compile a project to a standalone binary:

```sh
mkdir -p myapp/src
echo 'say "Hello, World!"' > myapp/src/go.s
./symta myapp            # cross-compiles the project to ./myapp/go
./myapp/go               # -> Hello, World!
```

Or just open the REPL:

```sh
./symta
```

### Native code, by default

Symta ships with an **x86_64 JIT** that emits native machine code
for ~99 % of bytecode opcodes -- typed-int arithmetic, list array
access, immediate compare, struct loads and stores all run as
inline x86 with no per-opcode helper-call overhead.  Hot loops
reach C-competitive single-iteration cost; a 21 kLOC game's cold
compile takes ~14 s.

On Windows, the JIT is **default-on** and the AOT cache embedded in
each `.sbc` makes startup near-instant.  On Linux the same SBCs
load via the runtime translator; after a Linux bootstrap they
carry SysV-tagged AOT code and install directly.  Add `--no-jit`
to force the interpreter for debugging.

---

## Learning path

1. **[examples/](examples/)** — 40 progressively richer programs,
   from FizzBuzz to a voxel renderer.  The 37-40 set added in
   2026 (`the` introspection, `typeof`, the static-check tool,
   double-entry bank) exercises the type-system work.
   Single-file examples (`00`–`09`, `11`, `14`, `17`–`20`,
   `22`–`40`) run with `symta -f examples/NN-*.s`; project examples
   (`10`, `12`, `13`, `15`, `16`, `21`, `31`) build with
   `symta examples/NN-name && examples/NN-name/go`.
2. **[dev/sbe.md](dev/sbe.md)** — *Learn Symta by Example*, the
   long-form tour of the language: variables, functions, lists,
   pattern matching, OOP, ECS, FFI, macros, the quirky bits, and
   the standard library reference.
3. **[AI.md](AI.md)** — a tight, paste-into-LLM-context cheat sheet
   covering syntax, semantics, the standard library, and common
   gotchas.
4. **[architecture.md](architecture.md)** — for contributors and the
   language-implementation curious: how the compiler, runtime, GC,
   and FFI fit together.

---

## What's in the box

| | |
|--|--|
| `symta.exe`        | The compiler + runtime + JIT, statically linked. Windows x64; matching Linux binary attached to each release. |
| `src/`             | Compiler, reader, macroexpander, stdlib — all in Symta. |
| `runtime/`         | C runtime: generational GC, bytecode interpreter, x86_64 JIT translator (`jit.c` / `jit_sbc.c`), built-ins. |
| `examples/`        | 40 self-contained example programs. |
| `c/`, `ffi/`       | FFI plugins — graphics, UI, fonts, SVG, voxel renderer. |
| `pkg/`             | Ready-made projects (hello, tests, the symta tool itself). |
| `tests/`           | Drift bootstrap, runtime correctness, AM, static-check, static-mode, unboxed-arith — 13 test suites. |

---

## Contact

Questions, bug reports, patches, or just to say hello:
**nangld85@gmail.com**

---

## License

Symta is dual-licensed under either of:

- [Apache License, Version 2.0](LICENSE-APACHE)
- [MIT License](LICENSE-MIT)

at your option. Contributions are accepted under the same dual
license unless you explicitly say otherwise.
