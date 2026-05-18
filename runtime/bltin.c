#include <stdarg.h>
#include <float.h>
#include <math.h>
#include <dlfcn.h>
#include <time.h>
#include <errno.h>

#include "common.h"
#include "sif.h"
#include "ng.h"
#include "flt16.h"
#include "fs.h"
#include "reader.h"
#include "meta_table.h"


#include "prf.h"
#define AM_IMPLEMENTATION
#include "am.h"


void *tag_of(void *o) {
  return types[O_TAG(o)].sname;
}


#define MAX_SINGLE_CHARS (1<<8)
dyn single_chars[MAX_SINGLE_CHARS];


void *fixtext_encode(char *p) {
  uint64_t r = 0;
  uint64_t c;
  int i = GID_SHFT;
  char *s = p;
  while (*s) {
    if (i+FIXTEXT_CHAR_BITS > FIXTEXT_LIM) return 0;
    c = (uint8_t)*s++;
    if (c & FIXTEXT_CHAR_IMASK) return 0;
    r |= c << i;
    i += FIXTEXT_CHAR_BITS;
  }
  return TAGIFY(r,T_FIXTEXT);
}
int fixtext_decode(char *dst, void *r) {
  uint8_t *p = (uint8_t*)dst;
  uint64_t x = (uint64_t)r;
  uint64_t c;
  int i = GID_SHFT;
  while (i < FIXTEXT_LIM) {
    c = (x>>i) & FIXTEXT_CHAR_MASK;
    i += FIXTEXT_CHAR_BITS;
    if (!c) break;
    *p++ = c;
  }
  *p = 0;
  return p-(uint8_t*)dst;
}

int fixtext_size(void *o) {
  uint64_t x = (uint64_t)o;
  uint64_t m = (uint64_t)FIXTEXT_CHAR_MASK << GID_SHFT;
  int l = 0;
  while (x & m) {
    m <<= FIXTEXT_CHAR_BITS;
    l++;
  }
  return l;
}


void *alloc_textdata(int l) {
  void *r;
  int a = l - 4; //-4 since we use the O_CODE field
  if (a < 0) a = 0;
  int nwords = ((a+ALIGN_MASK)>>ALIGN_BITS); // div word size and align
  OBJECT(r, T_TEXT, nwords);
  BIGTEXT_SIZE(r) = (uint32_t)l;
  return r;
}


void *alloc_bigtext(char *s, int l) {
  void *r = alloc_textdata(l);
  memcpy(BIGTEXT_DATA(r), s, l);
  return r;
}

void *bytes_to_text(uint8_t *bytes, int size) {
  char cs[10];
  if (size <= FIXTEXT_MAX_CHARS) {
    memcpy(cs, bytes, size);
    cs[size] = 0;
    return fixtext_encode(cs);
  } else {
    //disable threshold, since bytes can be BYTES_DATA(o)
    //which will be corrupted on gc
    GC_DISABLE();
    void *r = alloc_bigtext(bytes, size);
    GC_ENABLE();
    return r;
  }
}

void *alloc_bytes(int l) {
  void *r;
  int a = l - 4; //-4 since we use the O_CODE field
  if (a < 0) a = 0;
  int nwords = (a+ALIGN_MASK)>>ALIGN_BITS; // div word size and align
  OBJECT(r, T_BYTES, nwords);
  BYTES_SIZE(r) = (uint32_t)l;
  return r;
}

void *text_to_bytes(dyn o) {
  char tmp[12];
  char *p;
  int size;
  void *r;
  if (IS_FIXTEXT(o)) {
    size = fixtext_decode(tmp, o);
    p = tmp;
  } else if (IS_BIGTEXT(o)) {
    size = (int)BIGTEXT_SIZE(o);
    p = BIGTEXT_DATA(o);
  }
  //FIXME: threshould is unlocked, since after alloc_bytes
  //       p could point to a wrong area.
  GC_DISABLE();
  r = alloc_bytes(size);
  GC_ENABLE();
  memcpy(BYTES_DATA(r), p, size);
  return r;
}

void *alloc_text(char *s) {
  int l, a;
  void *r;

  r = fixtext_encode(s);
  if (!r) r = alloc_bigtext(s, strlen(s));

  return r;
}

char *decode_text(char *out, dyn o) {
  if (IS_FIXTEXT(o)) {
    out += fixtext_decode(out, o);
    *out = 0;
  } else if (IS_BIGTEXT(o)) {
      int size = (int)BIGTEXT_SIZE(o);
      char *p = BIGTEXT_DATA(o);
      while (size-- > 0) *out++ = *p++;
      *out = 0;
  } else {
    rterr("decode_text: invalid type (%d)", (int)O_TAG(o));
  }
  return out;
}

int texts_equal(dyn a, dyn b) {
  int64_t al, bl;
  if (IS_FIXTEXT(a) || IS_FIXTEXT(b)) return a == b;
  al = BIGTEXT_SIZE(a);
  bl = BIGTEXT_SIZE(b);
  return al == bl && !memcmp(BIGTEXT_DATA(a), BIGTEXT_DATA(b), al);
}


int text_size(void *o) {
  if (IS_FIXTEXT(o)) return fixtext_size(o);
  if (!IS_BIGTEXT(o)) {
    fprintf(stderr, "text_size: invalid type (%d)\n", (int)O_TAG(o));
    print_stack_trace();
    fatal("aborting");
  }
  return BIGTEXT_SIZE(o);
}

char *text_chars(dyn text) {
  int a;
  char buf[32];
  int size = text_size(text);
  char *s, *d;
  void *r;
  if (IS_FIXTEXT(text)) {
    decode_text(buf, text);
    s = buf;
  } else {
    s = (char*)BIGTEXT_DATA(text);
  }
  GC_DISABLE();
  r = alloc_bytes(size+1);
  GC_ENABLE();
  d = (char*)BYTES_DATA(r);
  memcpy(d, s, size);
  d[size] = 0;
  return d;
}

char *text_to_cstring(dyn text) {
  int l = text_size(text)+1;
  int al = arrlen(api.sbuf);
  if (al < l || al > l*2) {
    arrfree(api.sbuf);
    arrsetlen(api.sbuf,l);
  }
  decode_text(api.sbuf, text);
  return api.sbuf;
}


#define MOD_ADLER 65521
static uint32_t hash(uint8_t *data, int len) {
  uint32_t a = 1, b = 0;
  int index;
  for (index = 0; index < len; ++index) {
    a = (a + data[index]) % MOD_ADLER;
    b = (b + a) % MOD_ADLER;
  } 
  return (b << 16) | a;
}

void fatal(char *fmt, ...) { //for fatal unrecoverable errors
  va_list ap;
  va_start(ap,fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fflush(stderr); //make sure the message survives a CRASH
  CRASH; //allows gdb stack traces
  //exit(-1);
}

void rterr_(char *msg) {
  /* Flush any pending stdout BEFORE writing to stderr. Without this,
   * stdout content that was line-buffered (or block-buffered when
   * piped) is held until atexit, while stderr writes immediately,
   * so the merged 2>&1 stream sees stderr interleaved BEFORE the
   * preceding stdout content. Test goldens were captured on Windows
   * where stdout-then-stderr was the implicit order; matching that
   * on Linux just means flushing here. */
  fflush(stdout);
  if (!api.error_handler) {
    fprintf(stderr, "UNHANDLED ERROR\n");
    fprintf(stderr, "%s\n", msg);
    print_stack_trace();
    exit(-1);
  }
  /* CORE-3: the trace used to print here unconditionally even when
   * the caller wrapped in `btrap`, leaving a clean recovery looking
   * alarming on stderr.  The handler does a longjmp to its catch
   * point and never returns to this function, so we only print
   * the trace if we fall through past the CALL (handler exhausted
   * or otherwise didn't catch). */
  dyn type = FXN(0); //runtime error
  dyn smsg;
  GC_DISABLE();
  TEXT(smsg, msg);
  arrfree(msg);
  ARGLIST(2);
  GC_ENABLE();
  STARG(0,type);
  STARG(1,smsg);
  dyn r;
  CALL(r,api.error_handler);
  fprintf(stderr, "UNHANDLED ERROR\n");
  fprintf(stderr, "%s\n", text_chars(smsg));
  print_stack_trace();
  exit(-1);
}

static NOINLINE void bad_call(char *method_name, char *msg) {
  PROLOGUE(FRAME_HEADER_SIZE);
  int i, nargs = (int)LIST_SIZE(E);
  char *s = fmt("%s.%s: %s", print_object(getArg(0)),method_name, msg);
  arrfree(msg);
#if 0
  for (i = 1; i < nargs; i++) {
    char *t = fmt("%s,  %s", s,print_object(getArg(i)));
    arrfree(s);
    s = t;
  }
#endif
  rterr_(s);
}

#define BAD_CALL(method_name, ...) bad_call(method_name,fmt(__VA_ARGS__))

/* CORE-1 / RT-9: resolve a saved bytecode pin to (sbc, row, col).
 * Returns the owning sbc_t* (or NULL if not in any loaded SBC's
 * code section) and writes row/col via out-params.  Used by both
 * print_stack_trace (frame->pin -> caller source line) and the
 * RT-9 alloc-attribution dump (alloc-site pin -> source line). */
static sbc_t *resolve_pin(void *pin, int *row_out, int *col_out) {
  extern int sbcs_loaded;
  extern sbc_t *sbcs[];
  if (!pin) return 0;
  sbc_t *owner = 0;
  for (int s = 0; s < sbcs_loaded; s++) {
    sbc_t *sc = sbcs[s];
    if (pin >= (void*)sc->code && pin < (void*)(sc->code + sc->code_sz)) {
      owner = sc;
      break;
    }
  }
  if (!owner || !owner->lineno_table || !owner->lineno_sz) return owner;
  uint32_t target = (uint32_t)((uint8_t*)pin - owner->code);
  uint8_t *t = owner->lineno_table;
  int lo = 0, hi = owner->lineno_sz;
  int best = -1;
  /* Each entry: u32 pc, u32 row, u16 col, u16 pad = 12B. */
  while (lo < hi) {
    int mid = (lo + hi) >> 1;
    uint32_t pc = *(uint32_t*)(t + mid*12);
    if (pc <= target) { best = mid; lo = mid + 1; }
    else hi = mid;
  }
  if (best >= 0) {
    uint8_t *e = t + best*12;
    *row_out = (int)(*(uint32_t*)(e + 4));
    *col_out = (int)(*(uint16_t*)(e + 8));
  }
  return owner;
}

static int inside_stack_trace = 0;
void print_stack_trace() {
  /* `fn` was originally meant to be the per-frame function pointer
   * for symbolication, but the actual assignment was lost (or never
   * landed) — print_stack_trace just printed `void *fn` uninitialised
   * for as long as anyone remembers. On Win64 the stack layout
   * happened to leave it zero, so `%p` printed "0000000000000000"
   * and the test goldens captured that. On glibc the same uninit
   * pointer prints "(nil)", which breaks byte-identical comparison.
   *
   * Fix: explicitly zero `fn` and use a portable hex format that
   * produces the same 16-digit zero-padded representation on every
   * platform. When someone wires up real symbolication they can
   * replace the literal zero with a real address. */
  uintptr_t fn = 0;
  fn_meta_t *meta;
  int row, col;
  char *name, *origin;
  int sp_count = 0;
  if (inside_stack_trace) {
    //float fault happens when some bug corrupts everything
    exit(-1);
  }
  inside_stack_trace = 1;
  fprintf(stderr, "Stack Trace:\n");
  frame_t *frm;
  for (frm = api.frame; frm; frm = frm->prev) if (frm->clsr) {
    meta = O_META(frm->clsr);
    if (!meta) {
      name = "???"; //continue;
      row = -1;
      col = -1;
      origin = "???";
    } else {
      name = meta->name ? (char*)meta->name : "unnamed";
      origin = meta->origin ? (char*)meta->origin : "";
      /* CORE-1: prefer the SBC's lineno side table (looked up by
       * the frame's saved call-site pin) and fall back to the
       * fn_meta_t function-header position.  The side table is
       * populated by sif2sbc when the source carries `lsrc`
       * markers; the runtime never dispatches a per-line opcode
       * on the hot path. */
      row = meta->row;
      col = meta->col;
      resolve_pin(frm->pin, &row, &col);
    }
    fprintf(stderr, "  %016llx:%s:%d,%d,%s\n",
            (unsigned long long)fn, name, row, col, origin);
    if (++sp_count > 50) {
      fprintf(stderr, "  ...stack is too big...\n");
      return;
    }
  }
  inside_stack_trace = 0;
  return;
}

BUILTIN2("no.`><`",no_eq,C_ANY,a,C_ANY,b)
RETURNS(FXN(a == b))
BUILTIN2("no.`<>`",no_ne,C_ANY,a,C_ANY,b)
RETURNS(FXN(a != b))

/* `_.><` / `_.<>` default to identity comparison.  Symta's
 * `core_.s` used to define these as
 *
 *   _.`><` B = same Me B
 *   _.`<>` B = not Me >< B
 *
 * which costs two MCALLs + a `not` opcode per call -- ~200 ns on
 * a heap value where neither `Me` nor `B` has a type-specific
 * override.  The C-side handlers below collapse that to a single
 * builtin invocation = ~50 ns.  Type-specific overrides
 * (int.><, text.><, float.><, fn.><, no.><) still win via the
 * usual METHOD_FN dispatch; this is only the fallback. */
BUILTIN2("_.`><`",any_eq,C_ANY,a,C_ANY,b)
RETURNS(FXN(a == b))
BUILTIN2("_.`<>`",any_ne,C_ANY,a,C_ANY,b)
RETURNS(FXN(a != b))

/* OP-3: `_.<<` / `_.>` / `_.>>` -- raw-dyn ordering comparators
 * on T_OBJECT, mirroring OP-1's identity comparators above.  The
 * Symta-side defs in core_.s used to read
 *
 *   _.`<<` B = not B < Me
 *   _.`>`  B = B < Me
 *   _.`>>` B = not Me < B
 *
 * which is one MCALL into `<` plus a `not` opcode on top.  These
 * builtins skip both, doing the bit-level compare directly.
 *
 * Caveat: dyn-bit ordering is only meaningful when both operands
 * have the same tag; cross-type comparisons (T_NO < T_LIST) are
 * nonsensical.  In practice these are rarely called with
 * mismatched heap types, and every common type (int / float /
 * list / text) has its own type-specific override that wins via
 * METHOD_FN dispatch before we get here. */
BUILTIN2("_.`<<`",any_lte,C_ANY,a,C_ANY,b)
RETURNS(FXN((uintptr_t)a <= (uintptr_t)b))
BUILTIN2("_.`>`", any_gt, C_ANY,a,C_ANY,b)
RETURNS(FXN((uintptr_t)a >  (uintptr_t)b))
BUILTIN2("_.`>>`",any_gte,C_ANY,a,C_ANY,b)
RETURNS(FXN((uintptr_t)a >= (uintptr_t)b))
BUILTIN1("no.hash",no_hash,C_ANY,a)
RETURNS(FXN(0x12345678))


BUILTIN2("fn.`><`",fn_eq,C_ANY,a,C_ANY,b)
RETURNS(FXN(a == b))
BUILTIN2("fn.`<>`",fn_ne,C_ANY,a,C_ANY,b)
RETURNS(FXN(a != b))
BUILTIN1("fn.nargs",fn_nargs,C_ANY,o)
  fn_meta_t *meta = O_META(o);
  void *nargs = meta ? meta->nargs : No;
RETURNS(nargs)

BUILTIN1("bytes.n",bytes_n,C_ANY,o)
RETURNS(FXN(BYTES_SIZE(o)))
BUILTIN2("bytes.`.`",bytes_get,C_ANY,o,C_INT,index)
  uint64_t idx = (uint64_t)UNFXN(index);
  if ((uint64_t)BYTES_SIZE(o) <= idx) BAD_CALL(".", "index out of bounds");
RETURNS(FXN(BYTES_DATA(o)[idx]))
BUILTIN3("bytes.`=`",bytes_set,C_ANY,o,C_INT,index,C_INT,value)
  int64_t i = UNFXN(index);
  if (BYTES_SIZE(o) <= (uint64_t)i) BAD_CALL("=", "index out of bounds");
  BYTES_DATA(o)[i] = (uint8_t)UNFXN(value);
  R = 0;
RETURNS(R)
BUILTIN2("bytes.clear",bytes_clear,C_ANY,o,C_INT,value)
  int64_t i;
  int64_t size = BYTES_SIZE(o);
  uint8_t v = (uint8_t)UNFXN(value);
  uint8_t *p = BYTES_DATA(o);
  for (i = 0; i < size; i++) p[i] = v;
  R = 0;
RETURNS(R)
BUILTIN1("bytes.utf8",bytes_utf8,C_ANY,o)
RETURNS(bytes_to_text(BYTES_DATA(o), BYTES_SIZE(o)))


BUILTIN2("text.`><`",text_eq,C_ANY,a,C_ANY,b)
RETURNS(FXN(IS_BIGTEXT(b) ? texts_equal(a,b) : 0))
BUILTIN2("text.`<>`",text_ne,C_ANY,a,C_ANY,b)
RETURNS(FXN(IS_BIGTEXT(b) ? !texts_equal(a,b) : 1))
BUILTIN1("text.n",text_n,C_ANY,o)
RETURNS(FXN(BIGTEXT_SIZE(o)))
BUILTIN2("text.`.`",text_get,C_ANY,o,C_INT,index)
  uint64_t idx = (uint64_t)UNFXN(index);
  if ((uint64_t)BIGTEXT_SIZE(o) <= idx) BAD_CALL(".", "index out of bounds");
RETURNS(single_chars[(uint8_t)BIGTEXT_DATA(o)[idx]])
BUILTIN1("text.hash",text_hash,C_ANY,o)
RETURNS(FXN(hash(BIGTEXT_DATA(o), BIGTEXT_SIZE(o))))
BUILTIN1("text.utf8",text_utf8,C_ANY,o)
RETURNS(text_to_bytes(o))
BUILTIN2("text.tokenize",text_tokenize,C_ANY,o,C_ANY,orig)
RETURNS(tokenize(orig,o))

// C parser surface -- see runtime/reader.c. The Symta reader.s
// thunks through these so the public `text.parse` keeps working
// while the parser moves to C piece by piece.
BUILTIN1("add_bars_c_", add_bars_c_, C_ANY, xs)
RETURNS(reader_add_bars(xs))
BUILTIN1("parse_strip_c_", parse_strip_c_, C_ANY, x)
RETURNS(reader_parse_strip(x))
BUILTIN1("parse_tokens_c_", parse_tokens_c_, C_ANY, x)
RETURNS(reader_parse_tokens(x))
/* `reader_parse_table` is still used internally by parse_term's
 * `@{}` / `${}` cases, but nothing in Symta-side code calls it
 * any more, so the BUILTIN1 wrapper is gone.  See runtime/reader.c
 * for the live callers. */

/* Weak hashtable -- single global instance.  Keys are heap-object
 * dyns held weakly: GC drops entries whose key isn't reachable
 * from anywhere else.  Values are strongly held.  See
 * runtime/meta_table.h and tests/runtime/wh-test.s. */
BUILTIN2("wh_set_",   wh_set_,   C_ANY, key, C_ANY, val)
  meta_set_(key, val);
RETURNS(val)
BUILTIN1("wh_get_",   wh_get_,   C_ANY, key)
RETURNS(meta_get_(key))
BUILTIN1("wh_has_",   wh_has_,   C_ANY, key)
RETURNS(FXN(meta_has_(key)))
BUILTIN1("wh_del_",   wh_del_,   C_ANY, key)
  meta_del_(key);
RETURNS(No)
BUILTIN0("wh_clear_", wh_clear_)
  meta_clear_();
RETURNS(No)
BUILTIN0("wh_n_",     wh_n_)
RETURNS(FXN((int64_t)meta_n_()))


/* Help system -- HELP-3 final form.  Two backing stores, both
 * scanned lazily by `help_section_lookup_`:
 *
 *   1. SBC docs section -- one per loaded SBC.  Populated at
 *      compile time by the `_ssv` intrinsic; written by
 *      sif2sbc.c into a `(sym_ofs:24, val_ofs:24)` flat array;
 *      loaded by sbc.c into `sbc->doc_table` / `sbc->doc_sz`.
 *      This is where every Symta-defined function and method
 *      lives.
 *
 *   2. `builtin_docs[]` static table below -- linear array of
 *      `(name, doc)` pairs.  This is where runtime builtins
 *      (no Symta source body to host an `@"text"` head) live.
 *
 * Neither store builds a hashtable; lookups are linear at
 * `help name`-time and that's fine -- `help` is interactive.
 *
 * The legacy `help_set_` / `help_get_` / `help_names_` API
 * further down is still wired up only for the macros defined
 * in macro.s (their `=:` defs don't yet flow through
 * `prefix_doc`).  Once macro-doc support lands the whole
 * legacy section goes away.
 */

extern sbc_t *sbcs[];
extern int sbcs_loaded;

/* Read a NUL-terminated string at the given offset.  Offsets in
 * the docs section are relative to `sbc->code` (the start of
 * the code+data block) -- the same convention used by
 * `sbc->src_file_string` and friends.  Offset zero means
 * "no string". */
static char *sbc_data_str(sbc_t *sbc, uint32_t ofs) {
  if (!ofs) return 0;
  return (char *)(sbc->code + ofs);
}

/* HELP-3: static docstring table for runtime builtins.  These
 * symbols have no Symta source body to host an `@"text"` head,
 * so their docs live here instead.  Keep entries in alphabetical
 * order.  Terminate with a `{0, 0}` sentinel.
 *
 * Adding a new entry costs nothing at startup -- the array
 * is read-only data, scanned only when `help foo` is called. */
typedef struct {
  const char *name;
  const char *doc;
} builtin_doc_t;

static const builtin_doc_t builtin_docs[] = {
  /* Macros defined in macro.s via the `=:` form.  Those defs do
   * not flow through `prefix_doc`, so their docs cannot live in
   * an SBC docs section -- we keep them here alongside the true
   * runtime builtins. */
  {"got",        "Test whether a value exists.  Returns 1 if X is anything but No, else 0.\n"
                 "Distinct from `not X`, which tests for the integer 0.\n"
                 "See also: not (test for zero), No (the no-value marker)."},
  {"help",       "Print REPL help (no args) or documentation for a symbol.\n"
                 "Usage: `help` for the banner; `help say` for one symbol's docs;\n"
                 "`help_names()` to list every documented symbol."},
  {"less",       "Inverse of `when`: runs the body when the condition is falsy (zero).\n"
                 "Idiomatic for guard clauses at the top of a function.\n"
                 "Example:  less B: bad \"division by zero\""},
  {"max",        "Maximum of the given values.  Variadic.\n"
                 "Example: max 3 7 2 8 1 returns 8.\n"
                 "See also: min, list.max, list.min."},
  {"min",        "Minimum of the given values.  Variadic.\n"
                 "Example: min 3 7 2 8 1 returns 1.\n"
                 "For the minimum element of a list, use `.min` (the method).\n"
                 "See also: max, list.min, list.max."},
  {"no",         "Test whether a value is No.  Inverse of `got`.\n"
                 "Returns 1 if X is No, else 0."},
  {"not",        "Logical negation.  Returns 1 if X is the integer 0, else 0.\n"
                 "In Symta the only falsy value is 0; No and atoms and lists are all truthy.\n"
                 "Use `got` to check for No, `not` for zero."},
  {"when",       "Conditional execution -- runs the body if the condition is truthy.\n"
                 "The body is everything indented under the `when:` line.\n"
                 "Returns No when the condition is falsy (the integer 0).\n"
                 "See also: less (inverse), if (with else-branch)."},

  /* Genuine runtime builtins. */
  {"float.int",  "Truncate a float to an integer (toward zero)."},
  {"int.float",  "Convert an integer to a float."},
  {"rand_get",   "Return a pseudo-random integer.  Seeded from the random_seed_ table."},
  {"rand_pop",   "Restore the most recently pushed PRNG state."},
  {"rand_push",  "Push the current PRNG state on a stack for later restoration."},
  {"rand_set",   "Set the PRNG seed."},
  {"say_",       "Print values without appending a newline.  Same argument shape\n"
                 "as `say`.  Use for assembling text from several calls or for REPL-like prompts.\n"
                 "Example:  say_ \"> \"; X parse get_line()"},
  {"text.flt",   "Parse a text as a floating-point number.  Returns No on parse error."},
  {"text.n",     "Length of a text in characters (codepoints, not bytes)."},
  {"text.utf8",  "Convert text to its UTF-8 byte sequence as a list of integers."},
  {"typename",   "Return the type name of a value, as text.\n"
                 "Example:  typename 42         // \"int\"\n"
                 "          typename \"abc\"      // \"text\"\n"
                 "          typename [1 2 3]    // \"list\""},
  {0, 0}
};

BUILTIN1("help_section_lookup_", help_section_lookup_, C_TEXT, name_text)
  /* text_to_cstring returns a pointer into the shared api.sbuf
   * stretchy buffer.  We must finish using `name` before any other
   * runtime call that could call text_to_cstring (e.g. TEXT/
   * alloc_text), so we grab the matching val pointer first, break
   * out of the loop, and only then build the result. */
  char *name = text_to_cstring(name_text);
  R = No;
  const char *found_val = 0;

  /* (1) SBC docs sections. */
  for (int i = 0; i < sbcs_loaded && !found_val; i++) {
    sbc_t *sbc = sbcs[i];
    if (!sbc->doc_table || !sbc->doc_sz) continue;
    uint8_t *p = sbc->doc_table;
    for (uint32_t j = 0; j < sbc->doc_sz; j++) {
      uint32_t sym_ofs = p[0] | (p[1]<<8) | (p[2]<<16);
      uint32_t val_ofs = p[3] | (p[4]<<8) | (p[5]<<16);
      p += 6;
      char *sym = sbc_data_str(sbc, sym_ofs);
      char *val = sbc_data_str(sbc, val_ofs);
      if (sym && val && !strcmp(sym, name)) {
        found_val = val;
        break;
      }
    }
  }

  /* (2) Static builtin docs. */
  if (!found_val) {
    for (const builtin_doc_t *b = builtin_docs; b->name; b++) {
      if (!strcmp(b->name, name)) { found_val = b->doc; break; }
    }
  }

  if (found_val) {
    GC_DISABLE();
    TEXT(R, found_val);
    GC_ENABLE();
  }
RETURNS(R)

/* `help_banner_` -- printed when the user types bare `help` at
 * the REPL.  Used to be a Symta function in core_.s; lives here
 * now so the help system stops referencing anything Symta-side. */
BUILTIN0("help_banner_", help_banner_)
  printf("Symta REPL help\n");
  printf("----------------\n");
  printf("  help <name>            -- show docs for `<name>`\n");
  printf("                            (try: help say, help int.bump)\n");
  printf("  module_exports <mod>   -- list everything exported by a module\n");
  printf("                            (try: module_exports core_)\n");
  printf("  module_help <mod>      -- list a module's exports with one-line docs\n");
  printf("  usage                  -- list command-line arguments\n");
  printf("  exit / quit            -- leave the REPL\n");
  R = No;
RETURNS(R)

/* `help_lookup_ Key` -- what the `help X` macro emits.  Looks up
 * the doc via the same path help_section_lookup_ uses and prints
 * either the doc or a `no documentation` message.  Returns No. */
BUILTIN1("help_lookup_", help_lookup_, C_TEXT, name_text)
  char *name = text_to_cstring(name_text);
  const char *found_val = 0;

  for (int i = 0; i < sbcs_loaded && !found_val; i++) {
    sbc_t *sbc = sbcs[i];
    if (!sbc->doc_table || !sbc->doc_sz) continue;
    uint8_t *p = sbc->doc_table;
    for (uint32_t j = 0; j < sbc->doc_sz; j++) {
      uint32_t sym_ofs = p[0] | (p[1]<<8) | (p[2]<<16);
      uint32_t val_ofs = p[3] | (p[4]<<8) | (p[5]<<16);
      p += 6;
      char *sym = sbc_data_str(sbc, sym_ofs);
      char *val = sbc_data_str(sbc, val_ofs);
      if (sym && val && !strcmp(sym, name)) {
        found_val = val;
        break;
      }
    }
  }
  if (!found_val) {
    for (const builtin_doc_t *b = builtin_docs; b->name; b++) {
      if (!strcmp(b->name, name)) { found_val = b->doc; break; }
    }
  }

  if (found_val) {
    printf("%s\n", found_val);
  } else {
    /* `name` is still valid -- text_to_cstring's api.sbuf hasn't
     * been re-used because we haven't called anything that uses
     * it. */
    printf("No documentation for `%s`.\n", name);
  }
  R = No;
RETURNS(R)

BUILTIN1("text.flt",text_flt,C_ANY,o)
  LDFLT(R, atof(text_to_cstring(o)));
RETURNS(R)

BUILTIN2("text.`><`",fixtext_eq,C_ANY,a,C_ANY,b)
RETURNS(FXN(IS_FIXTEXT(b) ? texts_equal(a,b) : 0))
BUILTIN2("text.`<>`",fixtext_ne,C_ANY,a,C_ANY,b)
RETURNS(FXN(IS_FIXTEXT(b) ? !texts_equal(a,b) : 1))
BUILTIN1("text.n",fixtext_n,C_ANY,o)
RETURNS(FXN(fixtext_size(o)))
BUILTIN2("text.`.`",fixtext_get,C_ANY,o,C_INT,index)
  int i = UNFXN(index);
  dyn r;
#if 1
  uint64_t mask = (uint64_t)FIXTEXT_CHAR_MASK<<GID_SHFT;
  uint64_t c = ((uint64_t)o>>(i*FIXTEXT_CHAR_BITS)) & mask;
  if (!c) BAD_CALL(".", "index out of bounds");
  r = TAGIFY(c, T_FIXTEXT);
#else
  char tmp[FIXTEXT_MAX_CHARS+1];
  int len = fixtext_decode(tmp, o);
  if (i >= len) BAD_CALL(".", "index out of bounds");
  tmp[0] = tmp[i];
  tmp[1] = 0;
  TEXT(r,tmp);
#endif
RETURNS(r)
BUILTIN1("text.end",fixtext_end,C_ANY,o)
RETURNS(FXN(1))
BUILTIN1("text.hash",fixtext_hash,C_ANY,o)
RETURNS(FXN(((uint64_t)o&(((uint64_t)1<<32)-1))^((uint64_t)o>>32)))
BUILTIN1("text.code",fixtext_code,C_ANY,o)
RETURNS((dyn)STRIP_TAG(o))


BUILTIN1("list.head",cons_head,C_ANY,o)
RETURNS(CAR(o))
BUILTIN1("list.tail",cons_tail,C_ANY,o)
RETURNS(CDR(o))
BUILTIN1("list.end",cons_end,C_ANY,o)
RETURNS(FXN(0))
BUILTIN2("list.pre",cons_pre,C_ANY,o,C_ANY,head)
  GC_DISABLE();
  CONS(R, head, o);
  GC_ENABLE();
RETURNS(R)

BUILTIN1("list.n",view_n,C_ANY,o)
RETURNS(FXN(VIEW_SIZE(o)))
BUILTIN2("list.`.`",view_get,C_ANY,o,C_INT,index)
  uint32_t start = VIEW_START(o);
  uint32_t size = VIEW_SIZE(o);
  if ((uint64_t)size <= (uint64_t)UNFXN(index))
     BAD_CALL(".", "index out of bounds");
RETURNS(VIEW_LGET(o, start, UNFXN(index)))
BUILTIN3("list.`=`",view_set,C_ANY,o,C_INT,index,C_ANY,value)
  dyn base = VIEW_BASE(o);
  uint32_t start = VIEW_START(o);
  if (VIEW_SHARED(base)) {
    //base is a shared object, so copy it on write
    uint32_t size = VIEW_SIZE(o);
    dyn r;
    LIST(r, size);
    dyn *oo = &LGET(base, start);
    dyn *pp = &LGET(r,0);
    for (int i = 0; i < size; i++) {
      pp[i] = oo[i];
    }
    VIEW_START(o) = 0;
    VIEW_BASE(o) = VIEW_STRIP_SHARED(r); //personal copy, so not shared
  }
  uint64_t uindex = (uint64_t)UNFXN(index);
  if ((uint64_t)VIEW_SIZE(o) <= uindex) {
    BAD_CALL("=", "index out of bounds");
  }
  uint32_t ofs = start + uindex;
  void *p = &VIEW_LGET(o, 0, 0);
  lsetm(p,ofs,value);
RETURNS(No)
BUILTIN1("list.end",view_end,C_ANY,o)
RETURNS(FXN(0))
BUILTIN1("list.head",view_head,C_ANY,o)
RETURNS(VIEW_LGET(o, VIEW_START(o), 0))
BUILTIN1("list.tail",view_tail,C_ANY,o)
  uint32_t size = VIEW_SIZE(o);
  if (size == 1) R = Empty;
  else {
    dyn base = VIEW_BASE(o);
    if (!VIEW_SHARED(base)) {
      base = VIEW_MARK_SHARED(base);
      VIEW_BASE(o) = base;
    }
    GC_DISABLE();
    uint32_t start = VIEW_START(o);
    VIEW(R, base, start+1, size-1);
    GC_ENABLE();
  }
RETURNS(R)
BUILTIN2("list.take",view_take,C_ANY,o,C_INT,count)
  int64_t c = UNFXN(count);
  uint32_t size = VIEW_SIZE(o);
  if (c == 0) {
    R = Empty;
  } else if ((uint64_t)UNFXN(c) <= (uint64_t)size) {
    dyn base = VIEW_BASE(o);
    if (!VIEW_SHARED(base)) {
      base = VIEW_MARK_SHARED(base); //mark that base is a shared
      VIEW_BASE(o) = base;
    }
    GC_DISABLE();
    uint32_t start = VIEW_START(o);
    VIEW(R, base, start, c);
    GC_ENABLE();
  } else {
    BAD_CALL("take", "count %d is invalid for list size %d", (int)c, (int)size);
  }
RETURNS(R)
BUILTIN2("list.drop",view_drop,C_ANY,o,C_INT,count)
  int64_t c = UNFXN(count);
  uint32_t size = VIEW_SIZE(o);
  if (c == 0) {
    R = o;
  } else if ((uint64_t)UNFXN(c) <= (uint64_t)size) {
    dyn base = VIEW_BASE(o);
    if (!VIEW_SHARED(base)) {
      base = VIEW_MARK_SHARED(base); //mark that base is a shared
      VIEW_BASE(o) = base;
    }
    GC_DISABLE();
    uint32_t start = VIEW_START(o);
    VIEW(R, base, start+c, size-c);
    GC_ENABLE();
  } else if (c == size) {
    R = Empty;
  } else {
    BAD_CALL("drop", "count %d is invalid for list size %d", (int)c, (int)size);
  }
RETURNS(R)
BUILTIN2("list.pre",view_pre,C_ANY,o,C_ANY,head)
  GC_DISABLE();
  CONS(R, head, o);
  GC_ENABLE();
RETURNS(R)


BUILTIN1("list.n",list_n,C_ANY,o)
RETURNS(FXN(LIST_SIZE(o)))
BUILTIN2("list.`.`",list_get,C_ANY,o,C_INT,index)
  int64_t i = UNFXN(index);
  if (LIST_SIZE(o) <= (uint64_t)i) BAD_CALL(".", "index out of bounds");
RETURNS(LGET(o, i))
BUILTIN2("list.`.!`",list_uget,C_ANY,o,C_INT,index)
  int64_t i = UNFXN(index);
  if (LIST_SIZE(o) <= (uint64_t)i) return No;
RETURNS(LGET(o, i))
BUILTIN3("list.`=`",list_set,C_ANY,o,C_INT,index,C_ANY,value)
  int64_t i = UNFXN(index);
  if (LIST_SIZE(o) <= (uint64_t)i) BAD_CALL("=", "index out of bounds");
  lsetm(o,i,value);
  R = 0;
RETURNS(R)
BUILTIN3("list.`=!`",list_uset,C_ANY,o,C_INT,index,C_ANY,value)
  int64_t i = UNFXN(index);
  if (LIST_SIZE(o) <= (uint64_t)i) return No;
  lsetm(o,i,value);
  R = 0;
RETURNS(R)
BUILTIN2("list.clear",list_clear,C_ANY,o,C_ANY,value)
  uint32_t i;
  uint32_t size = LIST_SIZE(o);
  if (IMMEDIATE(value) || value == Empty|| value == No) {
    void **p = (void*)O_PTR(o);
    for (i = 0; i < size; i++) {
      p[i] = value;
    }
  } else {
    for (i = 0; i < size; i++) {
      lsetm(o,i,value); //FIXME: check for generation
    }
  }
  R = 0;
RETURNS(R)
BUILTIN1("list.end",list_end,C_ANY,o)
RETURNS(FXN(!LIST_SIZE(o)))
BUILTIN1("list.head",list_head,C_ANY,o)
  uint32_t size = LIST_SIZE(o);
  if (size < 1) BAD_CALL("head", "list head: list is empty");
RETURNS(LGET(o,0))
BUILTIN1("list.tail",list_tail,C_ANY,o)
  if (dbgv) {
    printf("%llx: %llx\n", dbgv, o);
  }
  uint32_t size = LIST_SIZE(o);
  if (size > 1) {
    uint32_t rsz = size-1;
    GC_DISABLE();
    if (rsz < VIEW_TS) { //cheaper to just copy?
      LIST(R, rsz);
      dyn *src = &LGET(o,1);
      dyn *dst = &LGET(R,0);
      for (uint32_t i = 0; i < rsz; i++) {
        dst[i] = src[i];
      }
    } else {
      VIEW(R, o, 1, rsz);
    }
    GC_ENABLE();
  } else if (size != 0) {
    R = Empty;
  } else {
    BAD_CALL("tail", "list is empty\n");
  }
RETURNS(R)
BUILTIN2("list.take",list_take,C_ANY,o,C_INT,count)
  int64_t c = UNFXN(count);
  int64_t size = LIST_SIZE(o);
  if (c == 0) {
    R = Empty;
  } else if ((uint64_t)c <= (uint64_t)size) {
    GC_DISABLE();
    if (c < VIEW_TS) { //cheaper to just copy?
      LIST(R, c);
      dyn *src = &LGET(o,0);
      dyn *dst = &LGET(R,0);
      for (int i = 0; i < c; i++) {
        dst[i] = src[i];
      }
    } else {
      VIEW(R, o, 0, c);
    }
    GC_ENABLE();
  } else {
    BAD_CALL("take", "count %d is invalid for list size %d", (int)c, (int)size);
  }
RETURNS(R)
BUILTIN2("list.drop",list_drop,C_ANY,o,C_INT,count)
  int64_t c = UNFXN(count);
  int64_t size = LIST_SIZE(o);

  if ((uint64_t)c < (uint64_t)size) {
    uint32_t rsz = size-c;
    GC_DISABLE();
    if (rsz < VIEW_TS) { //cheaper to just copy?
      LIST(R, rsz);
      dyn *src = &LGET(o,c);
      dyn *dst = &LGET(R,0);
      for (uint32_t i = 0; i < rsz; i++) {
        dst[i] = src[i];
      }
    } else {
      VIEW(R, o, c, rsz);
    }
    GC_ENABLE();
  } else if (c == size) {
    R = Empty;
  } else {
    BAD_CALL("drop", "count %d is invalid for list size %d", (int)c, (int)size);
  }
RETURNS(R)
BUILTIN2("list.pre",list_pre,C_ANY,o,C_ANY,head)
  GC_DISABLE();
  CONS(R, head, o);
  GC_ENABLE();
RETURNS(R)

BUILTIN1("list.f",list_f,C_ANY,o) //flip list elements
  uint32_t n = LIST_SIZE(o);
  if (!n) return Empty;
  GC_DISABLE();
  dyn r;
  LIST(r, n);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t j = n - 1 - i;
    LGET(r,j) = LGET(o,i);
  } 
  GC_ENABLE();
RETURNS(r)

static void *SortFn;
static void *SortArgs;

int qsort_comp(const void *elem1, const void *elem2)  {
  void *R;
  lsetm(SortArgs,0,*(void**)elem1);
  lsetm(SortArgs,1,*(void**)elem2);
  api.args = SortArgs;
  CALL(R,SortFn);
  if (R) return -1;
  return  1;
}
BUILTIN2("list.qsort",list_qsort,C_ANY,o,C_ANY,fn)
  uint32_t nitems = LIST_SIZE(o);
  SortFn = fn;
  GC_DISABLE();
  LIST(SortArgs,2);
  qsort(&LGET(o,0), nitems, sizeof(dyn), qsort_comp);
  GC_ENABLE();
RETURNS(o)

BUILTIN_VARARGS("list.text",list_text)
  int i;
  uint8_t *p;
  dyn words = getArg(0);
  int64_t words_size;
  int l; // length of result text
  dyn sep; // separator
  int sep_size;

  if (LIST_SIZE(E) == 1) {
    sep = 0;
    sep_size = 0;
  } else if (LIST_SIZE(E) == 2) {
    sep = getArg(1);
    if (!IS_TEXT(sep)) {
      BAD_CALL("text", "separator is not text (%s)", print_object(sep));
    }
    sep_size = text_size(sep);
  } else {
    BAD_CALL("text", "bad number of arguments");
  }

  words_size = LIST_SIZE(words);

  l = 0;
  if (sep && words_size) l += sep_size*(words_size-1);
  for (i = 0; i < words_size; i++) {
    dyn x = LGET(words,i);
    if (!IS_TEXT(x)) {
      BAD_CALL("text", "text: not a text (%s)", print_object(x));
    }
    l += text_size(x);
  }

  GC_DISABLE();
  char buf[128];
  int is_tiny = 0;
  if (l < 128) {
    is_tiny = 1;
    p = buf;
  } else {
    R = alloc_textdata(l); 
    p = BIGTEXT_DATA(R);
  }

  for (i = 0; i < words_size; ) {
    dyn x = LGET(words,i);
    p = decode_text(p, x);
    ++i;
    if (sep && i < words_size) {
      p = decode_text(p, sep);
    }
  }

  if (is_tiny) {
    *p = 0;
    TEXT(R,buf);
  }
  GC_ENABLE();
RETURNS(R)
BUILTIN2("list.apply",list_apply,C_ANY,as,C_FN,f)
  api.args = as;
  CALL_TAGGED(R,f);
RETURNS(R)
BUILTIN2("list.apply_method",list_apply_method,C_ANY,as,C_ANY,m)
  if (!LIST_SIZE(as)) BAD_CALL("apply_method", "empty list");
  api.args = as;
  api.method = UNFXN(m);
  dyn fn = get_method(UNFXN(m),LGET(as,0));
  CALL(R,fn);
RETURNS(R)

/* RT-9: C-side `list.l` for T_VIEW.  The Symta-side dispatch
 * `list.l = | N $n | Ys dup N | times I N: Ys.I = pop Me | Ys`
 * was the #2 emitter of T_LIST allocations on the ./game
 * compile (36 M+ calls).  The dup-macro generates _listn +
 * .clear + while loop, each call allocates LIST(N) AND incurs
 * the dispatch + clear + iteration overhead.  The C variant
 * allocates once and memcpy's the underlying slots.  Same
 * allocation count, much lower per-call overhead. */
BUILTIN1("list.l", view_l, C_ANY, o)
  uint32_t n = VIEW_SIZE(o);
  uint32_t start = VIEW_START(o);
  dyn base = VIEW_STRIP_SHARED(VIEW_BASE(o));
  GC_DISABLE();
  LIST(R, n);
  dyn *src = &LGET(base, start);
  dyn *dst = &LGET(R, 0);
  for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
  GC_ENABLE();
RETURNS(R)

/* RT-9: C-side replacement for the Symta-side `fn.\`()\` @As =
 * As.apply(Me)`.  The Symta-side defn was the top emitter of
 * size-1 LIST allocations on the ./game compile -- every
 * single-arg closure call (`f(x)`) allocated a size-1 LIST for
 * the @As collection just to immediately unpack it back in
 * As.apply.  This C-side variant skips the @As collection
 * entirely: it shifts api.args left by one slot to drop the
 * receiver, decrements the size, and CALLs the closure
 * directly.  No new LIST is allocated. */
BUILTIN_VARARGS("fn.`()`", fn_call)
  dyn me = getArg(0);  /* receiver = the closure */
  uint32_t n = LIST_SIZE(api.args);
  /* Shift api.args[1..n-1] -> api.args[0..n-2], shrink size. */
  for (uint32_t i = 1; i < n; i++) {
    lsetm(api.args, i - 1, LGET(api.args, i));
  }
  O_SIZE(api.args) = n - 1;
  CALL(R, me);
RETURNS(R)


BUILTIN1("float.neg",float_neg,C_ANY,a)
  float fa;
  STFLT(fa,a);
  LDFLT(R,-fa);
RETURNS(R)
BUILTIN2("float.`+`",float_add,C_ANY,a,C_ANY,b)
  float fa, fb;
  STFLT(fa,a);
  if (O_TAG(b) == T_FLOAT) STFLT(fb,b);
  else if (O_TAG(b) == T_INT) fb = (float)UNFXN(b);
  else bad_type("number", 1, "float.`+`");
  LDFLT(R,fa+fb);
RETURNS(R)
BUILTIN2("float.`-`",float_sub,C_ANY,a,C_ANY,b)
  float fa, fb;
  STFLT(fa,a);
  if (O_TAG(b) == T_FLOAT) STFLT(fb,b);
  else if (O_TAG(b) == T_INT) fb = (float)UNFXN(b);
  else bad_type("number", 1, "float.`-`");
  LDFLT(R,fa-fb);
RETURNS(R)
BUILTIN2("float.`*`",float_mul,C_ANY,a,C_ANY,b)
  float fa, fb;
  STFLT(fa,a);
  if (O_TAG(b) == T_FLOAT) STFLT(fb,b);
  else if (O_TAG(b) == T_INT) fb = (float)UNFXN(b);
  else bad_type("number", 1, "float.`*`");
  LDFLT(R,fa*fb);
RETURNS(R)
BUILTIN2("float.`/`",float_div,C_ANY,a,C_ANY,b)
  float fa, fb;
  STFLT(fa,a);
  if (O_TAG(b) == T_FLOAT) STFLT(fb,b);
  else if (O_TAG(b) == T_INT) fb = (float)UNFXN(b);
  else bad_type("number", 1, "float.`/`");
  if (fb == 0.0) fb = FLT_MIN;
  LDFLT(R,fa/fb);
RETURNS(R)
BUILTIN2("float.`%`",float_rem,C_ANY,a,C_ANY,b)
  float fa, fb, r;
  STFLT(fa,a);
  if (O_TAG(b) == T_FLOAT) STFLT(fb,b);
  else if (O_TAG(b) == T_INT) fb = (float)UNFXN(b);
  else bad_type("number", 1, "float.`%`");
  if (fb == 0.0) fb = FLT_MIN;
  r = fa/fb;
  LDFLT(R,(r-(float)(int64_t)r)*fb);
RETURNS(R)
BUILTIN2("float.`^^`",float_pow,C_ANY,a,C_ANY,b)
  float fa, fb;
  STFLT(fa,a);
  if (O_TAG(b) == T_FLOAT) STFLT(fb,b);
  else if (O_TAG(b) == T_INT) fb = (float)UNFXN(b);
  else bad_type("number", 1, "float.`^^`");
  LDFLT(R,pow(fa,fb));
RETURNS(R)
BUILTIN2("float.`><`",float_eq,C_ANY,a,C_ANY,b)
  // cant just == raw bits, since floats have negative and positive zeroes
  if (O_TAG(b) == T_FLOAT) {
    float fa, fb;
    STFLT(fa,a);
    STFLT(fb,b);
    R = FXN(fa == fb);
  } else {
    R = FXN(0);
  }
RETURNS(R)
BUILTIN2("float.`<>`",float_ne,C_ANY,a,C_ANY,b)
  if (O_TAG(b) == T_FLOAT) {
    float fa, fb;
    STFLT(fa,a);
    STFLT(fb,b);
    R = FXN(fa != fb);
  } else {
    R = FXN(0);
  }
RETURNS(R)
BUILTIN2("float.`<`",float_lt,C_ANY,a,C_FLOAT,b)
  float fa, fb;
  STFLT(fa,a);
  STFLT(fb,b);
RETURNS(FXN(fa < fb))
BUILTIN2("float.`>`",float_gt,C_ANY,a,C_FLOAT,b)
  float fa, fb;
  STFLT(fa,a);
  STFLT(fb,b);
RETURNS(FXN(fa > fb))
BUILTIN2("float.`<<`",float_lte,C_ANY,a,C_FLOAT,b)
  float fa, fb;
  STFLT(fa,a);
  STFLT(fb,b);
RETURNS(FXN(fa <= fb))
BUILTIN2("float.`>>`",float_gte,C_ANY,a,C_FLOAT,b)
  float fa, fb;
  STFLT(fa,a);
  STFLT(fb,b);
RETURNS(FXN(fa >= fb))
BUILTIN1("float.as_text",float_as_text,C_ANY,a)
  GC_DISABLE();
  TEXT(R, print_object(a));
  GC_ENABLE();
RETURNS(R)
BUILTIN1("float.float",float_float,C_ANY,o)
RETURNS(o)
BUILTIN1("float.int",float_int,C_ANY,o)
  float fo;
  STFLT(fo,o);
RETURNS(FXN((int64_t)fo))
BUILTIN1("float.int16",float_int16,C_ANY,o)
  float fo;
  STFLT(fo,o);
RETURNS(FXN((int64_t)f2h(fo)))
BUILTIN1("float.int32",float_int32,C_ANY,o)
  float fo;
  float tmp;
  STFLT(fo,o);
  tmp = fo;
RETURNS(FXN((int64_t)*(uint32_t*)&tmp))
BUILTIN1("float.sqrt",float_sqrt,C_ANY,o)
  float fo;
  STFLT(fo,o);
  LDFLT(R, sqrt(fo));
RETURNS(R)
BUILTIN1("float.log",float_log,C_ANY,o)
  float fo;
  STFLT(fo,o);
  LDFLT(R, log(fo));
RETURNS(R)
BUILTIN1("float.sin",float_sin,C_ANY,o)
  float fo;
  STFLT(fo,o);
  LDFLT(R, sin(fo));
RETURNS(R)
BUILTIN1("float.asin",float_asin,C_ANY,o)
  float fo;
  STFLT(fo,o);
  LDFLT(R, asin(fo));
RETURNS(R)
BUILTIN1("float.cos",float_cos,C_ANY,o)
  float fo;
  STFLT(fo,o);
  LDFLT(R, cos(fo));
RETURNS(R)
BUILTIN1("float.acos",float_acos,C_ANY,o)
  float fo;
  STFLT(fo,o);
  LDFLT(R, acos(fo));
RETURNS(R)
BUILTIN1("float.tan",float_tan,C_ANY,o)
  float fo;
  STFLT(fo,o);
  LDFLT(R, tan(fo));
RETURNS(R)
BUILTIN1("float.atan",float_atan,C_ANY,o)
  float fo;
  STFLT(fo,o);
  LDFLT(R, atan(fo));
RETURNS(R)
BUILTIN2("float.atan2",float_atan2,C_ANY,y,C_FLOAT,x)
  float fy, fx;
  STFLT(fy,y);
  STFLT(fx,x);
  LDFLT(R, atan2(fy,fx));
RETURNS(R)
BUILTIN1("float.floor",float_floor,C_ANY,o)
  float fo;
  STFLT(fo,o);
  LDFLT(R, floor(fo));
RETURNS(R)
BUILTIN1("float.ceil",float_ceil,C_ANY,o)
  float fo;
  STFLT(fo,o);
  LDFLT(R, ceil(fo));
RETURNS(R)
BUILTIN1("float.round",float_round,C_ANY,o)
  float fo;
  STFLT(fo,o);
  LDFLT(R, round(fo));
RETURNS(R)
BUILTIN1("float.mantissa",float_mantissa,C_ANY,o)
RETURNS(FXN(O_GID(o)&MSK(23)))
BUILTIN1("float.exponent",float_exponent,C_ANY,o)
RETURNS(FXN((O_GID(o)>>23)&MSK(8)))

BUILTIN1("int.neg",int_neg,C_ANY,o)
RETURNS(-(int64_t)o)
BUILTIN2("int.`+`",int_add,C_ANY,a,C_ANY,b)
  if (!TAGIS(T_INT,b)) {
    if (!TAGIS(T_FLOAT,b)) bad_type("number", 1, "int.`+`");
    float fa, fb;
    fa = (float)UNFXN(a);
    STFLT(fb,b);
    LDFLT(R,fa+fb);
    return R;
  }
RETURNS((int64_t)a + (int64_t)b)
BUILTIN2("int.`-`",int_sub,C_ANY,a,C_ANY,b)
  if (!TAGIS(T_INT,b)) {
    if (!TAGIS(T_FLOAT,b)) bad_type("number", 1, "int.`-`");
    float fa, fb;
    fa = (float)UNFXN(a);
    STFLT(fb,b);
    LDFLT(R,fa-fb);
    return R;
  }
RETURNS((int64_t)a - (int64_t)b)
BUILTIN2("int.`*`",int_mul,C_ANY,a,C_ANY,b)
  if (!TAGIS(T_INT,b)) {
    if (!TAGIS(T_FLOAT,b)) bad_type("number", 1, "int.`*`");
    float fa, fb;
    fa = (float)UNFXN(a);
    STFLT(fb,b);
    LDFLT(R,fa*fb);
    return R;
  }
RETURNS(UNFXN(a) * (int64_t)b)
BUILTIN2("int.`/`",int_div,C_ANY,a,C_ANY,b)
  if (!TAGIS(T_INT,b)) {
    if (!TAGIS(T_FLOAT,b)) bad_type("number", 1, "int.`/`");
    float fa, fb;
    fa = (float)UNFXN(a);
    STFLT(fb,b);
    if (fb == 0.0) fb = FLT_MIN;
    LDFLT(R,fa/fb);
    return R;
  }
RETURNS(FXN((int64_t)a / (int64_t)b))
BUILTIN2("int.`%`",int_rem,C_ANY,a,C_ANY,b)
  if (!TAGIS(T_INT,b)) {
    if (!TAGIS(T_FLOAT,b)) bad_type("number", 1, "int.`%`");
    float fa, fb, r;
    fa = (float)UNFXN(a);
    STFLT(fb,b);
    if (fb == 0.0) fb = FLT_MIN;
    r = fa/fb;
    LDFLT(R,(r-(float)(int64_t)r)*fb);
    return R;
  }
RETURNS((int64_t)a % (int64_t)b)
BUILTIN2("int.`^^`",int_pow,C_ANY,a,C_INT,b)
RETURNS(FXN((int64_t)pow((float)UNFXN(a), (float)UNFXN(b))))
BUILTIN2("int.`><`",int_eq,C_ANY,a,C_ANY,b)
RETURNS(FXN(a == b))
BUILTIN2("int.`<>`",int_ne,C_ANY,a,C_ANY,b)
RETURNS(FXN(a != b))
BUILTIN2("int.`<`",int_lt,C_ANY,a,C_INT,b)
RETURNS(FXN((int64_t)a < (int64_t)b))
BUILTIN2("int.`>`",int_gt,C_ANY,a,C_INT,b)
RETURNS(FXN((int64_t)a > (int64_t)b))
BUILTIN2("int.`<<`",int_lte,C_ANY,a,C_INT,b)
RETURNS(FXN((int64_t)a <= (int64_t)b))
BUILTIN2("int.`>>`",int_gte,C_ANY,a,C_INT,b)
RETURNS(FXN((int64_t)a >= (int64_t)b))
BUILTIN2("int.`-+-`",int_ior,C_ANY,a,C_INT,b)
RETURNS((uint64_t)a | (uint64_t)b)
BUILTIN2("int.`-^-`",int_xor,C_ANY,a,C_INT,b)
RETURNS((uint64_t)a ^ (uint64_t)b)
BUILTIN2("int.`-*-`",int_mask,C_ANY,a,C_INT,b)
RETURNS((uint64_t)a & (uint64_t)b)
BUILTIN2("int.`-<-`",int_shl,C_ANY,a,C_INT,b)
RETURNS((int64_t)a<<UNFXN(b))
BUILTIN2("int.`->-`",int_shr,C_ANY,a,C_INT,b)
RETURNS(FXN(((int64_t)a>>UNFXN(b))))
BUILTIN1("int.end",int_end,C_ANY,o)
RETURNS(FXN(1))
BUILTIN1("int.char",int_char,C_ANY,o)
  dyn r;
#if 1
  r = TAGIFY(o, T_FIXTEXT);
#else
  char tmp[2];
  tmp[0] = (char)(uint8_t)UNFXN(o);
  tmp[1] = 0;
  TEXT(r, tmp);
#endif
RETURNS(r)

INLINE uint64_t fmix64(uint64_t key) {
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccd;
  key ^= key >> 33;
  key *= 0xc4ceb9fe1a85ec53;
  key ^= key >> 33;
  return key;
}
BUILTIN1("int.hash",int_hash,C_ANY,o) //unsigned hash for an int
RETURNS(fmix64((uint64_t)o)&(GID_MASK^((uint64_t)1<<63)))
BUILTIN1("int.float",int_float,C_ANY,o)
  LDFLT(R, UNFXN(o));
RETURNS(R)
BUILTIN1("int.flt16",int_flt16,C_ANY,o)
  LDFLT(R, h2f(UNFXN(o)));
RETURNS(R)
BUILTIN1("int.flt32",int_flt32,C_ANY,o)
  float f;
  *(uint32_t*)&f = UNFXN(o);
  LDFLT(R, (float)f);
RETURNS(R)
BUILTIN1("int.int",int_int,C_ANY,o)
RETURNS(o)
BUILTIN1("int.sqrt",int_sqrt,C_ANY,o)
RETURNS(FXN((int64_t)sqrt((int64_t)UNFXN(o))))
BUILTIN1("int.log",int_log,C_ANY,o)
RETURNS(FXN((int64_t)log((float)(int64_t)UNFXN(o))))
BUILTIN1("int.bytes",int_bytes,C_ANY,count)
  GC_DISABLE();
  R = alloc_bytes(UNFXN(count));
  GC_ENABLE();
RETURNS(R)
BUILTIN3("int.get_bits",int_get_bits,C_ANY,val,C_INT,ofs,C_INT,len)
  uint64_t v = O_GID(val);
  uint64_t o = O_GID(ofs);
  uint64_t l = O_GID(len);
  uint64_t m = (1<<l) - 1;
RETURNS(((v>>o)&m)<<GID_SHFT)
BUILTIN4("int.set_bits",int_set_bits,C_ANY,val,C_INT,ofs,C_INT,len,C_INT,word)
  uint64_t v = O_GID(val);
  uint64_t o = O_GID(ofs);
  uint64_t l = O_GID(len);
  uint64_t w = O_GID(word);
  uint64_t m = (1<<l) - 1;
  w &= m;
  v &= m<<o;
  v |= w<<o;
RETURNS(v<<GID_SHFT)

BUILTIN2("set_finalizer",set_finalizer,C_ANY,o,C_ANY,fn)
  gc_set_finalizer(o, fn);
RETURNS(0)


static dyn unstub_method(uint64_t metid,dyn resolved, dyn object) {
  dyn met = get_method(metid,object);
  hook_t *hook = &O_HOOK(met);
  //printf("unstubbing %p (%s)\n", hook->payload, hook->meta->name);
  if (O_SIZE(met) < 1) {
    fatal("unstub_method: bad closure\n");
  }
  //O_CODE(met) = sbc_hook(cls_get_,0);
  hook_t *rhook = &O_HOOK(resolved);
  hook->handler = rhook->handler;
  hook->payload = rhook->payload;
  /* RT-5: CALL now dispatches via the inline (handler,payload)
   * slots stored on the closure itself, so a hook-table patch
   * isn't visible until we mirror it into `met`'s inline slots.
   * Without this `CALL(r,met)` below would re-enter `b_stub_`
   * (the unchanged inline handler) and we'd loop until the
   * shadow stack is exhausted. */
  CLOSURE_SYNC_HOOK(met);
  api.frame->clsr = met;
  O_SIZE(api.args) = O_SIZE(api.args)-1; //strip the resolved arg
  dyn r;
  CALL(r,met);
  return r;
}

BUILTIN2("stub_",stub_,C_ANY,o,C_ANY,resolved)
RETURNS(unstub_method(api.method, resolved, o))

//BUILTIN2("cls_get_",cls_get_,C_ANY,o)

static dyn nativize_method(uint64_t metid, dyn o, psf_t handler, dyn payload) {
  dyn met = get_method(metid,o);
  hook_t *hook = &O_HOOK(met);
  //printf("nativizing %p (%s)\n", hook->payload, hook->meta->name);
  if (O_SIZE(met) < 1) fatal("nativize_method: bad closure\n");
  hook->handler = handler;
  O_SIZE(met) = 1; //free everything unused;
  LSET(met,0,payload);
  /* RT-5: O_SIZE just shrank to 1, so the inline (handler,
   * payload) slots that CALL reads now live at slots[1] and
   * slots[2] (not slots[old_size] and slots[old_size+1]).  The
   * `O_SIZE >= 1` precondition guarantees the original
   * allocation (old_size + 2) reserved at least 3 slots, so this
   * write is in-bounds.  Mirror the freshly-patched hook into
   * the new slot positions. */
  CLOSURE_SYNC_HOOK(met);
  return met;
}


static dyn cls_get_(uint8_t *pin) {
  BLTIN_PROLOGUE("");
  CHECK_NARGS(1);
  dyn ref = getArg(0);
  dyn tbl = LGET(P,0);
  dyn r = amGidGet(tbl, ref);
  /* RT-8b: BLTIN_PROLOGUE allocated frame_t on our stack and
   * linked it into api.frame.  Direct callers (cls_get_stub_)
   * invoke us without going through the CALL macro, so the
   * caller's save/restore-api.frame pattern doesn't apply --
   * if we return without unwinding, api.frame is left dangling
   * at our just-deallocated frame.  CLOSE_FRAME reads
   * frm_->prev (set by PROLOGUE) and restores. */
  CLOSE_FRAME;
  return r;
}


static dyn cls_set_(uint8_t *pin) {
  BLTIN_PROLOGUE("");
  CHECK_NARGS(2);
  dyn ref = getArg(0);
  dyn val = getArg(1);
  dyn tbl = LGET(P,0);
  amGidSet(tbl, ref, val);
  /* RT-8b: see cls_get_ above for why this is needed. */
  CLOSE_FRAME;
  return No;
}

BUILTIN2("cls_get_stub_",cls_get_stub_,C_ANY,o,C_ANY,tbl)
  /* RT-8b: cls_get_'s PROLOGUE reads `frm_->clsr` from
   * `api.clsr_pending` (caller-callee handoff slot), not from
   * the caller's frame -- so writing only to `api.frame->clsr`
   * (which is OUR frame, the stub's) leaves cls_get_'s P
   * pointing at whatever closure last went through the CALL
   * macro.  Stage the patched closure into clsr_pending too. */
  dyn _met = nativize_method(api.method, o, cls_get_, tbl);
  api.frame->clsr = _met;
  api.clsr_pending = _met;
  O_SIZE(api.args) = 1; //since we call cls_get_
RETURNS(cls_get_(0))
BUILTIN3("cls_set_stub_",cls_set_stub_,C_ANY,o,C_ANY,v,C_ANY,tbl)
  /* RT-8b: see cls_get_stub_ above for why both frame->clsr
   * and api.clsr_pending need to point at the patched
   * closure. */
  dyn _met = nativize_method(api.method, o, cls_set_, tbl);
  api.frame->clsr = _met;
  api.clsr_pending = _met;
  O_SIZE(api.args) = 2; //since we call cls_set_
  cls_set_(0);
RETURNS(No)
BUILTIN2("gid_get_",gid_get_,C_ANY,o,C_ANY,ref) RETURNS(amGidGet(o,ref))
BUILTIN3("gid_set_",gid_set_,C_ANY,o,C_ANY,ref,C_ANY,value)
  amGidSet(o, ref, value);
RETURNS(No)
BUILTIN2("gid_refs_",gid_refs_,C_ANY,o,C_ANY,tag)
RETURNS(amRefs(o,UNFXN(tag)))
BUILTIN0("hmap_",hmap_) RETURNS(amNew())
BUILTIN2("tbl.has",tbl_has,C_ANY,o,C_ANY,key) RETURNS(amHas(o,key))
BUILTIN2("tbl.got",tbl_got,C_ANY,o,C_ANY,key) RETURNS(amGot(o,key))
BUILTIN2("tbl.`.`",tbl_get,C_ANY,o,C_ANY,key) RETURNS(amGet(o,key))
BUILTIN3("tbl.`=`",tbl_set,C_ANY,o,C_ANY,key,C_ANY,value)
  amSet(o,key,value);
RETURNS(No)
BUILTIN2("tbl.del",tbl_del,C_ANY,o,C_ANY,key) RETURNS(amDel(o,key))
BUILTIN1("tbl.is_table",tbl_is_table,C_ANY,o) RETURNS(FXN(1))
BUILTIN1("tbl.n",tbl_n,C_ANY,o) RETURNS(amN(o))
BUILTIN1("tbl.l",tbl_l,C_ANY,o) RETURNS(amL(o))
BUILTIN1("tbl.ks",tbl_ks,C_ANY,o) RETURNS(amKs(o))
BUILTIN1("tbl.clear",tbl_clear,C_ANY,o) RETURNS(amClear(o))
BUILTIN2("tbl.setNo",tbl_set_no,C_ANY,o,C_ANY,value)
  amSetNo(o,value);
RETURNS(No)
BUILTIN2("tbl.swap",tbl_swap,C_ANY,o,C_ANY,m) //advanced cls.s stuff needs it
  //currently breaks due to how AM_ATTRACT works on direct object address
  amSwap(o,m);
RETURNS(No)

dyn token_(dyn type, dyn val, dyn row, dyn col, dyn orig) {
  dyn r;
  OBJECT(r, T_TOK, 7);
  LGET(r,0) = type;
  LGET(r,1) = val;
  LGET(r,2) = 0; // pchar -- gc_alloc returns dirty memory; callers
                 // that set slot 2 to something meaningful overwrite,
                 // but a token whose .pchar is never set must read 0.
  LGET(r,3) = row;
  LGET(r,4) = col;
  LGET(r,5) = orig;
  LGET(r,6) = 0; // parsed -- same reason. The tok_ varargs builtin
                 // overwrites this with caller-supplied parsed value.
  return r;
}

BUILTIN_VARARGS("tok_",tok_)
  GC_DISABLE();
  dyn type = getArg(0);
  dyn val = getArg(1);
  //dyn pchar = getArg(2);
  dyn row = getArg(2);
  dyn col = getArg(3);
  dyn orig = getArg(4);
  dyn parsed = getArg(5);
  dyn r = token(type, val, row, col, orig);
  LGET(r,6) = parsed;
  //fprintf(stderr, "%s\n", print_object(type));
  GC_ENABLE();
RETURNS(r);

BUILTIN1("tok.type",tok_type,C_ANY,o)
RETURNS(LGET(o,0));

BUILTIN1("tok.value",tok_value,C_ANY,o)
RETURNS(LGET(o,1));

BUILTIN1("tok.pchar",tok_pchar,C_ANY,o)
RETURNS(LGET(o,2));

BUILTIN1("tok.row",tok_row,C_ANY,o)
RETURNS(LGET(o,3));

BUILTIN1("tok.col",tok_col,C_ANY,o)
RETURNS(LGET(o,4));

BUILTIN1("tok.orig",tok_orig,C_ANY,o)
RETURNS(LGET(o,5));

BUILTIN1("tok.parsed",tok_parsed,C_ANY,o)
RETURNS(LGET(o,6));

BUILTIN2("tok.=type",tok_stype,C_ANY,o,C_ANY,v)
  LSET(o,0,v);
RETURNS(0);

BUILTIN2("tok.=value",tok_svalue,C_ANY,o,C_ANY,v)
  LSET(o,1,v);
RETURNS(0);

BUILTIN2("tok.=pchar",tok_spchar,C_ANY,o,C_ANY,v)
  LSET(o,2,v);
RETURNS(0);

BUILTIN2("tok.=row",tok_srow,C_ANY,o,C_ANY,v)
  LSET(o,3,v);
RETURNS(0);

BUILTIN2("tok.=col",tok_scol,C_ANY,o,C_ANY,v)
  LSET(o,4,v);
RETURNS(0);

BUILTIN2("tok.=orig",tok_sorig,C_ANY,o,C_ANY,v)
  LSET(o,5,v);
RETURNS(0);

BUILTIN2("tok.=parsed",tok_sparsed,C_ANY,o,C_ANY,v)
  LSET(o,6,v);
RETURNS(0);


BUILTIN1("methods_",methods_,C_ANY,o)
  GC_DISABLE();
  R = Empty;
  type_t *t = &types[(int)O_TAG(o)];
  /* RT-6/6b: walk the packed slot table -- empty slots have mid==0. */
  for (uint32_t i = 0; i < t->method_cap; i++) {
    uint32_t mid = t->methods[i].mid;
    if (!mid) continue;
    void *c, *pair;
    LIST(pair, 2);
    LGET(pair,0) = method_names[mid];
    LGET(pair,1) = t->methods[i].fn;
    CONS(c, pair, R);
    R = c;
  }
  GC_ENABLE();
RETURNS(R)


BUILTIN1("get_meta_",get_meta_,C_ANY,o)
RETURNS(FXN((int64_t)O_CODE(o)))
BUILTIN2("set_meta_",set_meta_,C_ANY,o,C_ANY,v)
  O_CODE(o) = (uint32_t)UNFXN(v);
  /* RT-5: if the user just rewired O_CODE for a closure, the
   * dispatch the closure resolves to has changed.  Mirror the
   * new hooks_heap entry into the inline slots so the next
   * CALL(o) sees it. */
  if (TAGIS(T_CLOSURE, o)) CLOSURE_SYNC_HOOK(o);
RETURNS(o)

BUILTIN1("intern_",intern_,C_ANY,name)
RETURNS(FXN(intern(text_chars(name))))

BUILTIN2("ref_",ref_,C_ANY,tag,C_ANY,gid)
RETURNS(MKIMM(UNFXN(tag),UNFXN(gid)))

BUILTIN1("gid_",gid_,C_ANY,o)
RETURNS((dyn)STRIP_TAG(o))

BUILTIN1("imm_",imm_,C_ANY,o)
RETURNS(FXN(IMMEDIATE(o)))

BUILTIN1("typename",typename,C_ANY,o)
RETURNS(types[O_TAG(o)].sname);

/* TS-2.1: Walk the runtime type-tag super chain (set up by
 * add_subtype in main.c) and return a Symta list of text names,
 * starting from the receiver's own tag, then its parent, then
 * grandparent, up to the root.  Uses `types[tag].name` (the C
 * string) rather than `sname` because many internal types
 * (T_OBJECT, T_HARD_LIST, T_GENERIC_TEXT, ...) don't have an
 * sname set but DO have a `name`.  The caller-facing
 * `subtype_of X T` in core_.s walks this list. */
BUILTIN1("parents_of_",parents_of_,C_ANY,o)
  int tag = O_TAG(o);
  int chain[32];
  int n = 0;
  while (tag != END_TAG && n < 32) {
    chain[n++] = tag;
    tag = types[tag].super;
  }
  GC_DISABLE();
  R = Empty;
  /* Build list with head = X's own type-name, tail = ancestors */
  for (int i = n - 1; i >= 0; i--) {
    dyn name_text;
    TEXT(name_text, types[chain[i]].name);
    void *c;
    CONS(c, name_text, R);
    R = c;
  }
  GC_ENABLE();
RETURNS(R)


#define PU3(name,s,m,x,y,z) \
BUILTIN1(#name,name,C_ANY,o) \
  int64_t a = (uint16_t)UNFXN(LGET(o,0))&m; \
  int64_t b = (uint16_t)UNFXN(LGET(o,1))&m; \
  int64_t c = (uint16_t)UNFXN(LGET(o,2))&m; \
RETURNS(FXN((z<<(s*2)) | (y << s) | x))


#define UU3(name,s,m,x,y,z) \
BUILTIN1(#name,name,C_ANY,o) \
  uint64_t p = UNFXN(o); \
  dyn *r; \
  int64_t x =  p        &m; \
  int64_t y = (p>>s    )&m; \
  int64_t z = (p>>(s*2))&m; \
  GC_DISABLE(); \
  LIST(r,3); \
  LGET(r,0) = FXN(a); \
  LGET(r,1) = FXN(b); \
  LGET(r,2) = FXN(c); \
  GC_ENABLE(); \
RETURNS(r)

PU3(p3u10_,10,0x3FF,a,b,c)
UU3(u3u10_,10,0x3FF,a,b,c)
PU3(p3u16_,16,0xFFFF,a,b,c)
UU3(u3u16_,16,0xFFFF,a,b,c)

#define IDC_MAX 64
dyn IdCounter = 0;  //id 0 is a special nil entity
dyn idc_stack[IDC_MAX];
int idc_sp;


BUILTIN0("newId_",newId_)
  IdCounter = (dyn)((int64_t)IdCounter + (int64_t)FXN(1));
RETURNS(IdCounter)

BUILTIN1("newIds_",newIds_,C_ANY,size)
  dyn r = IdCounter;
  IdCounter = (dyn)((int64_t)IdCounter + (int64_t)size);
RETURNS(r)

BUILTIN0("topId_",topId_) RETURNS(IdCounter)

BUILTIN1("pushId_",pushId_,C_ANY,Id)
  idc_stack[idc_sp++] = IdCounter;
  IdCounter = Id-(int64_t)FXN(1);
RETURNS(No)

BUILTIN0("popId_",popId_)
  IdCounter = idc_stack[--idc_sp];
RETURNS(No)

void btjump(dyn land, dyn value) {
  jmp_state *js_;
  js_ = (jmp_state*)BYTES_DATA(land);
  GC_DISABLE();
  dyn *r;
  LIST(r,2);
  LSET(r,0,land);
  LSET(r,1,value);
  api.jmp_return = r;
  api.frame = js_->frame;
  while (js_->uwh != api.puwh) { //run finalizers
    dyn h_ = *api.puwh--;
    dyn k_;
    ARGLIST(0);
    CALL(k_,h_);
  }
  GC_ENABLE();
  longjmp(*js_->anchor, 1);
}


BUILTIN0("get_deh",get_deh)
RETURNS(api.error_handler)


BUILTIN1("set_deh",set_deh,C_ANY,o)
  dyn prev = api.error_handler;
  api.error_handler = o;
RETURNS(prev)

BUILTIN1("rterr_",rterr_,C_ANY,text)
  rterr("%s", text_chars(text));
RETURNS(0)

BUILTIN0("halt",halt)
  exit(0);
RETURNS(0)

BUILTIN1("set_dbgv_",set_dbgv_,C_ANY,val)
  dbgv = val;
RETURNS(No)

BUILTIN1("dbg",dbg,C_ANY,o)
  fprintf(stderr, "dbg: %s\n", print_object(o));
RETURNS(o)

BUILTIN1("say_",say_,C_TEXT,o)
  if (IS_FIXTEXT(o)) {
    char buf[32];
    char *out = buf;
    out += fixtext_decode(out, o);
    *out = 0;
    fprintf(stdout, "%s", buf);
  } else {
    fwrite(BIGTEXT_DATA(o), 1, (size_t)BIGTEXT_SIZE(o), stdout);
  }
RETURNS(No)


static uint64_t show_runtime_info() {
  uint64_t used0 = HEAP_SIZE - (api.hg0[0].top - api.hg0[0].heap);
  uint64_t used1 = HEAP_SIZE - (api.hg0[1].top - api.hg0[1].heap);
  fprintf(stderr, "-------------\n");
  fprintf(stderr, "heap used: %ld+%ld\n", (long)used0, (long)used1);
  fprintf(stderr, "heap size: %ld\n", (long)(HEAP_SIZE*2));
  fprintf(stderr, "types: %ld\n", arrlen(types));
  /* RT-6b: nmethods retired with the method_pages arena; sum
   * the per-type slot-table counts instead. */
  uint64_t total_methods = 0;
  for (int ti = 0; ti < arrlen(types); ti++) total_methods += types[ti].method_count;
  fprintf(stderr, "methods: %lu\n", (unsigned long)total_methods);
  fprintf(stderr, "gid_get_ calls: %d\n", prf.gid_get_);
  fprintf(stderr, "gid_set_ calls: %d\n", prf.gid_set_);
  fprintf(stderr, "\n");
}

/* RT-9 measurement: per-tag allocation counts.  Separate from
 * rtstat to keep workload-dependent output out of the 24-runtime
 * golden -- this one is for benchmark-time data capture. */
static void show_alloc_stats() {
  fprintf(stderr, "-------------\n");
  fprintf(stderr, "allocations by tag:\n");
  for (uint32_t t = 0; t < 32; t++) {
    if (!alloc_stats.by_tag[t]) continue;
    char *name = (t < (uint32_t)arrlen(types) && types[t].name) ? types[t].name : "?";
    fprintf(stderr, "  %-12s %lu\n", name, (unsigned long)alloc_stats.by_tag[t]);
  }
  fprintf(stderr, "list size distribution:\n");
  for (uint32_t b = 0; b < 16; b++) {
    if (!alloc_stats.list_size_bucket[b]) continue;
    if (b < 15)
      fprintf(stderr, "  size %-3u  %lu\n", b, (unsigned long)alloc_stats.list_size_bucket[b]);
    else
      fprintf(stderr, "  size 15+  %lu\n", (unsigned long)alloc_stats.list_size_bucket[b]);
  }
  /* RT-9: element-type distribution for size-1 LIST allocations
   * caught at the SBC_LIST1 store moment (LITERAL `[X]` subset). */
  uint64_t total_l1 = 0;
  for (uint32_t t = 0; t < 32; t++) total_l1 += alloc_stats.list1_elem_tag[t];
  if (total_l1) {
    fprintf(stderr, "size-1 LIST element types (LITERAL [X] subset, %lu samples):\n",
            (unsigned long)total_l1);
    for (uint32_t t = 0; t < 32; t++) {
      if (!alloc_stats.list1_elem_tag[t]) continue;
      char *name = (t < (uint32_t)arrlen(types) && types[t].name) ? types[t].name : "?";
      double pct = (100.0 * (double)alloc_stats.list1_elem_tag[t] / (double)total_l1);
      fprintf(stderr, "  %-12s %10lu  %5.1f%%\n", name,
              (unsigned long)alloc_stats.list1_elem_tag[t], pct);
    }
  }

  /* RT-9: top-N caller-pin attribution for T_LIST.  Walk the
   * open-addressed hash, copy populated entries to a flat array,
   * sort by count descending, print top 20 with resolved source. */
  uint32_t live = 0;
  alloc_pin_count_t *flat = 0;
  for (uint32_t i = 0; i < ALLOC_ATTRIB_BUCKETS; i++) {
    if (alloc_stats.pin_counts[i].pin) live++;
  }
  if (live) {
    flat = (alloc_pin_count_t*)malloc(live * sizeof(alloc_pin_count_t));
    uint32_t k = 0;
    for (uint32_t i = 0; i < ALLOC_ATTRIB_BUCKETS; i++) {
      if (alloc_stats.pin_counts[i].pin) {
        flat[k++] = alloc_stats.pin_counts[i];
      }
    }
    /* Simple in-place insertion sort by count desc -- only the
     * top-20 ordering matters; full sort is fine for tens of
     * thousands of entries (one-shot at exit). */
    for (uint32_t i = 1; i < live; i++) {
      alloc_pin_count_t cur = flat[i];
      int32_t j = (int32_t)i - 1;
      while (j >= 0 && flat[j].count < cur.count) {
        flat[j+1] = flat[j];
        j--;
      }
      flat[j+1] = cur;
    }
    fprintf(stderr, "top-20 T_LIST alloc sites (by caller pin):\n");
    uint64_t total = alloc_stats.by_tag[T_LIST];
    uint32_t n = live < 20 ? live : 20;
    for (uint32_t i = 0; i < n; i++) {
      int row = -1, col = -1;
      sbc_t *sc = resolve_pin(flat[i].pin, &row, &col);
      char *origin = sc ? sc->filename : "???";
      double pct = total ? (100.0 * (double)flat[i].count / (double)total) : 0.0;
      fprintf(stderr, "  %10lu  %5.1f%%  %s:%d,%d\n",
              (unsigned long)flat[i].count, pct, origin, row, col);
    }
    uint64_t covered = 0;
    for (uint32_t i = 0; i < live; i++) covered += flat[i].count;
    fprintf(stderr, "(%u distinct sites covered %lu of %lu allocs)\n",
            live, (unsigned long)covered, (unsigned long)total);
    free(flat);
  }
  fprintf(stderr, "\n");
}

BUILTIN0("rtstat",rtstat)
  show_runtime_info();
RETURNS(0)

BUILTIN0("alloc_stats_",alloc_stats_)
  show_alloc_stats();
RETURNS(0)

/* RT-9 measurement: if SYMTA_ALLOC_STATS is set in the env, dump
 * per-tag allocation counts on process exit.  Lets a cold ./game
 * compile capture the numbers without modifying the workload. */
static void alloc_stats_atexit(void) {
  show_alloc_stats();
}

/* Force a minor GC.  The collector chooses the generation based
 * on the usual triggers (gen0 fill, magnet/dirty signals); we just
 * call into it synchronously.  Useful for:
 *   - deterministic finalizer-firing in tests
 *   - GC-pause cost measurement in benchmarks
 *   - debugging suspected write-barrier / root-scan / age-tracking
 *     bugs (force a collection at a known point and verify the
 *     program still produces correct output) */
BUILTIN0("gc",gc)
  api.gc();
RETURNS(No)

/* RT-3: runtime tuning hooks for the GC.
 *
 * gc_set_gen0_pages N  --  resize gen0's *trigger threshold* to N
 * pages and return the previous value (in pages).  The underlying
 * heap region is fixed at boot; this only moves the `ts` pointer
 * up or down so collections fire sooner or later.  Useful for:
 *   - shrinking gen0 in stress tests so finalizers fire on small
 *     allocation counts (no need for `times I 10000` filler)
 *   - growing gen0 in steady-state benches so the measured kernel
 *     doesn't get a surprise GC pause mid-loop
 *   - parity with the SYMTA_GEN0_SIZE env knob (this is the same
 *     thing, exposed to live code)
 *
 * If `npages` is below 1 or above the gen's own capacity, we clamp
 * silently rather than error -- this is a tuning hint, not a hard
 * configuration.
 *
 * Re-sizing recomputes hg0->ts on the spot.  If the new threshold
 * is already crossed (top < ts), the next allocation triggers GC,
 * which is the intended semantics. */
BUILTIN1("gc_set_gen0_pages",gc_set_gen0_pages,C_INT,np)
  long npages = (long)UNFXN(np);
  /* hg->size is in 8-byte slots; PAGE_SIZE / 8 slots per page. */
  long slots_per_page = (long)(PAGE_SIZE / sizeof(void*));
  long old_slots = (long)api.hg0[0].size;
  long old_pages = old_slots / slots_per_page;
  if (npages < 1) npages = 1;
  /* Don't let the threshold exceed the physical region the gen
   * occupies (api.hg0[0] runs HEAP_SIZE slots upward from heap0). */
  long max_pages = (long)HEAP_SIZE / slots_per_page;
  if (npages > max_pages) npages = max_pages;
  api.hg0[0].size = (uint32_t)(npages * slots_per_page);
  api.hg0[0].ts = api.hg0[0].top - api.hg0[0].size;
RETURNS(FXN(old_pages))

/* Bytes currently allocated in gen0.  Cheap query for tests that
 * want to assert allocation pressure under a known threshold. */
BUILTIN0("gc_gen0_used",gc_gen0_used)
  uint64_t used_slots = (uint64_t)(api.hg0[0].base - api.hg0[0].top);
RETURNS(FXN(used_slots * sizeof(void*)))

/* CORE-1: print the live stack trace to stderr without raising
 * an error.  Used by tests/runtime/lineno-check.sh to validate
 * per-instruction positions (which CORE-3 made unobservable for
 * caught `bad`s).  Also useful for ad-hoc debugging.  Returns No. */
BUILTIN0("print_stack_trace_",print_stack_trace_)
  fflush(stdout);
  print_stack_trace();
RETURNS(No)

/* gen0's current trigger threshold, in bytes.  Mirrors the bytes
 * that SYMTA_GEN0_SIZE / gc_set_gen0_pages set. */
BUILTIN0("gc_gen0_size",gc_gen0_size)
  uint64_t sz_slots = (uint64_t)api.hg0[0].size;
RETURNS(FXN(sz_slots * sizeof(void*)))

BUILTIN0("stack_trace",stack_trace)
  fatal("FIXME: implement stack_trace");
  /*void **p;
  int64_t s = api.frame-api.frames-1;
  if (s == 0) {
    R = Empty;
  } else {
    GC_DISABLE();
    LIST(R,s-1);
    p = &LGET(R,0);
    while (s-- > 1) {
      int64_t l = s + 1;
      void *name;
      TEXT(name, "unknown");
      *p++ = name;
    }
    GC_ENABLE();
  }*/
RETURNS(R)

BUILTIN1("inspect",inspect,C_ANY,o)
  if (IMMEDIATE(o)) {
    fprintf(stderr, "%p: type=%d, age=immediate\n", o, (int)O_TAG(o));
  } else {
    fprintf(stderr, "%p: type=%d, age=%d\n", o, (int)O_TAG(o), O_AGE(o));
  }
RETURNS(0)

BUILTIN1("load_sbc",load_sbc,C_TEXT,path_text)
  char path[1024];
  decode_text(path, path_text);
  R = load_sbc(path);
RETURNS(R)

BUILTIN1("add_sbc_folder",add_sbc_folder,C_TEXT,path_text)
  char path[1024];
  decode_text(path, path_text);
  add_lib_folder(path);
RETURNS(No)

BUILTIN1("register_library_folder",register_library_folder,C_TEXT,path_text)
  char path[1024];
  decode_text(path, path_text);
  add_lib_folder(path);
RETURNS(No)

BUILTIN1("load_file",load_file,C_ANY,path)
  fatal("FIXME: implement load_file\n");
RETURNS(No)

BUILTIN1("utf8",utf8,C_ANY,bytes)
  fatal("FIXME: implement utf8\n");
RETURNS(No)

BUILTIN1("raw_file_",raw_file_,C_ANY,filename_text)
  int64_t file_size;
  char *filename = text_to_cstring(filename_text);
  uint8_t *contents = fs_get(&file_size, filename);
  GC_DISABLE();
  LIST(R, 2);
  LGET(R,0) = (dyn)contents;
  LGET(R,1) = FXN(file_size);
  GC_ENABLE();
RETURNS(R)

BUILTIN1("get_file_",get_file_,C_ANY,filename_text)
  int64_t file_size;
  char *filename = text_to_cstring(filename_text);
  uint8_t *contents = fs_get(&file_size, filename);
  if (contents) {
    GC_DISABLE();
    R = alloc_bytes(file_size);
    GC_ENABLE();
    memcpy(BYTES_DATA(R), contents, file_size);
    free(contents);
  } else {
    R = No;
  }
RETURNS(R)

BUILTIN2("set_file_",set_file_,C_ANY,filename_text,C_BYTES,bytes)
  char *filename = text_to_cstring(filename_text);
  write_whole_file(filename, BYTES_DATA(bytes), BYTES_SIZE(bytes));
RETURNS(0)

BUILTIN1("get_text_file_",get_text_file_,C_TEXT,filename_text)
  int64_t file_size;
  char *filename = text_to_cstring(filename_text);
  char *contents = (char*)fs_get(&file_size, filename);
  if (contents) {
    GC_DISABLE();
    TEXT(R, contents);
    GC_ENABLE();
    free(contents);
  } else {
    R = No;
  }
RETURNS(R)

BUILTIN2("set_text_file_",set_text_file_,C_TEXT,filename_text,C_TEXT,text)
  char buf[32];
  int size = text_size(text);
  char *filename;
  char *xs;
  if (IS_FIXTEXT(text)) {
    decode_text(buf, text);
    xs = buf;
  } else {
    xs = (char*)BIGTEXT_DATA(text);
  }
  filename = text_to_cstring(filename_text);
  write_whole_file(filename, xs, size);
RETURNS(0)


static char *sif_to_sbc_sub(char *filename, char *sift, int sift_len) {
  char *tfilename = malloc(strlen(filename)+8);
  sif_t *sif = sif_parse(sift, sift_len);
#if 0
  sprintf(tfilename,"%s.txt", filename);
  write_whole_file(tfilename, sift, sift_len);
#endif
  uint8_t *raw_sbc = sif2sbc(sif);
  write_whole_file(filename, raw_sbc, arrlen(raw_sbc));
  arrfree(raw_sbc);
#if 0
  sif_t *sif2 = sbc2sif(tfilename);
  sif_cmp(sif,sif2);
  sprintf(tfilename,"%s2.sbc", filename);
  sif2sbc(sif2, tfilename);
  //exit(-1);
#endif
  //write_whole_file(filename, "\0", 1);
  free(tfilename);
  sif_free(sif);
  return 0;
}

BUILTIN2("sif_to_sbc_",sif_to_sbc_,C_TEXT,filename_text,C_TEXT,text)
  char buf[32];
  int size = text_size(text);
  char *xs;
  if (IS_FIXTEXT(text)) {
    decode_text(buf, text);
    xs = buf;
  } else {
    xs = (char*)BIGTEXT_DATA(text);
  }
  char *filename = strdup(text_to_cstring(filename_text));
  char *result = sif_to_sbc_sub(filename, xs, size);
  free(filename);
  if (result) {
    GC_DISABLE();
    TEXT(R, result);
    GC_ENABLE();
    free(result);
  } else {
    R = No;
  }
RETURNS(R)



BUILTIN1("sbc_metadata_",sbc_metadata_,C_TEXT,filename_text)
  char *filename = strdup(text_to_cstring(filename_text));
  char *src, *deps, *export;
  int result = sbc_load_metadata(filename, &src, &deps, &export);
  free(filename);
  if (!result) return No;
  dyn *tsrc, tdeps, texport;
  GC_DISABLE();
  TEXT(tsrc, src);
  TEXT(tdeps, deps);
  TEXT(texport, export);
  free(src);
  free(deps);
  free(export);
  LIST(R, 3);
  LGET(R,0) = tsrc;
  LGET(R,1) = tdeps;
  LGET(R,2) = texport;
  GC_ENABLE();
RETURNS(R)

static dyn sif_eval_sub(char *sift, int sift_len) {
  sif_t *sif = sif_parse(sift, sift_len);
  uint8_t *raw_sbc = sif2sbc(sif);
  sif_free(sif);
  int64_t size = arrlen(raw_sbc);
  uint8_t *pin = malloc(size);
  memcpy(pin, raw_sbc, size);
  arrfree(raw_sbc);
  sbc_t *sbc = sbc_new(pin, size, "<eval>");
  //FIXME: some way to free the unused SBC parts
  //printf("....begin\n");
  dyn r = sbc_exec(sbc);
  //printf("....end\n");
  return r;
}

BUILTIN1("sif_eval_",sif_eval_,C_TEXT,text)
  char buf[32];
  int size = text_size(text);
  char *xs;
  if (IS_FIXTEXT(text)) {
    decode_text(buf, text);
    xs = buf;
  } else {
    xs = (char*)BIGTEXT_DATA(text);
  }
  R = sif_eval_sub(xs, size);
RETURNS(R)


#define POPC()         \
  (((*pin == '\n')     \
     ?(col = 0, ++row)  \
     :++col)               \
   ,*pin++)

#define NXTC() (*pin)

#include <ctype.h>

#define EXTRACT_SEEK 0
#define EXTRACT_USES 1
#define EXTRACT_EXPS 2

static char *parse_modhdr(char *text, char ***ruses, char ***rexps, int *erow) {
  int mode = EXTRACT_SEEK;
  int row = 0;
  int col = 0;
  char *pin = text;

  char **uses = 0;
  char **exps = 0;

  while (*pin) {
skipws:
    while (isspace(*pin)) POPC();
    if (pin[0] == '/' && pin[1] == '/') {
      while (pin[0] && pin[0]!='\n') POPC();
      goto skipws;
    } else if (pin[0] == '/' && pin[1] == '*') {
      POPC();
      POPC();
      while (pin[0]) {
        if (pin[0] == '*' && pin[1] == '/') {
          POPC();
          POPC();
          break;
        }
        POPC();
      }
      goto skipws;
    }
    if (!*pin) break;
    if (mode != EXTRACT_USES && col == 0 && !strncmp(pin, "use", 3)) {
      POPC();
      POPC();
      POPC();
      mode = EXTRACT_USES;
      goto skipws;
    } else if (mode != EXTRACT_EXPS && col == 0 && !strncmp(pin, "export", 6)) {
      for (int j = 0; j < 6; j++) {
        POPC();
      }
      mode = EXTRACT_EXPS;
      goto skipws;
    } if (mode == EXTRACT_USES || mode == EXTRACT_EXPS) {
      if (col == 0) break;
      char *item = 0;
      while (*pin && !isspace(*pin)) {
        if (pin[0] == '/' && (pin[1] == '/' || pin[1] == '*')) break;
        arrput(item, POPC());
      }
      arrput(item,0);
      if (mode == EXTRACT_USES) arrput(uses,item);
      else arrput(exps,item);
      goto skipws;
    } else {
      break;
    }
  }
  if (ruses) *ruses = uses;
  if (rexps) *rexps = exps;
  if (erow) *erow = row;
  return pin;
}

//Usage:
//[Uses Exps HSz Row] parse_module_hdr_ Text
//Xs Exps.text(' ').parse.0
BUILTIN1("parse_module_hdr_",parse_module_hdr_,C_TEXT,text)
  char *ctext = text_to_cstring(text);

  char **uses;
  char **exps;
  int erow;
  char *pin = parse_modhdr(ctext, &uses, &exps, &erow);
  int hdr_sz = pin-ctext;

  GC_DISABLE();
  dyn *us;
  LIST(us, arrlen(uses));
  for (int i = 0; i < arrlen(uses); i++) {
    dyn *item;
    TEXT(item, uses[i]);
    LGET(us,i) = item;
    arrfree(uses[i]);
  }

  dyn *es;
  LIST(es, arrlen(exps));
  for (int i = 0; i < arrlen(exps); i++) {
    dyn *item;
    TEXT(item, exps[i]);
    LGET(es,i) = item;
    arrfree(exps[i]);
  }

  LIST(R, 4);
  LGET(R,0) = us;
  LGET(R,1) = es;
  LGET(R,2) = FXN(hdr_sz);
  LGET(R,3) = FXN(erow);

  arrfree(uses);
  arrfree(exps);

  GC_ENABLE();
RETURNS(R)


#include "ncm.h"

static uint8_t *cbFileGet(void *handle, uint32_t *rsize, char *filename) {
  int64_t sz;
  uint8_t *data = fs_get(&sz, filename);
  uint8_t *r = 0;
  if (!data) return 0;
  arrsetlen(r, sz+1);
  memcpy(r, data, sz);
  r[sz] = 0;
  *rsize = sz;

  free(data);

  return r;
}

static int cbFileExist(void *handle, char *filename) {
  return fs_is_file(filename);
}

static char *cbCommand(void *handle, char *cmd) {
  if (!strncmp(cmd,"say ", 4)) {
    printf("%s\n", cmd+4);
    return 0;
  }
  return exec_command(cmd);
}

static void cbFatal(void *ncm, char *macro
                        ,char *file, int row, int col, char *text) {

  fprintf(stderr, "%s:%d:%d\n", file, row+1, col);
  if (macro)
    fprintf(stderr, "  During macro expansion `%s`\n", macro);
  fprintf(stderr, "  Fatal: %s\n", text);
  exit(-1);
}



BUILTIN3("ncm_process_",ncm_process_,C_TEXT,filename_text,C_TEXT,text
        ,C_ANY,ipaths)
  char **paths = 0;

  char *cfilename = strdup(text_to_cstring(filename_text));

  //fprintf(stderr, "%s\n", cfilename);
  url_t *url = url_parts(cfilename);

  //add source file folder into search path.
  if (url->dir[0]) arrput(paths, strdup(url->dir));
  
  for (int i = 0; i < O_SIZE(ipaths); i++) {
    char *path = strdup(text_to_cstring(LGET(ipaths,i)));
    //fprintf(stderr, "%d:%s\n", i,path);
    arrput(paths, path);
  }

  arrput(paths, 0); //terminate paths list

  ncm_options_t o = ncm_default_options();
  o.file_exist = cbFileExist;
  o.file_get = cbFileGet;
  o.command = cbCommand;
  o.err_fatal = cbFatal;
  o.paths = paths;
  o.mchar = '#';
  o.flags |= NCM_KEEP_COMMENTS;

  char *result = ncm_eval(&o, cfilename, text_to_cstring(text));

#if 0
  //printf("%s\n", result);
  if (!strcmp(cfilename, "/Users/macbook/Documents/git/symta/src/reader.s")) {
    char *tfilename = malloc(strlen(cfilename)+8);
    sprintf(tfilename,"./dump.txt");
    fprintf(stderr, "%s -> %s\n", cfilename, tfilename);
    write_whole_file(tfilename, result, strlen(result));
  }
#endif
  
  for (int i = 0; i < arrlen(paths); i++) free(paths[i]);
  arrfree(paths);

  free(cfilename);
  free(url);
  GC_DISABLE();
  TEXT(R,result);
  GC_ENABLE();
  ncm_free_result(result);
RETURNS(R)


BUILTIN2("fs_copy",fs_copy,C_TEXT,src,C_TEXT,dst)
  char *csrc = strdup(text_to_cstring(src));
  R = FXN(fs_copy(csrc, text_to_cstring(dst)));
  free(csrc);
RETURNS(R)

#ifdef WINDOWS
BUILTIN2("fs_chmod",fs_chmod,C_INT,mode,C_TEXT,filename)
  //char *cname = text_to_cstring(filename);
RETURNS(No)
#else
#include <sys/types.h>
#include <sys/stat.h>

BUILTIN2("fs_chmod",fs_chmod,C_INT,mode,C_TEXT,filename)
  char *cname = text_to_cstring(filename);
  int imode = (int)UNFXN(mode);
  int pu = (imode/100)%10;
  int pg = (imode/10)%10;
  int po = imode%10;
  imode = pu*8*8 + pg*8 + po;
  chmod(cname, imode);
RETURNS(No)
#endif

BUILTIN1("file_time_",file_time_,C_TEXT,filename_text)
  char *filename = text_to_cstring(filename_text);
  R = FXN(fs_time(filename));
RETURNS(R)

BUILTIN1("file_exists_",file_exists_,C_TEXT,filename_text)
RETURNS(FXN(fs_exists(text_to_cstring(filename_text))))

BUILTIN1("folder",text_folder,C_ANY,filename_text)
RETURNS(FXN(fs_is_folder(text_to_cstring(filename_text))))

BUILTIN1("file",text_file,C_ANY,filename_text)
RETURNS(FXN(fs_is_file(text_to_cstring(filename_text))))

BUILTIN_VARARGS("text.items",text_items)
  void *filename_text = getArg(0);
  //int list_hidden = (LIST_SIZE(E) == 2); //list hidden files?

  char *filename = text_to_cstring(filename_text);
  if (!fs_is_folder(filename)) return No;

  GC_DISABLE();
  int64_t size;
  char *data = fs_get(&size, filename);

  if (!data) {
    GC_ENABLE();
    return No;
  }
  int n = 0;
  for (int i = 0; i < size; i++) {
    n++;
    while (data[i] != '\n') i++;
    data[i] = 0;
  }
  LIST(R, n);
  int j = 0;
  for (int i = 0; i < size; i++) {
    dyn r;
    TEXT(r, data+i);
    LGET(R,j) = r;
    j++;
    while (data[i]) i++;
  }
  free(data);
  GC_ENABLE();
RETURNS(R)

BUILTIN0("get_work_folder",get_work_folder)
  char cwd[1024];
  if (getcwd(cwd, 1024)) {
    char *p;
    for (p = cwd; *p; p++) if (*p == '\\') *p = '/';
    GC_DISABLE();
    TEXT(R, cwd);
    GC_ENABLE();
  } else {
    R = No;
  }
RETURNS(R)

BUILTIN1("rt_get",rt_get,C_TEXT,name_text)
  char *name = text_to_cstring(name_text);
  void *one = FXN(1);
  R = 0;

  if (!strcmp(name, "version")) R = one;

#ifndef WINDOWS
  if (!strcmp(name, "unix")) R = one;
#endif

#ifdef WINDOWS
  if (!strcmp(name, "windows")) R = one;
#endif

#ifdef __APPLE__
  if (!strcmp(name, "osx")) R = one;
#endif

RETURNS(R)

BUILTIN1("mkpath_",mkpath_,C_TEXT,filename_text)
  mkpath(text_to_cstring(filename_text),NG_SKIPFN);
RETURNS(filename_text)

BUILTIN1("sh",sh,C_TEXT,command_text)
  char *command = text_to_cstring(command_text);
  char *contents = exec_command(command);
  if (contents) {
    GC_DISABLE();
    TEXT(R, contents);
    GC_ENABLE();
    free(contents);
  } else {
    R = No;
  }
RETURNS(R)

BUILTIN0("time",time)
RETURNS(FXN((int64_t)time(0)))

//BUILTIN0("clock",clock)
//  LDFLT(R, (float)clock()/(float)CLOCKS_PER_SEC);
//RETURNS(R)
BUILTIN0("clock",clock)
  /* Symta texts are 32-bit floats; storing a full Unix epoch
   * (~1.78e9 in 2026) into a float drops all subsecond precision
   * because the mantissa is only 24 bits. The game's view.update
   * runs a `till Time < NextUpdate: ... NextUpdate += 1/ups` catch-
   * up loop that depends on `clock()` advancing within a frame;
   * if every call returns the same float, the loop runs forever
   * and the render thread hangs at 100% CPU. Subtracting a fixed
   * offset captured at first call keeps the value small enough to
   * survive single-precision rounding (and matches Win32's
   * QueryPerformanceCounter-style "seconds since program start"
   * semantics that this code grew up against). */
  static int initialized = 0;
  static int64_t base_sec = 0;
  struct timespec time;
  cmt_clock_gettime(CLOCK_REALTIME,&time);
  if (!initialized) { base_sec = time.tv_sec; initialized = 1; }
  float dSeconds = (float)(time.tv_sec - base_sec);
  float dNanoSeconds = (float)time.tv_nsec/1000000000.0f;
  LDFLT(R, dSeconds+dNanoSeconds);
RETURNS(R)


BUILTIN1("sleep",sleep,C_FLOAT,secs)
  float s;
  struct timespec time;
  int res;

  STFLT(s,secs);
  memset(&time, 0, sizeof(struct timespec));
  time.tv_sec = s;
  time.tv_nsec = (s - (uint64_t)s)*1000000000L;

  float dSeconds = time.tv_sec;
  float dNanoSeconds = (float)time.tv_nsec/1000000000L;
  LDFLT(R, dSeconds+dNanoSeconds);
  do {
    res = nanosleep(&time, &time);
  } while (res && errno == EINTR);
RETURNS(0)

BUILTIN0("main_args", main_args)
RETURNS(main_args)

BUILTIN0("main_path", main_path)
  GC_DISABLE();
  TEXT(R, main_path);
  GC_ENABLE();
RETURNS(R)


static char *get_line() {
  char *line = malloc(100), * linep = line;
  size_t lenmax = 100, len = lenmax;
  int c;

  if(line == NULL) return NULL;

  for(;;) {
    c = fgetc(stdin);
    if(c == EOF) break;

    if(--len == 0) {
      len = lenmax;
      char *linen = realloc(linep, lenmax *= 2);
      
      if(linen == NULL) {
        free(linep);
        return NULL;
      }
      line = linen + (line - linep);
      linep = linen;
    }
    
    if((*line++ = c) == '\n') break;
  }
  line[-1] = '\0';
  return linep;
}

BUILTIN0("get_line", get_line)
  char *line = get_line();
  GC_DISABLE();
  TEXT(R,line);
  GC_ENABLE();
  free(line);
RETURNS(R)


typedef struct {
  char *name;
  void *handle;
} ffi_lib_t;

#define FFI_MAX_LIBS 1024
static ffi_lib_t ffi_libs[FFI_MAX_LIBS];
static int ffi_libs_used;


static void *ffi_loopkup(char *name) {
  int i;
  for (i = 0; i < ffi_libs_used; i++) {
    if (!strcmp(name, ffi_libs[i].name)) {
      return ffi_libs[i].handle;
    }
  }
  return 0;
}

#include "effi.h"

static void *ffi_load(char *lib_name, char *sym_name);

static effi_t *gfapi;

static effi_t *ffi_get_api() {
  if (gfapi) return gfapi;

  gfapi = malloc(sizeof(effi_t));

  gfapi->fatal = fatal;

  gfapi->ffi_load = ffi_load;

  gfapi->fs_resolve = fs_resolve;
  gfapi->fs_is_file = fs_is_file;
  gfapi->fs_is_folder = fs_is_folder;
  gfapi->fs_time = fs_time;
  gfapi->fs_exists = fs_exists;
  gfapi->fs_get = fs_get;
  gfapi->fs_copy = fs_copy;
  return gfapi;
}

// Stash for the most recent dlopen failure, so a later failed
// dlsym can blame the right thing (missing dependency vs. unknown
// symbol). dl_err_buf survives between calls.
static char dl_err_buf[1024];
static char dl_err_path[512];

void *ffi_dlopen(char *lib_name) {
  char *lib_path = cat(main_path, "ffi/", lib_name, ".ffi");
  void *lib = dlopen(lib_path, RTLD_LAZY);
  if (!lib) {
    // Snapshot the error string and the path that failed.
    char *err = dlerror();
    snprintf(dl_err_path, sizeof(dl_err_path), "%s", lib_path);
    snprintf(dl_err_buf, sizeof(dl_err_buf), "%s",
             err ? err : "(unknown dlopen error)");
  }
  free(lib_path);
  if (!lib) return 0;
  ffi_libs[ffi_libs_used].name = strdup(lib_name);
  ffi_libs[ffi_libs_used].handle = lib;
  ++ffi_libs_used;

  void *(*synit)(effi_t *) = dlsym(lib, "synit");
  if (synit) {
    synit(ffi_get_api());
  }

  return lib;
}

static void *ffi_load(char *lib_name, char *sym_name) {
  void *lib = ffi_loopkup(lib_name);
  if (!lib) lib = ffi_dlopen(lib_name);
  if (!lib) return 0;
  return dlsym(lib, sym_name);
}

BUILTIN2("ffi_load",ffi_load,C_TEXT,lib_name_text,C_TEXT,sym_name_text)
  char *lib_name = strdup(text_to_cstring(lib_name_text));
  char *sym_name = strdup(text_to_cstring(sym_name_text));
  void *pfn = ffi_load(lib_name, sym_name);
  if (!pfn) {
    // Three failure modes to disambiguate; all return No so that
    // try-each-candidate patterns
    //   for Lib [\msvcrt \c \System]: F ffi_load Lib Sym; when F: ret F
    // can iterate.  Previously every failure went through fatal()
    // which CRASH-es (for gdb stack traces) -- safe enough for
    // production scripts but no good for libc_resolve-style fallbacks.
    //
    //   1. .ffi file missing -- silent return of No (common when
    //      walking candidate library names).
    //   2. .ffi file present but won't load (transitive DLL
    //      dependency missing) -- one-line stderr diagnostic + No.
    //   3. library loaded but symbol not found -- one-line stderr
    //      diagnostic + No.
    if (!ffi_loopkup(lib_name)) {
      char *path = cat(main_path, "ffi/", lib_name, ".ffi");
      int exists = fs_is_file(path);
      if (exists) {
        fprintf(stderr,
                "ffi_load: cannot open `ffi/%s.ffi`\n"
                "  full path: %s\n"
                "  reason:    %s\n"
                "  Most often this means a transitive DLL dependency is\n"
                "  missing -- e.g. the SDL2 runtime DLLs alongside the .exe.\n",
                lib_name, dl_err_path, dl_err_buf);
      }
      free(path);
    } else {
      char *err = dlerror();
      fprintf(stderr,
              "ffi_load: symbol `%s` not found in `ffi/%s.ffi`%s%s\n",
              sym_name, lib_name,
              err ? "\n  dlsym: " : "",
              err ? err : "");
    }
    R = No;
  } else if (NFI_BADPTR(pfn)) { // just to be safe -- shouldn't happen
    fatal("dlsym symbol `/%s` has unexpected address = %p\n",
          lib_name, sym_name, pfn);
  } else {
    R = NFI_ENCPTR(pfn);
  }

  free(lib_name);
  free(sym_name);
RETURNS(R)

//NOTE: malloc is expected to alloc on 8-byte boudary
BUILTIN1("ffi_alloc",ffi_alloc,C_INT,size)
RETURNS((dyn)malloc(UNFXN(size)))

BUILTIN1("ffi_free",ffi_free,C_ANY,ptr)
  free((void*)ptr);
RETURNS(No)

BUILTIN3("ffi_memset",ffi_memset,C_ANY,ptr,C_INT,value,C_INT,size)
  memset((void*)ptr, UNFXN(value), UNFXN(size));
RETURNS(No)

/* ffi_memcmp(p,q,size) -> int. Returns 0 when the two buffers are
 * byte-identical, non-zero otherwise (the signed direction is
 * memcmp's: <0 if `p` < `q` lexicographically, >0 otherwise). Faster
 * than walking individual pixels through gfx.get when you have a
 * pixel buffer and you only need an equality test (e.g. PNG-pixel
 * golden comparison). */
BUILTIN3("ffi_memcmp",ffi_memcmp,C_ANY,ptr_a,C_ANY,ptr_b,C_INT,size)
  R = FXN(memcmp((void*)ptr_a, (void*)ptr_b, UNFXN(size)));
RETURNS(R)

/* OP-4: meta.__ direct-dispatch fast path.
 *
 * The reader (runtime/reader.c `make_meta_wrapper`) wraps every
 * parse-stripped list in a `type meta` instance carrying the
 * source-position info.  The old Symta-side definition was:
 *
 *   type meta.~ O M: object_!O meta_!M
 *   _.meta_ = No
 *   meta.__ Method Args =
 *   | Args.0 = $object_
 *   | Args.apply_method(Method)
 *
 * Forwarding cost 3 MCALLs per call: entry to `__`, body's
 * MCALL into `apply_method`, then the actual MCALL on the
 * unwrapped receiver.  The compiler + macroexpander pay this on
 * every method access on every meta-wrapped AST node -- which is
 * most nodes -- so it dominates cold-compile profiles.
 *
 * The C builtin below collapses unwrap + re-dispatch into one:
 *
 *   api.args[0] is the meta wrapper (the receiver); slot 0 of
 *   the wrapper is `object_`.  Overwrite api.args[0] with the
 *   unwrapped object, get_method(api.method, unwrapped), CALL.
 *
 * Calling convention -- per MCACHE_CALL's sink path (sbc.c) --
 * is:
 *   getArg(0) = receiver = the meta wrapper.
 *   api.args  = the original call's args list (length >= 1).
 *   api.method = the missed method id (raw int, not FXN-wrapped).
 *
 * The Symta-side `meta.__` defn was removed from src/core_.s.
 * Installation happens lazily from reader.c::find_meta_tag (the
 * first parse_strip after `type meta` was registered) because
 * the meta type tag doesn't exist at init_builtins time.
 * add_method's redefinition guard whitelists `m_underscore` so
 * pre-OP-4 bootstrap SBCs that still carry the Symta-side defn
 * are silently overridden by this C handler.
 */
BUILTIN_VARARGS("meta.`__`", meta_apply)
  dyn me = getArg(0);
  dyn unwrapped = LGET(me, 0); /* meta.object_ */
  lsetm(api.args, 0, unwrapped);
  dyn fn = get_method(api.method, unwrapped);
  CALL(R, fn);
RETURNS(R)

/* One-shot install of meta.__ on the `meta` type tag.  Called
 * by reader.c::find_meta_tag the first time the tag is located.
 * Idempotent: subsequent calls no-op. */
static int meta_dispatch_installed_g = 0;
void install_meta_dispatch(int tag) {
  if (meta_dispatch_installed_g) return;
  GC_DISABLE();
  setup_b_meta_apply();
  void *met;
  BUILTIN_CLOSURE(met, ((fn_meta_t*)meta_b_meta_apply)->hook);
  add_method(tag, api.m_underscore, met);
  meta_dispatch_installed_g = 1;
  GC_ENABLE();
}

BUILTIN_VARARGS("__",sink)
  void *o = getArg(0);
  dyn name = get_method_name(api.method);
  int tag = O_TAG(o);
  char *a = tag < arrlen(types)
            ? fmt("%s has no method ", types[tag].name)
            : fmt("Bad tag %d, for method call ", tag);
  char *b = fmt("%s\n", print_object(name));
  /* Zero-padded 16-digit hex for cross-platform output stability
   * (glibc prints `(nil)` and `0x...` for %p; Win CRT prints
   * `00000000...`). Test goldens already use the zero-padded form. */
  char *c = fmt("object=%016llx (tag=%ld)\n",
                (unsigned long long)(uintptr_t)o, (long)O_TAG(o));
  char *d = fmt("%s%s%s",a,b,c);
  arrfree(a);
  arrfree(b);
  arrfree(c);
  rterr_(d);
RETURNS(0)

dyn list_builtins();

BUILTIN0("builtins_", builtins_)
RETURNS(shget(lib_expts, "rt_"))

static struct {
  void *name;
  void *fun;
  void *setup;
} builtins[] = {
#define B(name) {#name, meta_b_##name, setup_b_##name},
  B(inspect)
  B(halt)
  B(typename)
  B(parents_of_)
  B(methods_)
  B(dbg)
  B(say_)
  B(rtstat)
  B(alloc_stats_)
  B(gc)
  B(gc_set_gen0_pages)
  B(gc_gen0_used)
  B(gc_gen0_size)
  B(print_stack_trace_)
  B(stack_trace)
  B(get_file_)
  B(set_file_)
  B(raw_file_)
  B(get_text_file_)
  B(set_text_file_)
  B(sif_to_sbc_)
  B(sif_eval_)
  B(file_exists_)
  B(file_time_)
  B(fs_copy)
  B(fs_chmod)
  B(get_work_folder)
  B(rt_get)
  B(mkpath_)
  B(load_sbc)
  B(add_sbc_folder)
  B(register_library_folder)
  B(sh)
  B(time)
  B(clock)
  B(sleep)
  B(main_args)
  B(main_path)
  B(get_line)
  B(ffi_load)
  B(ffi_alloc)
  B(ffi_free)
  B(ffi_memset)
  B(ffi_memcmp)
  B(ncm_process_)
  B(parse_module_hdr_)
  B(get_deh)
  B(set_deh)
  B(rterr_)
  B(builtins_)
  B(set_finalizer)
  B(hmap_)
  B(sbc_metadata_)
  B(tok_)
  B(add_bars_c_)
  B(parse_strip_c_)
  B(parse_tokens_c_)
  B(wh_set_)
  B(wh_get_)
  B(wh_has_)
  B(wh_del_)
  B(wh_clear_)
  B(wh_n_)
  B(help_section_lookup_)
  B(help_banner_)
  B(help_lookup_)
  B(get_meta_)
  B(set_meta_)
  B(intern_)
  B(gid_)
  B(ref_)
  B(imm_)
  B(gid_get_)
  B(gid_set_)
  B(gid_refs_)
  B(newId_)
  B(newIds_)
  B(topId_)
  B(pushId_)
  B(popId_)
  B(set_dbgv_)
  B(cls_get_stub_)
  B(cls_set_stub_)
  B(stub_)
  B(p3u10_)
  B(u3u10_)
  B(p3u16_)
  B(u3u16_)
#undef B
  {0, 0, 0}
};

fn_meta_t meta_0[1];

static void setup_0() {}

#define ADD_MET(tid,m_cfun) do {\
  setup_##m_cfun(); \
  BUILTIN_CLOSURE(met, ((fn_meta_t*)meta_##m_cfun)->hook); \
  add_method(tid, mid, met); \
  } while(0)

#define METHOD_FN(name, m_int, m_float, m_fn, m_list, m_fixtext, m_text, m_view, m_cons, m_no) {\
  int mid = resolve_method(name); \
  void *met; \
  if (m_int) ADD_MET(T_INT,m_int); \
  if (m_float) ADD_MET(T_FLOAT,m_float); \
  if (m_fixtext) ADD_MET(T_FIXTEXT,m_fixtext); \
  if (m_fn) ADD_MET(T_CLOSURE,m_fn); \
  if (m_list) ADD_MET(T_LIST,m_list); \
  if (m_text) ADD_MET(T_TEXT,m_text); \
  if (m_view) ADD_MET(T_VIEW,m_view); \
  if (m_cons) ADD_MET(T_CONS,m_cons); \
  if (m_no) ADD_MET(T_NO,m_no); \
}

#define METHOD_FN1(name, type, fn) {\
  int mid = resolve_method(name); \
  void *met; \
  ADD_MET(type,fn); \
  if (type == T_TEXT) ADD_MET(T_FIXTEXT,fn); \
}

void init_builtin_methods() {
  METHOD_FN("neg", b_int_neg, b_float_neg, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("+", b_int_add, b_float_add, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("-", b_int_sub, b_float_sub, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("*", b_int_mul, b_float_mul, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("/", b_int_div, b_float_div, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("%", b_int_rem, b_float_rem, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("^^", b_int_pow, b_float_pow, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("><", b_int_eq, b_float_eq, b_fn_eq, 0, b_fixtext_eq, b_text_eq, 0, 0, b_no_eq);
  METHOD_FN("<>", b_int_ne, b_float_ne, b_fn_ne, 0, b_fixtext_ne, b_text_ne, 0, 0, b_no_ne);
  METHOD_FN("<", b_int_lt, b_float_lt, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN(">", b_int_gt, b_float_gt, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("<<", b_int_lte, b_float_lte, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN(">>", b_int_gte, b_float_gte, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("-+-", b_int_ior, 0, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("-^-", b_int_xor, 0, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("-*-", b_int_mask, 0, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("-<-", b_int_shl, 0, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("->-", b_int_shr, 0, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("head", 0, 0, 0, b_list_head, 0, 0, b_view_head, b_cons_head, 0);
  METHOD_FN("tail", 0, 0, 0, b_list_tail, 0, 0, b_view_tail, b_cons_tail, 0);
  METHOD_FN("take", 0, 0, 0, b_list_take, 0, 0, b_view_take, 0, 0);
  METHOD_FN("drop", 0, 0, 0, b_list_drop, 0, 0, b_view_drop, 0, 0);
  METHOD_FN("pre", 0, 0, 0, b_list_pre, 0, 0, b_view_pre, b_cons_pre, 0);
  METHOD_FN("end", 0, 0, 0, b_list_end, 0, 0, b_view_end, b_cons_end, 0);
  METHOD_FN("n", 0, 0, 0, b_list_n, b_fixtext_n, b_text_n, b_view_n, 0, 0);
  METHOD_FN(".", 0, 0, 0, b_list_get, b_fixtext_get, b_text_get, b_view_get, 0, 0);
  METHOD_FN("=", 0, 0, 0, b_list_set, 0, 0, b_view_set, 0, 0);
  METHOD_FN("clear", 0, 0, 0, b_list_clear, 0, 0, 0, 0, 0);
  METHOD_FN("hash", b_int_hash, 0, 0, 0, b_fixtext_hash, b_text_hash, 0, 0, b_no_hash);
  METHOD_FN("code", 0, 0, 0, 0, b_fixtext_code, 0, 0, 0, 0);
  METHOD_FN("char", b_int_char, 0, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("text", 0, 0, 0, b_list_text, 0, 0, 0, 0, 0);
  METHOD_FN("qsort_", 0, 0, 0, b_list_qsort, 0, 0, 0, 0, 0);
  METHOD_FN("apply", 0, 0, 0, b_list_apply, 0, 0, 0, 0, 0);
  METHOD_FN("apply_method", 0, 0, 0, b_list_apply_method, 0, 0, 0, 0, 0);
  METHOD_FN("nargs", 0, 0, b_fn_nargs, 0, 0, 0, 0, 0, 0);
  METHOD_FN("as_text", 0, b_float_as_text, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("float", b_int_float, b_float_float, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("int", b_int_int, b_float_int, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("flt16", b_int_flt16, 0, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("flt32", b_int_flt32, 0, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("int16", 0, b_float_int16, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("int32", 0, b_float_int32, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("sqrt", b_int_sqrt, b_float_sqrt, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("log", b_int_log, b_float_log, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("sin", 0, b_float_sin, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("asin", 0, b_float_asin, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("cos", 0, b_float_cos, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("acos", 0, b_float_acos, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("tan", 0, b_float_tan, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("atan", 0, b_float_atan, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("atan2", 0, b_float_atan2, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("floor", 0, b_float_floor, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("ceil", 0, b_float_ceil, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("round", 0, b_float_round, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("exponent", 0, b_float_exponent, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("mantissa", 0, b_float_mantissa, 0, 0, 0, 0, 0, 0, 0);
  METHOD_FN("bytes", b_int_bytes, 0, 0, 0, 0, 0, 0, 0, 0);

  METHOD_FN1("get_bits", T_INT, b_int_get_bits);
  METHOD_FN1("set_bits", T_INT, b_int_set_bits);

  METHOD_FN1("utf8", T_TEXT, b_text_utf8);
  METHOD_FN1("tokenize", T_TEXT, b_text_tokenize);
  METHOD_FN1("file", T_TEXT, b_text_file);
  METHOD_FN1("folder", T_TEXT, b_text_folder);
  METHOD_FN1("items", T_TEXT, b_text_items);

  METHOD_FN1("flt", T_TEXT, b_text_flt);


  METHOD_FN1("n", T_BYTES, b_bytes_n);
  METHOD_FN1(".", T_BYTES, b_bytes_get);
  METHOD_FN1("=", T_BYTES, b_bytes_set);
  METHOD_FN1("clear", T_BYTES, b_bytes_clear);
  METHOD_FN1("utf8", T_BYTES, b_bytes_utf8);

  METHOD_FN1(".", T_TBL, b_tbl_get);
  METHOD_FN1("=", T_TBL, b_tbl_set);
  METHOD_FN1("l", T_TBL, b_tbl_l);
  METHOD_FN1("ks", T_TBL, b_tbl_ks);
  METHOD_FN1("del", T_TBL, b_tbl_del);
  METHOD_FN1("is_table", T_TBL, b_tbl_is_table);
  METHOD_FN1("n", T_TBL, b_tbl_n);
  METHOD_FN1("has", T_TBL, b_tbl_has);
  METHOD_FN1("got", T_TBL, b_tbl_got);
  METHOD_FN1("clear", T_TBL, b_tbl_clear);
  METHOD_FN1("setNo", T_TBL, b_tbl_set_no)
  METHOD_FN1("swap", T_TBL, b_tbl_swap);

  METHOD_FN1("f", T_LIST, b_list_f);

  METHOD_FN1(".!", T_LIST, b_list_uget);
  METHOD_FN1("=!", T_LIST, b_list_uset);

  METHOD_FN1("type", T_TOK, b_tok_type);
  METHOD_FN1("value", T_TOK, b_tok_value);
  METHOD_FN1("pchar", T_TOK, b_tok_pchar);
  METHOD_FN1("row", T_TOK, b_tok_row);
  METHOD_FN1("col", T_TOK, b_tok_col);
  METHOD_FN1("orig", T_TOK, b_tok_orig);
  METHOD_FN1("parsed", T_TOK, b_tok_parsed);

  METHOD_FN1("=type", T_TOK, b_tok_stype);
  METHOD_FN1("=value", T_TOK, b_tok_svalue);
  METHOD_FN1("=pchar", T_TOK, b_tok_spchar);
  METHOD_FN1("=row", T_TOK, b_tok_srow);
  METHOD_FN1("=col", T_TOK, b_tok_scol);
  METHOD_FN1("=orig", T_TOK, b_tok_sorig);
  METHOD_FN1("=parsed", T_TOK, b_tok_sparsed);

  /* RT-9: register the C-side fn.() handler for T_CLOSURE.
   * Replaces the Symta-side `fn.\`()\` @As = As.apply(Me)` whose
   * @As collection was the top emitter of size-1 LIST allocs. */
  METHOD_FN1("()", T_CLOSURE, b_fn_call);

  /* RT-9: C-side list.l for T_VIEW (replaces Symta-side
   * `list.l = ... Ys dup N | times I N: Ys.I = pop Me | Ys`).
   * T_CONS keeps the Symta-side because cons chains can be
   * IMPROPER (tail is a non-cons value that needs flattening),
   * and the dispatch-based pop in Symta handles that correctly. */
  METHOD_FN1("l", T_VIEW, b_view_l);


  m_add = resolve_method("+");
  m_sub = resolve_method("-");
  m_mul = resolve_method("*");
  m_div = resolve_method("/");
  m_rem = resolve_method("%");
  m_eq = resolve_method("><");
  m_ne = resolve_method("<>");
  m_lt = resolve_method("<");
  m_gt = resolve_method(">");
  m_lte = resolve_method("<<");
  m_gte = resolve_method(">>");
  m_and = resolve_method("-*-");
  m_xor = resolve_method("-^-");
  m_ior = resolve_method("-+-");
  m_shl = resolve_method("-<-");
  m_shr = resolve_method("->-");
  m_neg = resolve_method("neg");
  m_inc = resolve_method("++");
  m_dec = resolve_method("--");
  m_get = resolve_method(".");
  m_set = resolve_method("=");
  m_abs = resolve_method("abs");
}

int m_add;
int m_sub;
int m_mul;
int m_div;
int m_rem;
int m_eq;
int m_ne;
int m_lt;
int m_gt;
int m_lte;
int m_gte;
int m_and;
int m_xor;
int m_ior;
int m_shl;
int m_shr;
int m_neg;
int m_inc;
int m_dec;
int m_get;
int m_set;
int m_abs;

void init_root_sink() {
  //init the no.sink method (the default sink of all types)
  setup_b_sink();
  dyn sink_fn;
  BUILTIN_CLOSURE(sink_fn, meta_b_sink->hook);
  /* RT-6b: `sink` is now the dyn fn itself, not a node pointer.
   * Set the global BEFORE add_method_r so the caller (`intern()`)
   * picks up the live value when it writes `t->sink_fn = sink`. */
  sink = sink_fn;
  add_method_r(ADD_CORE_SINK, T_INT, api.m_underscore, sink_fn);
}


static void init_args(int argc, char **argv) {
  int i;
  char *lib_path;
  int main_argc = argc-1;
  char **main_argv = argv+1;

  if (strchr(argv[0], '/') || strchr(argv[0], '\\')) {
    char *q = strdup(argv[0]);
    char *p = strchr(q, 0);
    while (*p != '/' && *p != '\\') --p;
    *++p = 0;
    main_path = strdup(q);
    free(q);
  } else {
    main_path = strdup(".");
  }

  /* realpath(NULL) malloc()s the resolved path. Glibc's fortified
   * realpath aborts if the caller-supplied buffer is smaller than
   * PATH_MAX (4096) — passing a 1024-byte stack buffer used to
   * trigger "*** buffer overflow detected ***" on first launch.
   * Letting libc allocate the right-sized buffer is the portable
   * fix; we transfer ownership to main_path via the resulting
   * pointer.
   *
   * realpath() also strips the trailing separator that init_args
   * left on the parsed argv[0]. Callers (fmt("%ssbc.saf", main_path),
   * cat(main_path, "sbc"), ...) assume main_path ends with one,
   * so we add it back. */
  {
    char *resolved = realpath(main_path, NULL);
    free(main_path);
    if (resolved) {
      size_t n = strlen(resolved);
      main_path = (char*)malloc(n + 2);
      memcpy(main_path, resolved, n);
      main_path[n]   = '/';
      main_path[n+1] = 0;
      free(resolved);
    } else {
      main_path = strdup("./");
    }
  }

  LIST(main_args, main_argc);
  for (i = 0; i < main_argc; i++) {
    void *a;
    TEXT(a, main_argv[i]);
    LSET(main_args,i,a);
  }
}

void init_builtin_functions() {
  int i;
  void *rt;

  for (i = 0; builtins[i].name; i++) {
    ((void (*)())builtins[i].setup)();
  }

  for (i = 0; builtins[i].name; i++) {
    void *t;
    TEXT(builtins[i].name, builtins[i].name);
    BUILTIN_CLOSURE(t, ((fn_meta_t*)builtins[i].fun)->hook);
    builtins[i].fun = t;
  }


  LIST(rt, i);
  for (i = 0; builtins[i].name; i++) {
    void *pair;
    LIST(pair, 2);
    LGET(pair,0) = builtins[i].name;
    LGET(pair,1) = builtins[i].fun;
    LGET(rt,i) = pair;
  }

  shput(lib_expts, "rt_", rt);
  //printf("rt_: %s\n", print_object(rt));
}

//FIXME: better idea would be allocating builtins into the oldest generation.
//       possibly alterating between the two heaps
void init_builtins(int argc, char **argv) {
  int i;
  void *tmp;

  /* RT-9 measurement: opt-in alloc-stats-on-exit via env var.
   * Registered here (before any GC_ENABLE) so even early-exit
   * paths get the dump. */
  if (getenv("SYMTA_ALLOC_STATS")) atexit(alloc_stats_atexit);

  GC_DISABLE();

  api.alloc(T_CLOSURE,0);

  LIST(Empty, 0); //Can't do `MKIMM(T_LIST,0)`, cuz size is on heap
                  //And we also need uniform list type in C/C++ runtime code

  init_builtin_functions();

  /* Pre-built single-byte fixtexts, indexed by byte value 0..255.
   * Used by `text.[i]` on a bigtext to return the i-th byte as a
   * single-char fixtext without allocating.
   *
   * Historically the loop ran 0..127 only and bytes 128..255 were
   * aliased to single_chars[0] (empty), which silently truncated
   * any text-indexing operation that crossed a high-bit byte --
   * `"abcädef".[3]` returned empty, `"abcädef".l` had n=3, and so
   * on.  Symta texts are byte-strings (not codepoints), so every
   * byte value 0..255 needs a real entry.  Fixes FFI-4 (UTF-8 to
   * char* marshalling) and a class of "string truncates at the
   * first non-ASCII byte" bugs all over the standard library. */
  for (i = 0; i < MAX_SINGLE_CHARS; i++) {
    char t[2];
    t[0] = (char)i;
    t[1] = 0;
    TEXT(single_chars[i], t);
  }

  init_types();
  init_builtin_methods();
  init_subtypes();

  /* `_.><` / `_.<>` -- raw-dyn identity comparators on T_OBJECT.
   * `_.<<` / `_.>` / `_.>>` -- raw-dyn ordering comparators
   * (OP-3).  All registered AFTER init_subtypes so add_method
   * propagates them to every subtype that doesn't already have a
   * type-specific override (int / float have all six; list has
   * the four orderings Symta-side; text has equality C-side and
   * a Symta-side `<` that propagates via the order chain).
   * Bootstrap core_.sbc may still carry Symta-side defs of these
   * methods; add_method allows redefinition for the five specific
   * method ids so the latest registration wins. */
  {
    setup_b_any_eq();
    setup_b_any_ne();
    setup_b_any_lte();
    setup_b_any_gt();
    setup_b_any_gte();
    void *met;
    BUILTIN_CLOSURE(met, ((fn_meta_t*)meta_b_any_eq)->hook);
    add_method(T_OBJECT, api.m_equal, met);
    BUILTIN_CLOSURE(met, ((fn_meta_t*)meta_b_any_ne)->hook);
    add_method(T_OBJECT, api.m_ne_, met);
    BUILTIN_CLOSURE(met, ((fn_meta_t*)meta_b_any_lte)->hook);
    add_method(T_OBJECT, m_lte, met);
    BUILTIN_CLOSURE(met, ((fn_meta_t*)meta_b_any_gt)->hook);
    add_method(T_OBJECT, m_gt, met);
    BUILTIN_CLOSURE(met, ((fn_meta_t*)meta_b_any_gte)->hook);
    add_method(T_OBJECT, m_gte, met);
  }

  init_args(argc, argv);
  
  api.hgp->dirty = DRT_BUILTINS;
  
  GC_ENABLE();
}

