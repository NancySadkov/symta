<img src="../logo.webp" alt="Symta logo" width="96" align="right">

# Learn Symta by Example

A working tour of the language.  You should already know a little
programming — what a variable is, what a function does — but you
don't need to know any specific language to follow along.  Every
example below is real, runnable Symta.

---

## Why Symta exists

Most programs spend most of their lines on three operations:
**transform a list of things, filter it, and put it back together
in a different shape**.  Reading text into structured data and
emitting structured data back as text.  Walking trees.  Matching
patterns.  Replacing the parts that need replacing and leaving
the rest alone.

In C++, Python, or JavaScript, each of those is a different tool:
a for-loop, a regex, a list comprehension, a parser library, a
template engine.  You stitch them together by hand.  By the third
or fourth time on a single project, you're maintaining glue.

Symta says: those are all the same operation with a different
body.  Give them one syntax — `{}` — and let you write the body
in whatever style fits the problem, from a terse one-liner to a
multi-page well-commented function.  The language stays out of
your way.

```symta
// FizzBuzz, in full:
[:100]{~?%15=\FizzBuzz; ~?%3=\Fizz; ~?%5=\Buzz}

// Quicksort:
qsort@r H,@T = @T{:?<H}^r, H, @T{?<H=}^r

// Substitute %vars% in a template from a table -- 18 characters:
Form{@"%[V]%"=Data.V}

// Parse a sentence back into the values that built it:
say Form("Hi! My name is [Name], I'm [Age] years old." =: Name Age)
```

Every snippet above is one expression.  Every snippet is also
something you could spell out across twenty lines if a colleague
preferred — *Symta gives you the dial*, from terse to verbose,
without changing languages.

That's the elevator pitch.  The rest of this document shows you
why, when you've used the language for a week, the terse form
becomes the readable one.

### Aside: why it looks like this

Symta started in 2006 as an honest question: *why can't a general
purpose programming language do what `sed` and regular
expressions do, without you dropping out into a shell?*  The
prototype that became Symta has been refined for nearly twenty
years, through several discarded iterations, until the answer
settled into the language you're reading about now.

The languages that already give you APL- or J-style brevity for
list-processing demand you give up alphabetic identifiers and
grep-friendly source code in exchange.  Symta keeps the words
short but never replaces them with single Greek letters.  It is
deliberately terse without ever being cryptic — `{}` is one
construct, `?` is one role, `@` is one operation, and once
you've absorbed the half-dozen pieces that make up the language
core the rest is just standard library.

A consequence: a one-liner Symta program is a viable replacement
for an awk / sed / jq pipeline, *and* a viable medium for
exploratory statistical or scientific work where you'd otherwise
reach for J, K, or APL.  The same language fits both.  See
[`examples/33-csv.s`](../examples/33-csv.s),
[`examples/34-xml.s`](../examples/34-xml.s), and
[`examples/35-json.s`](../examples/35-json.s) for runnable
proofs — each handles its respective everyday data format
*without* a parser library, in well under a hundred lines.

---

## What Symta gives you that other dynamic languages don't

Three concrete promises, in plain English.

### 1.  Lexical scoping — *the variables on screen are the variables you get*

When you write:

```symta
Total 0
till Done:
  Total = Total + 1
```

`Total` is the same name throughout — created once on the first
line, reassigned on the third.  No surprises from a function call
"reaching into" your `Total` from somewhere else.

JavaScript, Python, and Lua were all designed with looser rules
that let nested functions and "global by default" name lookups
collide in subtle ways.  The classic trap is a `for i in ...:`
loop where `i` outlives the loop body and silently shows up in
the next function you wrote because nothing said it shouldn't.

Symta does what most modern languages now copy from Scheme and
ML: every name is visible exactly where the source code says it
is.  When you read a Symta function, the variables you see are
*all of them*; nothing leaks in from elsewhere and nothing leaks
out.

### 2.  A type system that isn't broken

Symta is dynamically typed — values carry their type, variables
don't.  That part is shared with Python, JS, and Lua.  What
Symta does differently is the *arithmetic of types*:

```symta
1 + 1            //  2     int + int   = int
1.0 + 1.0        //  2.0   flt + flt   = flt
1 + "1"          //  Err   int + text  = error, not "11" or 2
"abc" + "def"    //  abcdef text concatenation, explicit
```

JavaScript famously evaluates `1 + "1"` to `"11"` (string), then
`1 + 1 + "1"` to `"21"`, then `"1" + 1 + 1` to `"111"`.  Python
3 raises `TypeError`.  Symta raises a typed `bterror` you can
catch — and which carries the source location of the offending
expression.

There is no implicit "null-to-anything" conversion.  There is
no "truthy if you squint" rule.  `if`, `when`, `and`, `or`, and
`not` all treat the integer `0` as false and *every other value*
as true — the empty list, atoms, text, and even Symta's "no
value" marker `No` are all truthy.  This is the opposite of
Lua's "empty table is false" or Python's "empty list is false";
the distinction between "I have no value" (`No` — like SQL
`NULL`) and "I have the value zero" (`0`) is preserved.  Use
`got X` to test "is X a value" and `not X` to test "is X
zero".  Details in the *Truthiness* sidebar below.

### 3.  One operator that rewrites *trees*, not just elements

This is the headline feature.  In most languages, *map*,
*filter*, *reduce*, *substitute*, *parse*, and *destructure* are
six different tools.  In Symta they are six bodies of the same
construct — `{}` — and the construct is more powerful than any of
them on their own, because it can **rewrite consecutive runs of
elements at once**, not just single elements.  The technical
name for this is *term rewriting*, and Symta inherits it from
REFAL, the language Soviet computer scientists used in the 1970s
to do symbolic mathematics.

The basic forms first:

```symta
[1 2 3]{?+1}                  // (2 3 4)   -- map: ? is the current element
[-2 0 3 -1 5]{:?>0}           // (3 5)     -- filter: keep where ?>0 is true
[-2 0 3 -1 5]{?<<0=}          // (3 5)     -- drop where ?<=0; same effect
"hello"{l=\L}                 // "heLLo"   -- match the char l, replace with L
```

The form that makes Symta different from every other dynamic
language ships with the unassuming syntax `{A B = ...}`:

```symta
[1 2 3 4 5 6]{A B = A+B}          // (3 7 11)        sum of every pair
[a b c d e f]{X Y = Y X}          // (b a d c f e)   swap every pair
[1 2 3 4 5 6]{A B C = [A B C]}    // ((1 2 3) (4 5 6))  group every three

"the bad cat"{@\bad=\good}        // "the good cat"   multi-char replacement
```

`{A B = ...}` matches **two consecutive items** and replaces
them with whatever you write.  `{A B C = ...}` matches three.
The leftover tail (if the length isn't a multiple of the pattern)
passes through unchanged.  Bare lowercase characters in the
pattern match themselves; `\name`-prefixed atoms match their
spelling.

This is what people mean when they say Symta is a *list-processing
Lisp* — most "string mangling", "AST rewriting", "data
normalising" code is just a few of these clauses applied in
sequence.  The whole CSS pre-processor, the whole templating
engine, the whole "convert this awkward JSON to that other
awkward JSON" task is a one-screen pipeline.

The body inside `{}` composes with itself: you can put a `{}`
inside a `{}` inside a `{}`, each layer reading like a normal
sentence.  Template substitution from a hash table is one line:

```symta
Data name!\Nancy age!37 city!\Amsterdam
Form "Hi! My name is %name%, I'm %age% years old."
say Form{@"%[V]%"=Data.V}
//   Hi! My name is Nancy, I'm 37 years old.
```

`@"%[V]%"` matches the consecutive characters `%`, then any text
(captured into `V`), then `%`; the replacement is `Data.V`.  One
line of code replaces the entire library you'd use in Python,
Ruby, or PHP.

---

## A 60-second taste

```symta
// Forward and backward through the same shape:
Data name!\Nancy age!37 city!\Amsterdam
Form "Hi! My name is %name%, I'm %age% years old and I live in %city%."
say Form{@"%[V]%"=Data.V}
//   Hi! My name is Nancy, I'm 37 years old and I live in Amsterdam.


// Sum, max, min in three characters each:
say [3 1 4 1 5 9 2 6].z          // 31  -- sum
say [3 1 4 1 5 9 2 6].max        // 9
say [3 1 4 1 5 9 2 6].min        // 1


// Quicksort, one line:
qsort@r H,@T = @T{:?<H}^r, H, @T{?<H=}^r
say qsort(3,1,4,1,5,9,2,6,5,3)
//   (1 1 2 3 3 4 5 5 6 9)


// Frequency table from a string (auto-closure ~D collects counts):
say "hello world"{~D.?+}
//   @{w!1 ` `!1 h!1 l!3 d!1 o!2 e!1 r!1}


// Prolog-style inference engine in 11 lines.  Forward + backward
// chaining come for free because every fact is stored twice.
// (See examples/27-relational.s for the full runnable version.)
```

If any of that looks like noise: don't worry.  Below is the same
tour at human speed.

---

# Part I — The Basics

## Installation and your first program

A pre-built `symta.exe` ships in the repository (Windows x64).
On macOS / Linux, build from source:

```sh
make -f Makefile.osx       # or Makefile.w64 on Windows, Makefile.linux
```

The compiler is self-hosted, so it's a small make.  When it's
done, you have one binary that is the compiler, the runtime, and
the REPL.

Open a REPL:

```sh
./symta.exe
```

Run a one-file program:

```sh
echo 'say "Hello, World!"' > hello.s
./symta.exe -f hello.s
```

Compile a project directory to a redistributable binary:

```sh
mkdir -p myapp/src
echo 'say "Hello, World!"' > myapp/src/go.s
./symta.exe myapp           # produces myapp/go.exe
./myapp/go.exe              # -> Hello, World!
```

A "project" is any folder with a `src/go.s`.  Symta walks the
imports, compiles everything, and links a single executable that
needs nothing else at runtime.

## Saying things

```symta
say "Hello, World!"
say 42
say 3.14
say [1 2 3]
say name!\Nancy age!37
```

`say` prints anything, with a newline.  `say_` (with an
underscore) prints without one.  Symta's print is *introspective*:
lists print as lists, tables print with their keys, errors print
with their source location.  No more `print(json.dumps(obj))`.

Inside a double-quoted string, `[...]` interpolates:

```symta
Name \World
say "Hello, [Name]!"             // -> Hello, World!
say "[Name].length = [Name.n]"   // -> World.length = 5
```

Anything inside the brackets is an expression; the whole string
is rebuilt at runtime.

## Numbers and arithmetic

Two numeric types you'll meet day one: `int` (62-bit signed
integer — yes, sixty-two; the runtime uses two bits for type
tagging) and `float` (boxed IEEE-754 double).

```symta
2 + 3              // 5
10 / 3             // 3        integer division
10 / 3.0           // 3.33...  float division (mixed promotes)
10 % 3             // 1        modulo
(0-5).abs          // 5        method on int (note: `-5` alone parses oddly,
                   //          so write the negative as 0-5 in expressions)
min(3 7 2 8 1)     // 1
max(3 7 2 8 1)     // 8
```

Arithmetic is normal infix.  Function calls don't need parens when
the meaning is unambiguous: `min 3 7 2` is the same as
`min(3 7 2)`, the same as `min(3, 7, 2)`.  Use whichever reads
best.

There's no infix `^` for power — `^` in Symta means *apply on
the left*, so `3 ^ square` is `square(3)`.  Integer power is
`int.pow`; for floats use `Math.pow`.

Comparisons return `0` for false and `1` for true:

```symta
3 < 5              // 1
3 < 1              // 0
3 >< 3             // 1     -- `><` is structural equality, on anything
3 >< 5             // 0
"abc" >< "abc"     // 1
[1 2 3] >< [1 2 3] // 1
```

There is no separate `bool` type — `0` and `1` *are* the
booleans, and the only thing `if` / `when` / `and` / `or` / `not`
treat as false is the integer `0`.  Every other value — including
`No`, `[]`, atoms, lists, text, anything — is truthy.

## Variables — names that bind

```symta
X 42                // X is bound to 42
Y "hello"           // Y is "hello"
X = X + 1           // reassign X to 43
```

Two forms:

- `X EXPR` — binds `X` to the value of `EXPR`.  First time only.
- `X = EXPR` — reassigns `X`.  `X` must already exist in scope.

The distinction matters because Symta will tell you, at compile
time, if you tried to assign to a name that didn't exist.  This
single rule kills the most common typo bug in dynamic languages:

```symta
Conut 0                       // -- typo: should be `Count`
till Done:
  Count = Count + 1           // compile error: Count not bound
```

In Python that would silently create a global `Count` and never
touch the typo'd `Conut`.  In Symta it fails before the program
runs.

Variables start with an uppercase letter or end with `_`.
Functions and methods start with lowercase.  Atom literals
(symbols) start with `\`: `\red`, `\monday`, `\AI`.  The
distinction is syntactic, not enforced semantics — but every
Symta programmer reads `Count` and immediately thinks "that's a
variable, it can be reassigned".

---

# Part II — Functions

## Definition and call

A function is a name `=` followed by a body.  Arguments come
between the name and the `=`:

```symta
double X = X * 2
say: double 21                  // 42

add X Y = X + Y
say: add 2 3                    // 5
say: add(2, 3)                  // also 5
say: 3 ^ double                 // 6 -- "apply double on the left"
```

The last expression in the body is the return value.  No
`return` keyword needed for the normal case.

The `:` after `say` matters: `say` is variadic, so
`say double 21` would print *three* things — the symbol `double`,
the number `21`, and `say`'s own newline.  `say: double 21` says
"the rest of the line is one expression", so it prints the result
of `double 21`.

Method-call syntax `X.method(Y)` only works when `method` is
*defined on* the value's type — like `text.split` or `int.x`.
For a plain user-defined function you call it positionally:
`add 2 3`, never `2.add(3)`.

Bodies can be many lines.  Use indentation:

```symta
hypot A B =
  Asq A * A
  Bsq B * B
  (Asq + Bsq).float.sqrt

// Or:
hypot A B = (A*A + B*B).float.sqrt
```

The compiler doesn't care which one you write.  Pick whichever
the next person to read this file will thank you for.

## Lambdas

An anonymous function is `| Args => Body`.  Bind one to a name
with `&name`:

```symta
&square | X => X * X
say: square 9                   // 81

&dist | A B => (A*A + B*B).float.sqrt
say: dist 3 4                   // 5
```

Use `^` to "apply on the left" — a Forth-style postfix call:

```symta
say: 3 ^ square                 // 81
[1 2 3]^each{say "[?]"}         // print each on its own line
```

Higher-order functions take lambdas just like any other value:

```symta
[1 2 3 4 5].keep(X => X > 2)    // (3 4 5)
[10 20 30].map(X => X / 10)     // (1 2 3)
```

The bare `?` inside a `{}` body is shorthand for "the current
element", so `[10 20 30]{?/10}` is the same thing.

When the function you want is *already named*, use `&name` to
lift it into a lambda value — like C's address-of:

```symta
sq X = X * X
say [1 2 3 4]{&sq}              // (1 4 9 16)  -- map via &fn
say [1 2 3 4].map(&sq)          // same thing, via the method

is_positive X = X > 0
say [-2 1 0 3].keep(&is_positive)   // (1 3) -- filter via &fn
```

`{&fn}` is the cleanest way to map a named function over a list.
It replaces the longer `{? ^ fn}` or `(| X => fn X)` forms.
For filtering, use `.keep(&fn)`; for in-`{}` filtering the
shorter `{:Cond}` form is usually clearer than naming a
predicate.

## Multiple return values

A function can return a list and the caller can destructure it
on the spot:

```symta
divmod A B = [A/B A%B]

[Q R] divmod 17 5               // Q=3, R=2
say "Quotient [Q], remainder [R]"
```

The compiler sees `[Q R]` on the left of the binding and expects
a 2-element list on the right; it pulls them apart automatically.

---

# Part III — Lists

The single most important data type in Symta.

## Construction

```symta
[]                              // empty list
[1 2 3]                         // explicit literal
1,2,3                           // comma-separated, no brackets
Xs 5,6,7                        // bound to a variable
[:5]                            // (1 2 3 4 5) -- range 1..N
[1:5]                           // (1 2 3 4 5) -- explicit start..end
[0:10:2]                        // (0 2 4 6 8 10) -- range with step
[\a:\e]                         // (a b c d e) -- letter range
```

Symta uses *parenthesised* notation when *printing* lists,
because that's what fits naturally with how the language reads
them.  But you don't write `(1 2 3)` to build one — that's a
function call.  Use `[1 2 3]` or `1,2,3`.

## Indexing and slicing

```symta
Xs [10 20 30 40 50]

Xs[0]                            // 10  -- first
Xs[1]                            // 20
Xs[~]                            // 50  -- last; `~` means "the end"
Xs[:~]                           // (10 20 30 40)  -- "lead" (all but last)
Xs.n                             // 5   -- length

Xs[2:4]                          // (30 40)     -- slice [start:end]
Xs[:3]                           // (10 20 30)  -- first 3
Xs[3:]                           // (40 50)     -- drop first 3
Xs[0::2]                         // (10 30 50)  -- step 2
```

`~` inside `[...]` means "from the end".  It pairs with offsets:
`Xs[~+1:0:2]` walks the list backwards in step-2.  See
[`examples/29-subscript.s`](../examples/29-subscript.s) for the
full subscript catalogue.

## Iteration: the `{}` operator

```symta
// Map: apply a body to every element.  `?` is the current element.
[1 2 3]{?+1}                    // (2 3 4)
[1 2 3]{?*?}                    // (1 4 9)

// Filter: `:Cond` keeps elements where Cond is true.
[1 -2 3 -4 5]{:?>0}             // (1 3 5)

// "Drop where" -- the inverse, written as `Cond=`.
//  Replaces every element that matches Cond with nothing.
[1 -2 3 -4 5]{?<<0=}            // (1 3 5)  -- ?<<0 means ?<=0

// Both element and index, using the indexed form:
[10 20 30].i{[I X] = "[I]:[X]"} // ("0:10" "1:20" "2:30")
```

The pattern on the left of `=` matches the input; the
expression on the right is what comes out.  Drop the `= EXPR` to
get plain filtering; drop the pattern to use `?`.

## REFAL-style group rewriting

Here is where `{}` outgrows its sibling languages.  A pattern
with **several names** on the left matches **several consecutive
elements** and replaces them with the right-hand side:

```symta
[1 2 3 4 5 6]{A B = A+B}        // (3 7 11)        -- pairwise sum
[a b c d e f]{X Y = Y X}        // (b a d c f e)   -- pairwise swap
[1 2 3 4 5 6]{A B C = [A B C]}  // ((1 2 3) (4 5 6))  -- group by 3
[1 2 3 4 5 6 7]{A B = A+B}      // (3 7 11 7)      -- dangling tail passes
```

The right-hand side can also *splice* — `@X` inside the
replacement expands a list back into the surrounding list,
making `{}` a **length-changing** operation, not just a
"replace every N with N":

```symta
[[1 2 3] [4 5] [a b c d]]{X = @X}    // (1 2 3 4 5 a b c d)
//   -- flatten one level
```

That last example is **seven characters of body**.  The same
operation in other dynamic languages:

```python
# Python
[x for sub in L for x in sub]            # 27 chars, two-loop comprehension
# or:
import itertools; list(itertools.chain.from_iterable(L))   # 49 chars
```

```javascript
// JavaScript
L.flat()                                  // OK -- only after ES2019
// or:
L.reduce((a, b) => a.concat(b), [])
```

```lua
-- Lua
local r = {}
for _, sub in ipairs(L) do
  for _, x in ipairs(sub) do r[#r+1] = x end
end                                       -- four lines, two locals
```

This is *term rewriting* — the same trick REFAL gave the world
in 1968 and that has reappeared in TXL, Stratego, and OMeta.
You can use it to dispatch on the **shape of a run of elements**,
not just on a single value.  Pulling pairs of `(key, value)` out
of a flattened list, normalising chunks of an AST, collapsing
runs of whitespace, flattening or un-flattening trees, splitting
on a delimiter pattern — every one of these is two characters of
pattern and one expression of body.

This is what people mean when they say a language is a
*list-processing Lisp*.  The standard library functions you'd
reach for in another language (`flatMap`, `groupBy`, `partition`,
`chunks_of`, `windows`, `zip_with`) collapse into one-liners
right here in the syntax.

### AST / XML rewriting: `=:` and bare `@`

Two tiny sugars make `{}` into a serious tool for transforming
trees of data — compiler intermediate representations, XML and
JSON document trees, configuration formats, anything with a
recursive shape.

**Bare `@` in a pattern** is shorthand for `@_` — "match the
rest, throw it away".  So `[A B@]` means "a list with at least
two items; bind A and B to the first two and discard whatever
follows":

```symta
[[1 2 3] [4 5] [a b c d]]{[A B@] =: B A}
//   ((2 1) (5 4) (b a))
```

The first two elements get bound, the rest get dropped, then the
output is `[B A]`.

**`=:` on the right-hand side** is shorthand for `= [...]` —
"wrap the listed items into a fresh list as the replacement".
It removes the bracket noise that would otherwise clutter every
tree-rewrite rule.  Compare:

```symta
// Without the sugars, an AST rewrite reads like Lisp:
AST{[\plus A B] = [\sum A B]}

// With them, it reads like a rewrite rule should:
AST{[\plus A B] =: \sum A B}
```

A small example — rename `plus` and `mul` nodes inside a list of
expression trees:

```symta
Asts [[plus 1 2] [mul 3 4] [plus 5 6]]
say Asts{[\plus A B] =: \sum     A B}    // ((sum 1 2) (mul 3 4) (sum 5 6))
say Asts{[\mul  A B] =: \product A B}    // ((plus 1 2) (product 3 4) (plus 5 6))
```

Pipe several such rewrites together and you have an AST
transform pass.  Recurse into sub-lists (via `@?`) and you have
a *whole-tree* transform pass.  Whole compiler back-ends fit in
under fifty lines this way; the Symta compiler itself uses
exactly this pattern for several of its own simplification
passes.

The three knobs together — pattern, splice-in (`@X`), and
splice-out (`=: ...` or `= @[...]`) — are why a language with
six lines of grammar can express what AWK, sed, jq, XSLT, and
half of LINQ would need three different libraries for.

## Reduction

Symta's standard list methods cover most reductions you'd reach
for a fold for:

```symta
[1 2 3 4 5].z                   // 15  -- sum
[3 7 2 8 1].max                 // 8
[3 7 2 8 1].min                 // 1
[5 1 4 2].s                     // (1 2 4 5)  -- sort
[a b a c b].uniq                // (a b c)
```

For explicit folds, use `.fold InitVal Fn`:

```symta
[1 2 3 4 5].fold 100 (| A B => A+B)   // 115
```

Or use an auto-closure variable, which Symta makes especially
ergonomic:

```symta
S{~D.?+}                        // freq table -- D is built up as we go
"hello world"{~D.?+}            // @{h!1 e!1 l!3 o!2 ...}
```

`~D` introduces an auto-closure table named `D`, and `.?+` does
"look up the current character's count, post-increment".  The
final value of `D` is the result of the `{}` expression.

## Replacement and parsing — the magic trick

```symta
// Single character substitution in text:
"hello"{l=\L}                    // "heLLo"

// Multi-character substitution -- `@\bad` splices the chars b,a,d
// into the pattern:
"Now is a bad time, really bad!"{@\bad=\good}
//   "Now is a good time, really good!"

// Replace every name in a list of atoms:
[\alice \bob \carol]{\bob=\BOB}  // (alice BOB carol)

// Parse the values BACK OUT of a string, using `=:` to bind:
"Hi! My name is Nancy, I'm 37 years old."("Hi! My name is [N], I'm [A] years old." =: N A)
//   binds N="Nancy", A="37"
```

The `=` inside `{}` is *bidirectional*.  Forwards it substitutes;
the `=:` form on the right of a parse extracts.  This is what
makes the FizzBuzz one-liner readable: the body of `{}` is a tiny
pattern language that you happen to be using to define a numeric
rule.

---

# Part IV — Control Flow

## Conditionals

```symta
if X > 0: say "positive"
if X > 0 then say "positive" else say "not positive"

// Multi-branch -- note the `:` after the scrutinee:
classify X = case X:
  0       = "zero"
  1+2+3   = "one, two, or three"      // `+` is OR in patterns
  N<int?  = "some int [N]"            // `<int?` is a type-predicate guard
  Else    = "other: [Else]"

// Just want to run a block when something is true:
when Has_files:
  say "[Files.n] files found"
  process Files

// Inverse:
less X.is_int: bad "Expected an integer, got [X]"
```

`case` matches by pattern, top to bottom.  `Else` catches
anything left.  `N<int?` reads "name this match `N`, and only
match if it satisfies the predicate `int?`."

### Truthiness — and why `No` is not "false"

Symta has no `bool` type.  The integer `0` is the only thing
that `if`, `when`, `less`, `and`, `or`, and `not` treat as
false; *every other value*, including `No`, the empty list, and
atoms, is truthy.

`No` is something else entirely.  It's the "no value" marker —
the value you get back from a missing hashtable key, an SQL
column that holds `NULL`, the unset slot of a record, the start
value of an accumulator before you know whether you're summing
ints or floats.  In short, it's *the absence of a value*, not
"false".  Symta's `No` plays the same role `NULL` plays in SQL
or `None` plays in a Pandas dataframe — but never the role
`false` plays.

What this buys you:

- **`No` is the additive identity.**  `No + 7` is `7`, `5 + No`
  is `5`.  An accumulator initialised to `No` doesn't care
  whether the first value it sees is an int or a float; it
  picks up whatever type arrives first.  This is why
  `[1 2 3].z` works on any numeric type without you having to
  specify a starting zero — the runtime uses `No`.

- **Missing-key lookups don't crash.**  `T.unknown` returns
  `No`, not an exception.  Optional navigation is the default;
  the burden of "did I get a value?" is on the caller, not on
  every read site.

- **`got X` is the proper "X is a value" test.**  It returns
  `1` for everything except `No`.  Use `got X` when you want
  "is this populated", not `if X`.

- **`No` is *truthy* under `if`.**  This is the foot-gun.
  `if T.missing: say "got it"` will print "got it", because
  `No <> 0`.  Use `if got T.missing: ...` or `when got X: ...`
  instead.  The same applies to `while`, `until`, and the
  short-circuit operators.

If you've used SQL: `No` is `NULL`, `0` is `false`.  You
wouldn't write `WHERE col`; you'd write `WHERE col IS NOT NULL`
or `WHERE col = 0`.  Symta is the same — just spelled
`got X` and `not X`.

## Loops

```symta
// Range loop:
for I in :10:
  say "i = [I]"

// Range with explicit limits:
for I in 1:100:
  say I

// Loop over a list:
for Name in [\alice \bob \carol]:
  say "Hello, [Name]"

// Loop forever until a condition is set:
Done 0
till Done:
  // ... body that eventually sets Done = 1 ...

// While-style:
while X.has_more:
  X = X.advance

// Just iterate, no index:
xs^each{ ... }
```

Indentation is significant; the body of the loop is everything
indented further than the `for`.  Tabs and spaces both work, as
long as you're consistent within a function.

---

# Part V — Text

Strings are sequences of characters, immutable, UTF-8 internally.

```symta
S "hello world"

S.n                              // 11               length
S[0]                             // h                first char
S[~]                             // d                last char
S.split(" ")                     // (hello world)    split on a delimiter
S{l=\L}                          // "heLLo worLd"    replace every `l` with L
S{@\hello=\HELLO}                // "HELLO world"    multi-char replacement

S("[A] [B]" =: A B)              // A="hello", B="world"  -- parse extract
```

Interpolation works inside double quotes:

```symta
N 5
say "I have [N] apples"          // "I have 5 apples"
say "next: [N+1]"                // "next: 6"
say "list: [1,2,3]"              // "list: (1 2 3)"
```

Use single quotes for the rare case you want a literal string
with no interpolation: `'no $1 here'` — though you'll also see
backslash-escapes in double quotes: `"no \[bracket\] here"`.

---

# Part VI — Tables (Hash Maps)

Tables map keys to values.  Keys can be any value.

```symta
// Literal:
T name!\Nancy age!37 city!\Amsterdam

// Access:
T.name                           // \Nancy
T.age                            // 37
T.\city                          // \Amsterdam  -- atom-keyed access

// Update (returns a new table; tables are persistent):
T.age = 38

// Bulk-build from rows:
People @t: rowz:
  name age city
  \alice  30  \utrecht
  \bob    25  \rotterdam

People.alice.age                 // 30
```

Tables print readably: `say T` gives you back the literal you
typed.

---

# Part VII — Pattern Matching

The single most expressive form in Symta.  Pattern matching is
how you destructure data, how `case` decides which branch to
take, and how `{}` knows what to map over.

```symta
// Bind first and rest:
H,@T 1,2,3,4,5
// H=1, T=(2 3 4 5)

// Bind first two and rest:
[A B @Rest] 1,2,3,4,5
// A=1, B=2, Rest=(3 4 5)

// First two and last:
[A B @_ Z] 1,2,3,4,5
// A=1, B=2, Z=5  (underscore = "I don't care")

// Match a specific value in position:
[\start @Args] L                 // succeeds only if L starts with \start

// Constraint:
[N<1.is_int @_] L                // first element must be int; bind it as N
```

Patterns chain.  Inside a `case` or a function head:

```symta
length L = case L
  []     | 0
  H,@T   | 1 + length T

// Or, more compactly:
length: []=0; H,@T = 1 + length T
```

That last line is one expression containing two pattern
clauses.  Symta picks the first that matches the input.

### The guard syntax

`<` after a name in a pattern adds a constraint:

```symta
classify N = case N
  X<0      | "negative"
  0        | "zero"
  X<int?   | "positive int"
  X<float? | "positive float"
  _        | "other"
```

`<` reads as "such that".  `X<int?` is "bind X, such that X is
an integer."

---

# Part VIII — Objects and ECS

Symta's object system is *not* class-hierarchy OOP.  It's an
Entity-Component-System, which is a fancy name for "everything
is a row in a small relational database".  The headline syntax,
though, looks like classes:

```symta
cls point x y

P new x,3 y,4
say P.x                          // 3
say P.y                          // 4
P.x = 10                         // P is now {x=10, y=4}
```

Behind the scenes, "make a point" attaches an `x` component and
a `y` component to a fresh entity id.  Querying `P.x` is a hash
lookup keyed on that id.

The benefit: an entity can pick up extra components at runtime
without being declared as a subclass of anything.

```symta
P.color = \red                   // P now also has a color
P.color                          // \red
P.weight                         // No -- entity doesn't have a weight
```

And you can iterate by component:

```symta
// Every entity that has a `color`:
color^each{?, ?.color}
```

This is the design Symta is built around.  The full essay on
why is in [`cls.md`](cls.md) — it's worth reading once you have
something with more than a hundred entities of more than three
types.

---

# Part IX — The type system

Symta is dynamically typed by default, but the type system can
be *progressively* engaged on the parts of a program that
benefit from it.  The same surface vocabulary covers four
things — assertion, ascription, construction, and conversion —
and the static checker catches whatever it can prove at compile
time, falling back to a runtime check for everything else.

The shape of the surface is **typename-is-a-function**.  A
type name like `int`, `text`, or your own `account` is just a
function you can call.  The action depends on how you call it.

### Assertion: `_the T X` and `X^T`

"This value is already a `T`; if it isn't, raise an error."
No conversion is attempted.

```symta
A 5^int                          // A = 5
B "hello"^text                   // B = "hello"
F (=> 42)                        // a thunk that returns dyn
caught btrap: => F()^text        // runtime check fails -> bterror
```

`^T` is the postfix form of `_the T`.  Both lower to the same
runtime tag check.  Use whichever reads better at the call site.

### Ascription on declarations and parameters

The really useful place for `^T` is on the *boundary* of a
function or a binding — once a value is asserted to be a `T`,
the compiler knows it's a `T` from then on.

```symta
// Typed parameters: runtime check at entry, static type in body
deposit Acc^account Amt^money =
  // Inside here, Acc and Amt are KNOWN to be their types.
  // Acc.bal reads the `bal` field of an account; the compiler
  // verified Acc is an account when the function was entered.
  NewBal add Acc.bal Amt
  account Acc.id Acc.name NewBal

// Typed local binding
Balance 0^int                    // pinned -- can only be reassigned to int
Balance = 42                     // OK
// Balance = "broke"             // COMPILE ERROR: type mismatch on reassign
```

### Construction: `T X`

"Make a `T` out of `X`."  Calls `X.T` (the per-type conversion
method) then asserts the result is a `T`.

```symta
int 3.5                          // 3       -- 3.5.int truncates
int "42"                         // 42      -- parsed
float 7                          // 7.0
text 42                          // "42"
```

If the conversion fails, the runtime check trips.  No silent
"NaN" or "undefined" result.

### Pure conversion: `as_T X`

"Run the conversion, skip the check."  Faster, no safety net.

```symta
as_int 3.5                       // 3
as_list "abc"                    // (a b c)  -- text -> char-list
```

Use when you've already proven the input shape and want to
skip the redundant check on the hot path.

### Trust me: `_unsafe T X`

"I promise this is a `T`; don't check, just label it."  C-style
trust cast.  Undefined behaviour if you lie.  Use only when the
strict checker is provably wrong about a path it can't see
through.

```symta
U _unsafe int 9                  // skip both static and runtime
```

### Declaring your own types

`type` is just like `cls` from the previous part, but the
result also participates in the type system:

```symta
type money Cents Curr: cents!Cents currency!Curr
type account Id Name Bal: id!Id name!Name bal!Bal

M money 12500 \USD               // M is a money
A account 1 "Alice" M            // A is an account whose bal is M

A.bal.cents                      // 12500     normal field access
A^account                        // verified -- still A
```

`is_money X`, `is_account X` get auto-generated.  `_the money X`
and `X^money` work on user types exactly like primitives.

### The strict checker

The compiler tries to prove type mismatches before any runtime
check has to fire.  Anything it can prove statically becomes a
*compile error*, not a runtime trap.

```symta
_the int "abc"                   // COMPILE ERROR -- text != int
_the float 3                     // COMPILE ERROR -- int != float
```

Anything it can't prove falls through to the runtime check
(`_the` and `^T` both emit one).  `_unsafe` opts out of both.

What the checker proves today:

| Form                          | Example                                            |
|-------------------------------|----------------------------------------------------|
| Literal mismatch              | `_the int "x"` → text                              |
| Var-flow propagation          | `X int 5; _the text X` → int                       |
| Typed parameter               | `f X^int = _the text X` → int                      |
| Reassignment of typed var     | `X _the int 5; X = "hi"` → text vs int             |
| `if`-branch unification       | `_the int (if c then 5 else "x")` → mixed          |
| Function-return inference     | `f X = _the int X+1`; `_the text (f 3)` → int      |
| Case-arm narrowing            | `case L T?: body` types L as T over body           |
| Splat-tail narrowing          | `case L [X@Xs]: body` types Xs as list             |
| Typed element pattern         | `case L [X^int @Xs]: body` types X as int          |
| Fixed-length list pattern     | `case L [A B C]: body` types L as list             |
| Case-result unification       | All arms agree on T → case-expr has type T         |
| Method-return inference       | `X.int`, `X.text`, `X.list` known by name          |
| Arithmetic + comparisons      | `int+int=int`, `int<int=int`, etc.                 |

`dyn` is the escape hatch: anything the checker can't determine
is `dyn`, and `dyn` unifies with everything (the gradual typing
contract).  Falling back to runtime is always available.

### Worked example: a typed bank-account library

`examples/40-bank.s` (in this repo) puts the pieces together:

```symta
type money Cents Curr: cents!Cents currency!Curr
type account Id Name Bal: id!Id name!Name bal!Bal

add A^money B^money =
  less A.currency >< B.currency:
    bad "currency mismatch: [A.currency] vs [B.currency]"
  money A.cents+B.cents A.currency

deposit Acc^account Amt^money =
  NewBal add Acc.bal Amt
  account Acc.id Acc.name NewBal

withdraw Acc^account Amt^money =
  when money_lt Acc.bal Amt:
    bad "insufficient: [Acc.bal.cents] < [Amt.cents]"
  NewBal sub Acc.bal Amt
  account Acc.id Acc.name NewBal

Alice account 1 "Alice" (money 50000 \USD)
Bob   account 2 "Bob"   (money 30000 \USD)

Alice = deposit Alice (money 12500 \USD)
```

The type system enforces the invariants you actually want:

* `Acc^account` is a runtime tag check at entry — call
  `deposit "savings"` and it traps before touching the balance.
* Currency-mix gets caught (`add Alice.bal EUR_amt` -> `bad`).
* `_the int "fifty cents"` somewhere in the source is a
  *compile error*; the digit-typo doesn't survive to runtime.

The full demo + boundary catches + audit log is one screen,
runnable as `symta -f examples/40-bank.s`.

---

# Part X — Macros and Quasiquotation

Symta is a Lisp.  Every program is data, and every data shape
that names something can be transformed by a macro.  Macros are
defined in a separate `.s` file and `export`ed; their bodies are
plain Symta that runs at *compile* time, receives the arguments
as unevaluated AST, and returns new AST.

A first example.  This macro evaluates to a literal at compile
time:

```symta
// In mymac.s:
export 'pi'
pi K = K * 3.14159265
```

```symta
// In go.s:
use mymac
say "pi(2.0) = [pi 2.0]"        // compiler computes 6.2831853 here
```

A macro that needs to *return code*, not just a value, uses the
`form:` body and `~name` for gensyms:

```symta
// In mymac.s:
export 'swap'
swap A B = form:
  ~T A                            // ~T is a fresh, unique name per use
  A = B
  B = ~T
```

```symta
// In go.s:
use mymac
A 1; B 2
swap A B
say [A B]                       // (2 1)
```

Splice runtime values back in with `$Expr`; splice lists with
`$@List`:

```symta
// Unroll a fixed-count loop at compile time:
repeat N Body =
  Copies map I [:N] Body
  form: `|` $@Copies              // `|` chains multiple statements
```

The killer use of macros in practice is *embedded DSLs* — a few
lines of macro define a syntax for SQL queries, graphics
primitives, parser combinators, or state machines, that looks
like part of the language.

If you have ever bounced off Lisp because "code is data" felt
abstract: Symta's macros are deliberately friendlier than
Common Lisp's.  Indentation is preserved; error messages are
specific; the macro and the expanded code both have source
positions, so a stack trace lands on a real line in your
program.

---

# Part XI — Modules and Building

Every `.s` file is a module.  Imports look like:

```symta
use math                         // imports `math` from runtime
use my_helpers                   // imports a sibling .s file

say sin(1.0)
say my_helpers_function(42)
```

The `use` is resolved against, in order:

1.  Sibling files in the same `src/` directory
2.  Files under `pkg/<name>/src/`
3.  Built-in modules shipping with the runtime

Names you intend to expose from your module are declared in the
top-line `export`:

```symta
export hello_world greet
hello_world = "Hello, World!"
greet Name = "Hi, [Name]"
private_helper = 42              // visible only inside this module
```

Build a project:

```sh
symta myproj                     # produces myproj/go.exe
```

A project is a directory with a `src/go.s`.  Symta walks the
imports and emits one statically-linked executable.

---

# Part XII — Error Handling

`bad` raises a runtime error with a message:

```symta
divide A B =
  less B: bad "division by zero"
  A / B
```

Catch with `btrap`:

```symta
Result btrap: divide(10, 0)
if Result.is_bterror:
  say "got error: [Result.text]"
else
  say Result
```

A `bterror` is just a value — you don't unwind the stack
mid-expression unless you ask to with `btjump`.  That makes
error-aware code something you can think about locally:

```symta
[10 5 0 2]{divide 10 ?}.btrap
```

Each `divide` either returns a value or a `bterror`.  The list
that comes out is `(1 2 BTERR 5)` with the error sitting in its
own slot; no exception breaks the iteration.

---

# Part XIII — Foreign Function Interface

Symta calls into native code without an external build step.
Declare the C signature in the source:

```symta
ffi_begin local zlib
ffi zlibVersion.text                              // const char *zlibVersion(void)
ffi crc32.u4 Crc.u4 Buf.ptr Len.u4                // uLong crc32(uLong, ptr, uInt)
ffi compress.int Dst.ptr DstLen.ptr Src.ptr SrcLen.u4

say zlibVersion                                   // "1.2.13"
```

The runtime resolves the symbol at module load, generates a
trampoline, and you call it like an ordinary Symta function.
The `.type` suffix declares the C type for each parameter and
the return: `.text`, `.int`, `.u4`, `.ptr`, `.float`, `.double`,
`.void`.

For richer cases — SDL2 windows, audio, FreeType text rendering
— Symta ships pre-built `.ffi` plugin blobs.  `use uim` brings
in the UI toolkit; `use gfx` brings in the graphics layer.

---

# Part XIV — Getting help

Symta has a self-documenting REPL.  At the prompt:

```
> help
help                — print this message
help <symbol>       — show documentation for <symbol>
exit / quit         — leave the REPL

> help sum
sum
  Sum a list of numbers.

  [1 2 3 4 5].sum  // -> 15

  Empty list returns 0.  Mixed int/float promotes to float.

> help case
case
  Multi-branch pattern matching.

  case Expr
    PatA | branch_a
    PatB | branch_b
    Else | default

  ...
```

Documentation is attached to source declarations with
`help_set`:

```symta
help_set \sum 'Sum a list of numbers.
Empty list returns 0; mixed int/float promotes to float.
Example: [1 2 3].sum -> 6'

sum L = L{? => ?+R}
```

At REPL time, `help \sum` reads it back.  Documentation lives
*in* the source code where it belongs, so it never goes stale.

```symta
help                            // print general usage
help say                        // show docs for `say` -- no quoting needed
help list.keep                  // method docs -- dot form also unquoted
help_names()                    // list every documented symbol
module_exports core_            // list every export of the core module
module_help    core_            // same, with a one-line doc next to each
```

`help` is a macro: it picks up the AST shape of its argument
(plain name, dot-form, etc.) and looks up the matching key.  No
backslash, no quotes — just type the name.  The whole standard
library — including the `{}` operator helpers — comes with docs
out of the box.  Try `help text.parse`, `help when`, `help got`,
or `module_help macro` at the REPL.

Two refinements are on the milestone roadmap:

1.  **A `doc` macro** so the docstring can be written
    immediately *before* a definition without repeating the
    name:
    ```symta
    doc 'Sum a list of numbers.'
    sum L = L{? => ?+R}
    ```
    The macro will pair the string with the next top-level
    definition and route it to `help_set` for you.  Until it
    lands, write `help_set \sum '...'` explicitly above the
    definition; the runtime API is the same.

2.  **A dedicated SBC docstring section** sitting beside the
    existing line-number side-table.  Once that lands, docs
    are loaded lazily — only the offset table is read at
    module load.  External tooling can inspect docs without
    instantiating the runtime.

Both refinements are non-breaking — the
`help` / `help_set` / `help_names` API stays the same.

The stdlib reference that previous versions of this document
tried to inline is gone deliberately.  Use `help`.

---

# Where to go next

You now have enough Symta to be dangerous.  The most useful
follow-ups, in order:

1.  **`examples/`** — Twenty-six progressively richer programs.
    Run a single-file example with `symta -f examples/NN-*.s`;
    build a project example with `symta examples/NN-name &&
    examples/NN-name/go.exe`.

2.  **`AI.md`** — A tight one-page cheat sheet meant to be
    pasted into an LLM's context window so it can help you write
    Symta.  Doubles as a quick-reference for humans.

3.  **`dev/cls.md`** — The design essay for the ECS, the `cls` /
    `dsm` / IPS macros, and the GC integration.  Worth reading
    when your program grows past a few hundred entities.

4.  **`architecture.md`** — How the compiler, runtime, and FFI
    actually work.  Read this when you want to contribute or
    when you want to embed Symta in something larger.

5.  **The `examples/27-relational.s` Prolog-style inference
    engine** — about 100 lines of Symta that does what a small
    chunk of a logic programming language does.  A good test
    of whether you've internalised the patterns from this
    document.

If any chapter felt unclear, file an issue at
`https://github.com/NancySadkov/som` — concrete suggestions
("Chapter VII would be clearer if it covered X first") land
faster than abstract ones.

Happy hacking.

---

*Symta is dual-licensed under MIT OR Apache-2.0.*
