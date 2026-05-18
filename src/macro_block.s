// macro_block.s -- lambdas, fn-defs, blocks, assignments, type decls.
// expand_sugar_lambda, (), [ list-builder, [], named/lambda helpers,
// default_ret_, supply_ret, =>, expand_block_item_fn,
// expand_destructuring, expand_assign, =, <=, type-decl machinery,
// expand_block_item_method, take_init_vars, expand_block_vars,
// normalize_block_items, doc helpers, expand_block_item,
// make_multimethod, expand_block_helper, expand_block, |, ;, @, let_.
// Auto-included by macro.s via NCM #:macro_block.s

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
              then | when A^is_var_sym:
                     | Type GVarsTypes.A
                     | when got Type:
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
       // TS-3.2: declaration `A B` where A is a fresh var and B
       // has a statically inferable type -- wrap the body in
       // `_type T A R` so refs to A inside R are checked against
       // T.  Builds on the existing `_type` mex arm which pushes
       // T into GVarsTypes for the body's mex pass.
       | if A^is_var_sym then
           | TypedR | InferredType infer_type B
                    | if got InferredType
                        then [`_type` InferredType A R]
                        else R
           | [[let_ [[A B]] TypedR]]
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

