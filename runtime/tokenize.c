#include "common.h"
#include "ng.h"

#define EOS 1

static char *R_chars;
static int R_off;
static int R_len;
static int R_last;
static int R_row;
static int R_col;
static dyn R_origin;

//FIXME: character code 0x01 should be illegal
static int r_peek() {
  return R_off < R_len ? R_chars[R_off] : EOS;
}

static int r_next() {
  if (R_off >= R_len) return EOS;
  R_last = R_chars[R_off++];
  if (R_last != '\n') ++R_col;
  else {
    R_col = 0;
    ++R_row;
  }
  return R_last;
}

static dyn r_src() {
  dyn *r;
  LIST(r,3);
  LGET(r,0) = FXN(R_row);
  LGET(r,1) = FXN(R_col);
  LGET(r,2) = R_origin;
  return r;
}


static void r_cleanup() {
  GC_ENABLE();
  free(R_chars);
}

static void r_error_(char *msg) {
  char *em = fmt("%s:%d,%d: %s"
                ,print_object(R_origin), R_row, R_col, msg);
  arrfree(msg);
  r_cleanup();
  rterr_(em);
}

#define r_error(...) r_error_(fmt(__VA_ARGS__))

//prefix table in a form of a radix tree
//basically a technique to match multiple regexps in parallel
//TODO:
// * terminate earlier for population=1 branches
// * compress it with a bitmap
static void **ptbl;

static void add_lexeme2(int k, void **tbl, char *pat, dyn type) {
  if (!*pat) {
    tbl[0] = type;
    return;
  }

  char *spat = pat;
  int kleene = 0;
  char *cs, *cse;
again:;
  if (*pat == '\\') {
    cs = pat+1;
    cse = pat+2;
    pat += 2;
  } else if (*pat == '[') {
    cs = ++pat;
    while (*pat != ']') {
      if (!*pat) {
        fprintf(stderr, "add_lexeme: unclosed `[`\n");
        exit(-1);
      }
      pat++;
    }
    cse = pat++;
  } else {
    cs = pat;
    cse = pat+1;
    pat++;
  }

  if (*pat == '*') {
    kleene = '*';
    pat++;
  } else if (*pat == '+') { //one or more
    if (k) {
      kleene = '*';
      pat++;
    } else {
      kleene = '+'; //or we can just rewrite + with *
      pat = spat;
    }
  } 

  for (; cs < cse; cs++) {
    int c = *cs;
    void **next = tbl[c];
    if (!next) {
      if (kleene=='*') {
        next = tbl;
      } else {
        next = zmalloc(sizeof(void*)*128);
      }
      tbl[c] = next;
    }
    add_lexeme2(kleene=='+',next,pat,type);
  }
}

static void add_lexeme(char *pat, char *type) {
  dyn t;
  TEXT(t,type);
#if 1
  if (IS_BIGTEXT(t)) {
    rterr("add_lexeme(\"%s\"): big text token types are not allowed", type);
  }
#endif

  add_lexeme2(0, ptbl, pat, t);
  //exit(-1);
}


static struct { char *key; dyn value; } *spectoks;

static void add_spectok(char *pat, void *type) {
  dyn t;
  TEXT(t,type);
#if 1
  if (IS_BIGTEXT(t))
    rterr("add_spectok: big text token types are not allowed");
#endif
  shput(spectoks,pat,t);

  //exit(-1);
}


static void **fntbl = 0;

static void add_fn_lexeme(char *pat, void *fn) {
  int idx = arrlen(fntbl);
  dyn *t = FXN(idx);
  add_lexeme2(0, ptbl, pat, t);
  arrput(fntbl, fn);
}

#define TF_NORMAL_LIST  1
#define TF_BRACE_LIST   2
#define TF_CURLY_LIST   3
#define TF_TABLE_LIST   4

static dyn read_list(char *open, char *close);

static dyn read_normal_list(dyn t) {
  dyn ttype;
  TEXT(ttype, "()");
  LGET(t,0) = ttype;
  LGET(t,1) = read_list("(", ")");
  return t;
}

static dyn read_bracket_list(dyn t) {
  dyn ttype;
  TEXT(ttype, "[]");
  LGET(t,0) = ttype;
  LGET(t,1) = read_list("[", "]");
  return t;
}

static dyn read_curly_list(dyn t) {
  dyn ttype;
  TEXT(ttype, "{}");
  LGET(t,0) = ttype;
  LGET(t,1) = read_list("{", "}");
  return t;
}

static dyn read_table_list(dyn t) {
  dyn ttype;
  TEXT(ttype, "@{}");
  LGET(t,0) = ttype;
  LGET(t,1) = read_list("@{", "}");
  return t;
}

static dyn read_object_list(dyn t) {
  dyn ttype;
  TEXT(ttype, "${}");
  LGET(t,0) = ttype;
  LGET(t,1) = read_list("${", "}");
  return t;
}

static dyn read_string(int incut, int end);


static dyn read_normal_string(dyn t) {
  dyn type;
  TEXT(type, "text");
  LGET(t,0) = type;
  dyn esc;
  TEXT(esc, "\\");
  dyn l;
  LIST(l,2);
  LGET(l,0) = esc;
  LGET(l,1) = read_string(-1, '\'');
  LGET(t,0) = type;
  LGET(t,1) = l;
  return t;
}

static dyn read_spliced_string(dyn t) {
  dyn type;
  TEXT(type, "splice");
  LGET(t,0) = type;
  LGET(t,1) = read_string('[', '"');
  return t;
}

static dyn read_symbol_string(dyn t) {
  dyn type;
  TEXT(type, "symbol");
  LGET(t,0) = type;
  LGET(t,1) = read_string(0, '`');
  return t;
}

static dyn read_comment(dyn t);
static dyn read_multi_comment(dyn t);

static dyn handle_line(dyn t);

static dyn t_ws, t_unary;

static void init_tokenizer() {
  ptbl = zmalloc(sizeof(void*)*128);
  arrput(fntbl, 0); //index 0 is reserved

  char *Digit = "[0123456789]";
  char *HexDigit = "[0123456789ABCDEFabcdef]";
  char *BinDigit = "[01]";
  char *HeadChar = "[abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_?~]";
  char *TailChar = "[0123456789abcdefghijklmnopqrstuvwxyz"
                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ_?~]";

  char *ops = "-,/,%,^,.,->,|,;,:,=,=>,<=,-=,/=,%=,!,^^,..,"
              "--,-^-,-<-,->-,><,<>,<,>,<<,>>,&&,||,$,@,&,@@,"
              "),],},";
  for (char *p = ops; *p; p++) {
    char op[16];
    char *q = op;
    while (*p != ',') *q++ = *p++;
    *q = 0;
    add_lexeme(op, op);
  }
  add_lexeme(","     , ",");
  add_lexeme("\\+"   , "+");
  add_lexeme("\\+\\+", "++");
  add_lexeme("-\\+-" , "-+-");
  add_lexeme("-\\+" , "-+");
  add_lexeme("\\+="  , "+=");
  add_lexeme("\\*"   , "*");
  add_lexeme("\\*="  , "*=");
  add_lexeme("-\\*-" , "-*-");
  add_lexeme("-\\*" , "-*");
  add_lexeme("\\\\"  , "\\");

  add_lexeme("[ \n\x0d]+", "ws");
  add_lexeme(cat(HeadChar,TailChar,"*"), "symbol");
  add_lexeme(cat(Digit,TailChar,"*"), "int");
  
  TEXT(t_ws, "ws");
  TEXT(t_unary, "unary");

  add_fn_lexeme("(", read_normal_list);
  add_fn_lexeme("\\[", read_bracket_list);
  add_fn_lexeme("{", read_curly_list);
  add_fn_lexeme("@{", read_table_list);
  add_fn_lexeme("${", read_object_list);

  add_fn_lexeme("'", read_normal_string);
  add_fn_lexeme("\"", read_spliced_string);
  add_fn_lexeme("`", read_symbol_string);

  add_fn_lexeme("//", read_comment);
  add_fn_lexeme("/\\*", read_multi_comment);
  add_fn_lexeme("#line", handle_line);

  char *ss[] = {"if", "then", "else", "elif", "and", "or", 0};
  for (char **q = ss; *q; q++) add_spectok(*q, *q);
  add_spectok("No", "void");
}

#define MAX_TOK_LEN 1024
char tok[MAX_TOK_LEN];

dyn code2text(int c) {
  char cs[2];
  cs[0] = c;
  cs[1] = 0;
  dyn r;
  TEXT(r, cs);
  return r;
}

static dyn mktok(dyn type, dyn value, int pchar, int row, int col, dyn orig) {
  dyn *t = token(type, value, FXN(row), FXN(col), orig);
  LGET(t,2) = code2text(pchar);
  // gc_alloc doesn't zero-init -- explicitly clear `parsed` slot so
  // parse_strip's `if P then P.0` doesn't trip on garbage when the
  // token has no parsed cache.
  LGET(t,6) = 0;
  return t;
}

static dyn read_token(int left_spaced) {
again:;
  dyn orig = R_origin;
  int row = R_row;
  int col = R_col;
  int pchar = R_last;
  void **tbl = ptbl;
  int tl = 0;
  int c;

  while (1) {
    c = r_peek();
    void **ntbl = tbl[c];
    if (!ntbl) break;
    tok[tl++] = c;
    if (tl == MAX_TOK_LEN) r_error("max token length exceeded");
    tbl = ntbl;
    r_next();
    continue;
  }

  tok[tl] = 0;

  char *type = shget(spectoks,tok);
  if (!type) type = tbl[0];
  
  int b = tok[0];
  if ((b == '-' || b == '+' || b == '*' || b == '/' || b == '%' 
      || b == '<' || b == '>')) {
    int n = 1;
    if ((tok[n]==b && (b=='+' || b=='-')) ||
        ((tok[n]=='<' || tok[n]=='>') && (b=='<' || b=='>')))
          n++; //handle prefix `++` and `--`, as well as unary < and >
    if (!tok[n] && left_spaced && c != '\n' && c != ' ') {
      type = t_unary;
    }
  }

  if (!type) {
    if (c > 0x0D) {
      r_error("Unexpected %c (0x%X)", c, c);
    } else {
      if (c == EOS) return 0;
      r_error("Unexpected no-printable (0x%X)", c);
    }
  }

  if (!tl) return 0;
  if (type == t_ws) return read_token(1);

  // hack to handle -*p and -+p
  if (tok[0]=='-' && !tok[2] && (tok[1]=='*' || tok[1]=='+')) {
    R_last = tok[1];
    R_row = row;
    R_col = col+1;
    R_off = R_off - (strlen(tok) - 1);
    tok[1] = 0;
  }

  dyn tval;
  TEXT(tval, tok);
  dyn t = mktok(type, tval, pchar, row, col, orig);
  if (O_TAG(type) == T_INT) {
    int fidx = (int)UNFXN(type);
    return ((dyn (*)(dyn))fntbl[fidx])(t);
  }
  return t;
}

static int is_comment_char(int c) {return c != '\n' && c != EOS;}
static dyn read_comment(dyn t) {
  while (is_comment_char(r_next()));
  return read_token(0);
}

static dyn read_multi_comment(dyn t) {
  for (int o = 1; o > 0; ) {
    int a = r_next();
    int b = r_peek();
    if (b == EOS) {
      r_error("`/*`: missing `*/`");
    } else if (a == '*' && b == '/') {
      --o; r_next();
    } else if (a == '/' && b == '*') {
      ++o; r_next();
    }
  }
  return read_token(0);
}

//handle_line
static dyn handle_line(dyn t) {
  char *p = R_chars+R_off;
  dyn row = read_token(0);
  while (*p == ' ') p++;
  int irow = strtol(p,0,10);
  p = R_chars+R_off;
  while (*p == ' ') p++;
  dyn src = read_token(0);
  char *e = R_chars+R_off;
  if (p[0] == '"') p++;
  if (e[-1] == '"') e--;
  int l = e-p;
  char *csrc = malloc(l+1);
  memcpy(csrc, p, l);
  csrc[l] = 0;
  while (1) {
    int c = r_next();
    if (c == '\n' || c == EOS) break;
  }
  R_row = irow;
  R_col = 0;
  TEXT(R_origin, csrc);
  free(csrc);
  return read_token(0);
}

static dyn read_list(char *open, char *close) {
  dyn orig = R_origin;
  int row = R_row;
  int col = R_col;
  dyn t_close;
  TEXT(t_close, close);
  dyn *ts = 0;
  dyn r;

  while (1) {
    dyn t = read_token(0);
    if (!t) {
      rterr("%s:%d,%d: unclosed `%s`", print_object(orig),row,col, open);
    }
    if (LGET(t,0) == t_close) {
      int n = arrlen(ts);
      LIST(r,n);
      for (int i = 0; i < n; i++) {
        LGET(r,i) = ts[i];
      }
      arrfree(ts);
      break;
    }

    arrput(ts,t);
  }
  return r;
}

static dyn spliced_string_normalize(dyn *xs) {
  dyn etext;
  TEXT(etext, "");

  dyn *ys = 0;
  for (int i = 0; i < arrlen(xs); i++) {
    if (xs[i] != etext) arrput(ys, xs[i]);
  }
  dyn r;
  LIST(r, arrlen(ys));
  for (int i = 0; i < arrlen(ys); i++) {
    dyn y = ys[i];
    if (IS_TEXT(y)) {
      dyn type;
      TEXT(type, "symbol");
      y = mktok(type, y, -1, 0, 0, etext);
    }
    LGET(r,i) = y;
  }
  arrfree(ys);
  return r;
}

static dyn read_string(int incut, int end) {
  dyn *xs = 0;
  char *cs = 0;
  dyn *r;
  while (1) {
    int c = r_peek();
    if (c != incut) r_next();
    if (c == '\\') {
      c = r_next();
      if (c == 'n') arrput(cs,'\n');
      else if (c == 't') arrput(cs,'\t');
      else if (c == '\\') arrput(cs,'\\');
      else if (c == '[') arrput(cs,'[');
      else if (c == ']') arrput(cs,']');
      else if (c == '\'') arrput(cs,'\'');
      else if (c == '\"') arrput(cs,'\"');
      else if (c == '`') arrput(cs,'`');
      else if (c == incut) arrput(cs,c);
      else if (c == end) arrput(cs,c);
      else {
        r_error("Invalid escape code: \\%c", c);
      }
    } else if (c == end) {
      arrput(cs,0);
      TEXT(r, cs);
      arrfree(cs);
      if (end == '"') {
        arrput(xs,r);
        r = spliced_string_normalize(xs);
        arrfree(xs);
      }
      break;
    } else if (c == incut) {
      arrput(cs,0);
      TEXT(r, cs);
      arrfree(cs);
      cs = 0;
      arrput(xs,r);
      dyn m = read_token(0);
      TEXT(LGET(m,0) , "()");
      LGET(m,2) = code2text(' ');
      LGET(m,3) = 0;
      LGET(m,4) = 0;
      //fprintf(stderr, "%s\n", print_object(m));
      arrput(xs,m);
    } else if (c == EOS) {
      r_error("EOF in string\n"); //FIXME: use start of string coords
    } else {
      arrput(cs,c);
    }
  }
  return r;
}

/* DEBUG hook implemented in reader.c -- compiled-in but
 * inert unless SYMTA_SCAN_AST=1. */
extern void badtag_scan(dyn o, const char *label);

dyn tokenize(dyn orig, dyn o) {
  char *xs = strdup(text_to_cstring(o));

  GC_DISABLE();

  if (!ptbl) {
    init_tokenizer();
  }

  R_chars = xs;
  R_off = 0;
  R_last = ' ';
  // Row is 1-indexed (line 1 is the first line) but col stays 0-indexed
  // because reader.s's add_bars uses Col==0 as the "start of line"
  // marker for inserting top-level `|` separators.
  //
  // Project compiles always inject "#line Row+1" via lexical_macro_expand,
  // so this only mattered for the bare `-f file.s` path -- which used to
  // be off by one in error messages and stack traces.
  R_row = 1;
  R_col = 0;
  R_origin = orig;
  R_len = strlen(R_chars);


  dyn *ts = 0;
  dyn r;

  while (1) {
    dyn t = read_token(0);
    if (!t) {
      int n = arrlen(ts);
      LIST(r,n);
      for (int i = 0; i < n; i++) {
        LGET(r,i) = ts[i];
      }
      arrfree(ts);
      break;
    }
    arrput(ts,t);
  }
  
  r_cleanup();

  badtag_scan(r, "tokenize:out");
  return r;
}
