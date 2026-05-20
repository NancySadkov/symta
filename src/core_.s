export say bad new_macro meta methods typeof subtype_of
       rand_get rand_set rand_push rand_pop zip setters_ getters_ atan2
       btland btjump bterror btrap `..`

_.new_fn_ = No

_.free =


//No acts as an identity element in arithmetics
no.`+` B = B
no.`-` B = B
no.`*` B = B
no.`++` = 1
no.`--` = -1

float.`++` = Me + 1.0
float.`--` = Me - 1.0


no.'<' B =
  if B.is_int: ret B < 0
  0
no.'>' B =
  if B.is_int: ret B > 0
  0
no.`<<` B =
  if B.is_int: ret B << 0
  0
no.`>>` B =
  if B.is_int: ret B >> 0
  0

_.bool = 1
int.bool = if Me: 1 else 0
float.bool = Me <> 0.0
text.bool =
  @"0 for the empty text, 1 otherwise."
  if Me.end: 0 else 1
no.bool = 0

text.`+` B = "[Me][B]"

_._ = [Me]
list._ = Me
_list_._ = Me
_empty_._ = Me
hard_list._ = Me

// `_.><`/`_.<>`/`_.<<`/`_.>`/`_.>>` all come from runtime/bltin.c
// as direct C-level raw-dyn comparators -- 1 builtin call (~50 ns)
// instead of the chained Symta-side `same Me B` / `not Me >< B`
// (~250 ns).  Type-specific overrides (int.<, text.<, list.<, ...)
// still win via METHOD_FN dispatch before falling here.

_.is_int = 0
int.is_int = 1

_.is_float = 0
float.is_float = 1

_.is_fn = 0
fn.is_fn = 1

_.is_list = 0
list.is_list = 1

_.is_text = 0
text.is_text = 1

_.is_fixtext = 0
_fixtext_.is_fixtext = 1

//_.is_hard_list = 0
//list.is_hard_list = 1

_.is_table = 0

_.copy = Me
list.copy =
  @"Shallow copy of a list -- elements are shared."
  map X Me X

_.deep_copy = Me
list.deep_copy =
  @"Recursive deep copy of a list.  Nested lists are copied too."
  map X Me X.deep_copy

methods Object =
  @"Return the table of methods defined for the given value.
Example:  methods 42          // table of int methods"
  Object^methods_.t

// TS-2: type-system introspection.  `typeof X` is the design's
// chosen name for "what type is this value"; today it aliases
// the existing `typename` builtin which returns the runtime
// sname -- already normalised so that the four concrete list
// tags (T_LIST | T_VIEW | T_CONS | T_BYTES) all return "list"
// and the two text tags (T_TEXT | T_FIXTEXT) return "text",
// matching the multi-tag union semantics the type system uses.
//
// `subtype_of X T` is a type-equality check at the `typename`
// granularity (i.e. T_LIST and T_CONS both subtype-of `list`).
// Inheritance walk along the runtime parent chain (so e.g.
// `subtype_of P object` where P is an int returns 1) is a
// TS-2.1 follow-up that needs a runtime primitive exposing
// types[].super.
//
// T may be a text ("int") or a tag-symbol (\int) -- both compare
// equal to typename's text result via the existing `><` overload.
typeof X =
  @"Return the type name of X as text.  Alias for `typename` --
the type-system surface uses `typeof` as the canonical verb."
  typename X

subtype_of X T =
  @"Return 1 if X's type is T or T is an ancestor of X's
type.  T may be a text or tag-symbol naming a primitive
or a user-defined type.  Walks the runtime super chain
via parents_of_ -- the list starts with X's own type-name
then climbs through each ancestor up to the root."
  Ps parents_of_ X
  got Ps.find(? >< T)

_.`()` A = A.Me
// RT-9: fn.`()` is now a C-side BUILTIN_VARARGS in runtime/bltin.c
// (b_fn_call) -- avoids the @As collection that previously made it
// the top emitter of size-1 LIST allocations.

_.`{}` F = $map(F)

int.sign =
  @"-1 for negative, 0 for zero, 1 for positive."
  if Me < 0 then -1
  else if Me > 0 then 1
  else 0

float.sign =
  @"Same as int.sign but returns floats."
  if Me < 0.0 then -1.0
  else if Me > 0.0 then 1.0
  else 0.0

list.sign =
  @"Elementwise sign on a numeric list (each element -> -1, 0, or 1)."
  map X Me X.sign

int.abs =
  @"Absolute value of an integer.  Note: `abs(-5)` does NOT work because
`(-5)` is parsed as a function call.  Use `(0-5).abs` instead."
  _abs Me

float.abs =
  @"Absolute value of a float."
  _abs Me

float.log2 = $log/2.0.log

list.metric =
  R 0.0
  map X Me: R += @float X*X
  R

list.abs =
  R 0.0
  map X Me: R += @float X*X
  R.sqrt

list.normalize = Me / $abs

list.orth = [$1 -$0].normalize //perpendicular vector to a given

list.loop I =
  S $n
  if I<0 then Me.(S - I%S) else Me.(I%S)

list.neg = dup I $n -$I
list.`+` Ys = dup I $n $I+Ys.I
list.`-` Ys = dup I $n $I-Ys.I
list.`*` Ys =
   if Ys.is_list: dup I $n $I*Ys.I
   else map X Me: X*Ys
list.`/` A = map X Me: X/A
list.`%` A = map X Me: X%A
list.float =
  @"Convert every element of a numeric list to float."
  map X Me: X.float
list.int =
  @"Convert every element of a numeric list to int."
  map X Me: X.int
list.round = Me{?round}

zip Xs Ys =
  @"Element-wise pair two lists into a list of 2-element lists.
Example:  zip \[1 2 3\] \[a b c\]   // ((1 a) (2 b) (3 c))
The result is truncated to the length of the shorter input."
  Xs Xs.l
  Ys Ys.l
  Sz min Xs.n Ys.n
  dup I Sz: Xs.I,Ys.I

list.zip =
  @"Transpose a list of equal-length lists.
Example:  \[\[1 2 3\] \[a b c\]\].zip   // ((1 a) (2 b) (3 c))"
  dup I Me.0.n: map Xs Me Xs.I

text.`<` B =
  less B.is_text: bad "cant compare string `[Me]` with [B]"
  AS $n
  BS B.n
  if AS < BS
  then | times I AS:
         | AC $I.code
         | BC B.I.code
         | when AC <> BC: ret   AC < BC
       | 1
  else | times I BS:
         | AC $I.code
         | BC B.I.code
         | when AC <> BC: ret   AC < BC
       | 0

text.`*` N = @text: dup N: Me

text.is_upcase =
  @"Test whether every character is an uppercase ASCII letter."
  times I $n:
  | C $I.code
  | when C < 'A'.code or 'Z'.code < C: ret   0
  1

text.is_downcase =
  @"Test whether every character is a lowercase ASCII letter."
  times I $n:
  | C $I.code
  | when C < 'a'.code or 'z'.code < C: ret   0
  1

text.is_digit =
  @"Test whether every character is an ASCII digit (0-9)."
  times I $n:
  | C $I.code
  | when C < '0'.code or '9'.code < C: ret   0
  1

text.is_alpha =
  @"Test whether every character is an ASCII letter (a-z or A-Z)."
  times I $n:
  | C $I.code
  | when C < 'a'.code or 'z'.code < C:
    | when C < 'A'.code or 'Z'.code < C:
      | ret   0
  1

//shorthands to make text matching easier
text.is_d = $is_digit
text.is_a = $is_alpha

text.has F = bad "text.has is obsolete"

/*
text.has F =
| if F.is_fn then
  | times I $n: when F($I): ret   1
  else
  | times I $n: when $I >< F: ret   1
| 0
*/

text.u =
  @"Upcase a text.  Letters A-Z and a-z only; non-ASCII passes through.
Example:  \"Hello\".u   // \"HELLO\""
  Ys map Char $l
  | C Char.code
  | if C < 'a'.code or 'z'.code < C then Char
    else (C - 'a'.code + 'A'.code).char
  Ys.text

text.d =
  @"Downcase a text.  Letters A-Z and a-z only; non-ASCII passes through.
Example:  \"Hello\".d   // \"hello\""
  Ys map Char $l
  | C Char.code
  | if C < 'A'.code or 'Z'.code < C then Char
    else (C - 'A'.code + 'a'.code).char
  Ys.text


text.title =
  @"Capitalise the first character of a text.
Example:  \"hello world\".title   // \"Hello world\"   (first char only!)"
  less $n: ret   Me
  if $0.is_upcase then Me else "[$0.u][$tail]"

_.is_keyword = 0
text.is_keyword =
  @"A text is a \"keyword\" (variable) if it starts with an uppercase letter.
Example:  \"Foo\".is_keyword   // 1
          \"foo\".is_keyword   // 0"
  not: $n and $0.is_upcase

text.trim s/' ' i/0 l/1 r/1 =
| @"Strip leading and trailing instances of a separator (default space).
Keyword args: s/separator (default \" \"), l/1, r/1 select which sides.
Example:  \"  hi  \".trim         // \"hi\"
          \"***hi***\".trim(s/\\*)  // \"hi\""
| Xs $l
| when L:
  | It case Xs [$S@Zs] Zs
  | while It: Xs =  It
| when R
  | Xs Xs.f
  | It case Xs [$S@Zs] Zs
  | while It: Xs =  It
  | Xs =  Xs.f
| Xs.text

text.begin T =
| @"Test whether this text starts with the given prefix (or any of a list).
Example:  \"hello world\".begin(\"hello\")   // 1"
| if T.is_text: ret $upto(T.n) >< T
| T.any: X => $upto(X.n) >< X

int.map F = dup I Me: F(I)

int.keep F = $l.keep(F)
int.skip F = $l.skip(F)

list.i =
| @"Pair each element with its index, producing a list of \[I value\] pairs.
Useful for indexed iteration via destructured map bodies."
| dup I $n: [I Me^pop]

text.i = $l.i

list.`.` K =
| times I K: Me =  $tail
| $head

list.del K =
| @"Remove the element at index K.  Returns a new list."
| [@$take(K) @$drop(K+1)]
list.insert K V =
| @"Insert value V at index K, shifting later elements right."
| [@$take(K) V @$drop(K)]
list.change K V =
| @"Replace the element at index K with value V."
| [@$take(K) V @$drop(K+1)]

text.del K = [@$take(K) @$drop(K+1)].text
text.insert K V = [@$take(K) V @$drop(K)].text
text.change K V = [@$take(K) V @$drop(K+1)].text

`..` X N = dup N X

list.n =
| @"Length of a list -- O(1) on regular lists."
| S 0
| till $end
  | Me =  $tail
  | S+
| S

list.end =
| @"Test whether a list is empty.  Returns 1 if empty, else 0.
Use this instead of .n equal-test -- O(1) on every list representation."
| _eq $n 0

text.end = _eq $n 0


bytes.bytes = Me

list.bytes =
| N $n
| as Ys N.bytes: times I N: Ys.I =  pop Me

list.utf8 = $bytes.utf8

list.head =
| @"First element of a list.  Returns No on an empty list.
See also: .tail, .end (test for empty)."
| $0

list.tail =
| @"All elements after the first.  Returns the empty list when
the input has zero or one elements.
See also: .head, .lead (all but last)."
| $l.tail

no.'*head' = No
list.'*head' = if $end: No else $head
text.'*head' = if $end: No else $head
list.`++` = if $end: No else $tail

list.`><` B =
| less B.is_list: ret   0
| till $end or B.end: less Me^pop >< B^pop: ret   0
| $end and B.end

hard_list.`><` B =
| less B.is_list: ret   0 //FIXME: cons_list B will be O(n^2) slow
| N $n
| when _ne N B.n: ret   0
| times I N: less $I >< B.I: ret   0
| 1

_list_.`><` B =
  less B.is_list: ret   0 //FIXME: cons_list B will be O(n^2) slow
  when _ne (_tag B) 9: B = B.l //convert B to _list_
  N $n
  when _ne N B.n: ret   0
  times I N: less (_lget Me I) >< (_lget B I): ret   0
  1

list.`<` Xs =
| A B 0
| times I $n:
  | A =  $I
  | B =  Xs.I
  | when A <> B: ret   A < B
| ret   A < B

list.`>` Xs =
| A B 0
| times I $n:
  | A =  $I
  | B =  Xs.I
  | when A <> B: ret   A > B
| ret   A > B

list.`<<` Xs =
| A B 0
| times I $n:
  | A =  $I
  | B =  Xs.I
  | when A <> B: ret   A << B
| ret   A << B

list.`>>` Xs =
| A B 0
| times I $n:
  | A =  $I
  | B =  Xs.I
  | when A <> B: ret   A >> B
| ret   A >> B

list.f =
| @"Reverse a list (flip).  Returns a new list; the original is untouched."
| N $n
| Ys dup N
| while N
  | N-
  | Ys.N =  pop Me
| Ys

hard_list.f =
| N $n
| dup N
  | N-
  | $N

text.f = $l.f.text


text.cnt C = $l.cnt^C


list.map F =
| @"Apply a function to every element, collecting results.
Example: Xs.map(X => X * X) squares each element.
Use `&fn` to pass a named function: Xs.map(&sq).
The `Xs{Body}` operator is the same thing with a richer body grammar."
| dup $n: F(Me^pop)
hard_list.map F = dup I $n: F($I)
_list_.map F = dup I $n: F(|_lget Me I)
text.map F = $l.map(F)

list.`^^` X = map Y Me Y^^X

list.fold Run F =
| @"Left fold with an explicit initial value.
Example: Xs.fold 100 (Acc X => Acc + X) -- 100 plus sum of Xs.
Same shape as foldl in Haskell or reduce in JS.  For sum use .z."
| for X Me: Run = F(Run X)
| Run

list.e F = till $end: F(Me^pop)
hard_list.e F = times I $n: F($I)

list.z =
| @"Sum a list of numbers.  Mixed int/float promotes to float.
Empty list returns 0."
| S 0
| till $end: S += pop Me
| S

hard_list.z =
| S 0
| times I $n: S += $I
| S

list.cnt F =
| C 0
| if F.is_fn
  then till $end: when F(Me^pop): C+
  else till $end: when F><Me^pop: C+
| C

hard_list.cnt F =
| C I 0
| if F.is_fn
  then times I $n: when F($I): C+
  else times I $n: when F><$I: C+
| C

list.keep F =
| @"Keep elements for which the predicate returns truthy.
Example: Xs.keep(X => X > 0) keeps positives.
Use `&fn` to pass a named predicate: Xs.keep(&is_odd).
See also: .skip (inverse), the `:Cond` form for in-line filters."
| Ys:
| if F.is_fn
  then for X Me: when F(X): Ys =  [X@Ys]
  else for X Me: when F >< X: Ys =  [X@Ys]
| Ys.f

list.skip F =
| @"Drop elements for which the predicate returns truthy.  Inverse of .keep."
| Ys:
| if F.is_fn
  then for X Me: less F(X): Ys =  [X@Ys]
  else for X Me: less F >< X: Ys =  [X@Ys]
| Ys.f

list.j =
| @"Concatenate a list of lists into a single flat list.
Sometimes spelled \"join\" or \"flatten\" elsewhere."
| Rs dup $map(?n).z
| I 0
| for Ys Me: for Y Ys: Rs.(I+) =  Y
| Rs


_list_.l = Me
_bytes_.l = dup I $n $I
text.l = dup I $n $I
list.l =
| N $n
| Ys dup N
| times I N: Ys.I =  pop Me
| Ys
int.l = dup I Me: I //iota operator

list.apply F =
| @"Call a function with this list as its argument list.
Example:  \[1 2 3\].apply(min)   // min(1, 2, 3) -> 1"
| $l.apply(F)
list.apply_method F = $l.apply_method(F)

list.text @As =
| @"Join a list of text values into one text, optionally with a separator.
Example: Xs.text(\", \") joins with comma-space.  Non-text elements are coerced via interpolation."
| R $l
| if As.n then R.text(As.0) else R.text

text.text = Me

list.x @As = Me{as_text}.text(@As)

list.split S =
| F if S.is_fn then S else X => S >< X
| Ys:
| P $locate(F)
| while got P:
  | Ys =  [$take(P)@Ys]
  | Me =  $drop(P+1)
  | P =  $locate(F)
| [Me@Ys].f

text.split F =
| @"Split a text on a delimiter.  Returns a list of texts.
Empty pieces are kept."
| $l.split(F).map(X=>X.text)

text.all F = $l.all(F)
text.any F = $l.any(F)

text.rows = $split '\n'

text.get = get_file_ Me
text.getText = get_text_file_ Me
text.lines = $getText.rows
text.set Value =
| if Value.is_text then set_text_file_ Me Value else set_file_ Me Value.bytes
| 0
text.exists = file_exists_ Me
text.time = file_time_ Me
text.mkpath = mkpath_ Me


text.paths @As =
| Path if '/' >< $~ then Me else "[Me]/"
| Xs if As.n then $items(all) else $items
| map X Xs "[Path][X]"

text.urls = Me.paths{url}

text.folders = Me.items{url}.keep(?(:_ '' '')){?0.lead}

text.url =
| Name Ext ""
| Xs $l.f
| Sep Xs.locate(?><'/')
| Dot Xs.locate(?><'.')
| when got Dot and (no Sep or Dot < Sep):
  | Ext =  Xs.take(Dot).f.text
  | Xs =  Xs.drop(Dot+1)
  | when got Sep: Sep -= Dot+1
| Folder Name No
| if got Sep
  then | Folder =  "[Xs.drop(Sep+1).f.text]/"
       | Name =  Xs.take(Sep).f.text
  else | Folder =  ''
       | Name =  Xs.f.text
| [Folder Name Ext]

list.unurl =
| [Folder Name Ext] Me
| when Ext <> '': Ext =  ".[Ext]"
| "[Folder][Name][Ext]"

list.take N =
| @"First N elements of a list.  Returns the whole list if N >= length."
| dup N: Me^pop
hard_list.take N = dup I N $I

list.drop N =
| @"All elements after the first N.  Returns empty if N >= length."
| times I N Me^pop
| Me

hard_list.drop S = dup $n-S: Me.|S+

text.drop S = $l.drop(S).text
text.take S = $l.take(S).text
text.~ = $($n-1)
text.head = $0
text.tail = $drop(1)
text.lead = $take($n-1)

text.inc Val = Me.l("[Me]0":
                      @T '-' D<+d? = if T.end: "[T.text][-D.text.int+Val]"
                                     else "[T.text]-[D.text.int+Val]"
                      @T D<+d? = "[T.text][D.text.int+Val]")

text.`++` = $inc 1
text.`--` = $inc -1

list.~ = $($n-1)
list.suf X = [@Me X]
list.lead = $take($n-1)

// take up to N elements
list.upto N = $take(|max 0: min $n N)
text.upto N = $take(|max 0: min $n N)

text.`.!` I = if I < $n and I >> 0: Me.I

// drop up to N elements
list.wout N = $drop(|max 0: min $n N)
text.wout N = $drop(|max 0: min $n N)

list.cut P S = $drop(P).take(S)
text.cut P S = $drop(P).take(S)

list.slice B E S =
  Xs $l
  N Xs.n
  if B < 0:
    B = N+B
    if B < 0: B = 0
  else if B > N: B = N
  if E < 0:
    E = N+E
    if E < 0: E = 0
  else if E > N: E = N
  if B < E:
    N (E-B+S-1)/S
    B -= S
    dup N: Xs.|B += S
  else
    N (B-E+S-1)/S
    E += S*N
    dup N: Xs.|E -= S


text.slice B E S = $l.slice(B E S).text

int.bes E S = //implementation of [B:E:S]
  B Me
  if B<E:
    N (E-B+S)/S
    B -= S
    dup N: B += S
  else
    N (B-E+S)/S
    E += S*N
    dup N: E -= S

text.bes E S = [Me.code:E.code:S]{char}

text.keep Item = $l.keep(Item).text
text.skip Item = $l.skip(Item).text

list.takeIf F =
  R:
  while not $end and F(Me.0): push Me^pop R
  R

list.dropIf F =
  while not $end and F(Me.0): Me^pop
  Me

text.dropIf F = $l.dropIf(F).text
text.takeIf F = $l.takeIf(F).text

_.rmap F = F(Me)
list.rmap F = map X Me X.rmap(F)

list.infix Item =
| @"Insert a value between every two adjacent elements.
Example:  \[\\a \\b \\c\].infix(\\sep)   // (a sep b sep c)"
| N $n*2-1
| if N < 0 then [] else dup I N: if I%2 then Item else Me^pop

list.locate F =
| @"Return the INDEX of the first matching element, or No."
| less F.is_fn: F = (X => F >< X)
| for (I 0; not $end; I+): when F(Me^pop): ret   I

hard_list.locate F =
| if F.is_fn then times I $n: when F($I): ret   I
  else times I $n: when F >< $I: ret   I

text.locate F =
| if F.is_fn then times I $n: when F($I): ret   I
  else times I $n: when F >< $I: ret   I

list.find F =
| @"Return the first element for which the predicate is truthy, or No."
| if F.is_fn
  then for (I 0; not $end; I+):
  | It Me^pop; when F(It): ret   It
  else for (I 0; not $end; I+):
  | It Me^pop
  | when F><It: ret   It

hard_list.find F =
| if F.is_fn
  then | times I $n:
         | It $I
         | when F(It): ret   It
  else | times I $n:
         | It $I
         | when F><It: ret   It

_list_.find F =
| if F.is_fn
  then | times I Me^_size:
         | It _lget Me I
         | when F(It): ret   It
  else | times I Me^_size:
         | It _lget Me I
         | when F><It: ret   It


list.group N =
| @"Group a list into chunks of N elements.
Example:  \[1 2 3 4 5 6\].group(2)   // ((1 2) (3 4) (5 6))
          \[1 2 3 4 5\].group(2)     // ((1 2) (3 4) (5)) -- trailing partial"
| Y Ys []
| I 0
| till $end
  | push Me^pop Y
  | I+
  | when I >< N
    | push Y.f Ys
    | Y =  []
    | I =  0
| when Y.n: push Y.f Ys
| Ys.f

text.group N = $l.group(N){?text}

list.all F =
| if F.is_fn then for X Me: less F(X): ret   0
  else for X Me: less F >< X: ret   0
| 1

list.any F =
| if F.is_fn then for X Me: when F(X): ret   1
  else for X Me: when F >< X: ret   1
| 0

list.max =
| @"Largest element of a list, by < comparison.  Empty list returns No."
| when $end: ret   No
| M $head
| for X Me: when X > M: M =  X
| M

list.min =
| @"Smallest element of a list, by < comparison.  Empty list returns No."
| when $end: ret   No
| M $head
| for X Me: when X < M: M =  X
| M

list.maxBy F =
| when $end: ret   No
| M $head
| FM F(M)
| for X Me:
  | FX F(X)
  | when FX > FM:
    | M =  X
    | FM =  FX
| M

list.minBy F =
| when $end: ret   No
| M $head
| FM F(M)
| for X Me:
  | FX F(X)
  | when FX < FM:
    | M =  X
    | FM =  FX
| M

//list.has Item = got $find(Item)
list.has Item = bad "list.has is obsolete" 

HexChars '0123456789ABCDEF'

int.x =
| less Me: ret   '0'
| Cs:
| S ''
| when Me < 0
  | S =  '-'
  | Me =  -Me
| while Me > 0
  | Cs =  [HexChars.(Me%16) @Cs]
  | Me /= 16
| [S@Cs].text

_.as_text = "#([Me^typename] [Me^gid_.x])"

no.as_text = 'No'

int.as_text =
| less Me: ret   '0'
| Cs:
| S ''
| when Me < 0
  | S =  '-'
  | Me =  -Me
| while Me > 0
  | Cs =  [HexChars.(Me%10) @Cs]
  | Me /= 10
| [S@Cs].text

plain_char C =
| N C.code
| if   ('a'.code << N and N << 'z'.code)
    or ('A'.code << N and N << 'Z'.code)
    or ('0'.code << N and N << '9'.code)
    or '_'.code >< N
  then 1
  else 0

text.as_text =
| less $n: ret '``'
| case Me 'if'+'then'+'else'+'or'+'and': ret "`[Me]`"
| Cs:
| Q $0.is_digit
| for C Me
  | less plain_char C: Q =  1
  | when '`' >< C: C =  '\\`'
  | when '\\' >< C: C =  '\\\\'
  | push C Cs
| if Q then ['`' @['`' @Cs].f].text else Me

list.as_text = "([(map X Me X.as_text).text(' ')])"

_.textify_ = $as_text
text.textify_ = Me

say @As =
| @"Print one or more values, separated by spaces, followed by a newline.
Lists print as parenthesised forms; tables print as @-key-val groups.
Double-quoted text with bracketed expressions is interpolated.
See also: say_ (no newline), bad (raise an error)."
| say_ "[As{"[?]"}.text(' ')]\n"

bad @Text =
| @"Raise a runtime error with the joined-text argument list as the message.
Caught by `btrap` on the call path; otherwise terminates with the error and source location.
Example:  less B: bad 'division by zero'
See also: btrap (catch), bterror (test for error value)."
| rterr_ "[Text.text ' ']"

tbl.__ Method Args =
| if _gt Args.n 1
  then Args.0.(Method^_method_name.tail) =  Args.1 // strip `assign indicator`
  else Me.(Method^_method_name)
tbl.map F =
| @"Apply a function to each key-value pair, collecting the results."
| $l.map(F)
tbl.as_text =
| @"Render the table as its @-curly key!value literal text form."
| "@{[$l{"[?0.as_text]![?1.as_text]"}.text(' ')]}"
tbl.copy =
| @"Shallow copy of a table.  Values shared; keys are."
| $l.t
tbl.deep_copy =
| @"Recursive deep copy of a table."
| $l.t
tbl.s @As =
| @"Sort a tables key-value pairs the same way list.s does for a list of pairs."
| $l.s @As

list.t =
| @"Convert a list of pair-lists to a table.  Inverse of `tbl.l`."
| T!
| for [K V] Me: T.K =  V
| T

list.bag = Me{?,1}.t

list.uniq =
| @"Remove consecutive duplicates, preserving first occurrence.
Use .s first if you want to dedupe over the whole list regardless of order."
| Seen!
| $skip(X => got Seen.X or (Seen.X = 1) and 0)

text.pad Count Item =
  X "[Item]"
  when X.n > 1: bad "pad item: [X]"
  N  +Count - $n
  when N < 0: bad "text is larger than [Count.abs]: '[Me]'"
  Pad  @text: dup N X
  if Count < 0: "[Pad][Me]" else "[Me][Pad]"

list.pad Count Item =
  N  +Count - $n
  when N < 0: bad "list is larger than [Count.abs]: '[Me]'"
  Pad dup N Item
  if Count < 0: [@Pad @Me] else [@Me @Pad]


int.digits @Base =
  B if Base.end: 10 else Base.head
  Ys:
  while Me > 0:
    push Me%B Ys
    Me /= B
  Ys.l

list.digits @Base =
  B if Base.end: 10 else Base.head
  R 0
  for X Me:
    R *= B
    R += X
  R

type macro @new_macro N E: name!N expander!E

// `meta A B` -- attach a meta value B (typically a source-position
// triple) to A.  For heap-allocated A we stash (A -> B) in the
// runtime's weak hashtable via `meta_attach_` and return A
// unchanged; consumers still read it back via A.meta_ (which goes
// through `meta_lookup_`).  This means AST nodes coming out of the
// parser are plain lists -- no wrapper struct intercepting method
// dispatch -- which is what unblocks `text.parse` from switching
// to the consolidated C reader (`parse_strip_c_`).
//
// For immediates (int, fixtext, No, T_TAG, ...) we fall back to
// the legacy `meta_wrapper` type: immediates have no GC identity
// to key on in the weak table.  The parser doesn't attach meta to
// immediates today, but other consumers (e.g. ui_old's widget
// rects) might.
//
//~ means meta_wrapper doesn't inherit from _, so all methods route
//  through __ -- preserving the "forwarding wrapper" semantics for
//  the immediate case.
// `meta` -- wrapper type that pairs an object with a meta value
// (typically a source-position triple).  `~` means the type does
// not inherit methods from `_`; all non-field method calls route
// through `__` which forwards to the wrapped object_.  So a
// meta-wrapped AST node behaves identically to the underlying
// list/tag/closure/... for the compiler / macroexpander, but
// `.meta_` returns the source position directly via the auto-
// generated field accessor.
//
// The reader (runtime/reader.c `reader_parse_strip`) allocates
// these wrappers directly in C when stripping each list whose
// head was a token with a real source position.  Looking up the
// type tag via `intern("meta")` keeps the C side decoupled from
// the Symta-side type registration.
//
// `_.meta_ = No` is the default for non-wrapped objects.  The
// type's auto-generated `meta.meta_` field accessor overrides
// it for wrapped instances.  Same story for `is_meta`: `_` gets
// 0 by default from the auto-generated `type meta` predicate.
type meta.~ O M: object_!O meta_!M
_.meta_ = No
// `meta.__` (the universal-sink forwarder that unwraps `object_`
// and re-dispatches) is installed C-side as a direct builtin --
// see OP-4 in runtime/bltin.c (`b_meta_apply`).  Symta-side defn
// removed because the 3-MCALL forward dominated cold compile.


LCG_Seed  No
LCG_M     2147483647
LCG_M_F   LCG_M.float
LCG_A     16807
LCG_B     0

lcg_init Seed =
| LCG_Seed =  Seed
| 10.rand
| No

int.rand =
| LCG_Seed =  (LCG_Seed*LCG_A + LCG_B) % LCG_M
| @int: @round: LCG_Seed.float*$float/LCG_M_F

rand_get = LCG_Seed
rand_set V = LCG_Seed =  V

LCG_Stack    dup 32 0
LCG_StackTop 0

rand_push NewSeed =
| LCG_Stack.LCG_StackTop =  LCG_Seed
| LCG_StackTop += 1
| LCG_Seed =  NewSeed

rand_pop =
| R LCG_Seed
| LCG_StackTop -= 1
| LCG_Seed =  LCG_Stack.LCG_StackTop
| R

float.rand =
| LCG_Seed =  (LCG_Seed*LCG_A + LCG_B) % LCG_M
| LCG_Seed.float/LCG_M_F*Me

int.randr B =
  A Me
  B = B.int
  if B < A: swap A B
  (B-A).rand + A

float.randr B =
  A Me
  B = B.float
  if B < A: swap A B
  (B-A).rand + A

int.chnc = 100.rand << Me

float.chnc = 100.0.rand << Me

list.rand = $(@rand $n-1)

list.outcome = $find(s~+=?0; p~^(Me{?0}.z*)<<s~).1


GGensymCount 0
text.rand = "[Me]__[GGensymCount+]"

lcg_init: time

list.shuffle =
  Xs $copy
  N Xs.n
  while N > 1:
    N-
    R N.rand
    X Xs.R
    Xs.R =  Xs.N
    Xs.N =  X
  Xs


IValue  0
IParent 1
ILeft   2
IRight  3

merge H1 H2 =
| less H1: ret   H2
| less H2: ret   H1
| when H2.IValue < H1.IValue:
  | T H1
  | H1 =  H2
  | H2 =  T
| if 1.rand
  then | H1.ILeft =  merge H1.ILeft H2
       | when H1.ILeft: H1.ILeft.IParent =  H1
  else | H1.IRight =  merge H1.IRight H2
       | when H1.IRight: H1.IRight.IParent =  H1
| H1

sort_asc Xs =
| Root 0
| for X Xs
  | Root =  merge [X 0 0 0] Root
  | Root.IParent =  0
| dup Xs.n
  | V Root.IValue
  | Root =  merge Root.ILeft Root.IRight
  | V

list.s @As =
| @"Sort a list in ascending order, stable.  Accepts a comparator lambda
for descending or custom orderings.  Returns a fresh list."
| F No
| case As
  [A] | F =  A
  [] | ret  : sort_asc Me
  Else | bad "list.s: invalid number of arguments"
| merge H1 H2 =
  | less H1: ret   H2
  | less H2: ret   H1
  | when F(H2.IValue H1.IValue):
    | T H1
    | H1 =  H2
    | H2 =  T
  | if 1.rand
    then | H1.ILeft =  merge H1.ILeft H2
         | when H1.ILeft: H1.ILeft.IParent =  H1
    else | H1.IRight =  merge H1.IRight H2
         | when H1.IRight: H1.IRight.IParent =  H1
  | H1
| Root 0
| for X Me
  | Root =  merge [X 0 0 0] Root
  | Root.IParent =  0
| dup $n
  | V Root.IValue
  | Root =  merge Root.ILeft Root.IRight
  | V
  

list.sortBy F =
  Xs if F.is_int: Me{X=X.F,X} else Me{X=F(X),X}
  Xs.s(?0 < ??0){1}



//= parse text as integer; an optional argument provides Radix
text.int @Radix =
| @"Parse a text as an integer.  Returns No on parse error."
| Rdx 10
| when Radix.n: Rdx =  Radix.0
| T Me.u
| N $n
| R I 0
| Sign if $I >< '-'
       then | I+
            | -1
       else 1
| Base '0'.code
| AlphaBase 'A'.code - 10
| for (; I < N; I+)
  | C T.I.code
  | V if '0'.code << C and C << '9'.code then C - Base else C - AlphaBase
  | R =  R*Rdx + V
| R*Sign

//tries parsing as int, or returns Default
text.try_int Default = if $n>0 and $all(?is_digit): $int else Default


list.u4  = $3*0x1000000 + $2*0x10000 + $1*0x100 + $0
list.u4b = $0*0x1000000 + $1*0x10000 + $2*0x100 + $3
list.s4  = as R $u4: when R-*-0x80000000: R-=0x100000000
list.s4b = as R $u4b: when R-*-0x80000000: R-=0x100000000

list.u2 = $1*0x100 + $0
list.u2b = $0*0x100 + $1
list.s2 = as R $u2: when R-*-0x8000: R-=0x10000
list.s2b = as R $u2b: when R-*-0x8000: R-=0x10000

int.u4 = [Me%256 Me/0x100%256 Me/0x10000%256 Me/0x1000000%256]
int.u4b = [Me/0x1000000%256 Me/0x10000%256 Me/0x100%256 Me%256]
int.s4 =
| when Me < 0: Me += 0x100000000
| [Me%256 Me/0x100%256 Me/0x10000%256 Me/0x1000000%256]
int.s4b =
| when Me < 0: Me += 0x100000000
| [Me/0x1000000%256 Me/0x10000%256 Me/0x100%256 Me%256]

int.u2 = [Me%256 Me/0x100%256]
int.u2b = [Me/0x100%256 Me%256]
int.s2 = 
| when Me < 0: Me += 0x10000
| [Me%256 Me/0x100%256]
int.s2b =
| when Me < 0: Me += 0x10000
| [Me/0x100%256 Me%256]

int.bit N = _and 1: _shr Me N
int.bitSet N Bit = _xor Me (_xor (_shl Bit N) (_and Me (_shl 1 N)))

int.invert = 0xFFFFFFFFFFFFFFF -^- Me

int.bitSetM Mask V = if V:
                       if Me -*- Mask: Me
                       else Me -+- Mask
                     else
                       if Me -*- Mask: Me -*- Mask.invert
                       else Me

int.in Start End = Start << Me and Me < End
list.in [RX RY RW RH] = $0.in(RX RX+RW) and $1.in(RY RY+RH)

int.clip A B = if Me < A then A
               else if Me > B then B
               else Me

float.clip A B = if Me < A then A
                 else if Me > B then B
                 else Me

atan2 X Y =
| PI 3.141592653589793 //FIXME: use a macro
| R if X > 0.0 then @atan Y/X
    else if Y > 0.0 then
      if X < 0.0 then PI + (Y/X).atan
      else PI/2.0
    else if Y < 0.0 then
      if X < 0.0 then -PI + (Y/X).atan
      else PI*6.0/4.0
    else if X < 0.0 then PI
    else 0.0
| when R < 0.0: R += 2.0*PI
| R


list.<= Src = times I $n: $I = Src.I

_list_.<= Src = times I Me^_size: _lset Me I Src.I

list.div F =
  No //hack!!! otherwise R! wont be processed correctly
  R!
  for X Me:
    K F(X)
    Xs R.K
    R.K = if got Xs: [X@Xs] else [X]
  R{[?0 ?1.f]}.t

list.rip F = //rip list in two parts
| As $keep(F)
| Bs $skip(F)
| Bs,As

list.hash =
| H 0
| for X Me: H =  (H -<- 1) -^- X.hash
| if H<0 then -H else H

list.clear Value = times I $n: Me.I =  Value

type iter base p
iter.end = $base.n><$p
iter.`++` = $base.($p+)
iter.`--` = $base.($p-)
iter.head = $base.$p
iter.`.` N = $base.($p+N)
iter.`=` N V = $base.($p+N) =  V
iter.`+` N = iter $base $p+N

list.iter = iter $l 0

SetterCache!
GetterCache!

setters_ O =
| T typename O
| Ss SetterCache.T
| when no Ss:
  | Ss =  O^methods_.keep(?0.0 >< '='){[?0.tail ?1]}.t
  | SetterCache.T =  Ss
| Ss

getters_ O =
| T typename O
| Gs GetterCache.T
| when no Gs:
  | Setters setters_ O
  | Gs =  O^methods_.keep(K,V => got Setters.K).t
  | GetterCache.T =  Gs
| Gs

_.getter = getters_ Me

_.setter = setters_ Me


//backtracking landing place
//very fragile, and will crash and call any function other than F
btland F =
| K _setjmp
| if _lget K 0: F(| _lget K 1) else _lget K 1

//jumps to previously created backtrack landing
btjump Land Value = _longjmp Land Value

type bterror type text

btrap F = btland: K =>
| PrevDEH get_deh
| set_deh: Type Msg =>
  | set_deh PrevDEH
  | btjump K: bterror reader_error Msg
| R F()
| set_deh PrevDEH
| R

// Token type accessors.  The `tok` type itself comes from C
// (runtime/bltin.c's `tok_` varargs constructor); we just expose
// per-field readers + the `is_token` discriminator here.  All
// actual parsing lives in runtime/reader.c.
/*
type token Type Val Src P:
  type!Type
  value!Val
  src!Src   //source in the file
  parsed!P  //parsed expression associated with the token
  pchar     //prefix char
*/

_.is_token = 0
tok.is_token = 1

tok.src =: $row $col $orig
tok.=src S =
| $row = S.0
| $col = S.1
| $orig = S.2

tok.as_text = "#[$value]"


normalize_folder F =
| when F >< '': ret   './'
| if F.~ >< '/' then F else "[F]/"

//FIXME: eval.s/produce_sbc could use this function too
resolve_src_path Name Folders =
| for Folder Folders
  | SrcFile "[Folder][Name].s"
  | when SrcFile.exists: ret SrcFile
  | Donor "[Folder][Name].sbc"
  | when Donor.exists: ret Donor
| No

lexical_macro_expand SrcFile Text LexP =
| [Name Paths] LexP
| [Uses Exps HSz Row] parse_module_hdr_ Text
| Hdr Text.take(HSz)
| Body Text.drop(HSz)
| SrcFolder normalize_folder SrcFile.url.0
| Incs:
| NameFolder Name.url.0
| SrcFolder SrcFile.url.0
| if SrcFolder.url.0.split^'/'(:@_ ui ''): Uses =: @Uses cls uim 
| for U Uses:
  | D U
  | when NameFolder<>'' and "[SrcFolder][D].s".exists: D = "[NameFolder][D]"
  | SrcPath resolve_src_path D Paths
  | less got SrcPath: bad "couldn't find [U] for [SrcFile]"
  | [P F @_] SrcPath.url
  | HF "[P][F].h"
  | less HF.exists: pass
  | Incs =: @Incs U
| Incs = Incs{Inc => "#:[Inc]\n"}
| Text [@Hdr @Incs "#line [Row+1] \"[SrcFile]\"\n" @Body].text
| ncm_process_ SrcFile Text Paths.l

//lexP provides paths for the lexical macro-processor
//if macroexpansion without the paths is needed, just pass []
text.parse Src!'<none>' LexP!No =
| @"Parse text as Symta source code, returning the AST as nested lists.
Useful for DSL parsers and the JSON/XML examples that rewrite their syntax
into Symta source and let the reader do the heavy lifting."
| Text Me
| when got LexP: Text = lexical_macro_expand Src Text LexP
| R parse_strip_c_: parse_tokens_c_: add_bars_c_: Text.tokenize(Src)
| if R.end then [[]] else R.0.tail

// `text.sexp`: a simpler entry point than `text.parse` that
// skips the `add_bars_c_` toplevel-wrapping pass.  Used by
// places that want to parse one expression (or a small list
// of them) without the `|`-block structure -- e.g. the UIM
// event-stream and the show-wh tag in uim.s.  Returns the
// first expression by default; pass `list!1` to get the full
// list of parsed expressions.
text.sexp Src!'<none>' LexP!No List!0 =
| Text Me
| when got LexP: Text = lexical_macro_expand Src Text LexP
| R parse_strip_c_: parse_tokens_c_: Text.tokenize(Src)
| if List: ret R
| if R.end: No else R.0

