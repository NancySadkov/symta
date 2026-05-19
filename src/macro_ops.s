// macro_ops.s -- control flow + operators + infix syntax.
// min/max, if/when/less/and/or, mexlet, while/till/times,
// dup/map/for, forms (\\, form, expand_text_splice),
// pop/push/+= etc., let, arithmetic, comparisons, &&/||,
// bitwise, ^, ., ->, :, $, have, sbs.
// Auto-included by macro.s via NCM #:macro_ops.s

expand_minmax Op As =
| when As.n><0: ret No
| when As.n><1: ret As.0
| A As.0
| B As.1
| R form | ~A A
         | ~B B
         | if Op ~A ~B then ~A else ~B
| expand_minmax Op [R @As.drop(2)]

min @As = expand_minmax `<` As

max @As = expand_minmax `>` As

swap A B = form | ~T A
                | A =  B
                | B =  ~T

expand_cond Xs = case Xs
  [[`=` C B]@Rs] |: `if` C B (expand_cond Rs)
  [] | []
  Else | mex_error "unexpected `[Xs]`"

`if` @As =
  if As.n><1: ret: expand_cond As.0.tail
  A As.0
  case A [H<-(keyword?+[`.` _ _]+[`$` _]) @(T<[_@_])]:
    if H.is_text: ret: form: let_ ((H T)) (_if (got H) $@(As.tail))
    ret: form: case T H $(As.1) ~E $(if As.n < 3: No else As.2)
  [_if @As]
no @Xs =: _no Xs
got @Xs =: _got Xs
not @Xs =: _not Xs
`and` A B =: _if A B 0
`or` A B = form: let_ ((~V A)) (_if ~V ~V B)
when @Xs =: _if Xs.lead Xs.~ No
less @Xs =: _if Xs.lead No Xs.~

has_head Head Xs =
| if Xs.is_list and Xs.n then
    if Xs.0><Head then 1
    else Xs.any(X=>has_head Head X)
  else 0

mexlet @As =
#if #NCM_TRACE_MACROS
| mtrace_say \mexlet As
#endif
| case As
  [[@Bs] Body]
  | NBs map [Expr Value] Bs:
    | Noun,Name case Expr
      [Name<-list?] | 0,Name
      Name<-list?   | 1,Name
      ['\\' Name<-list?] | 2,Name
      Else | mex_error "mexlet: bad expr=[Expr]"
    | [Name Noun,Value GMexLets.Name]
  | for [Name Cur Prev] NBs: GMexLets.Name = Cur
  | Body =: _nomex Body^mex
  | for [Name Cur Prev] NBs: GMexLets.Name = Prev
  | Body
  [Expr Value Body] | form: mexlet ((Expr Value)) Body
  Else | mex_error "mexlet: bad arglist ([As])"

expand_loop Head Post Body =
| L @rand l
| Post if got Post then [Post] else []
| Break:
| when Body^has_head(pass):
  | Pass @rand pass
  | Body =  [mexlet [pass] [_goto Pass] Body]
  | push [_label Pass] Post
| when Body^has_head(done):
  | Done @rand done
  | Body =  [mexlet [done] [_goto Done] Body]
  | push [_label Done] Break
| [_progn [_label L]
          [_if Head
               [_progn Body @Post [_goto L]]
               No]
          @Break]

//normalize the `macro @Args: Body` expressions
norm_cbody As = if As.n > 1: [As.lead As.~] else [As.0 []]

while @As =
| Cond,Body norm_cbody As
| case Cond [H<-(keyword?+[`.` _ _]) @(T<[_@_])]:
  | if H.is_text: ret: form: while 1: let_ ((H T))
      (_if (got H) Body (done))
  | ret: form: while 1: case T H Body ~E (done)
| expand_loop Cond No Body
till @As =
  Cond,Body norm_cbody As
  expand_loop [not Cond] No Body

times Var Count Body =
  I if got Var: Var else @rand 'I'
  case Count [`,` W H]:
    case I:
      [`,` X Y] =  ret: form: times X W: times Y H Body
      Else =  ret: form: times ~X W: times ~Y H: `|` (I ~X,~Y) Body
  N @rand 'N'
  ['|' [N Count]
       [I [0]]
       [when [_tag N] [_fatal 'times: bad loop count']]
       (expand_loop [_lt I N] [_set I [_inc I]] Body)]

expand_dup Var Count Body =
| if Var><No: less Body.is_list:
  | ret: form | ~R _listn Count
              | ~R.clear Body
              | ~R
| I if got Var then Var else @rand 'I'
| N @rand 'N'
| Ys @rand 'Ys'
| ['|' [N Count]
       [I [0]]
       [when [_add [_tag N] [_lt N 0]]
         [_fatal 'dup: bad loop count']]
       [Ys [_listn N]]
       [while [_lt I N]
         ['|' [_lset Ys I Body]
              [_set I [_inc I]]]]
       Ys]

dup @As = case As:
  [X Xs Body] = expand_dup X Xs Body
  [Xs Body] = expand_dup No Xs Body
  [Xs] = expand_dup No Xs 0
  Else = mex_error "bad dup [As]"

expand_map_for Type Item Items Body =
  if Items.is_keyword: Items = form each(Items){$("[Items]_")}
  Xs @rand 'Xs'
  I @rand 'I'
  N @rand 'N'
  ['|' [Xs [_mcall Items l]]
       [Type I [_size Xs]
          ['|' [Item [_lget Xs I]]
               Body]]]

map @As = case As:
  Item Items Body = expand_map_for dup Item Items Body
  [`;` Entry Cond Post] Body =
    Xs @rand 'Xs'
    ['|' [Xs [_list]]
         Entry
         (expand_loop Cond Post [push Body Xs])
         [_mcall Xs f]]
  Else = mex_error "`map` has bad syntax [As]"

for @As = case As:
  Item Items Body = expand_map_for times Item Items Body
  [`;` Entry Cond Post] Body =: '|' Entry (expand_loop Cond Post Body)
  [Typename] = GLastType = Typename
  Else = mex_error "`for` has bad syntax [As]"

expand_quoted_list Xs =
| Ys map X Xs: if X.is_list then expand_quoted_list X else [_quote X]
| ['_list' @Ys]

expand_quasiquote O =
| less O.is_list:
  | if O.is_table: ret O.l{[[`!` ?0] [`\\` ?1]]}.j
  | ret [_quote O]
| case O
  [`$` X] | X
  Else | ['[]' @(map X O: expand_quasiquote X)]

`\\` O = expand_quasiquote O

expand_form O AGT =
| less O.is_list: ret
  if O.is_text and not O.is_keyword then O
  else if O.is_text and O.n > 1 and O.0 >< '~' then
    | AG AGT.O
    | when no AG
      | AG =  O.tail.rand
      | AGT.O =  AG
    | AG
  else [_quote O]
| case O
  [`$` X<0.is_keyword<-[`$` _]] | X
  Else | ['[]' @(map X O: expand_form X AGT)]

form O =
| AGT!
| R expand_form O AGT
| when AGT.n > 0:
  | R =  [let_ (map [K V] AGT [V [_mcall [_quote K.tail] rand]]) R]
| R

expand_text_splice Xs =
| case Xs
   [X] | when X.is_text: ret [_quote X]
   [] | ret [_quote '']
| As map X Xs: if X.is_text then [_quote X] else [_mcall X textify_]
| [_mcall [_list @As] text]

`"` @Xs /*"*/ = expand_text_splice Xs

pop O = form: as O.head: O = O.tail

push Item O = form: O = [Item @O]

`+=` A B = [`=` A [`+` A B]]
`-=` A B = [`=` A [`-` A B]]
`*=` A B = [`=` A [`*` A B]]
`/=` A B = [`=` A [`/` A B]]
`%=` A B = [`=` A [`%` A B]]


`++` O = form: `=` (O) (_inc O)
`+_` O = form: let_ ((~O O)) (`|` (`=` (O) (_inc ~O))  ~O)
`--` O = form: `=` (O) (_dec O)
`-_` O = form: let_ ((~O O)) (`|` (`=` (O) (_dec ~O))  ~O)


`*_` O = form $(O(keyword? =: '\\' O)).rand

`${}` @As = form (No.new_fn_)($@(As{K,V=: ',' K ['\\' V]}))

let @As =
| when As.n < 2: mex_error "bad let @As"
| Cond,Body norm_cbody As
| Gs map B Cond.group(2) ['G'.rand @B]
| R @rand 'R'
| [let_ [[R 0] @(map G Gs [G.0 G.1])]
    @(map G Gs [_set G.1 G.2])
    [_set R Body]
    @(map G Gs [_set G.1 G.0])
    R]

//bin_op A B Op Method = form: _mcall A Method B


bin_op A B Op Method = form: Op A B


`+` @As = case As:
  [O] = form: _abs O
  [A B] = bin_op A B _add `+`
  Else = mex_error "`+` got wrong number of args: [As]"
`-` @As = case As
  [O] | form: _neg O
  [A B] | bin_op A B _sub `-`
  Else | mex_error "`-` got wrong number of args: [As]"
`*` @As = case As
   [A B] | bin_op A B _mul `*`
   [O] | form: _mcall O `*head`
   Else | mex_error "`*` got wrong number of args: [As]"
emit_path_file Expr =
  Path:
  Ext \raw
  while Expr(:['/' A B]):
    [_ A B] Expr
    case B:
      ['.' N E] =
        push N(:['.' A B] = "[A].[B]") Path
        Ext = E
      Else = push B(['%' X] = X) Path
    Expr = A
  push Expr(['%' X] = X) Path
  R form: No.$"load_[Ext]" [$@(Path{X = form X.textify_})].text('/')
  R
is_path_file X =
  case X:
    1.is_keyword+['\\' 1.is_text] = 1
    ['/' A B] = is_path_file A
    Else = 0
`/` @As =
  case As:
    [A B] =
      if is_path_file A: emit_path_file ['/' @As]
      else bin_op A B _div `/`
    [A] = form: _if A.end No (pop A)
    Else = mex_error "`/` got wrong number of args: [As]"
`%` @As = case As
   [A B] | bin_op A B _rem `%`
   [['&' 1.is_keyword<F]] | form: _fn (~O) (not (F ~O))
   [1.is_keyword<M] | form: _fn (~O) (not ~O.M)
   [A] | form: not $@As
   Else | mex_error "`%` got wrong number of args: [As]"
`^^` A B = [_mcall A '^^' B]
`<` A B = bin_op A B _lt `<`
`>` A B = bin_op A B _gt `>`
`<<` A B = bin_op A B _lte `<<`
`>>` A B = bin_op A B _gte `>>`
//#SBC_TRANSITION
#if #SBC_TRANSITION
`><` A B =
   if A.is_int or case A ['\\'+'"'+_quote X] (X.is_int)
   then form: _same A B
   else [_mcall A '><' B]
`<>` A B =
   if A.is_int or case A ['\\'+'"'+_quote X] (X.is_int)
   then form: _vary A B
   else [_mcall A '<>' B]
#else
is_fixkw X = X.is_fixtext and X.is_keyword
// OP-2: when either side is the literal `No`, emit the dedicated
// SBC_NO / SBC_GOT opcode (`L[src]==No ? 1:0` / `L[src]!=No ? 1:0`
// -- ~5 ns each, no method dispatch) instead of falling into the
// IMMEQ slow path which MCALLs `m_eq` whenever the non-int side
// isn't a fixnum.  `No` in the AST IS the runtime No marker (the
// reader interns the bare `No` identifier), so `no X` on the AST
// node correctly identifies it.
`><` A B =
   if no A then form: _no B
   elif no B then form: _no A
   elif A.is_int or is_fixkw A
      or case A ['\\'+'"'+_quote X] (X.is_fixtext or X.is_int)
   then form: _same A B
   else [_eq A B]
`<>` A B =
   if no A then form: _got B
   elif no B then form: _got A
   elif A.is_int or is_fixkw A
      or case A ['\\'+'"'+_quote X] (X.is_fixtext or X.is_int)
   then form: _vary A B
   else [_ne A B]
#endif
`&&` A B = form: let_ ((~A A))
                   (_if ~A.bool
                      (let_ ((~B B))
                        (_if ~B.bool ~B 0)))
`||` A B = form: let_ ((~A A))
                   (_if ~A.bool
                        ~A
                      (let_ ((~B B))
                        (_if ~B.bool ~B 0)))
`-+-` A B = bin_op A B _ior '-+-'
`-^-` A B = bin_op A B _xor '-^-'
`-*-` A B = bin_op A B _and '-*-'
`-<-` A B = bin_op A B _shl '-<-'
`->-` A B = bin_op A B _shr '->-'

nullary_ Op = form: _fn (~A ~B) (Op ~A ~B)

norm_infix_arg A =
  case A [':' ':'+'!'<Op Body]:
    if Op><'!': mex_error "fixme: implement `^!`"
    ret Body
  A

`^` A B =
  if:
    // TS-1.1: when B is a known type, `X^T` is a typed
    // assertion -- runtime-checked unbox via `_the T X`.
    // Falls through to the original apply-on-left semantics
    // when B is just a function/value name.  When B IS a
    // type, emits `[_the B A]` directly -- assertion form
    // (no conversion).  For the conversion-form, use
    // `T X` directly (e.g. `int 3.5` -> 3).
    B^is_known_type = [`_the` B A]
    B.is_keyword = [B A]
    A.is_keyword =
      case B
        [`^...` @Args] | [A @Args{?^norm_infix_arg}]
        Else | [A B^norm_infix_arg]
    1 = case A
          [Z<`^`+`.` X Y] //supply argument to previous `^` or `.` call
            if Y.is_keyword:
              case B:
                [`^...` @Args] = ['()' A @Args{?^norm_infix_arg}]
                Else =
                  case A:
                    ['^' A Fn] = ['()' Fn A B^norm_infix_arg]
                    Else = ['()' A B^norm_infix_arg]
            else
              case B:
                [`^...` @Args] = [Z X [`^...` Y @Args]]
                Else = [Z X [`^...` Y B]]
          Else [A B]
sbs What With Src = form: case Src What With ~Else ~Else
`.` A B = 
  if B.is_keyword or case B [`@`+`&`+`^...` @_] 1:
  | less B.is_keyword
    | ret: case B
             [`@` Default] | sbs No Default A
             [`&` Fn] | [Fn A]
  | when A.is_keyword:
      A =: '\\' A
  | when A^is_var_sym:
    | Type GVarsTypes.A
    | when got Type:
      | Fields GTypes.Type
      // Subtle: `got Fields and Fields.locate(B)` short-circuits
      // to 0 when Fields is No -- but `got 0` is 1 (0 != No),
      // so the `when got P` below would fire with P=0 and emit a
      // wrong-field `_dget A 0`.  Guard with `if got Fields` so
      // P stays No for non-struct types like `list`/`int` that
      // aren't registered in GTypes.
      | P | if got Fields then Fields.locate(B) else No
      | when got P: ret [_dget A P]
  | ret ['()' ['.' A B]]
  case B [`:` ':'+'!'<Op Body]:
    As case Body
      [['|' [';' @As]]] | As
      [['|' @As]] | As
      ['='@_] | [Body]
      [':' A [['|' [';' X@Xs]]]] | [[':' A X] @Xs]
      [':' A [['|' X@Xs]]] | [[':' A X] @Xs]
      Else | [Body]
    Br if Op><':': '()' else '{}'
    if Br><'()':
      case As [-[':'@_]<X @Xs]: As = [[':' [] X] @Xs]
    As =: ';' @As
    ret [Br A @As]
  case B [`=>` @_]+[';' [`=>` @_] @_]:
    if A.is_keyword: A = [A]
    ret [B A]
  when A.is_keyword: A =: '\\' A
  form: _lget A B

`->` A B =
| when A.is_keyword: A =  ['\\' A]
| when B.is_keyword: B =  ['\\' B]
| form
  | ~A A
  | ~B B
  | ~G ~A.~B
  | when No >< ~G
    | ~G = !
    | ~A.~B =  ~G
  | ~G


expand_colon_r E Found =
| less E.is_list: ret E
| P E.locate($0 ["@@" Y]=>Y.is_keyword)
| less got P: ret: map X E: expand_colon_r X Found
| Name E.P.1
| Expr E.drop(P+1)
| G 'G'.rand
| Found(Name G)
| [@E.take(P) ['|' [`=` [G] Expr] G]]

`:` A B =
| case A []:
  | case B [[`|` @Xs]]: B = Xs.j
  | case B [`:` Xs Ys]: B = [@Xs [`|` Ys]]
  | ret [`[]` @B]
| L A.~
| case L [`!`@_]: ret: meta [@A B] GSrc
| Name 0
| G 0
| E expand_colon_r A: X Y => | Name =  X; G =  Y
| less Name: ret [@A B]
| B B.rmap(if Name >< ? then G else ?) //FIXME: preserve metainfo
| [let_ [[G 0]] [@E B]]


`,` X @Xs = case X [`,`@_] [@X @Xs]
                   Else [`[]` X @Xs]

`$` Expr = [`.` 'Me' Expr]

have Var Default = form | when (no Var) (`=` (Var) Default)
                        | Var

