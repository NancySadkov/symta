// reader.c -- C port of symta/src/reader.s.
//
// Token-list -> AST parser. Tokens come from tokenize.c (already C),
// the AST shape matches what compiler.s / macro.s consume.
//
// Migration strategy (see issues.md B8):
//   * Each public symbol of reader.s is mirrored by a C entry
//     declared in reader.h and exposed as a builtin from bltin.c.
//   * reader.s is patched to dispatch through those builtins so the
//     `text.parse` entrypoint keeps working.
//
// Token layout (T_TOK objects, 7 slots):
//   0 type   1 value   2 pchar   3 row   4 col   5 orig   6 parsed
//
// All operations run with GC disabled so we can hold raw `dyn`
// pointers across allocations without worrying about relocation.

#include "common.h"
#include "ng.h"
#include "am.h"
#include "reader.h"

// ====================================================================
// Token introspection.
// ====================================================================

static inline int is_tok(dyn o)     { return O_TAG(o) == T_TOK; }
static inline int is_list(dyn o)    { return O_TAG(o) == T_LIST; }

static inline dyn tok_type(dyn o)   { return LGET(o, 0); }
static inline dyn tok_value(dyn o)  { return LGET(o, 1); }
static inline dyn tok_pchar(dyn o)  { return LGET(o, 2); }
static inline dyn tok_row(dyn o)    { return LGET(o, 3); }
static inline dyn tok_col(dyn o)    { return LGET(o, 4); }
static inline dyn tok_orig(dyn o)   { return LGET(o, 5); }
static inline dyn tok_parsed(dyn o) { return LGET(o, 6); }

static inline int text_eq_c(dyn a, dyn b) {
  if (a == b) return 1;
  if (IS_BIGTEXT(a) && IS_BIGTEXT(b)) return texts_equal(a, b);
  return 0;
}

static inline int token_is(dyn o, dyn what) {
  if (!is_tok(o)) return 0;
  return text_eq_c(tok_type(o), what);
}

// ====================================================================
// Interned keyword bag.
// Filled lazily on first use; everything in here is a fixtext or bigtext
// so `text_eq_c` is fast.
// ====================================================================

#define KW_LIST \
  X(pipe,      "|"   ) X(then,    "then" ) X(else_,   "else" ) \
  X(elif,      "elif") X(if_,     "if"   ) X(symbol,  "symbol") \
  X(escape,    "escape") X(text,   "text"  ) X(splice, "splice") \
  X(int_,      "int" ) X(hex,     "hex"  ) X(bin,     "bin"  ) \
  X(void_,     "void") X(parens,  "()"   ) X(brackets,"[]"   ) \
  X(curly,     "{}"  ) X(curlyL,  "{"    ) X(table,   "@{}"  ) \
  X(object,    "${}" ) X(unary,   "unary") X(bracketsL,"["  ) \
  X(dot,       "."   ) X(caret,   "^"    ) X(arrow,   "->"  ) \
  X(eq,        "="   ) X(le,      "<="   ) X(plus,    "+"   ) \
  X(minus,     "-"   ) X(star,    "*"    ) X(slash,   "/"   ) \
  X(pct,       "%"   ) X(pow,     "^^"   ) X(range,   ".."  ) \
  X(shl,       "-<-" ) X(shr,     "->-"  ) X(band,    "-*-" ) \
  X(bor,       "-+-" ) X(bxor,    "-^-"  ) X(comma,   ","   ) \
  X(eqeq,      "><"  ) X(neq,     "<>"   ) X(lt,      "<"   ) \
  X(gt,        ">"   ) X(lte,     "<<"   ) X(gte,     ">>"  ) \
  X(and_,      "&&"  ) X(or_,     "||"   ) X(bang,    "!"   ) \
  X(arrow_eq,  "=>"  ) X(at,      "@"    ) X(amp,     "&"   ) \
  X(atat,      "@@"  ) X(plus_eq, "+="   ) X(minus_eq,"-="  ) \
  X(star_eq,   "*="  ) X(slash_eq,"/="   ) X(pct_eq,  "%="  ) \
  X(plusplus,  "++"  ) X(minusminus,"--" ) X(quotebs, "\\"  ) \
  X(dollar,    "$"   ) X(semi,    ";"    ) X(colon,   ":"   ) \
  X(at_curly,  "@{"  ) X(amp_amp, "^^"   )                    \
  X(none,      "<none>")

#define X(slot,_lit) static dyn KW_##slot;
KW_LIST
#undef X

static int kw_initted = 0;
static void kw_init(void) {
  if (kw_initted) return;
#define X(slot,lit) TEXT(KW_##slot, lit);
  KW_LIST
#undef X
  kw_initted = 1;
}

// Membership tests via switch -- compact and avoids hash overhead.

// reader.s DelimList: [`:` `=` `<=` `=>` `+=` `-=` `*=` `/=` `%=`]
static int is_delim_op(dyn t) {
  return text_eq_c(t, KW_colon) || text_eq_c(t, KW_eq) ||
         text_eq_c(t, KW_le)    || text_eq_c(t, KW_arrow_eq) ||
         text_eq_c(t, KW_plus_eq) || text_eq_c(t, KW_minus_eq) ||
         text_eq_c(t, KW_star_eq) || text_eq_c(t, KW_slash_eq) ||
         text_eq_c(t, KW_pct_eq);
}

// reader.s DelimOps (parse_delim "is_delim" filter):
// `:` `=` `<=` `=>` `if` `then` `else` `elif` ` =` `-=` `*=` `/=` `%=`
// (note: there's a `+=` typo in original Symta as ` =` -- preserve)
static int is_delim_filter(dyn t) {
  if (!is_tok(t)) return 0;
  dyn ty = tok_type(t);
  return text_eq_c(ty, KW_colon) || text_eq_c(ty, KW_eq) ||
         text_eq_c(ty, KW_le)    || text_eq_c(ty, KW_arrow_eq) ||
         text_eq_c(ty, KW_if_)   || text_eq_c(ty, KW_then) ||
         text_eq_c(ty, KW_else_) || text_eq_c(ty, KW_elif) ||
         text_eq_c(ty, KW_plus_eq) || text_eq_c(ty, KW_minus_eq) ||
         text_eq_c(ty, KW_star_eq) || text_eq_c(ty, KW_slash_eq) ||
         text_eq_c(ty, KW_pct_eq);
}

// reader.s OpsT.
static int is_opt(dyn t) {
  return text_eq_c(t, KW_plus) || text_eq_c(t, KW_minus) ||
         text_eq_c(t, KW_star) || text_eq_c(t, KW_slash) ||
         text_eq_c(t, KW_pct)  || text_eq_c(t, KW_caret) ||
         text_eq_c(t, KW_dot)  || text_eq_c(t, KW_arrow) ||
         text_eq_c(t, KW_pipe) || text_eq_c(t, KW_semi)  ||
         text_eq_c(t, KW_comma)|| text_eq_c(t, KW_colon) ||
         text_eq_c(t, KW_eq)   || text_eq_c(t, KW_arrow_eq) ||
         text_eq_c(t, KW_le)   || text_eq_c(t, KW_plus_eq) ||
         text_eq_c(t, KW_minus_eq) || text_eq_c(t, KW_star_eq) ||
         text_eq_c(t, KW_slash_eq) || text_eq_c(t, KW_pct_eq) ||
         text_eq_c(t, KW_bang) || text_eq_c(t, KW_bor) ||
         text_eq_c(t, KW_bxor) || text_eq_c(t, KW_band) ||
         text_eq_c(t, KW_shl)  || text_eq_c(t, KW_shr) ||
         text_eq_c(t, KW_amp_amp) || text_eq_c(t, KW_range) ||
         text_eq_c(t, KW_plusplus) || text_eq_c(t, KW_minusminus) ||
         text_eq_c(t, KW_eqeq) || text_eq_c(t, KW_neq) ||
         text_eq_c(t, KW_lt)   || text_eq_c(t, KW_gt) ||
         text_eq_c(t, KW_lte)  || text_eq_c(t, KW_gte) ||
         text_eq_c(t, KW_quotebs) || text_eq_c(t, KW_dollar) ||
         text_eq_c(t, KW_at) || text_eq_c(t, KW_amp);
}

// reader.s UnaryOps:
//   [`+` `-` `*` `/` `%` `++` `--` `<` `>` `<<` `>>` `<>` `><` `-+` `-*`]
// (note: these are the OPERATOR types -- the token types, not values --
// that can appear in unary position. tok.type==`unary` already covers
// the prefix case from tokenize.c.)
static int is_unary_op_type(dyn t) {
  return text_eq_c(t, KW_plus) || text_eq_c(t, KW_minus) ||
         text_eq_c(t, KW_star) || text_eq_c(t, KW_slash) ||
         text_eq_c(t, KW_pct)  || text_eq_c(t, KW_plusplus) ||
         text_eq_c(t, KW_minusminus) ||
         text_eq_c(t, KW_lt)   || text_eq_c(t, KW_gt) ||
         text_eq_c(t, KW_lte)  || text_eq_c(t, KW_gte) ||
         text_eq_c(t, KW_neq)  || text_eq_c(t, KW_eqeq);
  // `-+` `-*` aren't in our kw list but unused too.
}

static int is_suf_unary(dyn t) {
  // SufUnaryL: '{}' '()' '[]' '[' '{' '+' '-' '*' '/' '%' '<' '>' '<<' '>>'
  return text_eq_c(t, KW_curly) || text_eq_c(t, KW_parens) ||
         text_eq_c(t, KW_brackets) || text_eq_c(t, KW_bracketsL) ||
         text_eq_c(t, KW_curlyL) || text_eq_c(t, KW_plus) ||
         text_eq_c(t, KW_minus) || text_eq_c(t, KW_star) ||
         text_eq_c(t, KW_slash) || text_eq_c(t, KW_pct) ||
         text_eq_c(t, KW_lt) || text_eq_c(t, KW_gt) ||
         text_eq_c(t, KW_lte) || text_eq_c(t, KW_gte);
}

static int is_suf_unary_s(dyn t) {
  return text_eq_c(t, KW_plus) || text_eq_c(t, KW_minus) ||
         text_eq_c(t, KW_star) || text_eq_c(t, KW_slash) ||
         text_eq_c(t, KW_pct)  ||
         text_eq_c(t, KW_lt) || text_eq_c(t, KW_gt) ||
         text_eq_c(t, KW_lte) || text_eq_c(t, KW_gte);
}

static int is_suf_binary(dyn t) {
  return text_eq_c(t, KW_dot) || text_eq_c(t, KW_caret) ||
         text_eq_c(t, KW_arrow);
}

static int is_suf_op(dyn t) {
  return is_suf_binary(t) || is_suf_unary(t);
}

static int is_dollar_op(dyn t) {
  // DollarList: [`$` unary `\\` `@` `&` `@@`]
  return text_eq_c(t, KW_dollar) || text_eq_c(t, KW_unary) ||
         text_eq_c(t, KW_quotebs) || text_eq_c(t, KW_at) ||
         text_eq_c(t, KW_amp) || text_eq_c(t, KW_atat);
}

static int is_prefix_op(dyn t) {
  // PrefixList: [unary `\\` `@` `&` `@@`]
  return text_eq_c(t, KW_unary) || text_eq_c(t, KW_quotebs) ||
         text_eq_c(t, KW_at) || text_eq_c(t, KW_amp) ||
         text_eq_c(t, KW_atat);
}

static int is_bool_op(dyn t) {
  // BoolList: [`><` `<>` `<` `>` `<<` `>>`]
  return text_eq_c(t, KW_eqeq) || text_eq_c(t, KW_neq) ||
         text_eq_c(t, KW_lt) || text_eq_c(t, KW_gt) ||
         text_eq_c(t, KW_lte) || text_eq_c(t, KW_gte);
}

// DisjointChars: [',' ':' '<' '>' '<<' '>>']
static int is_disjoint_char(dyn t) {
  return text_eq_c(t, KW_comma) || text_eq_c(t, KW_colon) ||
         text_eq_c(t, KW_lt) || text_eq_c(t, KW_gt) ||
         text_eq_c(t, KW_lte) || text_eq_c(t, KW_gte);
}

// ParenChars: ['_' '`' '?' '~' `)` `}` `]` '"' "'"]
static int is_paren_char(char c) {
  return c=='_' || c=='`' || c=='?' || c=='~' || c==')' ||
         c=='}' || c==']' || c=='"' || c=='\'';
}

static int is_anchor_char_c(dyn pchar) {
  // pchar is a 1-char text (or No/0). text.tokenize emits via code2text.
  if (!pchar || pchar == No) return 0;
  char buf[8] = {0};
  decode_text(buf, pchar);
  char c = buf[0];
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || is_paren_char(c);
}

// ====================================================================
// Parser state.
//
// `gi` is the input list; we read forwards from gi[gi_pos]. To "push
// back" a token onto the input, we put it into the `pushback` array
// (LIFO). p_pop reads from pushback first.
//
// `go` is the GOutput stack used by parse_xs/parse_delim/parse_logic.
// It's stretchy.
// ====================================================================

typedef struct {
  dyn gi;          // input list (immutable)
  uint64_t gi_n;   // LIST_SIZE(gi)
  uint64_t gi_pos; // next index to read
  dyn *pushback;   // stretchy LIFO of pushed-back tokens
  dyn *go;         // output stack
} pstate_t;

static void p_init(pstate_t *p, dyn input) {
  p->gi = input;
  p->gi_n = is_list(input) ? LIST_SIZE(input) : 0;
  p->gi_pos = 0;
  p->pushback = 0;
  p->go = 0;
}

static void p_free(pstate_t *p) {
  if (p->pushback) arrfree(p->pushback);
  if (p->go) arrfree(p->go);
  p->pushback = 0;
  p->go = 0;
}

static inline int p_end(pstate_t *p) {
  return arrlen(p->pushback) == 0 && p->gi_pos >= p->gi_n;
}

static inline dyn p_peek(pstate_t *p) {
  int pn = arrlen(p->pushback);
  if (pn > 0) return p->pushback[pn - 1];
  return LGET(p->gi, p->gi_pos);
}

static inline dyn p_pop(pstate_t *p) {
  int pn = arrlen(p->pushback);
  if (pn > 0) { dyn t = p->pushback[pn - 1]; arrpop(p->pushback); return t; }
  dyn t = LGET(p->gi, p->gi_pos);
  p->gi_pos++;
  return t;
}

static inline void p_push_back(pstate_t *p, dyn t) {
  arrput(p->pushback, t);
}

// ====================================================================
// add_bars -- mirror of reader.s:30-42.
// Used both at toplevel (text.parse calls it) and as a building block.
// ====================================================================

// Build a `|` separator token mirroring [Row Col Orig].
// token_() leaves slot 2 (pchar) and slot 6 (parsed) uninitialised;
// must zero them or downstream `Tok.parsed` reads garbage.
static dyn make_bar(dyn row, dyn col, dyn orig) {
  dyn t = token(KW_pipe, KW_pipe, row, col, orig);
  LGET(t, 2) = 0;
  LGET(t, 6) = 0;
  return t;
}

dyn reader_add_bars(dyn xs) {
  GC_DISABLE();
  kw_init();
  uint64_t n = LIST_SIZE(xs);
  dyn *out = 0;
  int first = 1;
  for (uint64_t i = 0; i < n; i++) {
    dyn x = LGET(xs, i);
    if (is_tok(x)) {
      dyn type = tok_type(x);
      int is_continuation =
            text_eq_c(type, KW_pipe) || text_eq_c(type, KW_then) ||
            text_eq_c(type, KW_else_) || text_eq_c(type, KW_elif);
      int col = (int)(int64_t)UNFXN(tok_col(x));
      if ((col == 0 || first) && !is_continuation) {
        dyn col_m1; LDFXN(col_m1, col - 1);
        dyn bar = make_bar(tok_row(x), col_m1, tok_orig(x));
        arrput(out, bar);
        first = 0;
      }
    }
    arrput(out, x);
  }
  int outn = arrlen(out);
  dyn r;
  LIST(r, outn);
  for (int i = 0; i < outn; i++) LGET(r, i) = out[i];
  arrfree(out);
  GC_ENABLE();
  return r;
}

// ====================================================================
// parse_strip -- mirror of reader.s:460-472.
//
// Walks the token tree, replacing each token with its parsed value
// (or raw value).  For each non-empty list whose head is a token
// with a real source position, allocates a `meta` wrapper struct
// (`type meta.~ O M: object_!O meta_!M`, defined in src/core_.s)
// holding (stripped-list, [row col orig]).  Consumers later read
// `.meta_` directly off the field accessor, no hashtable lookup.
//
// `meta_tag_g` caches `intern("meta")` so the type-tag lookup is
// a single load on the hot path.  The tag is stable once the
// Symta `type meta` declaration has been registered, which always
// happens before any `text.parse` call (core_.s loads first).
// ====================================================================

static int meta_tag_g = -1;

/* Look up the `meta` type tag without creating it.  Returns -1 if
 * the type hasn't been registered yet (e.g. during the transitional
 * bootstrap rebuild where the OLD core_.sbc only knows
 * `meta_wrapper`).  In that case we return the plain stripped list
 * unwrapped, which is what the pre-Phase-3 weak-table flow produced
 * -- bootstrap can finish, regenerate the SBCs with the new
 * `type meta` declaration, and the next runtime invocation will
 * find the tag and start wrapping. */
extern type_t *types;
static int find_meta_tag(void) {
  if (meta_tag_g >= 0) return meta_tag_g;
  for (int i = 0; i < arrlen(types); i++) {
    if (types[i].name && !strcmp(types[i].name, "meta")) {
      meta_tag_g = i;
      /* OP-4: install the C-side direct-dispatch sink for
       * meta.__ now that the type tag exists.  Replaces the
       * Symta-side meta.__ defn (which paid 3 MCALLs per
       * forwarded method call) with a single inline unwrap +
       * re-dispatch.  Idempotent. */
      install_meta_dispatch(i);
      return i;
    }
  }
  return -1;
}

static dyn make_meta_wrapper(dyn obj, dyn src) {
  int tag = find_meta_tag();
  if (tag < 0) return obj;
  dyn w;
  OBJECT(w, (uint64_t)tag, 2);
  LGET(w, 0) = obj;   /* object_ */
  LGET(w, 1) = src;   /* meta_   */
  return w;
}

dyn reader_parse_strip(dyn x) {
  if (is_tok(x)) {
    dyn parsed = tok_parsed(x);
    if (parsed) return reader_parse_strip(LGET(parsed, 0));
    return tok_value(x);
  }
  if (!is_list(x)) return x;
  uint64_t n = LIST_SIZE(x);
  if (n == 0) return x;

  kw_init();  /* needed for KW_none below */

  /* Capture meta from the unstripped head before recursing -- once
   * we replace it with the stripped value we lose the source info. */
  dyn meta_src = 0;
  dyn head = LGET(x, 0);
  if (is_tok(head)) {
    dyn orig = tok_orig(head);
    /* Skip `<none>` source: matches the Symta-side
     * `Meta.2 <> '<none>'` guard. */
    if (!text_eq_c(orig, KW_none)) {
      LIST(meta_src, 3);
      LGET(meta_src, 0) = tok_row(head);
      LGET(meta_src, 1) = tok_col(head);
      LGET(meta_src, 2) = orig;
    }
  }

  GC_DISABLE();
  dyn out; LIST(out, n);
  for (uint64_t i = 0; i < n; i++) {
    LGET(out, i) = reader_parse_strip(LGET(x, i));
  }
  GC_ENABLE();

  if (meta_src) {
    out = make_meta_wrapper(out, meta_src);
  }
  return out;
}

// Forward declarations (mutually recursive).
static dyn parse_term(pstate_t *p);
static dyn parse_dollar(pstate_t *p);
static dyn parse_suf(pstate_t *p);
static dyn parse_suf_loop(pstate_t *p, dyn (*down)(pstate_t*), dyn e);
static dyn parse_prefix(pstate_t *p);
static dyn parse_pow(pstate_t *p);
static dyn parse_mul(pstate_t *p);
static dyn parse_add(pstate_t *p);
static dyn parse_sufs(pstate_t *p);
static dyn parse_b_shift(pstate_t *p);
static dyn parse_b_and(pstate_t *p);
static dyn parse_b_xor(pstate_t *p);
static dyn parse_comma(pstate_t *p);
static dyn parse_bool(pstate_t *p);
static dyn parse_blogic(pstate_t *p);
static dyn parse_exclamation(pstate_t *p);
static dyn parse_logic(pstate_t *p);
static dyn parse_offside(pstate_t *p, dyn type, int expect_eol,
                          int orow, dyn ocol);
static dyn parse_if(pstate_t *p, dyn sym);
static dyn parse_bar(pstate_t *p, dyn h);
static dyn parse_table(dyn xs);
static dyn parse_delim(pstate_t *p, int delim_ctx);
static dyn parse_xs(pstate_t *p, int delim_ctx);
static int parse_semicolon(pstate_t *p);
static dyn parse_tokens_inner(dyn input);

// ====================================================================
// Generic builders.
// ====================================================================

static dyn make_list_n(int n, dyn *xs) {
  dyn r;
  LIST(r, n);
  for (int i = 0; i < n; i++) LGET(r, i) = xs[i];
  return r;
}

static dyn make_list_from_arr(dyn *arr) {
  return make_list_n(arrlen(arr), arr);
}

// ====================================================================
// parser_error -- emits a positioned rterr similar to reader.s:44.
// ====================================================================

static void parser_error(const char *cause, dyn tok) {
  if (is_tok(tok)) {
    int row = (int)UNFXN(tok_row(tok));
    int col = (int)UNFXN(tok_col(tok));
    char *orig = "<none>";
    if (IS_BIGTEXT(tok_orig(tok))) orig = text_to_cstring(tok_orig(tok));
    dyn tv = tok_value(tok);
    char *vs = "eof";
    if (tv) vs = print_object(tv);
    rterr("%s:%d,%d: %s `%s`", orig, row, col, cause, vs);
  } else {
    rterr("<?>: %s `%s`", cause, tok ? print_object(tok) : "eof");
  }
}

// ====================================================================
// parse_op -- consume a token if its .type is in the predicate set.
// ====================================================================

typedef int (*op_pred_t)(dyn);

static dyn parse_op(pstate_t *p, op_pred_t pred) {
  if (p_end(p)) return 0;
  dyn t = p_peek(p);
  if (!is_tok(t)) return 0;
  if (!pred(tok_type(t))) return 0;
  return p_pop(p);
}

// ====================================================================
// Backwards-compatible builders for tokens we synthesize.
// `parsed` (slot 6) must always be 0 unless we have a parsed cache.
// ====================================================================

static dyn mk_token(dyn type, dyn val, dyn row, dyn col, dyn orig,
                    dyn parsed) {
  dyn t = token(type, val, row, col, orig);
  LGET(t, 2) = 0;       // pchar
  LGET(t, 6) = parsed;
  return t;
}

// retok: reuse src of an existing token, change type+value.
static dyn retok(dyn old, dyn new_type) {
  return mk_token(new_type, new_type,
                  tok_row(old), tok_col(old), tok_orig(old), 0);
}

// ====================================================================
// parse_term -- reader.s:128-172.
//
// Dispatches on the token type. Returns a parsed-expr or 0 if no
// match (in which case the token is pushed back).
// ====================================================================

static dyn parse_term(pstate_t *p) {
  if (p_end(p)) return 0;
  dyn tok = p_pop(p);

  // Cached parsed form?
  if (tok_parsed(tok)) return tok;

  dyn val = tok_value(tok);
  dyn type = tok_type(tok);
  dyn parsed = 0;  // the inner parsed-expression list
  // `parsed` doubles as a "got something to cache" sentinel. But
  // for an integer literal of 0, FXN(0) == 0 which collides with
  // the "no parse" value. Track presence explicitly.
  int have_parsed = 0;

  if (text_eq_c(type, KW_escape) || text_eq_c(type, KW_symbol) ||
      text_eq_c(type, KW_text)) {
    return tok;
  }
  if (text_eq_c(type, KW_int_)) {
    int n = IS_BIGTEXT(val) ? BIGTEXT_SIZE(val) : (IS_FIXTEXT(val) ? fixtext_size(val) : 0);
    char *s = text_to_cstring(val);
    long long v;
    if (n > 2 && s[1] == 'x') {
      // 0x... or Nx... (radix from first char)
      if (s[0] == '0') v = strtoll(s + 2, 0, 16);
      else {
        int radix = (int)(s[0] - '0');
        v = strtoll(s + 2, 0, radix);
      }
    } else {
      v = strtoll(s, 0, 10);
    }
    LDFXN(parsed, v);
    have_parsed = 1;
  } else if (text_eq_c(type, KW_hex)) {
    char *s = text_to_cstring(val);
    long long v = strtoll(s + 1, 0, 16);
    LDFXN(parsed, v);
    have_parsed = 1;
  } else if (text_eq_c(type, KW_bin)) {
    char *s = text_to_cstring(val);
    long long v = strtoll(s + 2, 0, 2);
    LDFXN(parsed, v);
    have_parsed = 1;
  } else if (text_eq_c(type, KW_void_)) {
    // Symta source says `void | No`. `No` is the tagged immediate
    // MKIMM(T_NO,0), NOT integer zero. Using `0` here would print
    // the same in some contexts but compares differently under
    // truthiness checks -- `if 0` is falsy, `if No` is truthy.
    parsed = No;
    have_parsed = 1;
  } else if (text_eq_c(type, KW_parens)) {
    parsed = parse_tokens_inner(val);
    have_parsed = 1;
  } else if (text_eq_c(type, KW_brackets)) {
    // `[]` -- [symbol-`[]` @parsed_inner]
    dyn inner = parse_tokens_inner(val);
    uint64_t in = LIST_SIZE(inner);
    dyn head = mk_token(KW_symbol, KW_brackets,
                        tok_row(tok), tok_col(tok), tok_orig(tok), 0);
    dyn r; LIST(r, in + 1);
    LGET(r, 0) = head;
    for (uint64_t i = 0; i < in; i++) LGET(r, i + 1) = LGET(inner, i);
    parsed = r;
    have_parsed = 1;
  } else if (text_eq_c(type, KW_curlyL) || text_eq_c(type, KW_curly)) {
    // `{` / `{}` -- [symbol-`{` @parsed_inner]
    dyn inner = parse_tokens_inner(val);
    uint64_t in = LIST_SIZE(inner);
    dyn head = mk_token(KW_symbol, KW_curlyL,
                        tok_row(tok), tok_col(tok), tok_orig(tok), 0);
    dyn r; LIST(r, in + 1);
    LGET(r, 0) = head;
    for (uint64_t i = 0; i < in; i++) LGET(r, i + 1) = LGET(inner, i);
    parsed = r;
    have_parsed = 1;
  } else if (text_eq_c(type, KW_splice)) {
    // String splice -- value is a list of `symbol`/`()` tokens.
    // Output:
    //   [(symbol `"` Tok.src 0) @quoted-parsed-inner]
    // where each parsed item whose value contains `~` or `?` gets
    // wrapped as [\\-token, item]. This is what tells the
    // compiler to treat e.g. `?` in `"?"` as a literal symbol
    // rather than the quick-lambda auto-arg.
    //
    // Symta source:
    //   splice |: (token symbol `"` Tok.src 0)
    //             @V^parse_tokens{(1.any(?(:'~'+'?'))).value
    //                  =:(token '\\' '\\' Q.src 0) Q}
    dyn inner = parse_tokens_inner(val);
    uint64_t in = LIST_SIZE(inner);
    static dyn quote_text = 0;
    static dyn backslash_text = 0;
    if (!quote_text) {
      TEXT(quote_text, "\"");
      TEXT(backslash_text, "\\");
    }
    dyn head = mk_token(KW_symbol, quote_text,
                        tok_row(tok), tok_col(tok), tok_orig(tok), 0);
    // For each inner element, decide whether to wrap it.
    dyn *items = 0;
    arrput(items, head);
    for (uint64_t i = 0; i < in; i++) {
      dyn q = LGET(inner, i);
      int wrap = 0;
      if (is_tok(q)) {
        dyn qv = tok_value(q);
        if (IS_TEXT(qv)) {
          char *s = text_chars(qv);
          if (s) {
            for (char *p = s; *p; p++) {
              if (*p == '~' || *p == '?') { wrap = 1; break; }
            }
          }
        }
      }
      if (wrap) {
        dyn bs = mk_token(backslash_text, backslash_text,
                          tok_row(q), tok_col(q), tok_orig(q), 0);
        dyn pair; LIST(pair, 2);
        LGET(pair, 0) = bs;
        LGET(pair, 1) = q;
        arrput(items, pair);
      } else {
        arrput(items, q);
      }
    }
    int n = arrlen(items);
    dyn r; LIST(r, n);
    for (int i = 0; i < n; i++) LGET(r, i) = items[i];
    arrfree(items);
    parsed = r;
    have_parsed = 1;
  } else if (text_eq_c(type, KW_table)) {
    // `@{}` -- table literal. Symta evaluates this at parse time:
    //   Ts = parse_strip(parse_tokens(V))
    //   result = @t(@ parse_table(parse_strip(Ts)))
    // i.e. it builds an actual table object whose key/value pairs
    // are the parsed contents. The downstream compiler bakes the
    // literal table into bytecode as a constant.
    //
    // We mirror that by parsing the inner forms, then calling
    // amNew() + amSet() to construct a fresh table. The parsed
    // table goes into tok.parsed so parse_strip returns it as-is.
    dyn ts_raw = parse_tokens_inner(val);
    dyn ts = reader_parse_strip(ts_raw);
    dyn kvs = reader_parse_table(ts);
    dyn tbl = amNew();
    uint64_t kvn = LIST_SIZE(kvs);
    for (uint64_t i = 0; i < kvn; i++) {
      dyn kv = LGET(kvs, i);
      if (!is_list(kv) || LIST_SIZE(kv) < 2) continue;
      amSet(tbl, LGET(kv, 0), LGET(kv, 1));
    }
    parsed = tbl;
    have_parsed = 1;
  } else if (text_eq_c(type, KW_object)) {
    // `${}` -- object literal. Complex: needs `new_fn_` + class
    // conversion. Bail by emitting a placeholder; downstream
    // macroexpand will likely fail on this but better than silent
    // wrong AST.
    dyn ts_raw = parse_tokens_inner(val);
    dyn ts = reader_parse_strip(ts_raw);
    dyn kvs = reader_parse_table(ts);
    static dyn t_dollar_brace = 0;
    if (!t_dollar_brace) TEXT(t_dollar_brace, "${}");
    uint64_t kvn = LIST_SIZE(kvs);
    dyn r; LIST(r, kvn + 1);
    LGET(r, 0) = t_dollar_brace;
    for (uint64_t i = 0; i < kvn; i++) LGET(r, i + 1) = LGET(kvs, i);
    parsed = r;
    have_parsed = 1;
  } else if (text_eq_c(type, KW_pipe)) {
    return parse_bar(p, tok);  // already has a parsed-expr structure
  } else if (text_eq_c(type, KW_if_)) {
    return parse_if(p, tok);
  } else {
    // splice, @{}, ${}, unary, and operator types fall through here.
    // For the unhandled "complex" cases (splice strings, @{}, ${}),
    // bail and let Symta-side parse_term handle them.
    // For unary/operator types, parse_unary handling.
    if (is_unary_op_type(type) ||
        (text_eq_c(type, KW_unary))) {
      // parse_unary handles unary tokens.
      // Implementation inline: reader.s:310 parse_unary
      // | if is_eol H: ret [nullary_ H]
      // | A parse_prefix
      // | less A: ret [nullary_ H]
      // | if H.value<>'-': ret [H A]
      // | less A^token_is(int) or hex or float: ret [H A]
      // | token A.type "[H.value][A.value]" H.src [-A.parsed.0]
      dyn h = tok;
      // is_eol: next is on a different row, or col != H.col + value.n.
      int is_eol = 0;
      if (p_end(p)) is_eol = 1;
      else {
        dyn nx = p_peek(p);
        if (!is_tok(nx)) is_eol = 1;
        else {
          int nrow = (int)UNFXN(tok_row(nx));
          int hrow = (int)UNFXN(tok_row(h));
          int ncol = (int)UNFXN(tok_col(nx));
          int hcol = (int)UNFXN(tok_col(h));
          int hvn = IS_BIGTEXT(tok_value(h)) ?
                    BIGTEXT_SIZE(tok_value(h)) :
                    (IS_FIXTEXT(tok_value(h)) ? fixtext_size(tok_value(h)) : 0);
          if (nrow != hrow || ncol != hcol + hvn) is_eol = 1;
        }
      }
      static dyn nullary_sym = 0;
      if (!nullary_sym) TEXT(nullary_sym, "nullary_");
      // CRITICAL: Symta's parse_unary RETURNS `[nullary_ H]` directly
      // -- it does NOT cache on H.parsed. Caching would set
      // H.parsed = [[nullary_ H]] which contains H itself, and
      // parse_strip then loops on it forever. So return a plain
      // 2-list, no caching.
      if (is_eol) {
        dyn r; LIST(r, 2);
        LGET(r, 0) = nullary_sym;
        LGET(r, 1) = h;
        return r;
      }
      dyn a = parse_prefix(p);
      if (!a) {
        dyn r; LIST(r, 2);
        LGET(r, 0) = nullary_sym;
        LGET(r, 1) = h;
        return r;
      }
      // Special: H is `-` and A is an int/hex/float token --
      // collapse into a single negated-literal token, matching
      //   token A.type "[H.value][A.value]" H.src [-A.parsed.0]
      // Without this, `-3` parses as `[- 3]` (a binary form) which
      // the compiler tries to call `-` as a function and crashes.
      if (text_eq_c(tok_value(h), KW_minus) && is_tok(a)) {
        dyn at = tok_type(a);
        if (text_eq_c(at, KW_int_) || text_eq_c(at, KW_hex) ||
            text_eq_c(at, KW_bin)) {
          dyn ap = tok_parsed(a);
          if (ap && LIST_SIZE(ap) > 0) {
            // [-A.parsed.0] -- negate the parsed integer.
            dyn av = LGET(ap, 0);
            long long iv = (long long)UNFXN(av);
            dyn nv; LDFXN(nv, -iv);
            dyn np; LIST(np, 1); LGET(np, 0) = nv;
            // Build "-N" string for the new token's value.
            char *as_str = text_to_cstring(tok_value(a));
            int alen = (int)strlen(as_str);
            char buf[64];
            buf[0] = '-';
            int copy = alen < 62 ? alen : 62;
            for (int i = 0; i < copy; i++) buf[1 + i] = as_str[i];
            buf[1 + copy] = 0;
            dyn nv_text;
            TEXT(nv_text, buf);
            return mk_token(at, nv_text,
                            tok_row(h), tok_col(h), tok_orig(h),
                            np);
          }
        }
      }
      // Default: [H A] -- return list directly, NO cache on H.parsed
      // because H is itself in the list (self-reference would loop
      // parse_strip).
      dyn r; LIST(r, 2);
      LGET(r, 0) = h;
      LGET(r, 1) = a;
      return r;
    }
    // Not a known type -- push back and return 0.
    p_push_back(p, tok);
    return 0;
  }

  // Cache parsed on tok.parsed = [parsed]. Use the have_parsed
  // flag rather than `if (parsed)` because for integer/void/etc.
  // a legitimate parsed value can be 0 (FXN(0) == 0).
  if (have_parsed) {
    dyn pp; LIST(pp, 1); LGET(pp, 0) = parsed;
    LGET(tok, 6) = pp;
  }
  return tok;
}

// ====================================================================
// parse_dollar -- reader.s:223-226.
// ====================================================================

static dyn parse_dollar(pstate_t *p) {
  dyn o = parse_op(p, is_dollar_op);
  if (!o) return parse_term(p);
  // is unary? -> parse_unary path (treats o as the unary op token).
  if (token_is(o, KW_unary)) {
    // parse_unary inlined.
    return parse_term((p_push_back(p, o), p));  // re-enter via parse_term
  }
  dyn rhs = parse_dollar(p);
  if (!rhs) {
    parser_error("no operand for", o);
  }
  dyn r; LIST(r, 2); LGET(r, 0) = o; LGET(r, 1) = rhs;
  return r;
}

// ====================================================================
// parse_suf_loop / parse_suf -- reader.s:267-307.
// Implementation NOTE: this section in reader.s is the most intricate
// part of the parser (handles `.`, `^`, `->`, `[...]`, `{...}`, etc.
// postfix forms). We port it faithfully but with explicit state.
// ====================================================================

static dyn parse_suf_op(pstate_t *p) {
  return parse_op(p, is_suf_op);
}

// Implements parse_suf_unary's logic for invocation-style postfixes
// like `[]`, `()`, `{}`, `[`, `{` plus the SufUnaryS operator-as-suffix.
// Returns 0 if no match (and pushes O back); otherwise wraps E.
static dyn parse_suf_unary(pstate_t *p, dyn (*down)(pstate_t*),
                            dyn e, dyn o) {
  if (!is_suf_unary(tok_type(o))) return 0;
  dyn type = tok_type(o);
  if (text_eq_c(type, KW_parens) || text_eq_c(type, KW_brackets) ||
      text_eq_c(type, KW_bracketsL) || text_eq_c(type, KW_curly) ||
      text_eq_c(type, KW_curlyL)) {
    int valid = 0;
    dyn pc = tok_pchar(o);
    if (pc && is_anchor_char_c(pc)) {
      valid = 1;
      if (text_eq_c(type, KW_brackets)) {
        LGET(o, 0) = KW_bracketsL;  // type = `[`
        type = KW_bracketsL;
      }
    }
    if (!valid) {
      if (text_eq_c(type, KW_curly)) {
        LGET(o, 0) = KW_curlyL;
      }
      p_push_back(p, o);
      dyn r; LIST(r, 1); LGET(r, 0) = e;
      return r;
    }
    // valid suffix
  } else if (is_suf_unary_s(type)) {
    int valid = 0;
    dyn pc = tok_pchar(o);
    if (pc && is_anchor_char_c(pc)) {
      // next_is_disjoint check
      int disjoint = 1;
      if (!p_end(p)) {
        dyn nx = p_peek(p);
        if (is_tok(nx)) {
          int nrow = (int)UNFXN(tok_row(nx));
          int orow = (int)UNFXN(tok_row(o));
          int ncol = (int)UNFXN(tok_col(nx));
          int ocol = (int)UNFXN(tok_col(o));
          int ovn = IS_BIGTEXT(tok_value(o)) ?
                    BIGTEXT_SIZE(tok_value(o)) :
                    (IS_FIXTEXT(tok_value(o)) ? fixtext_size(tok_value(o)) : 0);
          if (nrow == orow && ncol == ocol + ovn &&
              !is_disjoint_char(tok_type(nx))) {
            disjoint = 0;
          }
        }
      }
      if (disjoint) {
        valid = 1;
        // append `_` to o.value if not already ending in `_`
        dyn v = tok_value(o);
        char *s = text_to_cstring(v);
        int sl = (int)strlen(s);
        if (sl == 0 || s[sl-1] != '_') {
          char *buf = (char*)malloc(sl + 2);
          memcpy(buf, s, sl);
          buf[sl] = '_';
          buf[sl+1] = 0;
          dyn nv; TEXT(nv, buf);
          LGET(o, 1) = nv;
          free(buf);
        }
      }
    }
    if (!valid) {
      p_push_back(p, o);
      dyn r; LIST(r, 1); LGET(r, 0) = e;
      return r;
    }
    // ret:: (parse_suf_loop Down [O E]) -- wrap in 1-element list.
    dyn pair; LIST(pair, 2); LGET(pair, 0) = o; LGET(pair, 1) = e;
    dyn inner_result = parse_suf_loop(p, down, pair);
    dyn wrap; LIST(wrap, 1); LGET(wrap, 0) = inner_result;
    return wrap;
  } else {
    // Other SufUnary type not in '[' '(' '{' or SufUnaryS.
  }

  // The "valid" case for brackets/parens/curly group: parse the inner.
  dyn val = tok_value(o);
  dyn as = parse_tokens_inner(val);
  uint64_t an = LIST_SIZE(as);
  int has_delim = 0;
  for (uint64_t i = 0; i < an; i++) {
    if (is_delim_filter(LGET(as, i))) { has_delim = 1; break; }
  }
  if (has_delim) {
    dyn wrapper; LIST(wrapper, 1); LGET(wrapper, 0) = as;
    as = wrapper;
    an = 1;
  }
  dyn ot_pair; LIST(ot_pair, 1); LGET(ot_pair, 0) = tok_type(o);
  LGET(o, 6) = ot_pair;
  // [O E @as]
  dyn full; LIST(full, an + 2);
  LGET(full, 0) = o;
  LGET(full, 1) = e;
  for (uint64_t i = 0; i < an; i++) LGET(full, i + 2) = LGET(as, i);
  // ret:: parse_suf_loop ... -- wrap result in 1-element list.
  dyn inner_result = parse_suf_loop(p, down, full);
  dyn wrap; LIST(wrap, 1); LGET(wrap, 0) = inner_result;
  return wrap;
}

static dyn parse_suf_loop(pstate_t *p, dyn (*down)(pstate_t*), dyn e) {
  // Mirror of reader.s:272-304. NOT iterative -- the recursive case
  // is inside parse_suf_unary or in the binary-suffix fallback.
  //
  //   parse_suf_loop Down E =
  //   | O | parse_suf_op or ret E
  //   | R parse_suf_unary Down E O
  //   | if R: ret R.0           // R is always a single-element list
  //                             // wrapping the final value
  //   | B | Down()
  //   | less B:
  //     | ... `.` / `^` method-name idiom or parser_error ...
  //   | less O^token_is('.') and E^token_is(int) and B^token_is(int):
  //     | ret: parse_suf_loop Down [O E B]
  //   | V "[E.value].[B.value]"
  //   | F token float V E.src [V.flt]
  //   | ret: parse_suf_loop Down F
  dyn o = parse_suf_op(p);
  if (!o) return e;
  dyn r = parse_suf_unary(p, down, e, o);
  if (r) return LGET(r, 0);
  // parse_suf_unary returned 0 -- O is a binary suffix (`.` / `^` / `->`).
  dyn b = down(p);
  if (!b) {
    if (token_is(o, KW_dot) || token_is(o, KW_caret)) {
      if (!p_end(p)) {
        dyn t = p_peek(p);
        if (is_tok(t) && is_opt(tok_value(t))) {
          dyn tv = tok_value(t);
          // Symta reader.s:282-291: if T.value is `!`, transform to
          // `:`. Then if T.value is `:`, this is the .:` postfix
          // colon-line form -- run parse_delim under a fresh GO=[T],
          // retag T back to its original value, recurse parse_suf_loop.
          static dyn t_bang = 0;
          static dyn t_colon = 0;
          if (!t_bang) { TEXT(t_bang, "!"); TEXT(t_colon, ":"); }
          int was_bang = text_eq_c(tv, t_bang);
          if (was_bang) {
            LGET(t, 0) = KW_colon;
            LGET(t, 1) = t_colon;
            tv = t_colon;
          }
          if (text_eq_c(tv, t_colon)) {
            // R = let GOutput [T]: parse_delim 0; GOutput.f
            dyn *saved_go = p->go;
            p->go = 0;
            arrput(p->go, t);  // GOutput initial = [T]
            // parse_delim now sees `:` as first token. parse_op for
            // delim picks it up.
            (void)parse_delim(p, 0);
            int gn = arrlen(p->go);
            dyn r; LIST(r, gn);
            for (int i = 0; i < gn; i++) LGET(r, i) = p->go[i];
            arrfree(p->go);
            p->go = saved_go;
            // R.1 = T.retok(TV) -- create a fresh token with the
            // ORIGINAL value (so a `!` form recorded as `!`).
            dyn retv = was_bang ? t_bang : t_colon;
            dyn retok_tok = mk_token(retv, retv,
                                      tok_row(t), tok_col(t),
                                      tok_orig(t), 0);
            if (LIST_SIZE(r) >= 2) LGET(r, 1) = retok_tok;
            dyn triple; LIST(triple, 3);
            LGET(triple, 0) = o;
            LGET(triple, 1) = e;
            LGET(triple, 2) = r;
            return parse_suf_loop(p, down, triple);
          }
          // method-name idiom: X.op  (and `=` + keyword pair).
          // See reader.s:278-298 -- after consuming a `.` or `^`
          // and finding that the next token is an OpsT operator,
          // we pop it, retype to `symbol`, and if its value is
          // `=` we ALSO try to glue the following keyword onto
          // it so `tok.=src` becomes the single method-name
          // token "=src" rather than splitting into `=` and `src`.
          (void)p_pop(p);
          LGET(t, 0) = KW_symbol;
          // `=`+keyword combiner: produces setter-style names
          // like `=src` / `=value` / `=type`.
          static dyn t_eq = 0;
          if (!t_eq) TEXT(t_eq, "=");
          if (text_eq_c(tok_value(t), t_eq) && !p_end(p)) {
            dyn k = p_peek(p);
            if (is_tok(k) && IS_TEXT(tok_value(k))) {
              char *ks = text_chars(tok_value(k));
              if (ks && ks[0]) {
                unsigned char c0 = (unsigned char)ks[0];
                int is_keyword = !(c0 >= 'A' && c0 <= 'Z');
                if (is_keyword) {
                  (void)p_pop(p);
                  char *kvs = text_chars(tok_value(k));
                  char buf[128];
                  int n = snprintf(buf, sizeof(buf), "=%s",
                                   kvs ? kvs : "");
                  (void)n;
                  dyn nv;
                  TEXT(nv, buf);
                  LGET(t, 1) = nv;
                }
              }
            }
          }
          dyn r2; LIST(r2, 3);
          LGET(r2, 0) = o; LGET(r2, 1) = e; LGET(r2, 2) = t;
          // Symta's `ret [O E T]` returns without continuing the
          // postfix chain.
          return r2;
        }
      }
    }
    parser_error("no right operand for", o);
    return 0;
  }
  // X.Y where both are ints -> float token, then continue with that.
  if (token_is(o, KW_dot) && token_is(e, KW_int_) && token_is(b, KW_int_)) {
    // text_to_cstring uses a shared static buffer (api.sbuf), so a
    // second call clobbers the first. Snapshot the first into a
    // local before reading the second.
    char *raw1 = text_to_cstring(tok_value(e));
    int el = (int)strlen(raw1);
    char es_buf[64];
    if (el >= (int)sizeof(es_buf)) el = (int)sizeof(es_buf) - 1;
    memcpy(es_buf, raw1, el);
    es_buf[el] = 0;
    char *es = es_buf;
    char *bs = text_to_cstring(tok_value(b));
    int bl = (int)strlen(bs);
    char *buf = (char*)malloc(el + bl + 2);
    memcpy(buf, es, el);
    buf[el] = '.';
    memcpy(buf + el + 1, bs, bl);
    buf[el + bl + 1] = 0;
    dyn v; TEXT(v, buf);
    static dyn t_float = 0;
    if (!t_float) TEXT(t_float, "float");
    // Symta: `F token float V E.src [V.flt]` -- pre-fills the
    // parsed slot with [V.flt] so parse_strip yields the float
    // immediately rather than treating "3.14" as a symbol.
    double dv = atof(buf);
    dyn flt;
    LDFLT(flt, dv);
    dyn parsed_list; LIST(parsed_list, 1);
    LGET(parsed_list, 0) = flt;
    dyn ftok = mk_token(t_float, v, tok_row(e), tok_col(e),
                         tok_orig(e), parsed_list);
    free(buf);
    return parse_suf_loop(p, down, ftok);
  }
  dyn r2; LIST(r2, 3);
  LGET(r2, 0) = o; LGET(r2, 1) = e; LGET(r2, 2) = b;
  return parse_suf_loop(p, down, r2);
}

static dyn parse_suf(pstate_t *p) {
  dyn d = parse_dollar(p);
  if (!d) return 0;
  return parse_suf_loop(p, parse_dollar, d);
}

// ====================================================================
// parse_prefix / parse_unary stub (parse_unary handled inline in
// parse_term's unary branch).
// ====================================================================

static dyn parse_prefix(pstate_t *p) {
  dyn o = parse_op(p, is_prefix_op);
  if (!o) return parse_suf(p);
  // unary -> parse_term re-entry via push back
  if (token_is(o, KW_unary)) {
    p_push_back(p, o);
    return parse_term(p);  // parse_term will detect unary and chain
  }
  dyn pref = parse_prefix(p);
  if (!pref) {
    // Check for SufBinary following. Symta uses `/GInput` (pop),
    // i.e. it CONSUMES the suf-binary token to form the nullary
    // pair. Without the pop, the outer parse_suf_loop would see
    // the same token again and apply it as a postfix on the
    // [O [nullary_ .]] result -- producing `@.0` as
    // `(. (@ nullary_-dot) 0)` instead of two separate top-level
    // forms `(@ nullary_-dot)` and `0`.
    if (!p_end(p)) {
      dyn t = p_peek(p);
      if (is_tok(t) && is_suf_binary(tok_value(t))) {
        (void)p_pop(p);  // /GInput
        static dyn nullary_ = 0;
        if (!nullary_) TEXT(nullary_, "nullary_");
        dyn r; LIST(r, 2); LGET(r, 0) = nullary_; LGET(r, 1) = t;
        pref = r;
      }
    }
  }
  if (pref) {
    dyn r; LIST(r, 2); LGET(r, 0) = o; LGET(r, 1) = pref;
    return r;
  }
  dyn r; LIST(r, 1); LGET(r, 0) = o;
  return r;
}

// ====================================================================
// Precedence ladder helper.
// ====================================================================

static dyn binary_loop(pstate_t *p, op_pred_t ops_pred,
                       dyn (*down)(pstate_t*), dyn e) {
  for (;;) {
    dyn o = parse_op(p, ops_pred);
    if (!o) return e;
    dyn b = down(p);
    dyn ne;
    if (b) {
      LIST(ne, 3); LGET(ne, 0) = o; LGET(ne, 1) = e; LGET(ne, 2) = b;
    } else {
      // O.value = O.value + "_"
      dyn v = tok_value(o);
      char *s = text_to_cstring(v);
      int sl = (int)strlen(s);
      char *buf = (char*)malloc(sl + 2);
      memcpy(buf, s, sl);
      buf[sl] = '_';
      buf[sl+1] = 0;
      dyn nv; TEXT(nv, buf);
      LGET(o, 1) = nv;
      free(buf);
      LIST(ne, 2); LGET(ne, 0) = o; LGET(ne, 1) = e;
    }
    e = ne;
  }
}

static inline dyn parse_binary(pstate_t *p, op_pred_t ops_pred,
                                dyn (*down)(pstate_t*)) {
  dyn e = down(p);
  if (!e) return 0;
  return binary_loop(p, ops_pred, down, e);
}

static int op_pow(dyn t)    { return text_eq_c(t, KW_pow); }
static int op_mul(dyn t)    { return text_eq_c(t, KW_star) || text_eq_c(t, KW_slash) || text_eq_c(t, KW_pct); }
static int op_add(dyn t)    { return text_eq_c(t, KW_plus) || text_eq_c(t, KW_minus); }
static int op_sufs(dyn t)   { return text_eq_c(t, KW_range); }
static int op_bshift(dyn t) { return text_eq_c(t, KW_shl) || text_eq_c(t, KW_shr); }
static int op_band(dyn t)   { return text_eq_c(t, KW_band); }
static int op_bxor(dyn t)   { return text_eq_c(t, KW_bor) || text_eq_c(t, KW_bxor); }
static int op_comma(dyn t)  { return text_eq_c(t, KW_comma); }
static int op_blogic(dyn t) { return text_eq_c(t, KW_and_) || text_eq_c(t, KW_or_); }
static int op_excl(dyn t)   { return text_eq_c(t, KW_bang); }

static dyn parse_pow(pstate_t *p)       { return parse_binary(p, op_pow, parse_prefix); }
static dyn parse_mul(pstate_t *p)       { return parse_binary(p, op_mul, parse_pow); }
static dyn parse_add(pstate_t *p)       { return parse_binary(p, op_add, parse_mul); }
static dyn parse_sufs(pstate_t *p)      { return parse_binary(p, op_sufs, parse_add); }
static dyn parse_b_shift(pstate_t *p)   { return parse_binary(p, op_bshift, parse_sufs); }
static dyn parse_b_and(pstate_t *p)     { return parse_binary(p, op_band, parse_b_shift); }
static dyn parse_b_xor(pstate_t *p)     { return parse_binary(p, op_bxor, parse_b_and); }
static dyn parse_comma(pstate_t *p)     { return parse_binary(p, op_comma, parse_b_xor); }
static dyn parse_bool(pstate_t *p)      { return parse_binary(p, is_bool_op, parse_comma); }
static dyn parse_blogic(pstate_t *p)    { return parse_binary(p, op_blogic, parse_bool); }

static dyn parse_exclamation(pstate_t *p) {
  // Symta's parse_excl_loop: left-folds `!` without consuming a
  // right operand, and WITHOUT appending `_` to the op value:
  //   parse_excl_loop Ops E =
  //     | O | parse_op Ops or ret E
  //     | parse_excl_loop Ops [O E]
  // Different from binary_loop -- this preserves `Env!` as
  // `(`!` Env)` instead of `(`!_` Env)`. The empty-table macro
  // looks for the `!` keyword specifically.
  if (!p_end(p)) {
    dyn t = p_peek(p);
    if (is_tok(t) && text_eq_c(tok_type(t), KW_bang)) {
      return p_pop(p);
    }
  }
  dyn e = parse_blogic(p);
  if (!e) return 0;
  for (;;) {
    dyn o = parse_op(p, op_excl);
    if (!o) return e;
    // No right operand fetched -- just left-fold [O E].
    dyn ne; LIST(ne, 2); LGET(ne, 0) = o; LGET(ne, 1) = e;
    e = ne;
  }
}

// ====================================================================
// parse_offside / parse_if / parse_bar / parse_logic / parse_delim
//
// These are sufficiently complex that we delegate back to Symta for
// now. Once the inner ladder is verified, port them in a follow-up.
//
// To keep parse_term/parse_dollar/parse_suf working through these
// branches, parse_if and parse_bar must produce valid AST. We do
// the minimal structural work here.
// ====================================================================

// Build a synthetic call out to Symta. Not implemented in this
// initial cut; we expect text.parse to keep using the Symta-side
// parser until these functions are complete.
//
// As a placeholder, parse_if/parse_bar return 0 -- the caller's
// parse_term then returns 0, signalling no-match and parsing continues
// or errors. We never actually reach parse_term's `|` or `if` cases
// when text.parse is using the Symta path; this code only runs once
// we flip the switch in reader.s.

// ====================================================================
// parse_bar -- reader.s:87-97.
//
//   parse_bar H =
//   | C H.src.1
//   | Zs:
//   | while not GInput.end:
//     | Ys:
//     | while not GInput.end and GInput.0.src.1 > C: push GInput^pop Ys
//     | push Ys.f^parse_tokens Zs
//     | when GInput.end: ret   [H @Zs.f]
//     | X GInput.0
//     | less X^token_is('|') and X.src.1 >< C: ret   [H @Zs.f]
//     | pop GInput
//
// Gathers indented sub-blocks separated by `|` tokens at column C
// (the column of the leading `|`). Each block is parsed by recursing
// into parse_tokens_inner. Returns [H @blocks].
// ====================================================================

static dyn parse_bar(pstate_t *p, dyn h) {
  int c = (int)UNFXN(tok_col(h));
  dyn *zs = 0;
  for (;;) {
    if (p_end(p)) break;
    // Collect Ys = tokens with col > c (the block body).
    dyn *ys = 0;
    while (!p_end(p)) {
      dyn t = p_peek(p);
      if (!is_tok(t)) break;
      int tc = (int)UNFXN(tok_col(t));
      if (tc <= c) break;
      arrput(ys, p_pop(p));
    }
    dyn yslist;
    int yn = arrlen(ys);
    LIST(yslist, yn);
    for (int i = 0; i < yn; i++) LGET(yslist, i) = ys[i];
    arrfree(ys);
    dyn parsed = parse_tokens_inner(yslist);
    arrput(zs, parsed);
    if (p_end(p)) break;
    dyn x = p_peek(p);
    if (!is_tok(x)) break;
    if (!text_eq_c(tok_type(x), KW_pipe)) break;
    if ((int)UNFXN(tok_col(x)) != c) break;
    (void)p_pop(p);  // consume the next `|`
  }
  int zn = arrlen(zs);
  dyn r;
  LIST(r, zn + 1);
  LGET(r, 0) = h;
  for (int i = 0; i < zn; i++) LGET(r, i + 1) = zs[i];
  arrfree(zs);
  return r;
}

// ====================================================================
// parse_offside -- reader.s:369-411.
// Indentation-sensitive block parser. Collects same-col-or-deeper
// tokens, splitting at same-col occurrences (auto-bar) into separate
// statements, then parse_tokens on the result.
// ====================================================================

static int is_var_tok(dyn x) {
  if (!is_tok(x)) return 0;
  dyn v = tok_value(x);
  // Symta core_.s:222 defines `text.is_keyword = not: $n and $0.is_upcase`.
  // So `is_var_tok` == is_text && not is_keyword == is_text && (n>0 &&
  // first char is uppercase). Operationally: identifiers that start
  // with an uppercase ASCII letter are variables; everything else
  // (operators like `!`, `+`, lowercase keywords like `if`, `then`,
  // empty texts, numbers-as-text) is NOT a variable.
  if (!IS_TEXT(v)) return 0;
  char *s = text_chars(v);
  if (!s || !s[0]) return 0;
  unsigned char c = (unsigned char)s[0];
  return c >= 'A' && c <= 'Z';
}

static dyn parse_offside(pstate_t *p, dyn type, int expect_eol,
                          int orow, dyn ocol) {
  if (p_end(p)) {
    dyn empty;
    LIST(empty, 0);
    return empty;
  }
  // Symta's `ret : parse_xs 0` returns parse_xs result directly --
  // the `:` is part of `ret`'s colon-line syntax (`ret: X` means
  // `return X`), not a list constructor. parse_if's `ret:: Sym X.0`
  // takes the FIRST element of the parse_xs result, which is the
  // operator symbol for delim-headed forms.
  #define RET_XS_WRAPPED() return parse_xs(p, 0)
  dyn k = p_peek(p);
  while (is_list(k) && LIST_SIZE(k) > 0) k = LGET(k, 0);
  if (!is_tok(k)) RET_XS_WRAPPED();
  int k_row = (int)UNFXN(tok_row(k));
  int k_col = (int)UNFXN(tok_col(k));
  int lines_hack = 0;
  if (!(orow < k_row)) {
    if (expect_eol) RET_XS_WRAPPED();
    lines_hack = 1;
  }
  if (text_eq_c(tok_value(k), KW_pipe) ||
      text_eq_c(tok_value(k), KW_colon)) {
    RET_XS_WRAPPED();
  }
  int s_col = k_col;
  if (ocol != No && (int)UNFXN(ocol) >= s_col) {
    RET_XS_WRAPPED();
  }
  // Snapshot input position for potential rollback. We capture the
  // FULL pushback contents (not just the length): the lines_hack
  // rollback below may fire after the collection loop has consumed
  // tokens out of pushback, so we need to restore the originals,
  // not just trim what was added.
  uint64_t s_pos = p->gi_pos;
  int s_pn = arrlen(p->pushback);
  dyn *s_pb = 0;
  for (int i = 0; i < s_pn; i++) arrput(s_pb, p->pushback[i]);
  #define PARSE_OFFSIDE_ROLLBACK() do {                          \
    p->gi_pos = s_pos;                                           \
    arrsetlen(p->pushback, 0);                                   \
    for (int _i = 0; _i < s_pn; _i++) arrput(p->pushback, s_pb[_i]); \
  } while (0)
  dyn h = parse_exclamation(p);
  // Rollback (Symta restores GInput regardless of H's value).
  PARSE_OFFSIDE_ROLLBACK();
  // case H [X Y]: if X^token_is(`!`): less Y^is_var_tok: ret: parse_xs 0
  if (h && is_list(h) && LIST_SIZE(h) == 2) {
    dyn x = LGET(h, 0);
    dyn y = LGET(h, 1);
    if (is_tok(x) && text_eq_c(tok_type(x), KW_bang)) {
      if (!is_var_tok(y)) RET_XS_WRAPPED();
    }
  }
  dyn *zs = 0;
  dyn *ys = 0;
  // BSrc: [Src.row Src.col-1 Src.orig]
  dyn bsrc_row = tok_row(k);
  dyn bsrc_col; LDFXN(bsrc_col, k_col - 1);
  dyn bsrc_orig = tok_orig(k);
  while (!p_end(p)) {
    dyn x = p_peek(p);
    if (is_tok(x)) {
      int col = (int)UNFXN(tok_col(x));
      if (col < s_col) break;
      dyn v = tok_value(x);
      /* CORE-4: when collecting an `if X:` body, a same-indent
       * `then` / `else` / `elif` ends the body so parse_if's
       * outer trailer-handling can attach it.  Without this, e.g.
       *   if X > 0:
       *     say "positive"
       *     else say "no"
       * dropped `else` into the body's token stream and the
       * inner parse_tokens_inner failed with "unexpected else".
       *
       * Suppress the break if the current segment (`ys`) begins
       * with an `if` keyword -- the else/elif belongs to that
       * inner one-line if, e.g.
       *   if A:
       *     if X: 1
       *     else 2
       * keeps the inner else attached to `if X: 1`. */
      if (text_eq_c(type, KW_if_) && col == s_col &&
          (text_eq_c(v, KW_then) || text_eq_c(v, KW_else_) ||
           text_eq_c(v, KW_elif))) {
        int yn = arrlen(ys);
        // Suppress the break when the current segment STARTS with
        // `if` -- that means the else/elif belongs to that inner
        // one-line if (e.g. `if A: \n  if X: 1 \n  else 2`).  In
        // Symta `push` prepends, so `Ys.~` (last) corresponds to
        // the FIRST inserted item; our `arrput` appends so we look
        // at index 0 instead.
        dyn first = yn > 0 ? ys[0] : 0;
        int has_open_if = yn > 0 && is_tok(first) &&
                          text_eq_c(tok_value(first), KW_if_);
        if (!has_open_if) break;
      }
      if (col == s_col && arrlen(ys) > 0 &&
          !text_eq_c(v, KW_pipe) && !text_eq_c(v, KW_else_) &&
          !text_eq_c(v, KW_elif) && !text_eq_c(v, KW_then)) {
        /* Always insert an auto-bar at same-indent statement
         * boundaries.  The original Symta source had a
         * `less Type >< 'if'` guard here that suppressed the
         * bar in if-bodies -- dead code because parse_if used to
         * call parse_offside with Type=0, but it fired once we
         * began passing Type='if' for CORE-4 and joined separate
         * if-body statements into one expression. */
        dyn bar = make_bar(bsrc_row, bsrc_col, bsrc_orig);
        int yn = arrlen(ys);
        dyn block;
        LIST(block, yn + 1);
        LGET(block, 0) = bar;
        for (int i = 0; i < yn; i++) LGET(block, i + 1) = ys[i];
        arrput(zs, block);
        arrfree(ys); ys = 0;
      }
    }
    arrput(ys, p_pop(p));
  }
  {
    dyn bar = make_bar(bsrc_row, bsrc_col, bsrc_orig);
    int yn = arrlen(ys);
    dyn block;
    LIST(block, yn + 1);
    LGET(block, 0) = bar;
    for (int i = 0; i < yn; i++) LGET(block, i + 1) = ys[i];
    arrput(zs, block);
    arrfree(ys); ys = 0;
  }
  if (lines_hack && arrlen(zs) <= 1) {
    PARSE_OFFSIDE_ROLLBACK();
    arrfree(zs);
    arrfree(s_pb);
    RET_XS_WRAPPED();
  }
  arrfree(s_pb);
  #undef PARSE_OFFSIDE_ROLLBACK
  // Input = Zs.f.j -- forward, joined (each block is a list of toks)
  dyn *flat = 0;
  for (int i = 0; i < arrlen(zs); i++) {
    dyn block = zs[i];
    int bn = LIST_SIZE(block);
    for (int j = 0; j < bn; j++) arrput(flat, LGET(block, j));
  }
  int fn = arrlen(flat);
  dyn input;
  LIST(input, fn);
  for (int i = 0; i < fn; i++) LGET(input, i) = flat[i];
  arrfree(flat);
  arrfree(zs);
  dyn r = parse_tokens_inner(input);
  // Symta `R.f` reverses the parse_tokens result. In Symta this is
  // needed because parse_xs accumulates via push (which prepends);
  // .f undoes that to give insertion order. Our parse_xs uses
  // arrput which is already insertion order, so no reverse needed.
  return r;
}

// ====================================================================
// parse_if -- reader.s:58-85.
// ====================================================================

static int is_next(pstate_t *p, dyn type) {
  if (p_end(p)) return 0;
  dyn t = p_peek(p);
  return is_tok(t) && text_eq_c(tok_type(t), type);
}

static dyn parse_if(pstate_t *p, dyn sym) {
  int o_col = (int)UNFXN(tok_col(sym));
  dyn ocol_dyn; LDFXN(ocol_dyn, o_col);
  if (is_next(p, KW_colon)) {
    dyn t = p_pop(p);
    if (!text_eq_c(tok_type(t), KW_colon)) {
      parser_error("missing `:` for", sym);
      return 0;
    }
    dyn x = parse_offside(p, 0, 0, (int)UNFXN(tok_row(sym)), ocol_dyn);
    // ret:: Sym X.0
    if (is_list(x) && LIST_SIZE(x) > 0) {
      dyn r; LIST(r, 2);
      LGET(r, 0) = sym;
      LGET(r, 1) = LGET(x, 0);
      return r;
    }
    dyn r; LIST(r, 1);
    LGET(r, 0) = sym;
    return r;
  }
  // gather RawHead = tokens until end / : / then
  dyn *raw = 0;
  while (!p_end(p)) {
    if (is_next(p, KW_colon) || is_next(p, KW_then)) break;
    arrput(raw, p_pop(p));
  }
  dyn raw_list;
  int rn = arrlen(raw);
  LIST(raw_list, rn);
  for (int i = 0; i < rn; i++) LGET(raw_list, i) = raw[i];
  arrfree(raw);
  // Cnd = let GInput RawHead: parse_xs 1
  dyn cnd;
  {
    pstate_t inner;
    p_init(&inner, raw_list);
    cnd = parse_xs(&inner, 1);
    p_free(&inner);
  }
  // Symta keeps Cnd as the parse_xs list (e.g. `(X)`) -- don't .0
  // extract or we lose the wrapping.
  dyn then_, else_;
  if (is_next(p, KW_then)) {
    dyn t = p_pop(p);
    // CORE-4: pass `if` as parse_offside's Type so a same-indent
    // `else`/`elif`/`then` ends the body and propagates back up to
    // us (handled at the trailer below).  Without this, an `else`
    // at body indent gets absorbed into the body and parse_xs dies
    // with `unexpected else`.  The Symta-side parser made the same
    // switch (reader.s `Type='if'`).
    then_ = parse_offside(p, KW_if_, 0, (int)UNFXN(tok_row(t)), ocol_dyn);
  } else {
    if (!is_next(p, KW_colon)) {
      parser_error("missing `:` for", sym);
      return 0;
    }
    dyn t = p_pop(p);
    then_ = parse_offside(p, KW_if_, 0, (int)UNFXN(tok_row(t)), ocol_dyn);
  }
  // `Else: ` -- Symta initialises Else to empty list, not 0.
  LIST(else_, 0);
  if (is_next(p, KW_elif)) {
    dyn t = p_pop(p);
    // GInput =: T.retok(`else`) T.retok(`if`) @GInput
    dyn else_tok = retok(t, KW_else_);
    dyn if_tok = retok(t, KW_if_);
    p_push_back(p, if_tok);
    p_push_back(p, else_tok);
  }
  if (is_next(p, KW_else_)) {
    dyn t = p_pop(p);
    else_ = parse_offside(p, 0, 0, (int)UNFXN(tok_row(t)), ocol_dyn);
  }
  // `:Sym Cnd Then Else` -- 4-element list. Cnd/Then/Else stay
  // as-is (lists or whatever the inner parsers produced).
  dyn r;
  LIST(r, 4);
  LGET(r, 0) = sym;
  LGET(r, 1) = cnd;
  LGET(r, 2) = then_ ? then_ : ({ dyn e; LIST(e, 0); e; });
  LGET(r, 3) = else_;
  return r;
}

// ====================================================================
// parse_logic / parse_delim / parse_semicolon / parse_xs / parse_tokens
// ====================================================================

// parse_logic -- reader.s:349-362.
//
// Symta's logic ports as:
//   - If next token is NOT `and`/`or`: return parse_exclamation result
//     (no side effects on GOutput).
//   - Else: consume the operator. Snapshot current GOutput as the LHS
//     list. Recursively call parse_xs to consume the rest. Set GOutput
//     to [O lhs rhs] (post-reverse form). Return 0 (signals
//     "GOutput modified, parse_xs loop should exit").
//
// parse_logic with the LL(1) `:`-continuation hack from
// reader.s:349-362.
//
// After an `and`/`or` operator, look ahead for the first delim
// op (`:`/`=`/`<=`/`=>`/etc.):
//   - if no delim, or delim is in CndList (if/then/else/elif):
//     simple path -- consume the rest of the input as the RHS via
//     a nested parse_xs.
//   - else: take only the tokens BEFORE the delim as R, parse them
//     in isolation, and DON'T consume the delim. The outer
//     parse_xs will pick up the delim on its next iteration (or
//     parse_tokens will run the nullary-recovery dance on it).
//     For `:` specifically the lhs's head pops out of the inner
//     triple and becomes a sibling -- so the outer `:` form sees
//     it as its Pref.
//
// `Xs[lhs.head]` floats up in the colon case so that
// `till A or B or C: body` parses as `(: (till (or (A) (or (B) (C)))) (body))`
// instead of `((or (A till) (or (B) (: (C) (body)))))`.
static dyn parse_logic(pstate_t *p) {
  static int logic_initted = 0;
  static dyn t_and = 0, t_or = 0;
  static dyn t_if = 0, t_then = 0, t_else = 0, t_elif = 0;
  if (!logic_initted) {
    TEXT(t_and, "and");
    TEXT(t_or, "or");
    TEXT(t_if, "if");
    TEXT(t_then, "then");
    TEXT(t_else, "else");
    TEXT(t_elif, "elif");
    logic_initted = 1;
  }
  if (p_end(p)) return parse_exclamation(p);
  dyn t = p_peek(p);
  if (!is_tok(t)) return parse_exclamation(p);
  dyn ty = tok_type(t);
  if (!text_eq_c(ty, t_and) && !text_eq_c(ty, t_or)) {
    return parse_exclamation(p);
  }
  // Consume the and/or operator.
  dyn o = p_pop(p);
  // Snapshot p->go as the LHS list. Symta's `GOutput.f` reverses a
  // push-prepended GOutput to forward order. Our arrput is already
  // forward, so we copy without reversing.
  int gn = arrlen(p->go);
  dyn lhs;
  LIST(lhs, gn);
  for (int i = 0; i < gn; i++) LGET(lhs, i) = p->go[i];
  // Look ahead for first is_delim_op in remaining input.
  dyn delim_tok = 0;
  int found_at = -1;          // position relative to current pop point
  {
    int idx = 0;
    int pn = arrlen(p->pushback);
    for (int i = pn - 1; i >= 0; i--, idx++) {
      dyn x = p->pushback[i];
      if (is_tok(x) && is_delim_op(tok_type(x))) {
        delim_tok = x; found_at = idx; break;
      }
    }
    if (!delim_tok) {
      for (uint64_t i = p->gi_pos; i < p->gi_n; i++, idx++) {
        dyn x = LGET(p->gi, i);
        if (is_tok(x) && is_delim_op(tok_type(x))) {
          delim_tok = x; found_at = idx; break;
        }
      }
    }
  }
  int use_hack = 0;
  int delim_is_colon = 0;
  static dyn t_colon = 0;
  if (!t_colon) TEXT(t_colon, ":");
  if (delim_tok) {
    dyn dv = tok_value(delim_tok);
    int in_cndlist = text_eq_c(dv, t_if) || text_eq_c(dv, t_then) ||
                     text_eq_c(dv, t_else) || text_eq_c(dv, t_elif);
    if (!in_cndlist) {
      use_hack = 1;
      if (text_eq_c(dv, t_colon)) delim_is_colon = 1;
    }
  }
  if (!use_hack) {
    // Simple path: GOutput = [(parse_xs 0) GOutput O], yielding
    // post-reverse [O lhs rhs] in p->go.
    dyn *saved_go = p->go;
    p->go = 0;
    dyn rhs = parse_xs(p, 0);
    arrfree(p->go);
    p->go = saved_go;
    arrsetlen(p->go, 0);
    arrput(p->go, o);
    arrput(p->go, lhs);
    arrput(p->go, rhs);
    return No;
  }
  // LL(1) hack: R = take(found_at), parse it separately, leave the
  // delim in the input stream.
  dyn *r_arr = 0;
  for (int i = 0; i < found_at; i++) arrput(r_arr, p_pop(p));
  int rn = arrlen(r_arr);
  dyn r_list;
  LIST(r_list, rn);
  for (int i = 0; i < rn; i++) LGET(r_list, i) = r_arr[i];
  arrfree(r_arr);
  dyn rhs = parse_tokens_inner(r_list);
  arrsetlen(p->go, 0);
  if (delim_is_colon) {
    // GOutput = [[O GO.tail R-parsed] GO.head] (pre-reverse).
    // Post-reverse: [GO.head, triple].
    uint64_t ln = LIST_SIZE(lhs);
    if (ln == 0) {
      dyn triple; LIST(triple, 3);
      LGET(triple, 0) = o; LGET(triple, 1) = lhs; LGET(triple, 2) = rhs;
      arrput(p->go, triple);
    } else {
      dyn lhs_head = LGET(lhs, 0);
      dyn lhs_tail;
      LIST(lhs_tail, ln - 1);
      for (uint64_t i = 1; i < ln; i++) LGET(lhs_tail, i - 1) = LGET(lhs, i);
      dyn triple; LIST(triple, 3);
      LGET(triple, 0) = o;
      LGET(triple, 1) = lhs_tail;
      LGET(triple, 2) = rhs;
      arrput(p->go, lhs_head);
      arrput(p->go, triple);
    }
  } else {
    // Non-`:` delim. GOutput = [[O GO R-parsed]] (pre-reverse, 1-wrap).
    dyn triple; LIST(triple, 3);
    LGET(triple, 0) = o;
    LGET(triple, 1) = lhs;
    LGET(triple, 2) = rhs;
    arrput(p->go, triple);
  }
  return No;
}

static dyn parse_delim(pstate_t *p, int delim_ctx) {
  (void)delim_ctx;
  dyn o = parse_op(p, is_delim_op);
  if (!o) return parse_logic(p);
  // Pref = GOutput.f -- in Symta the output stack so far gets
  // included as the LHS of the delim form. We use p->go.
  int gn = arrlen(p->go);
  dyn pref;
  if (gn > 0) {
    LIST(pref, gn);
    for (int i = 0; i < gn; i++) LGET(pref, i) = p->go[i];
    arrsetlen(p->go, 0);
  } else {
    LIST(pref, 0);
  }
  int expect_eol = 0;
  if (text_eq_c(tok_value(o), KW_colon)) {
    expect_eol = 1;
    // Symta: `case Pref [X@_]: when X.is_token and X.value.is_keyword:
    //          ExpectEOL = 0`
    // is_keyword = NOT(n>0 AND first-char-upcase). So an identifier
    // that starts with a non-uppercase letter is a "keyword" here.
    // Identifiers starting with uppercase (variables like OpsT) leave
    // ExpectEOL = 1, which is critical for the parse_offside rollback.
    if (LIST_SIZE(pref) > 0) {
      dyn x = LGET(pref, 0);
      if (is_tok(x) && IS_TEXT(tok_value(x))) {
        char *s = text_chars(tok_value(x));
        if (s && s[0]) {
          unsigned char c0 = (unsigned char)s[0];
          int is_kw = !(c0 >= 'A' && c0 <= 'Z');
          if (is_kw) expect_eol = 0;
        }
      }
    }
  }
  dyn xs = parse_offside(p, 0, expect_eol,
                          (int)UNFXN(tok_row(o)), No);
  // Symta: `GOutput =: Xs Pref O` sets GOutput = [Xs Pref O], and
  // then parse_xs returns GOutput.f = [O Pref Xs]. We're using
  // arrput (insertion order, no reverse in parse_xs), so put the
  // elements directly in the FINAL desired order [O Pref Xs].
  arrsetlen(p->go, 0);
  arrput(p->go, o);
  arrput(p->go, pref);
  arrput(p->go, xs);
  return No;  // Symta returns No after modifying GO; parse_xs continues.
}

static int parse_semicolon(pstate_t *p) {
  // Find the first `|` or `;` token in remaining input.
  static dyn t_semi = 0;
  if (!t_semi) TEXT(t_semi, ";");
  int found_at = -1;
  dyn found_tok = 0;
  // Look in pushback (reverse order) then gi.
  int pn = arrlen(p->pushback);
  for (int i = pn - 1; i >= 0; i--) {
    dyn t = p->pushback[i];
    if (is_tok(t)) {
      dyn ty = tok_type(t);
      if (text_eq_c(ty, KW_pipe) || text_eq_c(ty, t_semi)) {
        found_at = -1 - i;  // negative encoding for pushback index
        found_tok = t;
        break;
      }
    }
  }
  if (!found_tok) {
    for (uint64_t i = p->gi_pos; i < p->gi_n; i++) {
      dyn t = LGET(p->gi, i);
      if (is_tok(t)) {
        dyn ty = tok_type(t);
        if (text_eq_c(ty, KW_pipe) || text_eq_c(ty, t_semi)) {
          found_at = (int)(i - p->gi_pos);
          found_tok = t;
          break;
        }
      }
    }
  }
  if (!found_tok) return 0;
  // If it's a `|`, do nothing (Symta returns 0).
  if (text_eq_c(tok_type(found_tok), KW_pipe)) return 0;
  // Otherwise it's `;` -- split.
  // L = parse_tokens(GInput.take(P))
  // R = parse_tokens(GInput.drop(P+1))
  // GInput = []
  // GOutput = if R.0^token_is(`;`) then [@R.tail.f L M] else [R L M]
  // where M is the `;` token.
  dyn left_list, right_list;
  {
    dyn *left = 0;
    for (int i = 0; i < found_at; i++) {
      arrput(left, p_pop(p));
    }
    int ln = arrlen(left);
    LIST(left_list, ln);
    for (int i = 0; i < ln; i++) LGET(left_list, i) = left[i];
    arrfree(left);
  }
  // Consume the `;`
  if (found_at >= 0) {
    (void)p_pop(p);  // drop the `;`
  }
  {
    // Consume rest into right_list.
    dyn *right = 0;
    while (!p_end(p)) arrput(right, p_pop(p));
    int rn = arrlen(right);
    LIST(right_list, rn);
    for (int i = 0; i < rn; i++) LGET(right_list, i) = right[i];
    arrfree(right);
  }
  dyn l = parse_tokens_inner(left_list);
  dyn r = parse_tokens_inner(right_list);
  // Symta sets GOutput = [@R.tail.f L M] or [R L M], then parse_xs
  // returns GOutput.f (reversed). Our p->go is what parse_xs returns
  // directly (no reverse), so write the post-reverse form:
  //   if-branch: [M L @R.tail]
  //   else:      [M L R]
  arrsetlen(p->go, 0);
  arrput(p->go, found_tok);
  arrput(p->go, l);
  if (is_list(r) && LIST_SIZE(r) > 0 &&
      is_tok(LGET(r, 0)) && text_eq_c(tok_type(LGET(r, 0)), t_semi)) {
    uint64_t rn = LIST_SIZE(r);
    for (uint64_t i = 1; i < rn; i++) arrput(p->go, LGET(r, i));
  } else {
    arrput(p->go, r);
  }
  return 0;
}

static dyn parse_xs(pstate_t *p, int delim_ctx) {
  // Symta: `X | parse_delim DelimCtx or ret loop GOutput.f`
  //        `when got X: push X GOutput`
  //
  // Three cases for parse_delim's return:
  //   * 0      -- nothing parsed, exit the loop.
  //   * No     -- GOutput was modified in-place (parse_delim hit a
  //               delim, or parse_logic ran its LL(1) hack). Loop
  //               CONTINUES; the modified GO is now the new state.
  //   * value  -- ordinary expression result, push to GO and continue.
  dyn *saved_go = p->go;
  p->go = 0;
  parse_semicolon(p);
  for (;;) {
    dyn x = parse_delim(p, delim_ctx);
    if (x == 0) break;            // truly nothing more
    if (x == No) continue;        // GO modified, keep looping
    arrput(p->go, x);
  }
  // GOutput.f -- forward order. Our arrput is already forward.
  int gn = arrlen(p->go);
  dyn out;
  LIST(out, gn);
  for (int i = 0; i < gn; i++) LGET(out, i) = p->go[i];
  arrfree(p->go);
  p->go = saved_go;
  return out;
}

// parse_tokens with the nullary-recovery dance (reader.s:449-457):
//
//   if parse_xs leaves stuff in GInput:
//     pop the offending token
//     if its type isn't in OpsT -> parser_error("unexpected", tok)
//     else: synthesize a `nullary_ Tok` symbol token, prepend to
//           input, and concatenate parse_xs's result with a
//           recursive parse_tokens on the now-prepended input.
//
// Used by parse_logic's LL(1) hack -- it leaves the `:` delim
// in the stream, and the outer parse_xs consumes the `:` form via
// parse_delim; if `:` ends up at top level (no preceding tokens),
// the nullary synthetic gets prepended so the `:` form has a Pref
// to read.
static dyn parse_tokens_inner(dyn input) {
  pstate_t p;
  p_init(&p, input);
  dyn xs = parse_xs(&p, 0);
  while (!p_end(&p)) {
    dyn tok = p_pop(&p);
    if (!is_tok(tok) || !is_opt(tok_type(tok))) {
      parser_error("unexpected", tok);
    }
    // Build a "nullary_ Tok" synthetic symbol token.
    static dyn nullary_sym = 0;
    if (!nullary_sym) TEXT(nullary_sym, "nullary_");
    // .parsed = [[nullary_ Tok]]
    dyn inner; LIST(inner, 2);
    LGET(inner, 0) = nullary_sym;
    LGET(inner, 1) = tok;
    dyn cache; LIST(cache, 1);
    LGET(cache, 0) = inner;
    // Build the synthetic token's value text "(nullary_ <Tok.value>)".
    char *vs = is_tok(tok) && IS_TEXT(tok_value(tok))
                 ? text_chars(tok_value(tok)) : "?";
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "(nullary_ %s)", vs ? vs : "?");
    (void)n;
    dyn ntext;
    TEXT(ntext, buf);
    dyn synth = mk_token(KW_symbol, ntext,
                          tok_row(tok), tok_col(tok), tok_orig(tok),
                          cache);
    // Push synth then the rest back onto p (LIFO pushback) so the
    // next p_pop reads synth, then tok, then whatever else.
    // First we'd have to put `tok` back too, then synth above it.
    p_push_back(&p, tok);
    p_push_back(&p, synth);
    dyn more = parse_xs(&p, 0);
    // Concatenate xs and more.
    uint64_t xn = LIST_SIZE(xs);
    uint64_t mn = LIST_SIZE(more);
    dyn combined; LIST(combined, xn + mn);
    for (uint64_t i = 0; i < xn; i++) LGET(combined, i) = LGET(xs, i);
    for (uint64_t i = 0; i < mn; i++) LGET(combined, xn + i) = LGET(more, i);
    xs = combined;
  }
  p_free(&p);
  return xs;
}

// WORK-IN-PROGRESS. Not wired into reader.s text.parse yet -- the
// scaffold (parser state, op tables, precedence ladder, parse_term
// for literals/lists, parse_suf chain) is in place but parse_if,
// parse_bar, parse_offside, parse_logic's LL(1) hack, parse_delim's
// offside coupling, and parse_xs's named-loop semantics are stubs
// that error out. Calling this on real input will segfault or hit
// an "not yet ported" error. See issues.md B8 for what's left.
//
// Kept reachable as `parse_tokens_c_` builtin only so test harnesses
// can probe it; the production text.parse still uses Symta.
dyn reader_parse_tokens(dyn input) {
  GC_DISABLE();
  kw_init();
  dyn r = parse_tokens_inner(input);
  GC_ENABLE();
  return r;
}

// ====================================================================
// parse_table -- reader.s:100-123.
// Walks a STRIPPED list of items where each `[`!` K V]` form pairs
// a key with a value, returning [[K V] ...] for the table builder.
// ====================================================================

dyn reader_parse_table(dyn xs) {
  static dyn t_bang = 0;
  if (!t_bang) TEXT(t_bang, "!");

  GC_DISABLE();
  dyn *as = 0;
  dyn key = 0;
  dyn *val = 0;
  int cnt = 0;
  uint64_t n = is_list(xs) ? LIST_SIZE(xs) : 0;

  for (uint64_t i = 0; i <= n; i++) {
    int flush = 0;
    int start_kv = 0;
    dyn x = 0;
    if (i == n) flush = 1;
    else {
      x = LGET(xs, i);
      // Check for [`!` K @rest] form
      if (is_list(x) && LIST_SIZE(x) >= 2 &&
          text_eq_c(LGET(x, 0), t_bang)) {
        flush = 1;
        start_kv = 1;
      }
    }

    if (flush && cnt) {
      // push_kv:
      int vn = arrlen(val);
      dyn vlist;
      if (vn == 0) {
        // Val = [1]
        LIST(vlist, 1);
        dyn one; LDFXN(one, 1);
        LGET(vlist, 0) = one;
      } else if (vn == 1) {
        // Val.0
        vlist = val[0];
      } else {
        LIST(vlist, vn);
        for (int j = 0; j < vn; j++) LGET(vlist, j) = val[j];
      }
      dyn pair; LIST(pair, 2);
      LGET(pair, 0) = key;
      LGET(pair, 1) = vlist;
      arrput(as, pair);
      arrfree(val); val = 0;
    }

    if (start_kv) {
      cnt++;
      key = LGET(x, 1);
      // case Key [`!` K]: push `[]` Val; Key = K
      if (is_list(key) && LIST_SIZE(key) == 2 &&
          text_eq_c(LGET(key, 0), t_bang)) {
        static dyn t_brackets = 0;
        if (!t_brackets) TEXT(t_brackets, "[]");
        arrput(val, t_brackets);
        key = LGET(key, 1);
      }
    } else if (!flush) {
      arrput(val, x);
    }
  }

  dyn r = make_list_from_arr(as);
  arrfree(as);
  if (val) arrfree(val);
  GC_ENABLE();
  return r;
}
