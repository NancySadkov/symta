export produce_ssa ssa_to_sif

GEnv No
GOut No // where resulting assembly code is stored
GNVars No // number of variable slots allocated on stack
GNVarsInit 2
GCurFn No // unique name of current function
GCurProperFn No // unique name of current function with prologue
GFnMeta No
GFnIdCount No
GFnId No //id of the current function
GInits No // stuff called on module initialization
GBytes No
GStrings No
GRTypes No // resolved types
GRTypesCount No
GTypeParams No
GImports No
GImportsCount No
GTexts No
GTextsMap No
GTextsCount No
GSsv No   // HELP-3: per-module set-section-symbol-value entries collected
          // at compile time from `_ssv` intrinsic.  Each entry is
          // [SectionLabel SymbolLabel ValueLabel] where each label
          // is a `b<rand>` cstring reference into the data section.
          // Flushed as `t doc <sym> <val>` SIF directives in
          // ssa_to_sif_do_header (currently only the `docs` section).
GMethods No //resolved methods
GMethodsCount No
GImportLibs No
GImportLibsCount No
GFns No
GClosure No // other lambdas, this lambda references
GBases No
GUniquifyStack No
GSrc: 0 0 unknown
GLastSrc: 0 0 unknown // CORE-1: most recent (Row,Col) we emitted
                     //         an `lsrc` marker for, so we can
                     //         skip emission when the position
                     //         didn't change since the last
                     //         instruction.
GAll @rand all
GRestartPoint No

//DoDebug 0

type fnmeta name!0 size!0 nargs!0 origin!0:
  name!Name size!Size /*closure size*/ nargs!Nargs origin!Origin

// CORE-1: emit an `lsrc Row Col` SIF line before each `ssa` call
// whose source position has *advanced* past the last marker.
// We require monotonic forward progress because `uniquify_form`
// wraps each sub-expression in `let GSrc <child src>` which
// restores GSrc to the enclosing position when the child returns;
// without the forward check we'd emit a spurious `lsrc <header>`
// after every nested let-binding pops back, leaving stack traces
// stuck at the function header (which is exactly the CORE-1 bug
// we're fixing).
ssa @As = | R GSrc.0
          | C GSrc.1
          | LR GLastSrc.0
          | LC GLastSrc.1
          | when R > LR or (R >< LR and C > LC):
            | push [lsrc R C] GOut
            | GLastSrc = GSrc
          | push As GOut
          | No

ssaI @As = | push As GInits
           | No

get_parent_index Parent =
| P GClosure.0.locate(E => Parent >< E)
| when got P: ret P
| Parents GClosure.head
| GClosure =  [[@Parents Parent] @GClosure.tail]
| Parents.n

path_to_sym X Es =
| when Es.end: ret No
| [Head@Tail] Es
| when case Head [U@Us] U >< GAll // reference to the whole arg-list?
  | Head =  Head.1
  | less Head.0 >< X: ret (path_to_sym X Tail)
  | when same Es GEnv: ret [GAll No] // argument of the current function
  | ret [GAll (get_parent_index Head.1)]
| P Head.locate(V => X >< V.0)
| when no P: ret (path_to_sym X Tail)
| when same Es GEnv: ret [P No] // argument of the current function
| [P (get_parent_index Head.P.1)]

ssa_symbol K X Value =
| case (path_to_sym X GEnv)
     [Pos Parent]
       | Base \E
       | when got Parent
         | Base =  ssa_var "B"
         | ssa ld Base \P Parent
       | when Pos >< GAll //entire arglist
         | when got Value: compiler_error "cant set [X]"
         | ssa mv K Base
         | ret No
       | when no Value: ret (ssa ld K Base Pos)
       | if Base >< \E and GBases.n >< 1
         then ssa st Base Pos Value
         else ssa st Base Pos Value // must be copied into parent environment
     Else
       | compiler_error "unknown symbol `[X]` (compiler bug)"

cstring_bytes S = [@S.l{?code} 0]

ssa_cstring Str =
| It GStrings.Str
| when got It: ret It
| as Name 'b'.rand:
  | GStrings.Str =  Name
  | push [Name Str^cstring_bytes] GBytes

type register N: idx!N start!No end!No remapped!0

register.as_text = "L\[[$idx]\]"

ssa_var Name =
| R register GNVars
| GNVars+
| R

ev X = as R 'r'^ssa_var: ssa_expr R X

ssa_quote K X = if X.is_text then ssa mv K X^ssa_text
                else if X.is_list then compiler_error "ssa_quote: got list [X]"
                else ssa_expr K X

ssa_resolve Name = [Name GCurFn]

ssa_fn_body K F Args Body O Prologue Epilogue NVarsInit =
| LocalEnv if Args.is_text
           then [[GAll [Args F]] @GEnv]
           else [Args.map(A=>[A F]) @GEnv]
| let GBases   [[]]
      GOut     []
      GNVars   NVarsInit
      GCurFn   F
      GEnv     LocalEnv
      GClosure [[]@GClosure]
      GFnId    GFnIdCount
  | GFnIdCount+
  | when Prologue
    | NArgs if Args.is_text then -1 else Args.n 
    | GFnMeta.F =  fnmeta nargs!NArgs origin!GSrc
    | when NArgs<>-1: ssa cnas NArgs
  | when no K: K =  ssa_var result
  | if Prologue then let GCurProperFn GCurFn | ssa_expr K Body
    else ssa_expr K Body
  | when Epilogue: ssa leave K
  | [GNVars GOut.f GClosure.0]

// FIXME:
// check if we really need new closure here, because in some cases we can reuse parent's closure
// a single argument to a function could be passed in register, while a closure would be created if required
// a single reference closure could be itself held in a register
// for now we just capture required parent's closure
ssa_fn K Args Expr O =
| F @rand f
| [NVars Body Cs] ssa_fn_body No F Args Expr O 1 1 GNVarsInit
| push [[label F] [subr NVars] @Body] GFns
| NParents Cs.n
| ssa closure K F NParents
| GFnMeta.F.size =  NParents
| for [I C] Cs.i: if same C GCurFn // self?
                  then ssa st K I \E
                  else ssa cp K I \P C^get_parent_index

ssa_if K Cnd Then Else =
  ThenLabel @rand `then`
  EndLabel @rand endif
  case Cnd:
    [_tag X] = //optimize checking for fixnum, used by arith macros
      swap ThenLabel EndLabel
      swap Then Else
      ssa iffxn X^ev ThenLabel
    Else =
      ssa b Cnd^ev ThenLabel
  ssa_expr K Else
  ssa jmp EndLabel
  ssa label ThenLabel
  ssa_expr K Then
  ssa label EndLabel

//FIXME: currently hoisting may clobber some toplevel syms;
//       make new syms valid only downstream
ssa_hoist_decls Expr Hoist = // combine layered lets into single one
| less Expr.is_list: ret Expr
| case Expr
     [_fn @Xs] | Expr
     [[_fn As @Xs] @Vs]
       | Vs =  Vs.map(V => ssa_hoist_decls V Hoist)
       | if As.is_text
         then | Hoist([As])
              | [_progn [_set As [_list @Vs]]
                        @Xs.map(X => ssa_hoist_decls X Hoist)]
         else | Hoist(As)
              | [_progn @As.map(A => [_set A Vs^pop])
                        @Xs.map(X => ssa_hoist_decls X Hoist)]
     Xs | Xs.map(X => ssa_hoist_decls X Hoist)

ssa_let K Args Vals Xs =
| Body ssa_hoist_decls [_progn @Xs]: Hs =>
       | Args =  [@Args @Hs]
       | Vals =  [@Vals @Hs.map(H => 0)]
| less Args.n:
  | ssa_expr K Body
  | ret No
| F @rand f
| [FnNVars SsaBody Cs] ssa_fn_body K F Args Body [] 0 0 GNVars
| GNVars =  FnNVars
| NParents Cs.n
| P 0
| when NParents:
  | P =  ssa_var l // parent environment
  | ssa closure P 0 NParents
| for [I C] Cs.i: if same C GCurFn // self?
                  then ssa st P I \E
                  else ssa cp P I \P C^get_parent_index
| E ssa_var env
| ssa_list E Vals
| SaveP 0
| SaveE ssa_var save_e
| when 1://NParents: //even with no parents it could require restoration
  | SaveP =  ssa_var save_p
  | ssa mv SaveP \P
| ssa mv SaveE \E
| ssa mv \E E
| when NParents: ssa mv \P P
| for S SsaBody: push S GOut
| when 1://NParents:
  | ssa mv \P SaveP
| ssa mv \E SaveE

ssa_apply K F As =
  case F [_fn Bs @Body]: less Bs.is_text: ret: ssa_let K Bs As Body
  let GBases [[] @GBases]:
    H ev F
    Vs map A As: ev A
    ssa arglist As.n
    for [I V] Vs.i: ssa starg I V
    if F.is_keyword then ssa call K H else ssa callt K H

resolve_type Name =
  It GRTypes.Name
  when got It: ret It.1
  N GRTypesCount+
  R "ty\[[N]\]"
  GRTypes.Name =  [N R Name^ssa_cstring]
  R

resolve_method Name =
  It GMethods.Name
  when got It: ret It.1
  N GMethodsCount+
  R "mt\[[N]\]"
  GMethods.Name =  [N R Name^ssa_cstring]
  R

ssa_import K Lib Symbol =
  Lib = Lib.1
  Symbol = Symbol.1
  Key "[Lib]::[Symbol]"
  Im GImports.Key
  when no Im:
    N GImportsCount+
    R "im\[[N]\]"
    Im =: N R Key GImportLibs.Lib Symbol^ssa_cstring
    GImports.Key =  Im
  ssa mv K Im.1

ssa_apply_method K Name O As =
  let GBases [[] @GBases]: named block:
    As =  [O@As]
    Vs map A As: ev A
    ssa arglist As.n
    for [I V] Vs.i: ssa starg I V
    ssa mcall K Vs.0 Name.1^resolve_method

ssa_set K Place Value =
  R ev Value
  ssa_symbol No Place R
  ssa mv K R

// FIXME: _label should be allowed only inside of _progn
ssa_progn K Xs =
  when Xs.end: Xs =  [[]]
  D 'dummy'
  for X Xs:
    case X [_label Name]:
      GBases = [["[Name]$[GFnId]" @GBases.head] @GBases.tail]
  till Xs.end:
    X pop Xs
    when Xs.end: D =  K
    // CORE-1: every statement in a block has its own source
    // position via X.meta_.  Update GSrc into that scope so the
    // `ssa` wrapper emits an `lsrc Row Col` line at the right
    // place even when the inner expression doesn't carry meta
    // itself (bare identifier calls, simple atoms, etc).
    Src X.meta_
    if got Src: let GSrc Src: ssa_expr D X
      else ssa_expr D X
    when Xs.end and case X [_label@Zs] 1: ssa_atom D No

compiler_error Msg =
  [Row Col Orig] GSrc
  bad "[Orig]:[Row],[Col]: [Msg]"

expr_symbols_sub Expr Syms =
  if Expr.is_text then Syms.Expr =  1
  else when Expr.is_list: map X Expr: expr_symbols_sub X Syms 

expr_symbols Expr =
  Syms (!)
  expr_symbols_sub Expr Syms
  Syms

uniquify_let Xs =
  case Xs [[_fn As @Body] @Vs]:
    when As.is_text: ret Xs
    when As.n <> Vs.n: compiler_error "bad number of arguments in [Xs]"
    when no Vs.find(V => case V [_import Lib Sym] 1): ret Xs
    Used expr_symbols Body
    NewAs:
    NewVs:
    till As.end:
      A pop As
      V pop Vs
      case V:
        [_import [_quote Lib] [_quote Sym]] =
          when no GImportLibs.Lib: GImportLibs.Lib = GImportLibsCount+
          when got Used.A:
            push A NewAs
            push V NewVs
        Else =
          push A NewAs
          push V NewVs
    Xs =: [_fn NewAs.f @Body] @NewVs.f
  Xs


uniquify_form Expr =
| Src Expr.meta_
| R let GSrc (if got Src then Src else GSrc)
  | case Expr
    [_fn As @Body]
      | Bs if As.is_text then [As] else As
      | BadArg Bs.find(?.is_text^not)
      | when got BadArg: compiler_error "invalid argument [BadArg]"
      | when Bs.n <> Bs.uniq.n: compiler_error "duplicate args in [Bs]"
      | Rs Bs.map([? ?.rand])
      | let GUniquifyStack [Rs @GUniquifyStack]
        | Bs Rs.map(?.1)
        | Bs if As.is_text then Bs.0 else Bs
        | [_fn Bs @Body.map(&uniquify_expr)]
    [_quote X] Expr
    [_label X] Expr
    [_goto X] Expr
    [_call @Xs] Xs^uniquify_form
    Xs | Xs =  uniquify_let Xs
       | Xs.map(&uniquify_expr)
| when got Src: R =  meta R Src
| R

uniquify_name S =
| for Closure GUniquifyStack: for X Closure: when X.0 >< S: ret X.1
| No

uniquify_atom Expr =
| less Expr.is_text: ret Expr
| when Expr.n and Expr.0 >< _: ret Expr
| Renamed uniquify_name Expr
| when no Renamed: compiler_error "undefined variable `[Expr]`"
| Renamed

uniquify_expr Expr = if Expr.is_list
                     then uniquify_form Expr
                     else uniquify_atom Expr

uniquify Expr = //gives each variables unique name
| let GUniquifyStack []
  | R uniquify_expr Expr
  | R

ssa_list K Xs =
| less Xs.n: ret: ssa mv K 'Empty'
| when Xs.n >< 1:
  | // RT-9: fused size-1 list literal.  Size-1 is the most
  | // common bucket (65% of alloc traffic); the LITERAL subset
  | // shrinks by one dispatch per [X].  Preserve the legacy
  | // [0] optimization: bare SBC_LIST 1 leaves the slot at the
  | // default 0 value, so no store is needed.
  | when 0><Xs.0:
    | ssa list K 1
    | ret
  | A Xs.0^ev
  | ssa list1 K A
  | ret
| when Xs.n >< 2:
  | // RT-9: fused size-2 list literal -- SBC_LIST2 K A B allocates
  | // and stores in one opcode (vs SBC_LIST + 2x SBC_STOR).
  | // size-2 LISTs are ~25% of all alloc traffic on a ./game compile;
  | // this cuts their bytecode dispatch count 3:1.
  | A Xs.0^ev
  | B Xs.1^ev
  | ssa list2 K A B
  | ret
| ssa list K Xs.n
| H Xs.0
| less H.is_list: when Xs.all(H):
  | when 0><H: ret
  | V ev H
  | for [I X] Xs.i: ssa st K I V
  | ret
| for [I X] Xs.i: when X: ssa st K I X^ev

ssa_data K Type Xs =
| Size Xs.n
| TypeName Type.1
| TypeVar resolve_type TypeName
| when no GTypeParams.TypeName:
  | GTypeParams.TypeName =  1
  | ssaI tinit TypeVar Size Type.1^ssa_text
| ssa object K TypeVar Size
| for [I X] Xs.i: ssa st K I X^ev

ssa_subtype K Super Sub =
| ssa subtype Super.1^resolve_type Sub.1^resolve_type
| ssa mv K 0

ssa_dget K Src Off =
| less Off.is_int: compiler_error "dget: offset must be integer"
| ssa ld K Src^ev Off

ssa_dset K Dst Off Value =
| less Off.is_int: compiler_error "dset: offset must be integer"
| D ev Dst
| V ssa_var r
| ssa_expr V Value
| ssa st D Off V
| ssa mv K V

ssa_dmet K MethodName TypeName Handler =
| MethodVar MethodName.1^resolve_method
| TypeVar resolve_type TypeName.1
| ssa dmet MethodVar TypeVar Handler^ev
| ssa mv K 0

ssa_label Name = ssa label "[Name]$[GFnId]"

ssa_goto Name =
| MName "[Name]$[GFnId]"
| N GBases.locate(B => got B.locate(?><MName))
| when no N: compiler_error "cant find label [Name]"
| ssa jmp MName

ssa_mark Name =
| GFnMeta.GCurProperFn.name =  Name
| ssa cmnt "function [Name]"

// CORE-8 #1: constant folding for the fixnum arithmetic /
// comparison / bitwise opcodes.  When both arguments are
// integer literals at compile time, evaluate the operation in
// Symta and emit a single `ldfxn` instead of three
// (`ldfxn A; ldfxn B; fxnop K A B`).  Saves two instructions
// per fold and frees the two intermediate SSA slots.  Pervasive:
// the compiler itself, macros, and most arithmetic-heavy code
// hit this on every literal pair.
//
// We factor each fold through a small named helper because case-
// arm trailers can't carry an arbitrary infix expression (the
// Symta grammar parses `pattern = something + something_else` as
// a malformed assignment); single-call trailers are fine.

fadd_ A B = A + B
fsub_ A B = A - B
fmul_ A B = A * B
fdiv_ A B = A / B
frem_ A B = A % B
fand_ A B = A -*- B
fior_ A B = A -+- B
fxor_ A B = A -^- B
fshl_ A B = A -<- B
fshr_ A B = A ->- B
finc_ X = X + 1
fdec_ X = X - 1
fneg_ X = 0 - X
fabs_ X = if X < 0 then 0 - X else X

ssa_fixed1 K Op X =
  if X.is_int and Op >< 'neg' then ssa_atom K (fneg_ X)
  elif X.is_int and Op >< 'abs' then ssa_atom K (fabs_ X)
  elif X.is_int and Op >< 'inc' then ssa_atom K (finc_ X)
  elif X.is_int and Op >< 'dec' then ssa_atom K (fdec_ X)
  elif X.is_int and Op >< 'not' then ssa_atom K (if X >< 0 then 1 else 0)
  else ssa Op K X^ev

ssa_fixed2 K Op A B =
  if A.is_int and B.is_int:
    // TS-4.1: the unboxed `iadd`/`isub`/... constant-fold the
    // same way the tag-aware `fxnadd`/... do.  Listing both
    // names per branch lets the macro layer feed either form
    // without compiler.s caring whether the static checker
    // upstream proved a type.
    if Op >< 'fxnadd' or Op >< 'iadd' then ssa_atom K (fadd_ A B)
    elif Op >< 'fxnsub' or Op >< 'isub' then ssa_atom K (fsub_ A B)
    elif Op >< 'fxnmul' or Op >< 'imul' then ssa_atom K (fmul_ A B)
    elif (Op >< 'fxndiv' or Op >< 'idiv') and B <> 0 then ssa_atom K (fdiv_ A B)
    elif (Op >< 'fxnrem' or Op >< 'irem') and B <> 0 then ssa_atom K (frem_ A B)
    elif Op >< 'fxnand' then ssa_atom K (fand_ A B)
    elif Op >< 'fxnior' then ssa_atom K (fior_ A B)
    elif Op >< 'fxnxor' then ssa_atom K (fxor_ A B)
    elif Op >< 'fxnshl' and B >> 0 and 63 >> B then ssa_atom K (fshl_ A B)
    elif Op >< 'fxnshr' and B >> 0 and 63 >> B then ssa_atom K (fshr_ A B)
    elif Op >< 'fxneq'  then ssa_atom K (if A >< B then 1 else 0)
    elif Op >< 'fxnne'  then ssa_atom K (if A <> B then 1 else 0)
    elif Op >< 'fxnlt'  then ssa_atom K (if A <  B then 1 else 0)
    elif Op >< 'fxngt'  then ssa_atom K (if A >  B then 1 else 0)
    elif Op >< 'fxnlte' then ssa_atom K (if A << B then 1 else 0)
    elif Op >< 'fxngte' then ssa_atom K (if A >> B then 1 else 0)
    else ssa Op K A^ev B^ev
  else ssa Op K A^ev B^ev
  // NOTE: one-sided identity folds (X+0 → X, etc.) were tried
  // and produced wrong output for `0 + No` -- the existing
  // `[_add 0 No]` path goes through fxnadd's bit-level int
  // semantics, which has non-trivial behaviour on the T_NO tag
  // (returns 0.0 because No happens to share bits with float
  // representation in the int range).  Leaving identity folds
  // to a future pass that knows the static type of both args.

ssa_fixed3 K Op A B C = ssa Op K A^ev B^ev C^ev

ssa_listn K N = ssa fxnlistn K N^ev

ssa_text String =
  when String.is_fixtext: ret "FIXTEXT([String.code]ULL)"
  It GTextsMap.String
  when got It: ret It
  push String^ssa_cstring GTexts
  as Tx "tx\[[GTextsCount+]\]": GTextsMap.String = Tx

// `_ssv` compile-time intrinsic.  Each arg is a `(_quote "...")`
// node from the macro; we pull out the string and pin it as a
// cstring in the data section, recording (Section, Symbol, Value)
// label triple in GSsv for emission to a SIF `t doc` directive.
ssa_ssv K As =
| Section get_quote_text As.0
| Symbol  get_quote_text As.1
| Value   get_quote_text As.2
| push [Section^ssa_cstring Symbol^ssa_cstring Value^ssa_cstring] GSsv
| ssa mv K 0

get_quote_text X =
  case X:
    [_quote S] = S
    Else = compiler_error "_ssv arg must be a quoted string, got [X]"

ssa_ffi_call K Type F As =
| F =  ev F
| As =  map A As: ev A
| Type =  map X Type.tail: X.1
| [ResultType @AsTypes] Type
| less As.n >< AsTypes.n:
  | compiler_error "argument number doesn't match signature"
| for A As:
  | T pop AsTypes
  | ssa nfa A T
| ssa nfi F
| ssa nfr K ResultType

hcase SsaFormCases Xs (K)
  [_fn As Body] | ssa_fn K As Body Xs
  [_if Cnd Then Else] | ssa_if K Cnd Then Else
  [_quote X @Xs] | ssa_quote K X
  [_set Place Value] | ssa_set K Place Value
  [_progn @Xs] | ssa_progn K Xs
  [_label Name] | ssa_label Name
  [_goto Name] | ssa_goto Name
  [_mark Name] | ssa_mark Name.1
  [_data Type @Xs] | ssa_data K Type Xs
  [_subtype Super Sub] | ssa_subtype K Super Sub
  [_dget Src Index] | ssa_dget K Src Index
  [_dset Dst Index Value] | ssa_dset K Dst Index Value
  [_dmet Method Type Handler] | ssa_dmet K Method Type Handler
  [_mcall O Method @As] | ssa_apply_method K Method O As
  [_list @Xs] | ssa_list K Xs
  [_listn N] | ssa_listn K N
  [_text X] | ssa mv K X^ssa_text
  [_import Lib Symbol] | ssa_import K Lib Symbol
  [_add A B] | ssa_fixed2 K fxnadd A B
  [_sub A B] | ssa_fixed2 K fxnsub A B
  [_mul A B] | ssa_fixed2 K fxnmul A B
  [_div A B] | ssa_fixed2 K fxndiv A B
  [_rem A B] | ssa_fixed2 K fxnrem A B
  // TS-4.1: typed-int arithmetic.  Emitted by macro_ops's `+`/`-`/
  // `*`/`/`/`%` macros when infer_type proves both operands int.
  // Same constant-folding path as the fxn variants when both are
  // literals; the runtime opcode skips the tag check otherwise.
  [_iadd A B] | ssa_fixed2 K iadd A B
  [_isub A B] | ssa_fixed2 K isub A B
  [_imul A B] | ssa_fixed2 K imul A B
  [_idiv A B] | ssa_fixed2 K idiv A B
  [_irem A B] | ssa_fixed2 K irem A B
  [_eq A B] | ssa_fixed2 K fxneq A B
  [_ne A B] | ssa_fixed2 K fxnne A B
  [_lt A B] | ssa_fixed2 K fxnlt A B
  [_gt A B] | ssa_fixed2 K fxngt A B
  [_lte A B] | ssa_fixed2 K fxnlte A B
  [_gte A B] | ssa_fixed2 K fxngte A B
  [_and A B] | ssa_fixed2 K fxnand A B
  [_ior A B] | ssa_fixed2 K fxnior A B
  [_xor A B] | ssa_fixed2 K fxnxor A B
  [_shl A B] | ssa_fixed2 K fxnshl A B
  [_shr A B] | ssa_fixed2 K fxnshr A B
  [_same A B] | ssa_fixed2 K same A B
  [_vary A B] | ssa_fixed2 K vary A B
  [_lget A B] | ssa_fixed2 K fxnlget A B
  [_lset A I V] | ssa_fixed3 K fxnlset A I V
  [_size A] | ssa_fixed1 K fxnsize A
  [_fatal Msg] | ssa fatal Msg^ev
  [_this_method] | ssa curmet K
  [_method_name Method] | ssa mname K Method^ev
  [_method Name] | ssa mv K: resolve_method Name.1
  [_abs O] | ssa abs K O^ev
  [_neg O] | ssa neg K O^ev
  [_dec O] | ssa dec K O^ev
  [_inc O] | ssa inc K O^ev
  [_not O] | ssa not K O^ev
  [_got O] | ssa got K O^ev
  [_no O] | ssa no K O^ev
  [_gid O] | ssa gid K O^ev
  [_tag X] | ssa_fixed1 K fxntag X
  [_setjmp] | ssa setjmp K
  [_longjmp State Value] | ssa longjmp State^ev Value^ev
  [_set_unwind_handler H] | ssa set_unwind_handler H^ev
  [_remove_unwind_handler] | ssa remove_unwind_handler
  [_ffi_call Type F @As] | ssa_ffi_call K Type F As
  [_ffi_get Type Ptr Off] | ssa nld K Type.1 Ptr^ev Off^ev
  [_ffi_set Type Ptr Off Val] | ssa nst Type.1 Ptr^ev Off^ev Val^ev
                              | ssa mv K 0
  // `_ssv <section> <symbol> <value>` -- set-section-symbol-value
  // compile-time intrinsic.  See ssa_ssv below.
  [_ssv @As] | ssa_ssv K As

ssa_form K Xs =
| when Xs.end:
  | ssa_atom K No
  | ret  
| Src Xs.meta_
| let GSrc (if got Src then Src else GSrc):
  hcase_go SsaFormCases Xs (K)
      (Head NArgs =>
          compiler_error "special form `[Head]` expects [NArgs] arguments")
  | ssa_apply K Xs.0 Xs.tail
| 0

ssa_atom K X =
| if X.is_int then
    | when X > 0x7FFFFFFF or X < -0x7FFFFFFF: X =  "[X]LL" //FIXME: kludge
    | ssa ldfxn K X
  else if X.is_text then ssa_symbol K X No
  else if no X then ssa mv K 'No'
  else if X.is_float then ssa ldflt K X
  else compiler_error "bad atom: [X]"

ssa_expr K X = if X.is_list then ssa_form K X else ssa_atom K X

ssa_fnmeta_entry Fn Name Size NArgs Origin =
| OrigString ssa_cstring "[Origin.2]"
| NameBytes if Name then Name^ssa_cstring else 0
| [Size NArgs NameBytes Fn Origin.0 Origin.1 OrigString]

find_nearest_meta Expr =
| if Expr.is_meta then Expr.meta_
  else if Expr.is_list then
   | for X Expr:
     | It find_nearest_meta X
     | when got It: ret It
   | No
  else No

peephole_optimize Xs =
  /*Vs (!) //variables
  Cs (!) //counts
  for X Xs: when X.is_list and X.n:
    if X.0><var then Vs.(X.1) =  1
    else for A X.tail: when A.is_text and got Vs.A: Cs.A = Cs.A.@0 + 1*/
  Ys:
  till Xs.end:
    X pop Xs
    case X:
      [mv dummy V] = pass
      [ld dummy Base Pos] = pass
    push X Ys
  Ys.f

optimize_tail_call Xs =
| XsS Xs.n
| Y Xs.(XsS-1)
| less Y.0><ret and Y.1.is_register: ret Xs
| RRId Y.1.idx
| X Xs.(XsS-2)
| when X.0><call and X.1.is_register and RRId >< X.1.idx:
  | Xs.(XsS-2) =  [tcall X.2]
  | ret Xs.lead
| when X.0><mcall and X.1.is_register and RRId >< X.1.idx:
  | Xs.(XsS-2) =  [tmcall X.2 X.3]
  | ret Xs.lead
| when X.0><ldfxn and X.1.is_register and RRId >< X.1.idx
       and X.2.is_int and X.2<0x10000:
  | Xs.(XsS-2) =  [ret X.2]
  | ret Xs.lead
| Xs

compress_regs F =
| [Name Subr @Xs] F
| Xs = Xs.l
| RP register 0
| RP.start =  -1
| RE register 1
| RE.start =  -1
| FRs:
| LabelS!
| LabelE!
| AllRegs dup Subr.1
| for I,X Xs.i:
  | H X.0
  | when H >< label: LabelS.(X.1) =  I
  | when H >< jmp: when got LabelS.(X.1):
    | LabelE.(X.1) =  I
  | when H >< b or H >< bz: when got LabelS.(X.2):
    | LabelE.(X.2) =  I
  | for J,R X.i:
    | when R.is_register: when R.idx << 1:
      | R =  if R.idx><0 then RP else RE
      | X.J =  R
    | when R >< \P or R >< \E:
      | R =  if R >< \P then RP else RE
      | X.J =  R
    | when R.is_register:
      | AllRegs.(R.idx) =  R
      | when no R.start: R.start =  I
      | R.end =  I
| LastBackJmp -1
| for L,E LabelE: //ensure loops maintain register lifespan
  | LastBackJmp =  max E LastBackJmp
  | S LabelS.L
  | Rs:
  | for R AllRegs: when R:
    | when S << R.end and R.end < E:
      | less S < R.start and R.start < E: push R Rs
  | Xs.E =: @Xs.E @Rs.l //add it as deps to the jmp statement
| for R AllRegs: when R:
  | R.end =  No // have to recalc ends
| XsS Xs.n
| I XsS-1
| while I >> 0:
  | As Xs.I
  | for A As: when A.is_register:
    | when no A.end: A.end =  I
  | H As.0
  | when I>LastBackJmp: when H >< mv or H >< ld or H >< closure:
    | R As.1
    | when R.is_register and R.end >< I:
      | Xs.I =  0
      | for A As: when A.is_register and A.end >< I:
        | when A.start >< I: push A.idx FRs
        | when A.start < LastBackJmp:
          | X Xs.LastBackJmp
          | less X.any(R=>R.is_register and R.idx><A.idx):
            | Xs.LastBackJmp =  [@X A] //extend lifespan to jmp
        | A.end =  No
  | I-
| UsedRegs!
| CurReg 0
| UsedRegs.0 = CurReg+
| UsedRegs.1 = CurReg+
| less AllRegs.0: push 0 FRs
| less AllRegs.1: push 1 FRs
| for I,X Xs.i: when X: for R X:
  | when R.is_register:
    | if R.end<<I then push R.idx FRs
      else if R.start><I then
      | less FRs.end:
        | R.idx =  FRs.~
        | FRs =  FRs.lead
      else
    | when no UsedRegs.(R.idx): UsedRegs.(R.idx) = CurReg+
| for X Xs: for R X:
  | when R.is_register:
    | less R.remapped:
      | NI UsedRegs.(R.idx)
      | R.remapped =  1
      | R.idx =  NI
| Xs =  Xs.skip(0)
| for I,X Xs.i:
  | H X.0
  | if H >< jmp then Xs.I =  [X.0 X.1]
    else if H >< b or H >< bz then Xs.I =  [X.0 X.1 X.2]
    else
| Xs optimize_tail_call Xs
| Subr.1 =  CurReg
| [Name Subr @Xs].l



produce_ssa Expr =
  let GEnv []
      GOut []
      GNVars GNVarsInit
      GFns []
      GFnMeta (!)
      GFnId 0
      GFnIdCount 1
      GInits []
      GBytes []
      GStrings (!)
      GRTypes (!)
      GRTypesCount 0
      GTypeParams (!)
      GTexts []
      GTextsMap (!)
      GTextsCount 0
      GSsv []
      GMethods (!)
      GMethodsCount 0
      GImportLibs (!)
      GImportLibsCount 0
      GImports (!)
      GImportsCount 0
      GClosure []
      GBases [[]]
      :
    Origin find_nearest_meta Expr
    if no Origin: Origin = [-1 -1 unknown]
    OrigString ssa_cstring "[Origin.2]"
    R ssa_var result
    Expr = uniquify Expr
    ssa_expr R Expr
    ssa leave R
    EntryBody GOut.f
    GOut = []
    //if Origin.2.url.1><macro: DoDebug = 1
    Libs map K,N GImportLibs [K^ssa_cstring N]
    Libs Libs.s(?1<??1){?0}
    GFnMeta.entry = fnmeta name!'<toplevel>' size!0 nargs!0 origin!Origin
    Types map Name,[Index SN CStr] GRTypes: Index,CStr
    Types Types.s(?0 < ??0){?1}
    Meths map Name,[Index SN CStr] GMethods: Index,CStr
    Meths Meths.s(?0 < ??0){?1}
    Metas map Fn,M GFnMeta: ssa_fnmeta_entry Fn M.name M.size M.nargs M.origin
    Imps map Name,[N R Key Lib Symbol] GImports: [N Lib Symbol]
    Imps Imps.s(?0 < ??0)
    Header: header OrigString GBytes tbls fmtbl,Metas
            [tx,GTexts.f ty,Types mt,Meths
             libs,Libs imlib,Imps{?1} im,Imps{?2}
             doc,GSsv.f]
    for X GInits.f: push X GOut
    EntryHeader GOut.f
    GOut = []
    Entry: [label entry] [subr GNVars] @EntryHeader @EntryBody
    push Entry GFns
    GFns = map F GFns: compress_regs F
    Rs GFns.j
    Rs = peephole_optimize Rs
    //DoDebug = 0
    [Header @Rs]

GCompiled No

emit Statement = push Statement GCompiled

sifnorm [X@Xs] = emit "  [X] [(map X Xs "[X]").text(' ')]"

stable Type Xs =
  Xs map X Xs: "t [Type] [if X.is_text then X else "[X]"]"
  Xs.text('\n')

ssa_to_sif_do_header DepsList ExportsList Header =
  [HeaderId FileSrc Bytes TotName [MetaName MetaEntries] Tables] Header
  MetaXs map [Size NArgs Name Fn Row Col Src] MetaEntries:
    if FileSrc><Src: "[NArgs] [Name] [Fn] [Row] [Col]"
    else "[NArgs] [Name] [Fn] [Row] [Col] [Src]"
  TableDecls: stable(fm MetaXs)
  for [Name Xs] Tables:
    if Name >< doc
      // HELP-3 doc entries: each X is a [SectionLabel SymLabel ValLabel]
      // triple; emit `t doc <sym> <val>` (Section is dropped for now --
      // only one section recognised downstream by sif2sbc.c).
      then DocLines map [_ Sym Val] Xs: "t doc [Sym] [Val]"
           when DocLines.n > 0: push DocLines.text('\n') TableDecls
      else push stable(Name Xs) TableDecls
  DepsExpInc:
  if ExportsList:
    push [__deplist DepsList.text(' ')^cstring_bytes] Bytes
    push [__explist ExportsList.text(' ')^cstring_bytes] Bytes
    DepsExpInc = ["deps __deplist" "export __explist"]
  ByteDelcs:
  for Name,Xs Bytes:
    push "[Name]: bs [Xs{as_text}.text(' ')]" ByteDelcs
  : "src [FileSrc]" @DepsExpInc @TableDecls.f @ByteDelcs

ssa_to_sif Xs DepsList ExportsList = let GCompiled []:
  Statics Decls []
  Imports (!)
  TableDecls ssa_to_sif_do_header DepsList ExportsList: pop Xs
  for X Xs: case X:
    [label Label] = emit "[Label]:"
    [cmnt Text] = emit "; [Text]"
    Else = sifnorm X
  GCompiled =:
    //@Decls.f
    @TableDecls
    @GCompiled.f
  GCompiled.text('\n')
