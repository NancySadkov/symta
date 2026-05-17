export macroexpand 'mexlet' 'let_' 'let' 'default_ret_' 'ret'
       'if' 'case'
       '[]' '\\' 'form'
       'mtx' 'rows' 'rowz' 'list' 'no' 'got' 'not' 'and' 'or'
       'when' 'less' 'while' 'till'
       'dup' 'times' 'map' 'for' 'type'
       'named' 'export' 'export_' 'pop' 'push' 'as' 'callcc' 'fin'
       '|' ';' ',' '$' '@'
       '+' '-' '*' '/' '%' '^^'
       '<' '>' '<<' '>>' '><' '<>' '&&' '||'
       '^' '.' '->' ':' '{}' '()' '[' '=>' '"' 'btret'
       '=' '+=' '-=' '*=' '/=' '%=' '++' '--' '+_' '-_' '*_' '<=' '${}'
       '-+-' '-^-' '-*-' '-<-' '->-'
       'cons' 'uncons' 'same' 'on'
       'ffi_begin' 'ffi' 'min' 'max' 'swap' 'have' 'source_' 'compile_when' 
       'nullary_' 'hcase' 'hcase_go' 'mac' 'prx' 'mdbg'
       'help'


GExpansionDepth No
GExpansionDepthLimit 4000
GMacros No
GDefaultLeave No
GModuleCompiler No
GModuleFolders No
GSrc: 0 0 unknown
GTypes No
GVarsTypes:
GMexLets No //verb mexlets
GLastType 0
FFI_Lib No
GExports No

GQLMBHitFn 0
GQLMBHitExtractor 0
GQLMBAllowFn 1
GQLMBAllowSet 1
GQLMBAllowMapSet 0 
GQLMBAllowACVar 0
GQLMBAllowExtractor 0 //determines if qlmb handles `~`
GQLMBLVAL 0
GQLMBACVar No // `~` auto closure variables
GQLMBACVarDefs No //default values for ac-vars
GQLMBACVarFins No
GQLMBNot 0
GQLMBSubstExpr 0

mex_error Message =
| [Row Col Orig] GSrc
| bad "[Orig]:[Row],[Col]: [Message]"

source_ = [_quote GSrc]

//MACRO-1: macro-expansion trace.  To enable, add a line
//`##NCM_TRACE_MACROS 1` (without the leading `##` escape --
//the escape is here so this comment doesn't itself define the
//macro!) immediately before the `#if #NCM_TRACE_MACROS` block
//below.  ncm then splices the trace-emit body into mtrace_say
//and wraps each entry point with a trace call.  When the macro
//is undefined, ncm elides every `#if` block byte-for-byte --
//the drift test's 5-stage bootstrap reaches fixed point in one
//round either way.
//
//Output format: one line per entry, prefixed by GExpansionDepth
//for cheap hierarchical context (mex bumps the depth on every
//recursive call, so indented reading is automatic).  Wire format
//is human-readable; upgrade to JSONL once we know what fields
//we actually want to grep on.
//
//Wrapped entry points: macroexpand, mex, mex_normal,
//expand_hole_term, mexlet.

#if #NCM_TRACE_MACROS
mtrace_say Name X = say "[GExpansionDepth]> [Name]: [X]"
#endif

is_var_sym X = X.is_text and not X.is_keyword

is_list_case V =
  V(:[_ _ @_]+[]+[['@'+'/' @_]+['+'+'*' _]+['<' _ ['+'+'*'+'/' _]]])


//FIXME: it is slow and can be speedup with caching
load_symbol Library Name =
| Module GModuleCompiler(Library)
| when no Module: mex_error "couldn't compile [Library]"
| Found Module^load_sbc.find(X => X.0 >< Name)
| less got Found: mex_error "couldn't load `[Name]` from `[Library]`"
| Found.1

expand_list_hole_advanced H Hs Key Hit Miss Size!No =
| [Again Took Rest Xs I N Else] form: ~Again ~Took ~Rest ~Xs ~I ~N ~Else
| Fail form: if I < N
             then | I+
                  | _goto Again
             else Miss
| Body form: case Took
               H | case Rest
                   [$@Hs] Hit
                   Else Fail
               Else | Fail
| when got Size:
  | Body = form: case Took.n
                   Size Body
                   Else | Fail
| form | Xs Key.l // ensure it is simple list
       | I 0
       | N Xs.n
       | _label Again
       | Took Xs.take(I)
       | Rest Xs.drop(I)
       | Body


expand_text_incut_matcher Xs Hit =
| Vs:
| Cs map X Xs: if X.is_text then map C X.l [`\\` C]
               else | V form ~G
                    | N X.0
                    | if N.is_text: less N >< _: push [N [_mcall V text]] Vs
                      else case N
                        [`&` [push '_' Var]]
                         | push [push [_mcall V text] Var] Vs
                        [`&` Var]
                         | push ['=' [Var] [_mcall V text]] Vs
                        Else
                         | V = X
                    | [[`@` V]]
| Hit form: `|` $@(Vs)
                Hit
| Cs.j,Hit

expand_kleene_star_hole Key Var Item Rest Hit Miss =
  [G Else] form: ~G ~Else
  Hole form: @Var @(`<` G [-Item @_]+[])
  Hit form: case G [$@Rest] Hit Else Miss
  expand_list_hole Key Hole Hit Miss

expand_list_hole Key Hole Hit Miss = case Hole
  [] | [_if [_mcall Key end] Hit Miss]
  [[`@` Item<['"'+'\\' @Xs]] @More]
    | case Item ['\\' 0.is_text]:
      | mex_error "bad match subcase: [Item]"
    | Cs,Hit expand_text_incut_matcher Xs Hit
    | expand_list_hole Key [@Cs @More] Hit Miss
  [[`@` @Zs]] | case Zs.n
                  0 | expand_hole Key _ Hit Miss
                  1 | expand_hole Key Zs.0 Hit Miss
                  Else | mex_error "bad match case: [Hole]"
  [[`@` Zs] @More] | expand_list_hole_advanced Zs More Key Hit Miss
  [[`*` Item] @Xs] | expand_kleene_star_hole Key _ Item Xs Hit Miss
  [[`+` X] @Xs] | expand_list_hole Key (form: X *X $@Xs) Hit Miss
  [['<' Var ['*' Item]] @Xs] | expand_kleene_star_hole Key Var Item Xs Hit Miss
  [['<' Var ['+' Item]] @Xs]
     | expand_kleene_star_hole Key (form [Item@_]<Var) Item Xs Hit Miss
  [['<' Var ['/' Size]] @Xs]
     | expand_list_hole Key [['/' Var Size] @Xs] Hit Miss 
  [[`/` @Sub Size] @Xs]
    | Sub = if Sub.n: Sub.0 else '_'
    | case Size
      ['$' Size]
      | Sz @rand 'Sz'
      | Ys @rand 'Ys'
      | Zs @rand 'Zs'
      | Hit expand_list_hole Zs Xs Hit Miss
      | form | Sz Size
             | _if Key.n < Sz
                   Miss
                   (`|` (Ys Key.take(Sz))
                        (Zs Key.drop(Sz))
                        $(expand_hole Ys Sub Hit Miss))
      Else
      | expand_list_hole_advanced Sub Xs Key Hit Miss Size!Size
  [X@Xs] | H @rand 'X'
         | Hs @rand 'Xs'
         | Hit =  expand_list_hole Hs Xs Hit Miss
         | [`if` [_mcall Key end]
                 Miss
                 [let_ [[H [_mcall Key head]]
                        [Hs [_mcall Key tail]]]
                   (expand_hole H X Hit Miss)]]

expand_hole_keywords Key Hit Xs =
| [I As Size] form: ~I ~As ~Size
| form: `|` $@(Xs{[?.1.title 0]})
            (As Key)
            (Size As.n)
            $@(map [O K V] Xs
               | KWSym K.d
               | KWVar K.title
               | L @rand 'l'
               | when V.is_keyword: V =: _quote V
               | form: named L
                 | times I Size: less I%2
                   | when KWSym >< As.I
                     | KWVar =  As.(I+1)
                     | ret L 0
                 | KWVar = V)
            Hit

// OP-5: map a `case X type?:` predicate name like "int" to the
// runtime type-tag constant (runtime/symta.h).  Returns No for
// predicates that aren't a single-tag test (multi-tag ones like
// "text" / "list", or non-tag ones like "keyword") so the caller
// falls back to the regular `_.is_T` MCALL dispatch.
//
// Hot single-tag predicates (`int?` / `float?` / `fn?` / `fixtext?`)
// in `case` arms previously paid one MCALL per dispatch -- worst
// case a megamorphic mcache miss at ~50 ns.  Inlining them as
// `_eq (_tag Key) $TAG` lowers to SBC_FXNTAG + SBC_IMMEQ (both
// ~5 ns each, IMMEQ's int-int fast path applies since both sides
// are tagged ints), trading ~45 ns of dispatch for two opcodes.
//
// Caveat: this changes the call semantics from "dispatch through
// MCALL and respect any user-side override" to "raw tag check".
// In practice the four predicates listed below are tag-bound by
// definition; user code overriding e.g. `mytype.is_int = 1` to
// claim int-ness would no longer be recognised by `case X int?:`
// patterns.  If anyone genuinely needs override-respecting checks,
// `if X.is_int then ...` (the explicit MCALL form, used widely in
// core_.s) is unaffected.
tag_for_predicate Name =
  if Name >< "int"     then 0
  else if Name >< "float"   then 1
  else if Name >< "fixtext" then 2
  else if Name >< "fn"      then 8
  else No

// OP-5b: multi-tag predicates.  `text?` matches both T_FIXTEXT and
// T_TEXT; `list?` matches the four concrete list-like tags
// (T_LIST, T_VIEW, T_CONS, T_BYTES -- the T_HARD_LIST + T_CONS
// subtypes of T_GENERIC_LIST).  Same override-bypass tradeoff as
// the single-tag predicates above: user-defined types overriding
// `is_text` / `is_list` would no longer be recognised in
// `case X type?:` patterns, but the explicit `if X.is_text`
// MCALL form is unaffected.
tags_for_predicate Name =
  if Name >< "text"      then [2 13]
  else if Name >< "list" then [9 10 11 18]
  else No

// OP-5b helper: right-fold a list of tag values into a nested
// `_if` chain that tests each tag against `(_tag Key)`.  Hit
// duplicates per branch (typically a small body or label-jump in
// pattern matching, so duplication is cheap).  Uses only
// primitive forms so the result is compiler-ready without
// further macroexpand.  Re-evaluates `(_tag Key)` per branch
// (cheap: just an O_TAG read + FXN-wrap), to avoid let_-binding
// a fresh variable which surprisingly didn't preserve identity
// in early prototypes.
build_tag_chain_simple Key Tags Hit Miss =
| when Tags.end: ret Miss
| [_if [_eq [_tag Key] Tags.0] Hit (build_tag_chain_simple Key Tags.tail Hit Miss)]

expand_hole_term Key Hole Hit Miss =
#if #NCM_TRACE_MACROS
| mtrace_say \expand_hole_term [Key Hole]
#endif
| when Hole >< '_': ret Hit
| when Hole >< '~':
   say "~ is obsolete"
   ret: form: if Key >< No then Miss else Hit
| when Hole.is_keyword:
  | when Hole.n and Hole.~ >< '?':
    | Lead Hole.lead
    | Tag tag_for_predicate Lead
    | if got Tag
        then ret: form: _if (_eq (_tag Key) $Tag) Hit Miss
    | // OP-5b: multi-tag predicates (text?, list?).  Re-evaluate
    | // `(_tag Key)` per branch (cheap O_TAG read + FXN-wrap);
    | // an attempt to bind it once via let_ surprisingly didn't
    | // preserve identity in early prototypes.  Hit duplicates N
    | // times for an N-tag predicate -- bounded since the worst
    | // case is `list?` with 4 tags and pattern-arm bodies are
    | // typically small.
    | Tags tags_for_predicate Lead
    | if got Tags
        then ret: build_tag_chain_simple Key Tags Hit Miss
    | ret: form: _if (@$"is_[Lead]" Key) Hit Miss
  | Hole =  [_quote Hole]
| ret: if Hole.is_text then [let_ [[Hole Key]] Hit]
       else [_if ['><' Hole Key] Hit Miss]

expand_hole_subscript Key Hit Miss Xs N =
  KeyN form ~KeyN
  Min Max Base No
  case N [':' A B] | Min = A; Max = B; N = No
  case Max [':' A B] | Max = A; N = B.0
  Min(:M@_ = | X ['<<' M '?']; N = N(X:-No=['<' X N]))
  Max(:M@_ = | X ['<<' '?' M]; N = N(X:-No=['<' X N]))
  if got Base: N = ['<' Base N]
  if N(:[]+No):
    ret [_if [_mcall Key is_list]
         (expand_hole KeyN N (expand_hole Key Xs Hit Miss) Miss)
         Miss]
  [_if [_mcall Key is_list]
       [let_ [[KeyN [_mcall Key n]]]
             (expand_hole KeyN N (expand_hole Key Xs Hit Miss) Miss)]
       Miss]

expand_hole Key Hole Hit Miss =
| less Hole(:X@Xs): ret: expand_hole_term Key Hole Hit Miss
| case Hole
  [`[]` @Xs]
    | Ks NewXs []
    | CurKW No
    | for X Xs:
       if got CurKW then
         | push [CurKW.0 CurKW.1 X] Ks
         | CurKW = No
       elif case X [`/` A B] A.is_keyword then
         //say GSrc
         | push [`!` X.1 X.2] Ks
       elif X(:`!` _) then
         | CurKW = X
       else
         | push X NewXs
    | Xs =: @NewXs.f @Ks.f
    | P Xs.locate($0 [`!` K V]=>1)
    | when got P: Xs =  [@Xs.take(P) [`@` [`!!` @Xs.drop(P)]]]
    | [_if [_mcall Key is_list]
          (expand_list_hole Key Xs Hit Miss)
          Miss]
  [`\\` X] | form: _if Hole >< Key Hit Miss
  [@B Op<'?'+'~?' @E] | if Op><'?': [_if [@B Key @E] Hit Miss]
                        else [_if [@B Key @E] Miss Hit]
  [`,` [`,` @Ys] @Xs] | expand_hole Key [`,` @Ys @Xs] Hit Miss
  [`,` X @Xs] | expand_hole Key [`[]` X @Xs] Hit Miss
  [Op<`<`+`>` X] | [_if [Op Key X] Hit Miss]
  [`<` A B] | expand_hole Key B (expand_hole Key A Hit Miss) Miss
  [`+` @Xs] | [_if (expand_match Key (map X Xs [X 1]) 0 No) Hit Miss]
  [`-` @Xs]
     | if Xs.n><1 and Xs.0.is_text and not Xs.0.is_keyword:
       | X Xs.0
       | form: _if Key (let_ ((X Key)) Hit) Miss
       else
       | [_if (expand_match Key (map X Xs [X 1]) 0 No) Miss Hit]
  [O<`+`+`-` [$O @Xs] @Ys] | expand_hole Key [O @Xs @Ys] Hit Miss
  [X<`.`+`^` [Y<`.`+`^` A B] @As]
    | G @rand 'G'
    | Hit expand_hole G [X G @As] Hit Miss
    | expand_hole Key [Y [`<` G A] B] Hit Miss
  [X<`.`+`^` [`()` [Y A B] @As] @Bs]
    | expand_hole Key [X [Y A B @As] @Bs] Hit Miss
  [`.` A B @As] | G @rand 'G'
                | [let_ [[G (if As.n
                             then [`()` [`.` Key B] @As]
                             else [`.` Key B])]]
                    (expand_hole G A Hit Miss)]
  [`^` A B @As] | G @rand 'G'
                | [let_ [[G [`()` B @As Key]]]
                    (expand_hole G A Hit Miss)]
  [`()` [Op A B] @As] | expand_hole Key [Op A B @As] Hit Miss
  [`=` A B] | [case Key A.0 [_if B Hit Miss] "Else".rand 0]
  [push V Var]
    | if V><'_': V = Key
    | form:
      | push V Var
      | Hit
  ['+_' L]
      | ret: form:
             | less L.is_list: L = []
             | push Key L
             | Hit

  ['&' X]
    | case X [push V Var]:
      | if V><'_': V = Key
      | ret: form:
             | push V Var
             | Hit
    | if X.is_keyword: ret: form: let_ ((X Key)) Hit //allow functions
    | form:
      | X = Key
      | Hit
  [`$` X] | form: _if X >< Key Hit Miss
  [`%` X] | form: _if Key.X Hit Miss
  ['!'+'{' @X]  | X = expand_qlmb X
                | form: _if X(Key) Hit Miss
  [`!!` @Xs] | expand_hole_keywords Key Hit Xs
  [`[` Xs N] | expand_hole_subscript Key Hit Miss Xs N
  [`"` @Xs] /*"*/ //FIXME: use special matcher for text
    | Cs,Hit expand_text_incut_matcher Xs Hit
    | form: _if (_mcall Key is_text)
                $(expand_list_hole Key Cs Hit Miss)
                Miss
  [[X@Xs]] | expand_hole Key [X@Xs] Hit Miss
  Else | mex_error "bad match case: [Hole]"

// FIXME: use `coma_list_normalize`
expand_match Keyform Cases Default Key =
| when no Key: Key =  @rand 'Key'
| E @rand end
| D @rand default
| R @rand 'R'
| Ys:
| for Case Cases.f
  | Name @rand c
  | NextLabel if Ys.end then D else Ys.0.1
  | Miss: _goto NextLabel
  | Hit: _progn [_set R [_progn @Case.tail]] [_goto E]
  | Ys = [[_label Name] (expand_hole Key Case.head Hit Miss) @Ys]
| [let_ [[Key Keyform]
         [R 0]]
    @Ys.tail
    [_label D]
    [_set R Default]
    [_label E]
    R]

case KeyForm @Cases =
  Cs case Cases:
    [Case] =
      case Case:
        [[`!` _] @_] = norm_keywords 0 Case
        [[`|` @Xs]] =
          map X Xs: case X:
            [`=` [A] B] = [A B]
            [`=` [A@As] B] = [[`[]` A@As] B]
            Else = mex_error "invalid syntax `[X]`"
        [`=` [A] B] =: [A B]
        [`=` [A@As] B] =: [[`[]` A@As] B]
        Else = mex_error "invalid syntax `[Case]`"
    Else = Cases.group 2
  expand_match KeyForm Cs 0 No

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
  | when A^is_var_sym: case GVarsTypes.find(?0><A) [Var Type]
    | Fields GTypes.Type
    | P | got Fields and Fields.locate(B)
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


is_qlmb_set_var X = not X.is_keyword or| X.n>1 and| X.0><'~' or X.~><'~'


//is auto-closure var
is_acvar X = X.is_text and X.n>1 and (X.0 >< '~' or X.~ >< '~')

is_kwacvar X =
  less X.is_text and X.n>1 and (X.0 >< '~' or X.~ >< '~'): ret 0
  Name if X.0 >< `~`: X.tail else X.lead
  less Name.is_keyword: ret 0
  1

GQLMBDoIncs 0
QLMBIncOps: '/' '+' '+_' '-_' '--'
QLMBIncOps = QLMBIncOps.bag
QLMBDebug 0
QLMBFastRet 0

expand_qlmb_r A Got =
| when A.is_text
  | when A >< '?' or A >< '??': when GQLMBAllowFn: ret Got(A)
  | when GQLMBAllowExtractor and A >< '~': ret Got(A)
  | less A.n > 1: ret A
  | when A.0 >< '~' or A.~ >< '~':
    | when A.0 >< '~' and A.1 >< '?':
      | less GQLMBAllowFn: ret A
      | GQLMBNot = 1
      | ret: expand_qlmb_r A.tail Got
    | when GQLMBAllowACVar: ret Got(A)
  | when A.0 >< '?': when GQLMBAllowFn:
    | M A.tail
    | V '?'
    | when M.0 >< '?':
      | V =  '??'
      | M =  M.tail
    | when M.is_digit: M =  M.int
    | ret: expand_qlmb_r ['.' V M] Got
  | ret A
| less A.is_list: ret A
| case A
   [form @Xs]+[`:` [form @_] @_]+[':' '!' _] | ret A
   [`()`+`{}` X @Xs] | [A.0 (expand_qlmb_r X Got) @Xs]
   ['.' X Y<[':' '!' _]] | [A.0 (expand_qlmb_r X Got) Y]
   [`|`+`=`+`=>`+`case` @_]
     | if GQLMBAllowMapSet: case A ['=' X Y]: //special case for Xs{A=B}
         ret: let GQLMBAllowMapSet 0
              | XX let GQLMBAllowSet 0
                       GQLMBLVAL 1
                   | expand_qlmb_r X Got
              | YY expand_qlmb_r Y Got
              | ['=' XX YY]
     | case A ['=' X<[0.is_keyword+1^is_acvar @_] Y]:
         XX let GQLMBAllowSet 0: expand_qlmb_r X Got
         YY expand_qlmb_r Y Got
         ret ['=' XX YY]
     | A
   [`\\` @_] | A
   [`%_` X]
     | QLMBFastRet = 1
     | GQLMBSubstExpr = 1
     | [btret expand_qlmb_r(X Got)]
   [_quote @_] | A
    ['{' @Xs]
      | let GQLMBAllowFn 0
        | ['{' @expand_qlmb_r(Xs Got)]
   Else
     | when GQLMBAllowACVar:
       | less GQLMBLVAL: when GQLMBDoIncs or not GQLMBAllowExtractor:
         | case A [Op<(QLMBIncOps.has ?)
                   Val<0.is_text+1.is_keyword<0^is_kwacvar]:
           | when Op><'/' or Val(1:['.'+'$' @_]=0):
             | ret [Op expand_qlmb_r(['^' "[lrnd*]~" Val] Got)]
       | case A [`.` Var<1^is_kwacvar Val]:
         | Name if Var.0 >< `~`: Var.tail else Var.lead
         | have GQLMBACVarDefs: !
         | have GQLMBACVarFins: !
         | GQLMBACVarDefs.Name =: ['[]']
         | GQLMBACVarFins.Name = | X => form X.f
         | ret [push expand_qlmb_r(Val Got) Got(Var)]
       | case A [`^` Var<1^is_acvar Val<0.is_keyword+1^is_acvar]:
         | have GQLMBACVarDefs: !
         | Name if Var.0 >< `~`: Var.tail else Var.lead
         | GQLMBACVarDefs.Name =: expand_qlmb_r(Val Got)
         | ret Got(Var)
     | S No
     | R map X A:
       | less GQLMBAllowMapSet and not GQLMBAllowFn:
         | case X ['&' 1^is_qlmb_set_var<V]:
           | GQLMBSubstExpr = 1
           | if GQLMBAllowACVar and V(:0.is_text+1.is_keyword<0^is_kwacvar):
             | less V(:['^'+'.' _ _]):
               | V = expand_qlmb_r(['^' "[lrnd*]~" V] Got)
           | S = V
           | X = V
       | expand_qlmb_r X Got
     | if got S:
       | case S ['()' 0.is_keyword<O @_]: S = O
       | R = form: S = R
     | R

expand_qlmb Expr =
| X Y E No
| GQLMBHitFn = 0
| GQLMBHitExtractor = 0
| GQLMBACVar = No
| GQLMBACVarDefs = No
| GQLMBACVarFins = No
| GQLMBNot = 0
| GQLMBSubstExpr = 0
| QLMBFastRet = 0
| R expand_qlmb_r Expr (N =>
     N('?'  = have X: form ~X
      ;'??' = have Y: form ~Y
      ;1^is_acvar = (
        | case GMexLets.N
            2,R | GQLMBSubstExpr = 1
                | R
            Else | have GQLMBACVar (!); have GQLMBACVar.N: 'I'.rand)
      ;'~' =
        (if no E: have E: form ~E
         else
         | EE form ~E
         | E =: @E._ EE
         | EE)
      ;Else = bad "expand_qlmb_r: got wrong item `[N]`"))
| As [X Y].skip(No)
| if got GQLMBACVar or GQLMBSubstExpr: Expr = R
| if As.n or got E:
    Expr = R
    if got E:
      when As.n: mex_error 'invalid \`~\` expr'
      if E.is_list: E =: '[]' @E
      GQLMBHitExtractor = E
| when As.n:
    GQLMBHitFn = 1
    if GQLMBNot: Expr = form: not Expr
    Expr = form: _fn As Expr
| Expr

expand_qlmb_curly Expr =
| less Expr^is_var_sym:
  | if not Expr.is_text or Expr.n><0 or Expr.0<>'?':
    | if not Expr.is_list or Expr(:`\\` _):
      | ret: form: _fn (~X) ~X.Expr
| expand_qlmb Expr


is_fcall_head X = X(:1.is_keyword+['@'+'$' 1.is_keyword]+['.' _ 1.is_keyword])
is_list_body X = X(:-?^is_fcall_head _ @_)
is_incut_body X = X(:[['@' 0.is_keyword] @_])

supply_text_transfer H Body =
| HH form ~H
| form:
  | HH H
  | ~IsText HH.is_text
  | if ~IsText: HH = HH.l
  | ~R $(Body(HH))
  | if ~IsText: ~R{textify_}.text else ~R

list.rany O = Me(0:H@T=H^r||T^r;=0;$O=1)

qlmb_getcvars =
  Its Ivs Index 0
  if got GQLMBACVar:
    Its = GQLMBACVar.l.skip(X,_=>X.0><'~'){A,B=A.lead,B}
    Ivs = GQLMBACVar.l.keep(X,_=>X.0><'~'){A,B=A.tail,B}
    Index = Ivs.keep(?(0:\~,_=1))(0:[A,B]=B) //handle ~~
    Ivs = Ivs.skip(?(0:\~,_=1))
  IndexInc if Index: ['|' ['++' Index]] else []
  its!Its ivs!Ivs index!Index indexInc!IndexInc
          defaults!GQLMBACVarDefs fins!GQLMBACVarFins
          fastRet!QLMBFastRet

acv_fin Fins X =
  K,V X
  if no Fins: ret V
  F Fins.K
  if no F: ret V
  F(V)

acv_init Defs X = //auto closure variable init
  if got Defs:
    R Defs.X
    if R.is_list: ret R.0
  if X.is_keyword: (if X.~><_: (if X.0.is_keyword: No else ['[]'])
                    else 0)
  else ['!']

`btret` Value =
  K case GMexLets.'btk_'
      2,R | R
      Else | mex_error "`btret`: no btland context"   
  form: btjump K Value

qlmb_supply_cvars QL R =
  Index,Its,Ivs QL[index its ivs]
  if Index: R = form: let_ ((Index -1)) R
  if Its or Ivs:
    Ds QL.defaults
    Fs QL.fins
    if Its.n: R = form: let_ $(Its{?1,(acv_init Ds ?0)}) R
    if Ivs.n:
      Ret if Ivs.n > 1: form [$@(Ivs{(acv_fin Fs ?)})] else acv_fin Fs Ivs.0
      R = form: let_ $(Ivs{?1,(acv_init Ds ?0)})
                | R
                | Ret
    KVs: @Its{"[?0]~",?1} @Ivs{"~[?0]",?1}
    R = form: mexlet $(map K,V KVs: ['\\' K],V) R
  if QL.fastRet:
    R = form: mexlet ((\btk_ ~K)) (btland: ~K => R)
  R

expand_curly_matcher H As =
| let GQLMBAllowFn 0
      GQLMBAllowMapSet 1
      GQLMBAllowACVar 1
  | As = expand_qlmb As
| QL qlmb_getcvars
| E "E".rand
| HasElse 0
| DVal 0
| case As [`;` [`:` DV A] @R]:
  | DVal = DV
  | As =: `;` A@R
| case As [`;` [[`|` @AAs]]]: As =: `;` @AAs
| EC form ~E
| EB EC
| if DVal: case DVal
    ['_'] | Q form ~Q
          | EC = form: Q
          | EB = form: Q._
    [] | As =: @As (form: _ =)
    Else | if DVal.rany \Q: EC = \Q
         | EB = DVal
| ListCase 0
| Ys map A As.tail: case A
  [`=` X Y]
    | ListCase = ListCase || X^is_list_case
    | NeedsQ Y.rany \Q
    | case Y []: Y = form: @[]
    | case X
      [@_ ['&']]
      | X = X[:~]
      | Y = if Y^is_list_body: form: @[$@(Y)]
            else form: @($@(Y))
      Else
      | if Y^is_list_body: Y = form: @[$@(Y)]
    | if NeedsQ and X.n:
        XX X.0(:['@' A]=['@' ['<' A \Q]];A<['+'+'*'+'/' _]=['<' \Q A];E=['<' E \Q])
        X =: XX @X.tail
    | [X Y]
  [] | [['[]'] []]
  [_ _ @_]<X
    | ListCase = 1
    | XX 0
    | let GQLMBAllowExtractor 1:
       let GQLMBAllowFn 0:
         XX = expand_qlmb X
    | if GQLMBHitExtractor: [[[`[]` @XX]] [GQLMBHitExtractor]]
      else | Q form ~Q
           | [['@' ['<' [`[]` @X] Q]] [Q]]
  X | XX 0
    | ListCase = ListCase || X^is_list_case
    | let GQLMBAllowExtractor 1:
       let GQLMBAllowFn 0:
         XX = expand_qlmb X
    | if GQLMBHitExtractor: [XX [GQLMBHitExtractor]]
      else | Q form ~Q
           | [[['<' X.0 Q]] [Q]]
| if ListCase:
  | MF 0  //match forward
  | B form ~B
  | E "E".rand
  | A form ~A
  | Ys = map [X Y] Ys.f:
         | NMF "mf".rand
         | Y Y(-?^is_incut_body = if Y^is_list_body: [['[]' @Y]] else [Y])
         | XX X(:[['<' A B]]=[A])
         | R case XX
             [] | form: NMF A =
                    case A
                      [] [$@(Y)]
                      $@(if MF: form: ~E [$@(Y) @MF(A) $@(Y)]
                         else form: ~E [$@(Y) @~E $@(Y)])
             [['@' '_'+0.is_keyword<1.is_text]]
               | form: NMF A = 
                  _if A.n
                    (case A
                      [@B $@(X)]
                      [@$(if MF: form MF(B) else B)
                       $@(Y)
                       ]
                      $@(if MF: form: ~E MF(A) else form: EC EB))
                    (if MF: form MF(A) else A)
             Else | form: NMF A =
                      case A
                        [@B $@(X) @E]
                        [@$(if MF: form MF(B) else B)
                         $@(Y)
                         @NMF(E)
                         ]
                        $@(if MF: form: ~E MF(A) else form: EC EB)
         | MF = NMF
         | R
  | R supply_text_transfer H: HH => form: `|` $@(Ys) MF(HH)
  | ret: qlmb_supply_cvars QL R
| Ys Ys{[[X] Y]=[X Y]}
| if Ys.any(?1^is_incut_body):
  | Ys = map [X Y] Ys: [X ['[]' @Y]]
  | R form
      $| supply_text_transfer H:
           HH => form: @j: map E HH: $@(QL.indexInc) (case E $@(Ys.j) EC [EB])
  | R = qlmb_supply_cvars QL R
  | ret R
| Ys Ys{[X Y<?^is_list_body] =: X ['[]' @Y]}
| R supply_text_transfer H:
           HH => form: map E HH: $@(QL.indexInc) (case E $@(Ys.j) EC EB)
| R = qlmb_supply_cvars QL R
| R


`{}` H @As =
| if H.is_keyword: H = form each(H){$("[H]_")}
| if As.end: ret: form: let_ ((~H H)) (~A => ~H.~A)
| case As [[':' @_]]:  As = [';' As.0]
| case As [[`=` @_]]: As = [';' As.0]
| case As [`;`  @_]: ret: expand_curly_matcher H As
| As = map A As:
   R 0
   let GQLMBAllowExtractor 1
       GQLMBAllowACVar 1
       GQLMBDoIncs 1
   | R = expand_qlmb_curly A
   if A><'~': ret: expand_curly_matcher H [';' [[`[]` @As]]]
   if GQLMBHitExtractor:
     ret: expand_curly_matcher H [';' As]
   R
| QL qlmb_getcvars
| E "E".rand
| R case As
    [A] | form: map E H: $@(QL.indexInc) A(E)
    Else | form: map E H: $@(QL.indexInc) [$@(As{A=form A(E)})]
| R = qlmb_supply_cvars QL R
| R

expand_sugar_lambda H DVal A As =
  HH 0
  Invert 0
  if H.is_keyword: H = [H]
  Name \r
  when As.end:
    case A [`=` [No] B]: A =: A.0 [[`\\` No]] A.2 //Hack cuz No confuses `=>`
  case A [`:` [N] AA]:
    Name = N
    A = AA
  As =: A @As
  case As [[[`|` @AAs]]]: As = AAs
  let GQLMBAllowFn 0
      GQLMBAllowMapSet 1
      GQLMBAllowACVar 1
  | As = expand_qlmb As
  QL qlmb_getcvars
  if DVal.rany \Q: HH = \Q
  Opts form: @Name
  As         As{[`=` X<[_ _ @_]+[] Y] = if Y.rany \R: HH = \Q
                                        [[[`[]` @X]] Y]
               ;[`=` X Y] = if Y.rany \Q: HH = \Q
                            if X^is_list_case: X = [[`[]` @X]]
                            [X Y]
               ;X = if DVal.end:
                      DVal = [0]
                      Invert = 1
                    if X^is_list_case: X = [[`[]` @X]]
                    Extractor 0
                    XX 0
                    let GQLMBAllowExtractor 1
                        GQLMBAllowFn 0
                        GQLMBAllowSet 0
                    | XX = expand_qlmb X
                    if GQLMBHitExtractor: [XX GQLMBHitExtractor]
                    else if Invert: [X 1]
                         else (HH = HH||(form ~H);[X HH])}
   //escape so `$` wont be mistaken for default
  [[X Y]@As] As{[[['$' _]<H@T] Y]=[[[H]@T] Y]}
  less DVal.end:
    case DVal [X<'_'+'~']: DVal = X
    Opts =: @Opts ['$' DVal]
  As =: [[@Opts @X] Y] @As
  As = As{[X Y<?^is_list_body] =: X [['[]' @Y]]}
  if QL.index: As = As{X,Y =: X ['|' QL.indexInc Y]}
  L [`;` @As{X,Y=[`=>` X Y]}]
  R if HH: [let_ [[HH H]] [L HH]] else [L H]
  R = qlmb_supply_cvars QL R
  R


`()` H @As =
| case As [[`:` _ _]]: As = [';' As.0]
| case As [`;` [`:` DVal A] @As]: ret: expand_sugar_lambda H DVal A As
| case As [[`=` _ _]]: As = [';' As.0]
| case As [`;` [`=` _ _] @_]:
  | if H.is_keyword: H = [H]
  | Ys map X As.tail:
    | case X
      [`=` X Y] | X = if X^is_list_case: [[`[]` @X]] else X.0
                | if Y^is_list_body: Y = [['[]' @Y]]
                | [X Y]
      Else | ['Else'.rand Else]
  | ret: form: case H $@(Ys.j) ~Else ~Else
| case As [`;` @_]:
  | As = [As]
| Extractor 0
| let GQLMBAllowExtractor 1
      GQLMBAllowACVar 1
  | F expand_qlmb As
  | As = map X As:
            Y expand_qlmb X
            if GQLMBHitExtractor:
              let GQLMBAllowFn 0: Y = expand_qlmb As
              Extractor = case Y
                [['"' @_]] | form H(No:Y=GQLMBHitExtractor)
                Else | form H(No:[$@Y]=GQLMBHitExtractor)
              done
            Y
| when Extractor: ret Extractor
| R case H
  [`.` A B]
    | case B [`&` Fn] | ret [B @As A]
    | if A.is_keyword: A =: '\\' A
    | [_mcall A B @As]
  [`$` B] | [_mcall \Me B @As]
  [`^` A B] | [B @As A]
  Else | if H.is_keyword then [H @As] else [_mcall H '()' @As]
| QL qlmb_getcvars
| R = qlmb_supply_cvars QL R
| R

`[` H @As =
  if H.is_keyword: H = [H]
  case As []: ret: form H.end
  GH 0
  GLIdx 0
  GSz 0
  getvr X =
    case X
      '~~' | if GH: ret GH
           | GH = H
           | H = "H".rand
           | GH
      'n~' | if GSz: ret GSz
           | getvr '~~'
           | GSz = "Sz".rand
           | GSz
      '~' | if GLIdx: ret GLIdx
          | getvr '~~'
          | GLIdx = "Last".rand
          | GLIdx
      Else | X

  supply_vars Body =
    Vs:
    if GH: push (form: H GH) Vs
    if GSz: push (form: GSz H.n) Vs
    if GLIdx: push (if GSz: form: GLIdx GSz-1 else form: GLIdx H.n-1) Vs
    for V Vs: Body = [let_ [V] Body]
    Body

  norm_sub GetVar X = X.:
     X<['[' @_] = X
     X<[@_]     = X{&r}
     X          = GetVar(X)

  case As [[':' N M]]: less case N [X@_] (X.is_keyword and X<>~ and X<>'!'):
    S 1
    case M [':' MM SS]: | M = MM; S = SS
    N = if N.end: 0 else norm_sub &getvr N
    M = if M.end: getvr 'n~' else norm_sub &getvr M
    S = norm_sub &getvr S
    ret: supply_vars: form H.slice(N M S)

#if 0
  case As [['=' @_]]+[';' ['=' @_] @_]: ret: form H{?($@(As))}

  case As [';' I @Rs]: //for loop
    getvr '~~'
    Default No
    N M 0
    case Rs [[-'=' @_] @RRs]:
      [D B] | Default = D; M = B
      B | N = B
    M = norm_sub &getvr M
    Df No
    case M [';' A B]:
      Df = M.0
      N = A
      M = B
    I = I([]="I".rand;I.0)
    N = N([]="N".rand;N.0)
    RL "RL".rand
    ret: if GLIdx: form (H => | GLIdx H.n-1; for I H.n: case H.I N M; Df)(GH)
          else form (H => | for I H.n: case H.I N M; Df; Df)(GH)
#endif
  Unbound 0
  case As ['!' @Xs]:
    As = Xs
    Unbound = 1

  GetMet if Unbound: '.!' else '.'
  SetMet if Unbound: '=!' else '='
  Take if Unbound: \upto else \take
  Drop if Unbound: \wout else \drop

  case As [['!' _] @_]:
    As = norm_keywords 0 As
    getvr '~~'
    if As.any(?([['@' _] _]=1;0)):
      case As.0
        [['@' K] V] | ret: form | H GH; ~KK K; [@H.Take(~KK) V @H.Drop(~KK)]
        Else | bad 'FIXME: plural Xs[Key!!Val...]'
    As2 As{[K V]=>form: _mcall H SetMet $(norm_sub &getvr K)
                                        $(norm_sub &getvr V)}
    ret: if GLIdx: form | H (GH[:]); GLIdx H.n-1; (`|` $@(As2)); H
         else form | H (GH[:]); (`|` $@(As2)); H

  As2 norm_sub &getvr As
  if GLIdx: As = As2

  As map A As: if A.is_keyword: [`\\` A] else A

  if As.any(?([`@`@_]=1;E=0)):
    getvr '~~'
    ret: supply_vars: form: map ~K [$@(As)]: _mcall H GetMet ~K

  when As.n>1:
    getvr '~~'
    ret: supply_vars: form | `[]` $@(As{A => form: _mcall H GetMet A})

  supply_vars^[_mcall H GetMet As.0]



is_incut X = case X [`@` Xs] 1

`[]` @As =
| case As [':' N M]:
    case As [':' [] []+[':' [] _]]:  mex_error '[:] is reserved'
    S 1
    case M [':' MM SS]: | M = MM; S=SS
    N N(=1)
    M M(=1)
    S S(=2) //sensible pick, cuz 1 is already default
    ret: form N.bes(M S)
| IncutCount As.cnt(&is_incut)
| when IncutCount >< 0: ret [_list @As]
| when IncutCount >< 1:
  | case As.~
    [`@` Xs] | As As.f.tail
             | till As.end: Xs =  [_mcall Xs pre As^pop]
             | ret Xs
| As map A As: if A^is_incut then A.1 else [_list A]
| [_mcall [_list @As] j]

//FIXME: move it to compiler.s
mangle_name Name =
| Rs map C Name:
  | N C.code
  | if   ('a'.code << N and N << 'z'.code)
      or ('A'.code << N and N << 'Z'.code)
      or ('0'.code << N and N << '9'.code)
    then C
    else "_[N.x.pad(-2 0)]"
| [_ @Rs].text

result_and_label Name =
| Mangled mangle_name Name
| ["ReturnOf[Mangled]_" "end_of[Mangled]_"]

expand_named Name Body =
  [R End] result_and_label Name
  [let_ [[R 0]]
    [_set R Body]
    [_label End]
    R]

named @As = expand_named As.head [_progn @As.tail]

add_pattern_matcher Args Body =
| All @rand 'As'
| Default form All.0
| case Args
    [[`$` '~'] @Zs] | Default = form: _fatal "couldn't match args list"
                    | Args = Zs
    [[`$` '_'] @Zs] | Default = form All.0._; Args = Zs
    [[`$` D] @Zs] | Default =  D; Args =  Zs
| Body = expand_qlmb Body
| case Args
   [[`@` All]] | Args =  All
   Else | Body =  expand_match All [[['[]' @Args] Body]] Default No
        | Args =  All
| [Args Body]

pattern_arg X = not X.is_text or X.is_keyword

expand_lambda As Body =
| Name 0
| case As [[`@` N] @Zs]: when N.is_keyword
  | Name = N
  | As = Zs
| [A B] if no As.find(&pattern_arg) then [As Body]
        else add_pattern_matcher As Body
| R: _fn A B
| when Name: R =  [let_ [[Name 0]] [`|` [_set Name R] [`&` Name]]]
| R

default_ret_ Name Body = let GDefaultLeave Name [_nomex Body^mex]

supply_ret Name Body =
  less has_head ret Body: ret Body
  less got Name: Name =  'lmb_'.rand
  : default_ret_ Name (expand_named Name Body)

`=>` As Body =
  Body = expand_qlmb Body
  Body = supply_ret No Body
  expand_lambda As [`|` Body]

expand_block_item_fn Name As Body =
| Body = expand_qlmb Body
| Body = supply_ret Name Body
| Body = [_progn [_mark Name] Body]
| [Name (expand_lambda As Body)]

expand_destructuring Value Bs Body =
| O @rand 'O'
| Ys map [I B] Bs.i: [B [_mcall O '.' I]]
| [let_ [[O Value]] [let_ Ys Body]]

expand_assign Place Value =
| case Place
  [`.` A B] | if B.is_keyword
              then | when A^is_var_sym: case GVarsTypes.find(?0><A) [Var Type]
                     | Fields GTypes.Type
                     | P | got Fields and Fields.locate(B)
                     | when got P: ret [_dset A P Value]
                   | [_mcall A "=[B]" Value]
              else [_lset A B Value]
  [`$` Field] | expand_assign [`.` \Me Field] Value
  [`&` Name] | if Name.is_keyword:  [_set Name Value]
               else bad "expand_assign: not a keyword: [Place]"
  [`[` A @B] | expand_assign [`.` A B] Value
  Else | [_set Place Value]

`=` Place Value = expand_assign Place.0 expand_qlmb(Value)

`<=` Place Value = form Place.`<=`(Value)

norm_keywords IsFields Xs =
| As:
| Key No
| Val:
| Cnt 0
| push_kv = if Cnt: push [Key Val.f] As
| for X Xs:
    if X(:`!` _) then
    | push_kv
    | Cnt+
    | Key = X.1
    | Val =:
    | case Key [`!` K]:
      | push `[]` Val
      | Key = K
    else
    | if IsFields and (not Val.end or no Key):
        push_kv
        Cnt = 0
        Val =:
        Key = No
        push [X [0]] As
      else push X Val
| push_kv
| As.f

process_type_args Name Args =
| Parent CtorName CtorBody 0
| CtorArgs:
| Super: _
| ProvideCopy 1
| As Args
| case As [@FsP [[`|` @L [`=` [] []] @R ]]]: As =: @FsP [`=` L.j [`|` @R]]
| if As.end: As = [[]]
| Inits As.~
| if Inits.is_text: Inits = []
  else As = As.lead
| case Inits [`=` A B]:
  | CtorBody = B
  | Inits = A
| less CtorBody: less Inits.end:
  | L Inits.~
  | case L [`|`@_]:
    | CtorBody = L
    | Inits = Inits.lead
| while Name.is_list: case Name
  ['.' A B] | Name =  A
            | if B >< ~ then Super =  []
              else if B >< no_copy then ProvideCopy =  0
              else if B.is_keyword then Super =  [B]
              else | Super =  []
                   | Parent =  B
  Else | mex_error "type: bad declarator [Name]"
| when case As [['@' A]@_] A.is_keyword: CtorName =  As^pop.1
| IsExcl 0
| CtorArgs = map A As:
  | case A [`!`_]: IsExcl = 1
  | if not IsExcl and A.is_keyword:
    | G "[A.head.u][A.tail]"
    | push G Inits
    | push [`!` A] Inits
    | A = G
  | A
| less CtorName: CtorName = Name
| Fields norm_keywords 1 Inits
| Vs:
| Fs map Name,Value Fields:
  | if Value><[]: Value = 0
    else case Value [X@_]: Value = X
  | push Value Vs
  | Name
| Fs Fs.skip(No)
| Vs Vs.f
| [Name Fs Vs Super Parent CtorName CtorArgs CtorBody ProvideCopy]

type Name @Fields =
| D process_type_args Name Fields
| [Name Fs Vs Super Parent CtorName CtorArgs CtorBody ProvideCopy] D
| GLastType = Name
| GTypes.Name =  Fs
| Ctor if CtorBody
       then [`=` [CtorName @CtorArgs]
                 [`|` ['Me' [_data Name @Vs]]
                      [_type Name 'Me' CtorBody]
                      'Me']]
       else [`=` [CtorName @CtorArgs] [_data Name @Vs]]
| V @rand 'V'
| Copy if ProvideCopy
       then [[`=` [[`.` Name "copy"]] [_data Name @(map F Fs [`$` F])]]
             [`=` [[`.` Name "deep_copy"]]
                  [_data Name @(map F Fs [`.` [`$` F] deep_copy])]]]
       else []
| Heir if Parent
       then form ((Name.__ ~Method ~Args = //sink unhandled to parent
                   | ~Args.0 =  Parent
                   | ~Args.apply_method(~Method)))
       else []
| ['@' ['|' Ctor
            @Copy
            @(map S Super [_subtype S Name])
            [`=` [[`.` Name "fields_"]] ['[]' @Fs]]
            [`=` [[`.` Name "is_[Name]"]] 1]
            [`=` [[`.` '_' "is_[Name]"]] 0]
            @(map [I F] Fs.i [`=` [[`.` Name F]]  [_dget 'Me' I]])
            @(map [I F] Fs.i [`=` [[`.` Name "=[F]"] V]  [_dset 'Me' I V]])
            @Heir
            ]]

expand_block_item_method Type Name Args Body =
| less Name >< __:
  | push \Me Args
  | when got GTypes.Type: Body =  form: _type Type $\Me Body
| when Name >< __:
  | case Args
    [Method As] | Args =  [['@' As]]
                | Body =  form: `|` (Method _this_method)
                                    ($\Me As.0)
                                    (_type Type $\Me Body)
    Else | mex_error "bad arglist for __; should be: Method Args"
| Body =  supply_ret Name Body
| Fn: `=>` Args [_progn [_mark "[Type].[Name]"] Body]
| Fn =  meta Fn GSrc
| [No [_dmet Name Type Fn]]

take_init_vars Xs =
| Vars:
| for X Xs:
  | less X^is_var_sym or X(:'&'+`[]`+`,` @): done
  | push X Vars
| Vars

expand_block_vars Expr =
  case Expr [`:` [N] Value]:
    when N^is_var_sym:
      case Value [[`|` @Xs]]: Value = Xs.j
      ret [[N [`[]` @Value]]]
  case Expr [[`!` [N]]]: when N^is_var_sym: ret [[N [`!`]]]
  less Expr.is_list: ret 0
  when Expr.end: ret 0
  Vars take_init_vars Expr.lead
  when Vars.end: ret 0
  Value Expr.drop(Vars.n)
  Head:
  when Vars.n > 1:
    V @rand 'V' //avoid eval'ing value more than once
    Head =  [[V Value]]
    Value =  V
  Vars map Var Vars:
    case Var ['&' Name]: Var = Name
    case Var [`,` A B]:
      while 1:
        case Var [`,` [`,` A B] @Xs]:
          Var = [Var.0 A B @Xs]
          pass
        done
      Var = [`[]` @Var.tail]
    [Var Value]
  [@Head @Vars]

normalize_block_items Xs =
  Ys:
  for X Xs:
    case X:
      [`:` A<[_ _ @_] B] =
          Vs take_init_vars A
          when Vs.end:
            push X Ys
            pass
          Z: @Vs [X.0 A.drop(Vs.n) B]
          push Z Ys
      [`;` @Xs] =  for X Xs: push X Ys
      Else = push X Ys
  Ys.f

expand_block_item_qlmb Expr =
  Expr = let GQLMBAllowACVar 1: expand_qlmb Expr
  qlmb_supply_cvars (qlmb_getcvars) Expr

expand_block_item_insert Xs = //insert a `@| ...` style expression
  Xs normalize_block_items Xs
  Ys map X Xs: expand_block_item X
  Ys.j

// `@"text"` at the head of a definition's body becomes the
// definition's docstring.  The function-defining macro prepends
// the body with `[_ssv 'docs' Name Text]` -- a compile-time
// intrinsic (see `src/compiler.s` SsaFormCases) that records the
// triple in `GSsv`, emits it as a SIF `t doc` directive, and
// `sif2sbc.c` packs it into the SBC's docs section.  No runtime
// call happens at definition time.  Full pipeline trace in
// `../dev/help-pipeline.md`.

// Returns the docstring text if X is `(@ (" Text))`; else No.
// Tolerates the 1-element list wrapping that the parser puts
// around each statement inside a `|` block.
maybe_doc X =
| Y if X.is_list and X.n >< 1 and X.0.is_list then X.0 else X
| less Y.is_list and Y.n >< 2: ret No
| less Y.0 >< '@': ret No
| Z Y.1
| less Z.is_list and Z.n >< 2: ret No
| less Z.0 >< '"': ret No
| if Z.1.is_text then Z.1 else No

// Returns Body unchanged unless its first statement was `@"text"`,
// in which case the first statement is replaced with a call to
// `_ssv 'docs' Name <text>`.  Text args are wrapped in `_quote`
// because docstrings often start with uppercase letters that
// uniquify would otherwise treat as variable names.
//
// The reader produces three body shapes; we handle all three by
// inspecting Body[0] rather than matching nested case patterns:
//   ((| stmt1 stmt2 ...))   -- multi-line `|` block, 1-elem outer
//   ((stmt1))               -- single-expr body, 1-elem outer
//   (stmt1 stmt2 ...)       -- flat multi-stmt (method-style)
build_ssv Name T =
  [_ssv [_quote 'docs'] [_quote Name] [_quote T]]

// Cheap shape test -- does Body start with an `@"text"` doc
// head?  Avoids running the expensive name-interpolation +
// list rebuild in `prefix_doc` on the 99 % of definitions
// (game / app code) that have no docstring.
has_doc_head Body =
| less Body.is_list and Body.n > 0: ret 0
| First Body.0
| if Body.n >< 1 and First.is_list and First.n > 0 and First.0 >< '|'
    then
      Stmts First.tail
      if Stmts.n > 0 then got maybe_doc Stmts.0 else 0
    else got maybe_doc First

prefix_doc Body Name =
| less Body.is_list and Body.n > 0: ret Body
| First Body.0
| if Body.n >< 1 and First.is_list and First.n > 0 and First.0 >< '|'
    then
      // `|` block: pull stmts out of the block.
      Stmts First.tail
      less Stmts.n > 0: ret Body
      T maybe_doc Stmts.0
      less got T: ret Body
      [[`|` [build_ssv(Name T)] @Stmts.tail]]
    else
      // Flat or single-expr: First IS the first statement.
      T maybe_doc First
      less got T: ret Body
      if Body.n >< 1
        then [build_ssv(Name T)]
        else [build_ssv(Name T) @Body.tail]

expand_block_item Expr =
  Y case Expr:
      [`=` [[Op<`.`+`.=` Type<1.is_keyword Method] @Args] Body] =
        when Op><`.=`: Method = "=[Method]"
        when has_doc_head Body:
          Body = prefix_doc Body "[Type].[Method]"
        expand_block_item_method Type Method Args Body
      [`=` [['@' Method] @Args] Body] =
        less GLastType: mex_error "no type declared beforehard"
        case Method nullary_,Name: Method = Name
        when has_doc_head Body:
          Body = prefix_doc Body "[GLastType].[Method]"
        expand_block_item_method GLastType Method Args Body
      [`=` [Name @Args] Value] =
        if Name.is_keyword
          then
            when has_doc_head Value:
              Value = prefix_doc Value "[Name]"
            expand_block_item_fn Name Args Value
          else
            when Args.n: mex_error "`=`: left side has too many expressions"
            [No (expand_assign Name Value)]
      Else =
        Vars expand_block_vars Expr
        when Vars: ret Vars
        Expr = expand_block_item_qlmb Expr
        Z mex Expr
        case Z [_insert [`|` @Xs]]: ret: expand_block_item_insert Xs
        [No [_nomex Z]]
  [Y]

make_multimethod Xs =
| when case Xs [[`=>` As Expr]] (As.n >< 0 or As.0^is_var_sym)
  | ret Xs.0
| All @rand 'As'
| Default form All.0  ///
| Name:
| Xs map X Xs: case X
    [`=>` Args Expr]
      | case Args [[`@` N] @Zs]: when N.is_keyword:
        | Name =  [[`@` N]]
        | Args =  Zs
      | case Args
          [['$' '~'] @Zs] | Default =: _fatal "couldn't match lambda"
                          | Args = Zs
          [[`$` '_'] @Zs] | Default = form All.0._; Args = Zs
          [['$' D] @Zs] | Default = D; Args = Zs
      | [['[]' @Args] expand_qlmb(Expr)]
| ['=>' [@Name ['@' All]] (expand_match All Xs Default No)]

coma_list_normalize E = 
| less E(:`,` _ _): ret E
| R E.: [`,` A B] = [@A^r B]
        X = [X]
| [`[]` @R]

expand_block_helper R A B =
  if no A then [B @R]
  else if A.is_keyword then [[_set A B] @R]
  else | R if R.n then [_progn @R] else No
       | if A^is_var_sym then [[let_ [[A B]] R]]
         else
           | A coma_list_normalize A
           | if case A [`[]` @Bs] Bs.all(?^is_var_sym) then
               [(expand_destructuring B A.tail R)]
             else [(expand_match B [[A R]]
                                 [_fatal "couldnt match [B] to [A]"] No)]

supply_meta Object Source =
| when Source.is_meta and not Object.is_meta:
  | Object =  meta Object Source.meta_
| Object

is_singleton_block Xs =
  less Xs.n >< 1: ret 0
  X Xs.0
  when case X [`=` @Zs] 1: ret 0
  less X.is_list: ret 1
  when X.end: ret 1
  case X [[`!` N]]: ret 0
  Vs take_init_vars X.lead
  less Vs.end: ret 0
  case X [`:` A<[_] _]:
    Vs take_init_vars A
    less Vs.end: ret 0
  1

expand_block Xs =
  when Xs^is_singleton_block:
    Z expand_block_item_qlmb Xs.0
    case Z [_insert [`|` @Xs]]: ret: expand_block_item_insert Xs
    ret Z
  Ms Ys []
  Xs normalize_block_items Xs
  for X Xs:
    case X:
      [`=>` A B] = push X Ms
      Else = push X Ys
  less Ms.end: push Ms.f^make_multimethod Ys
  Xs = Ys.f
  Xs = map X Xs:
    Src when X.is_meta: X.meta_
    Rs let GSrc (if got Src then Src else GSrc):
          expand_block_item X
    when X.is_meta: Rs =  map R Rs: meta R X.meta_
    Rs
  Xs = Xs.j
  R:
  for X Xs.f:
    [A B] X
    when B.is_list: B =  supply_meta B X
    R = expand_block_helper R A B
  R = [_progn @R]
  Bs Xs.keep(X => X.0.is_keyword)
  when Bs.n: R =  [let_ (map B Bs [B.0 No]) R]
  R

`|` @Xs = expand_block Xs
`;` @Xs = expand_block Xs
`@` X = [_insert [_nomex X]]

let_ @As = [_call [_fn (map B As.0 B.0) [_progn @As.tail]]
                  @(map B As.0 B.1)]


expand_ret Name Value =
  [R End] result_and_label Name
  [_progn [_set R Value] [_goto End]]

ret @As = case As
  [Name Value] | expand_ret Name Value
  [Value] | when no GDefaultLeave: mex_error "missing default ret"
          | expand_ret GDefaultLeave Value
  [] | when no GDefaultLeave: mex_error "missing default ret"
     | expand_ret GDefaultLeave No
  Else | mex_error "errorneous ret syntax"

as @As = case As
  [Value Expr] | form: as ~Name Value Expr
  [Name Value Expr] | form | Name Value
                           | Expr
                           | Name

callcc F =
| K @rand 'K'
| R @rand 'R'
| [`|` [K [_setjmp]]
       [`if` [_mcall K is_int]
             [F [_fn [R] [_longjmp K [_list R]]]]
             [_mcall K '.' 0]]]

fin Finalizer Body =
| B @rand b
| F @rand f
| R @rand 'R'
| [[_fn [B] [B [_fn [] Finalizer]]]
   [_fn [F]
     ['|' [_set_unwind_handler ['&' F]]
          [R Body]
          [_remove_unwind_handler]
          [F]
          R]]]

compile_when @Conds Body =
| Xs map C Conds: case C
     [`-` X] | not rt_get X
     Else  | rt_get C
| if Xs.all(1) then form @(`|` Body) else 0

copy_ffi SF DF =
| when not DF.exists or DF.time < SF.time:
  | fs_copy SF DF
  | fs_chmod 755 DF

// Runtime DLLs that have to sit next to go.exe for the ui plugin's
// SDL-import-table to resolve at load time. Kept here (vs each
// project committing its own copies) so a user who's never run
// `make plugins` still gets a working window the first time their
// project does `use uim`. Hardcoded list because Symta has no
// directory-glob primitive at compile time; revise when SDL ships
// new sidecar DLLs or when ui swaps to a renderer that doesn't
// need them. See symta/sdl/README.md.
SDLBundle ['SDL2.dll' 'SDL2_mixer.dll' 'libFLAC-8.dll' 'libmikmod-2.dll'
           'libmodplug-1.dll' 'libogg-0.dll' 'libvorbis-0.dll'
           'libvorbisfile-3.dll' 'smpeg2.dll']

stage_ui_dlls Root Build =
| Src "[Root]sdl/"
| less Src.exists:
  | mex_error "Missing [Src] (ui plugin needs SDL DLLs staged from symta/sdl/)"
| for F SDLBundle:
  | SF "[Src][F]"
  | DF "[Build][F]"
  | when SF.exists: copy_ffi SF DF
| 0

// Compiler-supplied default fonts staged from symta/ttf/ into a
// project's ttf/. We only do this for projects that DON'T already
// have a ttf/ directory of their own — that's the signal "I have
// my own font collection, please don't drop strangers in". The
// SoM game ships ttf/sarabun*; touching nothing keeps Sarabun the
// game's default. A fresh symta project gets Inter (OFL, designed
// for screens). See symta/ttf/README.md for swap-out steps.
TTFBundle ['inter.ttf' 'inter_license.txt']

stage_ttf_fonts Root Build =
| TTFDir "[Build]ttf/"
| less TTFDir.exists:
  | TTFDir.mkpath
  | Src "[Root]ttf/"
  | less Src.exists:
    | mex_error "Missing [Src] (ttf plugin needs default fonts staged from symta/ttf/)"
  | for F TTFBundle:
    | SF "[Src][F]"
    | DF "[TTFDir][F]"
    | when SF.exists: copy_ffi SF DF
| 0

FFI_AutoExport 0

ffi_begin AutoExport Name =
| FFI_AutoExport = AutoExport
| [Root Build Srcs] GModuleFolders()
| RootFFI "[Root]ffi/[Name].ffi"
| BldFFI "[Build]ffi/[Name].ffi"
// Search order:
//   1. Symta install ffi/<Name>.ffi (standard plugins: gfx, ui, ttf, svg, vfx)
//   2. Project-local ffi/<Name>.ffi (self-contained test suites that stage
//      their own .ffi without polluting the install dir, e.g. tests/ffi/)
// In case 2 the .ffi is already at BldFFI so no copy is needed.
| less RootFFI.exists or BldFFI.exists:
  | mex_error "Missing [RootFFI]"
| "[Build]ffi/".mkpath
| when RootFFI.exists: copy_ffi RootFFI BldFFI
| when Name >< \ui:  stage_ui_dlls   Root Build
| when Name >< \ttf: stage_ttf_fonts Root Build
| FFI_Lib = form \Name
| 0

//for defining inline macros
mac F =
| [Root Build Srcs] GModuleFolders()
| case F
   [`=` [Name @Args] Body]
     | Fn [`=>` Args Body].eval(BuildFolder!Build)
     | if Fn.is_bterror: bad "mac `[Name]`: [Fn.text]"
     | GMacros.Name = new_macro Name Fn
   Else | bad "invalid mac syntax in `[F]`"
| 0

expand_ffi Name Result Symbol Args =
| FFI_Package GSrc.2.url.1 // determine package from current filename
| F "FFI_[FFI_Package]_[Name]_"
| ATs map A Args A.2 // argument types
| ANs map A Args A.1 // argument names
| Export if FFI_AutoExport><export: [export Name] else [0]
| if FFI_AutoExport><macro:
  | R form @| F ffi_load FFI_Lib \Symbol
            | mac: Name $@ANs =
                   | ~F \F
                   | form: _ffi_call \(Result $@ATs) ~F $@ANs
  | ret R
| R form @| F ffi_load FFI_Lib \Symbol
          | Name $@ANs = _ffi_call \(Result $@ATs) F $@ANs
          | $@Export
| R

ffi @Xs = case Xs
  [[`.` Symbol Result] @Args] | expand_ffi Symbol Result Symbol Args
  [Name [[`.` Symbol Result] @Args]] | expand_ffi Name Result Symbol Args
  Else | mex_error "ffi: bad arglist = [Xs]"

exports_preprocess Xs = 
| map X Xs: case X
    [`\\` N] | V if N.is_keyword then [`&` N] else N
             | [_list [_quote N] [new_macro [_quote N] V] ]
    Else | V if X.is_keyword then [`&` X] else X
         | [_list [_quote X] V]

export @Xs =
| GExports =: @Xs @GExports
| 0

export_ =: _list @GExports^exports_preprocess

handle_extern X =
| P X.locate('?')
| L X.n
| less got P and P > 0 and P < L-1: ret X
| Pkg X.take(P)
| Sym X.drop(P+1)
| [Pkg Sym]

mex_extern Pkg Name =
  Sym load_symbol Pkg Name
  when Sym.is_macro:
    when Name.is_keyword:
      mex_error "cant reference macro's value in [Pkg]?[Name]"
    ret: mex Sym.expander
  ret: mex: form: let_ ((~R (_import (_quote Pkg) (_quote Name)))) ~R

normalize_arg X =
  if X.is_keyword:
    case X^handle_extern:
      [Pkg Name] = mex_extern Pkg Name
      Else = [_quote X]
  else imex X

mex_table Xs =
  Var No
  case Xs [[`!` V]]:
    Var = V
    Xs = []
  case Xs [[`!`]]: Xs = []
  when no Var: case Xs [[`!` V] [`!` X]@_]:
    Var = V
    Xs = Xs.tail
  As norm_keywords 0 Xs
  T form ~T
  Tbl if As.n:
        form: `|` (T hmap_)
                  $@(map [K V] As:
                      if K >< No: form: T.setNo V
                      else
                        when V.is_text: V = form \V
                        form: T.K = V)
                  T
      else form: hmap_
  if got Var: Tbl = form @| Var $@Tbl
  Tbl

mex_try_special X Xs =
  less Xs.end: case X [`.`+`$` @_]:
    case Xs [[`:` _ _]]:
      Xs = [Xs] //escape, otherwise `()` treat it as O(:) matcher
    ret: mex:: `()` X @Xs
  when X >< `!`:
    case Xs [[`!`_]@_]: ret: mex: mex_table Xs
    ret: mex: mex_table [[X@Xs]]
  case X [`!` _]: ret: mex: mex_table [X@Xs]
  case X [`@` Z]:
    case Z nullary_,N: Z = N
    case Z [N @Zs]:
      Z = N
      Xs =: @Zs @Xs
    ret: mex [_mcall Xs.~ Z @Xs.lead] //method call
  when got Xs.locate($0 [`@` X]=>1):
    when X >< _mcall: ret: mex: form:
      _mcall [$(Xs.0) $@(Xs.drop(2))] apply_method (_method $(Xs.1))
    when X.is_keyword: X =: '&' X
    ret: mex: form [$@Xs].apply(X)
  0

mex_normal X Xs Rec =
#if #NCM_TRACE_MACROS
| mtrace_say \mex_normal [X@Xs]
#endif
| when GExpansionDepth > GExpansionDepthLimit:
  | mex_error "macroexpansion depth exceed at [[X@Xs]]"
| Macro when X.is_keyword:
  | case GMexLets.X 0,R: ret: if Rec: mex R else R
  | GMacros.X
| when X.is_text: case X^handle_extern [Pkg Sym]: when Sym.is_keyword:
  | M load_symbol Pkg Sym
  | if M.is_macro
    then Macro =  M
    else | S Sym.rand
         | R [let_ [[S [_import [_quote Pkg] [_quote Sym]]]] [S @Xs]]
         | ret: if Rec: mex R else R
| when no Macro:
  | S mex_try_special X Xs
  | when S: ret S
  | Ks NewXs []
  | CurKW No
  | for X Xs:
      if got CurKW then
        push [CurKW X] Ks
        CurKW = No
      elif case X [`!` A] A.is_text then
        CurKW = X.1.d
      else
        push X NewXs
  | when Ks.n: Xs =: @NewXs.f @Ks.f.j
  | Y if Rec: X^mex else X
  | if (X.is_list and not Y.is_list) or (X.is_text and X <> Y)
    then ret: if Rec: mex [Y@Xs] else [Y@Xs]
    else ret [Y @(map X Xs: normalize_arg X)]
| Expander Macro.expander
| NArgs Expander.nargs
| when NArgs >> 0 and NArgs <> Xs.n:
  | [Row Col Orig] GSrc
  | mex_error "bad number of args to macro [Macro.name]"
| R Xs.apply(Expander)
| if Rec: mex R else R

mdbg X =
  X normalize_nesting X
  if X.is_list: X = mex_normal X.head X.tail 0
  ['\\' X]

normalize_nesting O =
| case O [X] | if X.is_keyword then O else normalize_nesting X
         X | X



//used to handle _insert outside of `|`.
//These almost never happen, so we commented it out
/*imex Expr =
  EExpr mex Expr
  case EExpr [_insert X]: EExpr = mex X
  EExpr*/

imex Expr = mex Expr

hcase MexFormCases Expr ()
  [_fn As Body] | [_fn As Body^imex]
  [_set Place Value]
    | [_set Place (if Value.is_keyword: [_quote Value] else imex Value)]
  [_label Name] | Expr
  [_goto Name] | Expr
  [_quote X] | if X.is_list: expand_quoted_list X else Expr
  [_nomex X] | X // no macroexpand
  [_type Type Var Body] | let GVarsTypes [[Var Type]@GVarsTypes]: mex Body
  [`&` O] | if O.is_keyword then O else bad "implement `&Var` ([O])" // [O^mex]

mex ExprIn =
#if #NCM_TRACE_MACROS
| mtrace_say \mex ExprIn
#endif
| when no GMacros: mex_error 'lib_path got clobbered again'
| Expr normalize_nesting ExprIn
| when Expr.is_text
  | case Expr^handle_extern [Pkg Name]: ret: mex_extern Pkg Name
  | when not Expr.is_keyword and got GMacros.Expr: Expr =  GMacros.Expr.expander
| when not Expr.is_list or Expr.end:
  | when Expr.is_table:
    | T Expr.l{[[`!` ?0] [`\\` ?1]]}.j
    | Expr = mex T
  | ret Expr
| R let GExpansionDepth GExpansionDepth+1: hcase_go MexFormCases Expr ()
      (Head NArgs =>
          mex_error "special form `[Head]` expects [NArgs] arguments")
  | Src when Expr.is_meta: Expr.meta_ 
  | let GSrc (if got Src then Src else GSrc)
    | mex_normal Expr.head Expr.tail 1
| when R.is_list: R = supply_meta R ExprIn
| R

macroexpand Expr Macros ModuleCompiler ModuleFolders =
#if #NCM_TRACE_MACROS
| mtrace_say \macroexpand Expr
#endif
| let GMacros Macros
      GExpansionDepth 0
      GExports []
      GModuleCompiler ModuleCompiler
      GModuleFolders ModuleFolders
      GTypes (!)
      GMexLets (!)
      GLastType 0
  | R mex Expr
  | R,GExports

list @Xs = form [$@Xs]

mtx @Xs =
| Ys map X Xs: case X [`|` @Zs] (Zs{[`[]` @?]}) X [X]
| form [$@(Ys.j)]

rows Xs =: `[]` @Xs.0.tail{[`[]` @?]}

rowz Xs =: `[]` @Xs.0.tail

cons Field Xs = form
| ~R 0
| for ~X Xs
  | ~X.Field =  ~R
  | ~R =  ~X
| ~R

uncons Field Item = form
| ~Xs []
| ~X Item
| while ~X
  | ~Xs = [~X@~Xs]
  | ~X = ~X.Field
| ~Xs

same A B = [_same A B]

on @Xs X = [X @Xs]

hcase Name Expr Args @Cases =
| Cs Cases.group(2)
| HeadRnd @rand 'Hd'
| Cs map A,B Cs:
  | Pat: HeadRnd @A.drop(2)
  | N case A.~
        [`@` Xs] | -1
        Else | Pat.n
  | [A.1 N | form: _fn (Expr $@Args)
                     | [$@Pat] Expr
                     | B]
| form @| Name! $@| @j: map A,B,C Cs: form: A![B C]

hcase_go Cases Expr Args Err Default =
| form:
  | ~E Expr
  | ~H ~E.0
  | ~R 0
  | when ~H.is_text:
    | ~C Cases.~H
    | when got ~C:
      | ~N ~C.0
      | if ~N >< -1 or ~E.n >< ~N
        then ~R = (~C.1)(~E $@Args)
        else ~R = Err(~H ~N)
      | _goto ~end
  | _label ~default
  | ~R =  Default
  | _label ~end
  | ~R


//prx is useful for manual code unrolling and expression parametrization
//prx say hello _0 | \Antonina; \Nancy; \Catherine
//`__` = argument index
//`@interger` = argument is a list which should be interpolated after
//              dropping integer number of elements
prx @P As =
  as_arg X = if X.is_text and X.n><2 and X.0 >< _ and X.1.is_digit: X.1.int
  As = As(`|` [`;` @Bs] =: [`|` @Bs])(:[[`|` @As]] = As; [@_] =: Q)
  Bs map I,A As.i:
    case A:
      `=` [] Expr = Expr // pass as is
      Else =
        P(:@Xs ['@' N<1.is_int] @Ys =: @Xs^r @A.drop^N @Ys^r
          ;@Xs (N<-No)^as_arg @Ys =: @Xs^r A.N @Ys^r
          ;@Xs '__' @Ys =: @Xs^r I @Ys^r
          ;H@T = H^r@T^r
          ; =: )
  : `|` @Bs


// `help` macro -- newbie-friendly REPL doc lookup.  Lets users
// type `help say` or `help int.bump` without remembering atom
// quoting.  Converts the arg's AST shape into a text key at
// compile time, then emits a runtime call to `help_lookup_`.
//
//   help                    -> help_banner_
//   help say                -> help_lookup_ 'say'
//   help int.bump           -> help_lookup_ 'int.bump'
//   help list.keep          -> help_lookup_ 'list.keep'
//
// Walks the dot-form AST: `(`.` A B)` -> "A.B" (recursively).
ast_key X =
  if X.is_text then X
  else if X.is_list and X.n >< 3 and X.0 >< '.'
    then "[ast_key X.1].[ast_key X.2]"
    else "[X]"

help @Args = case Args:
  []  = form: help_banner_
  [X] = [help_lookup_ [_quote ast_key(X)]]
  Else = mex_error "help takes 0 or 1 arg"
