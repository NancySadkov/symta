export macroexpand 'mexlet' 'let_' 'let' 'default_ret_' 'ret'
       'if' 'case'
       '[]' '\\' 'form'
       'mtx' 'rows' 'rowz' 'no' 'got' 'not' 'and' 'or'
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
       'static'
       \int \float \text \fixtext \list
       \as_int \as_float \as_text \as_fixtext \as_list \as_bytes


GExpansionDepth No
GExpansionDepthLimit 4000
GMacros No
GDefaultLeave No
GModuleCompiler No
GModuleFolders No
GSrc: 0 0 unknown
GTypes No
GVarsTypes No // hash: Var -> Type.  Cleared per macroexpand via (!)
GFnReturns No // TS-3.8: hash: fnname -> return-type-text.
              // Populated by expand_block_item_fn when the
              // function body has a statically inferable
              // return.  Read by infer_type to type call sites.
GMethodReturns No // TS-4.5 Phase 2: hash: "Type.Method" -> return-type.
                  // Populated by expand_block_item_method when the
                  // method body has a statically inferable return.
                  // Read by infer_type's `_mcall` arm to type
                  // `Recv.M` calls where Recv has a known type.
GMexLets No //verb mexlets
GLastType 0
GStaticMode No  // TS-4.4: when set, the surrounding `static Expr`
                // tightens the rules: bin_op demands BOTH operands
                // have a statically-known numeric type (else
                // mex_error), and the sideband channel in
                // expand_block enables type propagation through
                // declarations using `infer_type` (permissive, so
                // bare-literal init like `Acc 0` now narrows Acc).
                // Macro-internal gensyms (`__N` suffix) are
                // filtered out of propagation so case/for/etc.
                // standard macros still work inside `static`.
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

// TS-4.4: detect a macro-generated gensym name.  `text.rand`
// (= `@rand`) appends a `__N` suffix: `"R".rand -> "R__123"`.
// The `__` infix is reserved and never appears in user-written
// identifiers, so a name containing two consecutive `_` is
// unambiguously compiler-generated.  Walks the char-list once
// using `text.locate` to find the first `_`, then peeks the
// next char -- no qlmb / lambda allocation.
is_gensym_name X =
| less X.is_text: ret 0
| P X.locate(\_)
| less got P: ret 0
| less P+1 < X.n: ret 0
| X.(P+1) >< \_

// TS-4.5 Phase 1: whitelist of primitive receiver types that
// `expand_block_item_method` auto-wraps in `_type Type Me Body`.
// Excludes the universal `_` receiver, the `no`/`meta`/`iter`
// dynamic-dispatch wrappers, the `tbl.__` forwarder type, and
// `tok` (low impact, untested).  Each included type is a
// stable runtime tag whose Me-receiver methods overwhelmingly
// do arithmetic / index access on Me's primitive shape; auto-
// typing Me unblocks IADD/IMUL/etc. across the entire stdlib.
is_auto_typed_receiver T =
| case T
    \int   | 1
    \float | 1
    \text  | 1
    \fixtext | 1
    \list  | 1
    \hard_list | 1
    \_list_  | 1
    \_text_  | 1
    \_fixtext_ | 1
    \cons  | 1
    \bytes | 1
    \_bytes_ | 1
    \view  | 1
    \fn    | 1
    Else   | 0

// TS-4.5 Phase 2: types whose `.n` method returns int.  Covers
// the sequence-like and table-like receivers that `Xs.n` is
// the natural size accessor on.  Used as a KISS shortcut in
// `infer_type`'s `_mcall` arm before the registry lookup so
// the common `Xs.n + K` case routes to _iadd without requiring
// every individual `T.n -> int` to be explicitly registered.
is_sequence_type T =
| case T
    \list      | 1
    \hard_list | 1
    \_list_    | 1
    \cons      | 1
    \view      | 1
    \text      | 1
    \fixtext   | 1
    \_text_    | 1
    \_fixtext_ | 1
    \bytes     | 1
    \_bytes_   | 1
    \tbl       | 1
    Else       | 0

is_list_case V =
  V(:[_ _ @_]+[]+[['@'+'/' @_]+['+'+'*' _]+['<' _ ['+'+'*'+'/' _]]])


//FIXME: it is slow and can be speedup with caching
load_symbol Library Name =
| Module GModuleCompiler(Library)
| when no Module: mex_error "couldn't compile [Library]"
| Found Module^load_sbc.find(X => X.0 >< Name)
| less got Found: mex_error "couldn't load `[Name]` from `[Library]`"
| Found.1

// ====================================================================
// macro.s has been split along thematic lines.  Each module is
// included below via the NCM lexical preprocessor.  Top-level
// defs in Symta are order-independent, so the seven modules below
// form one logical translation unit -- but editing one at a time
// is much easier than scrolling 2.6k lines.
//
//   macro_pattern.s   case/match: expand_hole_*, expand_match, case
//   macro_type.s      type-system substrate: tag_for_predicate,
//                     infer_type, types_compatible, T constructors,
//                     as_* converters, list
//   macro_ops.s       control flow + operators + infix syntax
//   macro_qlmb.s      quasi-lambda + curly matchers
//   macro_block.s     lambdas, fn defs, blocks, assignments,
//                     type declarations, methods
//   macro_mex.s       mex dispatch core, special forms, ffi,
//                     exports + shape utilities
// ====================================================================

#:macro_pattern.s
#:macro_type.s
#:macro_ops.s
#:macro_qlmb.s
#:macro_block.s
#:macro_mex.s
