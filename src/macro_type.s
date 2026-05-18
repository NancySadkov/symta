// macro_type.s -- type-system substrate: tag predicates, infer_type,
// types_compatible, type-constructor macros (int/float/text/fixtext,
// as_*, list).  Auto-included by macro.s via NCM #:macro_type.s


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

// TS-1.1: is `Name` a recognised type-name at this point in
// macroexpansion?  Used by `^` and by expand_block_item to
// decide whether `X^T` / `X^T = E` is a typed-decl / assertion
// (T is a type) or a regular apply-on-left / function-def
// (T is just a variable name).  Checks:
//   - primitive types via the tag predicates above
//   - user-defined types via GTypes (populated by `type Foo:...`)
// Per-translation-unit visibility -- a `type Foo:...` earlier
// in the same file (or in an imported macro library) makes Foo
// a known type for all uses below it.
//
is_known_type Name =
  | when no Name.is_text: ret 0
  | when got tag_for_predicate Name: ret 1
  | when got tags_for_predicate Name: ret 1
  | got GTypes.Name

// TS-3.1: compile-time type inference for an expression.
// Returns the type name (text) if statically known, else No.
// Conservative -- only fires when we can prove the type
// without execution.  Used by `_the` to raise compile errors
// on obvious mismatches, and by the var-flow propagation
// path in `_set` / expand_block_helper.
//
// Shapes recognised:
//
// Post-mex literals + AST nodes:
//   `5`                              -> "int"
//   `1.5`                            -> "float"
//   [_quote V] V.is_text             -> "text"
//   [_quote V] V.is_list             -> "list"
//   [_the T _]                       -> T  (TS-1)
//   [_unsafe T _]                    -> T  (TS-1)
//   [_type T _ _]                    -> T  (existing scoped form)
//   variable ref via GVarsTypes
//
// Pre-mex user forms (so the var-flow path can catch
// `X int 5` and `X 5^int` declarations):
//   [T @_] where T is a known type   -> T  (TS-1.2 constructor)
//   [`^` _ T]                        -> T  (TS-1.1 ascription)
//
// Anything else returns No -- the runtime check stays the
// fallback for cases we can't prove statically.
// TS-3.3 helper: peek through a single 1-element-list wrapper
// that expand_block_item / expand_assign put around pre-mex
// RHS values.  Only used for the "find a typed shape" cases
// below -- we don't propagate the unwrapped form, just check
// if a typed shape is inside.
infer_peek_inner Expr =
  case Expr [E] | E
              X | X

infer_type Expr =
  | when Expr.is_int: ret "int"
  | when Expr.is_float: ret "float"
  | when Expr.is_text and not Expr.is_keyword:
    // Variable reference (uppercase-start text token).  Look
    // up its tracked type in GVarsTypes.  If unknown, fall
    // through to No.
    | T GVarsTypes.Expr
    | when got T: ret T
    | ret No
  | when Expr.is_list:
    | case Expr
      [`_quote` V]
        | when V.is_text: ret "text"
        | when V.is_list: ret "list"
        | ret No
      [`_the` T _] | ret T
      [`_unsafe` T _] | ret T
      // TS-3.6 fix: `_type T Var Body` scopes Var:T over Body,
      // but the expression's value is Body's value, not T.
      // (Common case: receiver-type wrap `[_type id Me body]`
      // around a method body -- the body returns whatever it
      // returns, NOT id.)  Recurse into Body to find the
      // actual result type.  Terminates because each recursion
      // strips an outer node.
      [`_type` _ _ B] | ret: infer_type B
      // TS-3.4: method call whose method name is a known type.
      // Conversion methods (.int, .float, .text, .fixtext,
      // .bytes) return a value of the named type regardless
      // of receiver type -- they're the universal coercers
      // defined per-type in core_.s.  The method name after
      // mex is `[_quote M]`; bare text would mean a dynamic
      // dispatch we can't statically resolve.
      [`_mcall` _ [`_quote` M] @_]
        | when M.is_text and M^is_known_type: ret M
      // TS-3.7: `_if cond Then Else` (post-mex `if`-form) --
      // if both branches have the same inferable type, the
      // if-expression has that type.  Used by `if`, `when`,
      // `less`, and any other macros that lower to `_if`.
      //
      // For `when X.is_int X No` (else branch is No):
      //   Then = X (typed int if X is) ; Else = No (dyn).
      //   Treat `No` as "dyn"-compatible so the if-expr
      //   takes Then's type.  Same for `less ... No ...`.
      [`_if` _ Then Else]
        | T1 infer_type Then
        | T2 infer_type Else
        | when Then >< \No: T1 = T2
        | when Else >< \No: T2 = T1
        | when got T1 and got T2 and T1 >< T2: ret T1
      // TS-3.6: pre-mex text literal `"abc"` parses to
      // `[`"` "abc"]` (backtick-`"` operator).  Recognise it
      // so the mismatch check sees `_the int "abc"` as text.
      [`"` @_] | ret "text"
      // TS-3.3: detect pre-mex typed shapes INSIDE a 1-element
      // wrapper.  expand_block_item / expand_assign put `[ ... ]`
      // around the RHS value, so the typed shape sits one level
      // deeper than the post-mex case.  We peek non-destructively.
      //
      // Catches:
      //   [`^` E T]      -- TS-1.1 ascription, lowers to _the T E
      //   [T @_] where T is a known type -- TS-1.2 constructor
      //   [`"` @_]       -- text literal
      //   bare int/float -- literal inside a 1-wrap
      Else
        | Inner infer_peek_inner Expr
        | when Inner.is_int: ret "int"
        | when Inner.is_float: ret "float"
        | when Inner.is_list:
          | case Inner
            [`^` _ T] | when T.is_text and T^is_known_type: ret T
            [`"` @_] | ret "text"
            [Head @_]
              | when Head.is_text and Head^is_known_type: ret Head
        | ret No
  | No

// TS-3.6: like `infer_type` but only returns a type when the
// expression EXPLICITLY declares one -- bare literals (`5`,
// `"abc"`) return No.  Used by the var-flow propagation paths
// (`_set` reassign-check and `expand_block_helper` _type wrap)
// so `R 0` doesn't make R int-typed against the user's intent.
//
// Explicit-type forms:
//   [_the T _]                       -- TS-1 assertion
//   [_unsafe T _]                    -- TS-1 trust cast
//   [_type _ _ B]                    -- recurse on body
//   [_mcall E [_quote M]] (M known)  -- TS-3.4 conversion method
//   [T @_] where T is known          -- TS-1.2 constructor call
//   [`^` _ T] where T is known       -- TS-1.1 ascription
//   variable ref via GVarsTypes      -- previously-declared
//
// Bare literals (`5`, `1.5`, `[_quote ...]`, `[`"` ...]`) are
// deliberately NOT explicit.  `R 0` -> R is dyn, not int.
infer_declared_type Expr =
  | when Expr.is_text and not Expr.is_keyword:
    | T GVarsTypes.Expr
    | when got T: ret T
    | ret No
  | when Expr.is_list:
    | case Expr
      [`_the` T _] | ret T
      [`_unsafe` T _] | ret T
      [`_type` _ _ B] | ret: infer_declared_type B
      [`_mcall` _ [`_quote` M] @_]
        | when M.is_text and M^is_known_type: ret M
      Else
        | Inner infer_peek_inner Expr
        | when Inner.is_list:
          | case Inner
            [`^` _ T] | when T.is_text and T^is_known_type: ret T
            [Head @_]
              | when Head.is_text and Head^is_known_type: ret Head
        | ret No
  | No

// TS-3.1: are two type names compatible for a static check?
// Compatible means: same name, or either side is `dyn` (the
// gradual escape), or one is a known sub/super of the other.
// Today: same-name equality only, with dyn as the universal
// escape.  Inheritance walks will land in TS-3.2 when needed.
types_compatible Have Want =
  | when Have >< Want: ret 1
  | when Have >< \dyn: ret 1
  | when Want >< \dyn: ret 1
  | when Have >< "dyn": ret 1
  | when Want >< "dyn": ret 1
  | 0

// TS-1.2: type-functions for primitives.  `int X`, `float X`,
// `text X`, etc. behave as **type constructors**: they coerce
// X to the target type via the existing `.T` method (which is
// identity when X is already T, a conversion when X has a
// registered conversion method, runtime error otherwise),
// then `_the T` runtime-checks the result.  In practice the
// post-conversion check is redundant on success and trips on
// failure -- exactly the dependent-type interface the user
// asked for.  Same `^`-overload at line 838 unifies
// `X^int` <-> `int X`.
//
//   int 3        -- 3.int = 3 (b_int_int identity); _the int 3 -> 3
//   int 3.5      -- 3.5.int = 3 (b_float_int conversion)
//   int "42"     -- "42".int = 42 (text.int parse)
//   int "abc"    -- "abc".int errors at runtime (parse failure)
//
// Multi-arg fallback (`Else` branch) is for callers that
// build literal lists with the type-symbol as head -- e.g.
// expand_ffi at macro.s:2097 emits `[int A B C]` as a
// type-spec list, not a function call.
// TS-1.3+ recovery: inline the body in each type-constructor.
// Trying to factor it through a separate `type_constructor`
// function broke when the exports list at load_macros time
// gave `type_constructor` a callable interface that mex
// invokes as a macro -- the args end up as raw AST atoms
// (identifier `As`) instead of a runtime list, and case
// patterns crash on the non-list.  Inlining avoids that
// entirely: each `int`/`float`/etc. body emits its AST
// directly without an intermediate call.
int @As =
  less As.is_list: ret [\int As]
  case As
    [] | [`_the` \int 0]
    [E] | [`_the` \int [`_mcall` E [`_quote` \int]]]
    Else | [\int @As]
float @As =
  less As.is_list: ret [\float As]
  case As
    [] | [`_the` \float 0.0]
    [E] | [`_the` \float [`_mcall` E [`_quote` \float]]]
    Else | [\float @As]
text @As =
  less As.is_list: ret [\text As]
  case As
    [] | [`_the` \text ""]
    [E] | [`_the` \text [`_mcall` E [`_quote` \text]]]
    Else | [\text @As]
fixtext @As =
  less As.is_list: ret [\fixtext As]
  case As
    [] | [`_the` \fixtext ""]
    [E] | [`_the` \fixtext [`_mcall` E [`_quote` \fixtext]]]
    Else | [\fixtext @As]

// TS-1.3: `as_<T> X` -- **pure converter**.  Calls X's
// conversion method directly (`.int`, `.l`, etc.) without
// any assertion.  Distinct from `T X` (TS-1.2, convert-and-
// check) and `_the T X` / `X^T` (assert only).  Use when
// you want the conversion only and trust the source.
//
// `as_list X` delegates to `.l` (the existing universal
// to-list method at core_.s:514+), not to a `.list` method
// (which doesn't exist; `list` is the type-name, not a
// per-type method name).
as_int @As =
  less As.is_list: ret [\as_int As]
  case As [E]: ret [`_mcall` E [`_quote` \int]]
  [\as_int @As]
as_float @As =
  less As.is_list: ret [\as_float As]
  case As [E]: ret [`_mcall` E [`_quote` \float]]
  [\as_float @As]
as_text @As =
  less As.is_list: ret [\as_text As]
  case As [E]: ret [`_mcall` E [`_quote` \text]]
  [\as_text @As]
as_fixtext @As =
  less As.is_list: ret [\as_fixtext As]
  case As [E]: ret [`_mcall` E [`_quote` \fixtext]]
  [\as_fixtext @As]
as_list @As =
  less As.is_list: ret [\as_list As]
  case As [E]: ret [`_mcall` E [`_quote` \l]]
  [\as_list @As]
as_bytes @As =
  less As.is_list: ret [\as_bytes As]
  case As [E]: ret [`_mcall` E [`_quote` \bytes]]
  [\as_bytes @As]
// `fn` is NOT registered as a type-constructor: it collides
// with user code that declares local fns (e.g.
// game/src/view_render.s:1106 `fn I = "..."`).  For "is X a
// fn" semantics use `_the fn X` / `X^fn`.

// TS-1.3+ phase 5 (list-transition.md): `list` is a pure
// type-constructor.  Construction uses the native `[X Y Z]`
// literal or its underlying `` `[]` X Y Z `` form.  The
// `list_` alias is gone -- all 47 call sites identified in
// list-transition.md have moved to `[...]` literal or
// `` `[]` `` form.
//
// `list X` semantics: assertion across all 4 concrete tags
// (T_LIST | T_VIEW | T_CONS | T_BYTES) via the multi-tag
// `is_list` predicate.  No coercion -- the list union is
// flat at the surface; choose a concrete representation
// (`[...]`, `Xs.pre H`, `Xs[S:E]`, `N.bytes`) when building.
// `as_list X` (above) is the explicit converter via `.l`.
//
// Multi-arg `list X Y Z` is no longer the variadic builder.
// If any survive, they trip the Else-branch fallback which
// delegates to the `[]` list-builder macro -- still produces
// the expected list, but emits a deprecation-style fall-back
// rather than the direct path.
list @As =
  less As.is_list: ret [`[]` As]
  case As
    [] | [`_the` \list []]
    [E] | [`_the` \list E]
    Else | [`[]` @As]
