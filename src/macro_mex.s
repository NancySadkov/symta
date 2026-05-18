// macro_mex.s -- mex dispatch core + special forms + ffi + exports.
// ret, as, callcc, fin, compile_when, copy_ffi, stage_*, ffi_begin,
// mac, expand_ffi, ffi, exports_preprocess, export, handle_extern,
// mex_extern, normalize_arg, mex_table, mex_try_special, mex_normal,
// mdbg, normalize_nesting, imex, MexFormCases, mex, macroexpand,
// plus shape utilities (mtx, rows, rowz, cons, uncons, same, on,
// hcase, hcase_go, prx, ast_key, help).
// Auto-included by macro.s via NCM #:macro_mex.s

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

// MACRO-2: short-circuit the list-pattern checks (1, 3, 4) when X
// is not a list -- the common case for plain function/macro calls.
// Most macroexpand_normal invocations hit the non-list path, and
// each skipped `case X [...]` is ~20-30 ns; the `Xs.locate`
// args-splat scan also gets a fast bypass when no list-shape
// special is possible.
mex_try_special X Xs =
  when X.is_list:
    less Xs.end: case X [`.`+`$` @_]:
      case Xs [[`:` _ _]]:
        Xs = [Xs] //escape, otherwise `()` treat it as O(:) matcher
      ret: mex:: `()` X @Xs
    case X [`!` _]: ret: mex: mex_table [X@Xs]
    case X [`@` Z]:
      case Z nullary_,N: Z = N
      case Z [N @Zs]:
        Z = N
        Xs =: @Zs @Xs
      ret: mex [_mcall Xs.~ Z @Xs.lead] //method call
  when X >< `!`:
    case Xs [[`!`_]@_]: ret: mex: mex_table Xs
    ret: mex: mex_table [[X@Xs]]
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
    // TS-3.2/3.7: when reassigning a var via `=`, detect
    // Value's static type BEFORE imex (after imex the AST is
    // a let_ wrapper and infer_type can't see through it).
    //
    // TS-3.7: if Place ALREADY has a tracked type from an
    // earlier typed declaration / scoped narrowing, and the
    // new value's static type is incompatible, raise a
    // compile error.  Catches `X _the int 5; X = "hi"`.
    // Falls through silently if either side is unknown -- the
    // runtime check stays the safety net.
    //
    // Note: this is the REASSIGNMENT path (`X = ...`).  The
    // DECLARATION path (`X ...`) goes through expand_block_helper
    // which wraps the body in `_type T X body` -- the existing
    // `_type` arm above already pushes the type fact for the
    // scoped body.
    // TS-3.6: use `infer_declared_type` for var-flow (skips
    // bare-literal types) -- `R 0` shouldn't make R int-typed.
    // But the reassign mismatch check uses the more permissive
    // `infer_type` for the new value, so `X _the int 5; X = "hi"`
    // catches text-literal mismatch.
    // TS-3.6: only CHECK reassignment against PriorType (set
    // by `_type T A Body` scoping from a typed declaration);
    // do NOT propagate a new type onto Place.  Propagation
    // here would leak types across function scopes -- e.g.
    // `Dst = gfx $w $h` inside one method would falsely type
    // Dst in another method's call.  Scoping must come from
    // `_type` which has proper push/pop.
    | OrigType infer_type Value
    | NewValue if Value.is_keyword: [_quote Value] else imex Value
    | when Place^is_var_sym:
      | PriorType GVarsTypes.Place
      | when got PriorType and got OrigType
             and not types_compatible OrigType PriorType:
        | mex_error "type mismatch on reassign: `[Place]` is `[PriorType]`, got `[OrigType]`"
    | [_set Place NewValue]
  [_label Name] | Expr
  [_goto Name] | Expr
  [_quote X] | if X.is_list: expand_quoted_list X else Expr
  [_nomex X] | X // no macroexpand
  [_type Type Var Body]
    | Old GVarsTypes.Var
    | GVarsTypes.Var =  Type
    | R mex Body
    | GVarsTypes.Var =  Old
    | R
  // TS-1: `_the T E` -- DYN -> typed boundary.  Runtime-checks
  // that E's value is of type T, then propagates T statically
  // via the existing `_type T G G` mex form so the type-aware
  // fast-paths at macro.s:803 / :1518 fire for downstream uses.
  // If the check fails, `bad` raises a runtime error.
  //
  // `mex E` normalises the value expression -- in particular
  // `normalize_nesting` unwraps a 1-element list wrapper that
  // expand_block_item / `=` puts around the RHS of an assignment.
  // Without this, `X^int = 5` would bind G to `[5]` (list) and
  // the `is_int` check would always fail.
  [_the Type E]
    | G @rand 'G'
    | Mname "is_[Type]"
    | Msg "value not [Type]"
    | E2 mex E
    // TS-3.1: static check.  If E2's type can be inferred at
    // mex time and conflicts with Type, raise a compile error
    // before we emit the runtime check.  Conservative: only
    // fires when both sides are known.  Falls through to the
    // runtime check for any case we can't prove statically.
    | Inferred infer_type E2
    | when got Inferred and not types_compatible Inferred Type:
      | mex_error "type mismatch: expected `[Type]`, got `[Inferred]` (in `_the [Type] [E]`)"
    | Check [`_if` [`.` G Mname] [`_type` Type G G] [bad Msg]]
    | mex [`let_` [[G E2]] Check]
  // TS-1: `_unsafe T E` -- C-style trust-me cast.  Skips the
  // runtime check; propagates T statically.  UB if you lie.
  // Use only when the strict checker is provably wrong.
  [_unsafe Type E]
    | G @rand 'G'
    | E2 mex E
    | mex [`let_` [[G E2]] [`_type` Type G G]]
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
      GVarsTypes (!)
      GMexLets (!)
      GLastType 0
  | R mex Expr
  | R,GExports

// `list_` was the temporary alias for `list @Xs = form [$@Xs]`
// during the list-transition.  After phase 5, all call sites
// use `[...]` literal or `` `[]` `` form directly; the alias
// is gone.  `list` is the pure type-constructor (see line 370).

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
