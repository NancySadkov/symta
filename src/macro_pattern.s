// macro_pattern.s -- pattern matching: hole expansion, case dispatch.
// Auto-included by macro.s via NCM #:macro_pattern.s

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

// TS-3.5: case-arm type narrowing.  For a predicate-arm
// pattern like `int?` / `text?` / `list?`, return the
// narrowing type name (without the `?`).  Else No.
// Used by expand_match to wrap the arm body in `[_type T X _]`
// so the matched variable's type is visible inside the arm.
case_narrow_type Pattern =
  | when Pattern.is_keyword and Pattern.is_text and Pattern.n >> 1:
    | when Pattern.~ >< '?':
      | Lead Pattern.lead
      | when Lead^is_known_type: ret Lead
  | No

expand_match Keyform Cases Default Key =
| when no Key: Key =  @rand 'Key'
| E @rand end
| D @rand default
| R @rand 'R'
| NarrowVar if Keyform^is_var_sym: Keyform else No
| Ys:
| for Case Cases.f
  | Name @rand c
  | NextLabel if Ys.end then D else Ys.0.1
  | Miss: _goto NextLabel
  // TS-3.5: when matched value is a bare var AND this case
  // arm is a single-predicate pattern (`int?` etc.), wrap the
  // arm body in `[_type T Var body]` so refs to Var inside
  // see Var as T.  Composes with TS-3.1's mex-time check.
  | Body Case.tail
  | NarrowT if got NarrowVar then case_narrow_type Case.head else No
  | when got NarrowT:
    | Body = [[`_type` NarrowT NarrowVar [`_progn` @Body]]]
  | Hit: _progn [_set R [_progn @Body]] [_goto E]
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
