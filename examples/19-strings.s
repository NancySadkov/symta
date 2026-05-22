// 19-strings.s -- text processing toolkit
//
// Symta's text type behaves like a list of characters, so most
// list methods (`.n`, `.head`, `.tail`, `.take`, `.drop`, `.f`,
// `.split`, `.text`) work on it. The `{}` map operator does
// substring replacement and per-character transformations.
// Pattern-based parsing uses the same syntax as list destructuring.
//
// Run:  symta -f examples/19-strings.s


// ----------------------------------------------------------------
// 1. Basic indexing and slicing.
// ----------------------------------------------------------------
S "Hello, World!"
say "length:  [S.n]"
say "first:   [S.head]"
T1 S.take 5
say "first 5: [T1]"
T2 S.drop 7
say "drop 7:  [T2]"
say ""


// ----------------------------------------------------------------
// 2. Splitting / joining. Lists of text join via `.text(infix)`.
// ----------------------------------------------------------------
Path "/usr/local/bin/symta"
Parts Path.split^'/'
say "split: [Parts]"
say "joined back with ': ' = [Parts.text(': ')]"

// `.url` decomposes a path into (folder name extension)
[Dir Name Ext] "src/main/foo.png".url
say "dir=[Dir]  name=[Name]  ext=[Ext]"
say ""


// ----------------------------------------------------------------
// 3. The {} map operator on text -- search and replace.
// ----------------------------------------------------------------
Sentence "Now is a bad time to live, really bad!"
say "[Sentence{@'bad'=@'good'}]"

// per-character: replace digits with `X`
P "phone 555-1234"
say "censor digits: [P{d?=\X}]"
say ""


// ----------------------------------------------------------------
// 4. Parsing structured strings via destructuring.
//    The same `[A B C]` syntax that destructures lists also works
//    on text when given a template; named holes are bound.
// ----------------------------------------------------------------
Profile "Hi! My name is Nancy, I'm 37 years old."
[N A] Profile("Hi! My name is [N], I'm [A] years old." =: N A)
say "parsed: name=[N], age=[A]"

// CSV row
Row "alice,30,engineer"
[Name AgeT Job] Row.split^','
say "csv: name=[Name] age=[AgeT] job=[Job]"

// Convert the captured age text to int (split^ returns text fields).
Age AgeT.int
say "age + 1 = [Age+1]"
say ""


// ----------------------------------------------------------------
// 5. Title-casing -- the canonical "humanize an identifier" idiom
//    seen all over the standard library: split, capitalize each
//    piece, join with a space.
// ----------------------------------------------------------------
Ident "user_full_name"
Human Ident.split^'_'{?title}.text(' ')
say "[Ident] -> [Human]"
say ""


// ----------------------------------------------------------------
// 6. Reading a text file. `.lines` returns a list of text lines.
//    The README that ships with this repo is a convenient subject,
//    but its line count drifts as docs evolve; print only the
//    first 3 lines so the test stays stable across doc edits.
// ----------------------------------------------------------------
F "README.md"
when F.exists:
  L F.lines
  say "README, first 3 lines:"
  for Line L[:3]: say "  | [Line]"
say ""


// ----------------------------------------------------------------
// 7. utf-8 round-trip. `text.utf8` -> bytes; `bytes.utf8` -> text.
// ----------------------------------------------------------------
G "Wave: hi!"
B G.utf8
say "bytes ([B.n]): [B.l]"
say "back:    [B.utf8]"
say ""


// ----------------------------------------------------------------
// 8. Text multiplication + nested string interpolation.
//    `text * N` repeats N times.  String interpolation `[...]`
//    can contain arbitrary expressions including nested `"..."`,
//    so building structured output like a 10x10 matrix is a one-
//    liner -- no loop, no buffer, no separator bookkeeping.
// ----------------------------------------------------------------
M ("[("0 "*10)]\n") * 10
say "10x10 zeros ([M.n] chars):"
say M

// A row of `=` of any width: also `text * N`.
say "[("=" * 30)]"
say "separator bar -- 30 chars"
