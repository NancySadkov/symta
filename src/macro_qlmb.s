// macro_qlmb.s -- quasi-lambda (QLMB) expansion + curly matchers.
// is_qlmb_set_var, is_acvar, is_kwacvar, expand_qlmb_r,
// expand_qlmb, expand_qlmb_curly, supply_text_transfer,
// qlmb_getcvars, acv_fin, acv_init, btret, qlmb_supply_cvars,
// expand_curly_matcher.
// Auto-included by macro.s via NCM #:macro_qlmb.s


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
