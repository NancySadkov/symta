#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dlfcn.h>
#include <stdarg.h>

#include <time.h>
#include <math.h>

#include <dirent.h>

#include "ctx.h"
#include "common.h"
#include "ng.h"
#include "sif.h"
#include "fs.h"



dyn dbgv;

// used for debugging
#define D fprintf(stderr, "%d:%s\n", __LINE__, __FILE__);


char *main_lib;
char *main_path;
void *main_args;

static char *version = "1.1";


dyn sink;  /* RT-6b: default sink fn (was method_node_t*) */

alloc_stats_t alloc_stats;  /* RT-9 measurement; see common.h */

lib_expts_t lib_expts;

static char **lib_folders;



api_t api_g;

static gc_heap_t gc_heap;

#define gc_gens (gc_heap.gc_gens)


static int print_object_error;
static int print_depth = 0;
#define MAX_PRINT_DEPTH 32


//FIXME: while making types[] into a global array of MAX_TYPES
//       improves the speed a litter, we want lookup to happen in place.
//       which requires types to be a pointer inside api_t anyway.
type_t *types;


collector_t *collectors;



void **method_names;


text_table_t *text_tables;
module_imp_t *module_imports;


static struct { char *key; int value; } *methlut;

int resolve_method(char *name) {
  void *text_name;

  intptr_t index = shget(methlut,name);
  if (index != -1) return index;

  index = arrlen(method_names);
  shput(methlut,strdup(name),index);

  GC_DISABLE();
  TEXT(text_name, name);
  GC_ENABLE();

  arrput(method_names,text_name);

  api.hgp->dirty |= DRT_METHOD_NAMES;

  return index;
}

dyn get_method_name(uint32_t method_id) {
  return method_names[method_id];
}

/* RT-6/RT-6b: probe-based dispatch with inline fn.
 * See method_slot_t in common.h. */

dyn get_method(int method_id, dyn object) {
  type_t *t = types + O_TAG(object);
  uint32_t mask = t->method_cap - 1;
  uint32_t i = (uint32_t)method_id & mask;
  method_slot_t *ms = t->methods;
  for (;;) {
    uint32_t slot_mid = ms[i].mid;
    if (!slot_mid) return t->sink_fn;
    if (slot_mid == (uint32_t)method_id) return ms[i].fn;
    i = (i + 1) & mask;
  }
}

/* RT-6b: same probe by raw tag (no dyn object handy).  Used by
 * MCACHE_CALL on miss to fill `(tid, mid, fn)` into the cache. */
dyn get_method_for_tag(int method_id, int tag) {
  type_t *t = types + tag;
  uint32_t mask = t->method_cap - 1;
  uint32_t i = (uint32_t)method_id & mask;
  method_slot_t *ms = t->methods;
  for (;;) {
    uint32_t slot_mid = ms[i].mid;
    if (!slot_mid) return t->sink_fn;
    if (slot_mid == (uint32_t)method_id) return ms[i].fn;
    i = (i + 1) & mask;
  }
}

/* Returns the slot pointer for `mid` in t's table, or NULL if absent. */
static method_slot_t *find_method_slot(type_t *t, int mid) {
  uint32_t mask = t->method_cap - 1;
  uint32_t i = (uint32_t)mid & mask;
  method_slot_t *ms = t->methods;
  for (;;) {
    uint32_t slot_mid = ms[i].mid;
    if (!slot_mid) return 0;
    if (slot_mid == (uint32_t)mid) return &ms[i];
    i = (i + 1) & mask;
  }
}

/* Find the slot to insert `mid` into.  Returns either the existing
 * matching slot or the first empty one found by linear probing.
 * Caller is responsible for resizing before calling so load < 1. */
static method_slot_t *find_insert_slot(type_t *t, int mid) {
  uint32_t mask = t->method_cap - 1;
  uint32_t i = (uint32_t)mid & mask;
  method_slot_t *ms = t->methods;
  for (;;) {
    uint32_t slot_mid = ms[i].mid;
    if (!slot_mid || slot_mid == (uint32_t)mid) return &ms[i];
    i = (i + 1) & mask;
  }
}

static void resize_method_table(type_t *t) {
  uint32_t old_cap = t->method_cap;
  method_slot_t *old_slots = t->methods;
  uint32_t new_cap = old_cap * 2;
  method_slot_t *new_slots = (method_slot_t*)calloc(new_cap,
                                                    sizeof(method_slot_t));
  uint32_t new_mask = new_cap - 1;
  for (uint32_t i = 0; i < old_cap; i++) {
    if (!old_slots[i].mid) continue;
    uint32_t j = (uint32_t)old_slots[i].mid & new_mask;
    while (new_slots[j].mid) j = (j + 1) & new_mask;
    new_slots[j] = old_slots[i];
  }
  free(old_slots);
  t->method_cap = new_cap;
  t->methods = new_slots;
}

//O_TAG(object)

static struct { char *key; int value; } *typelut;

//Basically the Lisp's intern, which we use for type tag symbols
intptr_t intern(char *name) {
  for (int i = 0; i < arrlen(types); i++)
    if (!strcmp(types[i].name, name)) return i;

  int index = shget(typelut,name);
  if (index != -1) return index;

  index = arrlen(types);
  type_t *t = arraddnptr(types,1);
  t->name = strdup(name);
  t->sname = 0;
  t->size = 0;

  t->id = index;
  t->super = END_TAG;
  t->subtypes = END_TAG;
  t->next = END_TAG;

  /* RT-6: start with a small open-addressed table.  Grows on
   * load >= 75 % via resize_method_table. */
  t->method_cap = INITIAL_METHOD_CAP;
  t->method_count = 0;
  t->methods = (method_slot_t*)calloc(INITIAL_METHOD_CAP,
                                      sizeof(method_slot_t));

  shput(typelut,t->name,index);
  arrput(collectors,gc_custom);

  if (!sink) init_root_sink();
  t->sink_fn = sink; //default sink method

  api.hgp->dirty |= DRT_TYPES;

  return index;
}

/* RT-6b: insert (mid, fn) into t's slot table.  Resizes first if
 * the load factor would exceed 75 %.  If a slot for `mid` already
 * exists, just overwrites its `fn` (used both for the bootstrap-
 * SBC-overrides-C-handler case and for the user-redefinition
 * whitelist path). */
static void insert_method(type_t *t, int mid, dyn fn) {
  if (t->method_count + 1 >= (t->method_cap * 3) / 4) {
    resize_method_table(t);
  }
  method_slot_t *s = find_insert_slot(t, mid);
  if (!s->mid) {
    s->mid = (uint32_t)mid;
    t->method_count++;
  }
  s->fn = fn;
  api.hgp->dirty |= DRT_TYPE_METHODS;
}

static void inherit_methods(type_t *parent, type_t *child) {
  uint32_t cap = parent->method_cap;
  method_slot_t *pms = parent->methods;
  for (uint32_t i = 0; i < cap; i++) {
    if (!pms[i].mid) continue;
    int mid = (int)pms[i].mid;
    /* Skip if the child already defines its own version of mid. */
    if (find_method_slot(child, mid)) continue;
    insert_method(child, mid, pms[i].fn);
  }
}

void add_subtype(int tag, int subtag) {
  type_t *parent = &types[tag];
  type_t *child = &types[subtag];
  if (tag == subtag)
    rterr("add_subtype: type `%s` inherits from itself", parent->name);
  child->super = tag;
  child->next = parent->subtypes;
  parent->subtypes = subtag;
  inherit_methods(&types[tag], &types[subtag]);
}

void add_method_r(int depth, int type_id
                 ,int method_id, void *handler) {
  type_t *s, *t = &types[type_id];

  if (depth == ADD_CORE_SINK) {
    insert_method(t, method_id, handler);
    return;
  }

  method_slot_t *n_slot = find_method_slot(t, method_id);
  dyn n_fn = n_slot ? n_slot->fn : 0;
  if (n_slot) {
    /* Walk the super chain to detect inherited-and-still-matches. */
    for (int si = t->super; si != END_TAG; si = s->super) {
      s = &types[si];
      method_slot_t *m_slot = find_method_slot(s, method_id);
      if (m_slot && m_slot->fn == n_fn) goto inherited;
      if (!s->super) break;
    }
    if (depth) return; //subtype already has this method
    /* `_.><` / `_.<>` / `_.<<` / `_.>` / `_.>>` (OP-1, OP-3) and
     * `__` (OP-4: meta.__ direct-dispatch sink) get registered
     * both via the C-side init (fast direct-dispatch handlers)
     * and via the bootstrap SBC's Symta-side definitions of the
     * same methods.  Allow the silent override so the latest
     * registration wins.  Any OTHER method redefinition is still
     * an error -- usually a real bug. */
    if (method_id == api.m_equal || method_id == api.m_ne_
     || method_id == m_lte || method_id == m_gt || method_id == m_gte
     || method_id == api.m_underscore) {
      goto replace;
    }
    rterr("add_method: redefining %s.%s"
         ,t->name, print_object(method_names[method_id]));
  inherited:;
  }

replace:
  {
    insert_method(t, method_id, handler);

    for (int si = t->subtypes; si != END_TAG; si = s->next) {
      s = &types[si];
      add_method_r(depth+1, s->id, method_id, handler);
    }

    if (method_id == api.m_underscore) {
      t->sink_fn = handler;
      api.hgp->dirty |= DRT_TYPES;
    }
  }
}


void add_method(int type_id, int method_id, void *handler) {
  add_method_r(0, type_id, method_id, handler);
}

void set_type_size_and_name(int tag, int size, void *name) {
  types[tag].sname = name;
  types[tag].size = size;
}

static dyn exec_module(char *path) {
  //FIXME: discern here beteween compiled and bytecode modules
  return sbc_exec_file(path);
}

//FIXME: resolve circular dependencies
//       instead of exports list we may return lazy list
void *load_sbc(char *name) {
  int i;
  char tmp[1024];
  void *exports;

  if (name[0] == '/' || name[1] == ':') {
    //if (!fs_is_file(name))
    {
      sprintf(tmp, "%s.sbc", name);
      name = tmp;
    }
  }
  if (name[0] != '/' && name[1] != ':' && strcmp(name,"rt_")) {
    for (i = 0; i < arrlen(lib_folders); i++) {
      //sprintf(tmp, "%s/%s", lib_folders[i], name);
      //if (file_exists(tmp)) break;
      sprintf(tmp, "%s/%s.sbc", lib_folders[i], name);
      if (fs_is_file(tmp)) break;
    }
    if (i == arrlen(lib_folders)) {
      fprintf(stderr, "load_module: couldnt locate `%s.sbc` in:\n", name);
      for (i = 0; i < arrlen(lib_folders); i++) {
        fprintf(stderr, "  %s\n", lib_folders[i]);
      }
      rterr("aborting");
    }
    name = tmp;
  }
  
  exports = shget(lib_expts, name);
  if (exports) return exports;


  //fprintf(stderr, "load_sbc begin: %s\n", name);

  name = strdup(name);
  exports = exec_module(name);

  //fprintf(stderr, "load_sbc end: %s\n", name);

  shput(lib_expts, name, exports);
  api.hgp->dirty |= DRT_LIB_EXPORTS;

  return exports;
}

void add_lib_folder(char *folder) {
  char *d = strdup(folder);
  int l = strlen(folder)-1;
  while (l >= 0 && (d[l] == '/' || d[l] == '\\')) d[l--] = 0;
  arrput(lib_folders, d);
}

static char *print_object_r(char *out, void *o);
char* print_object_f(void *object) {
  print_depth = 0;
  print_object_error = 0;
  arrfree(api.sbuf);
  api.sbuf = print_object_r(0, object);
  aput(api.sbuf, 0);
  return api.sbuf;
}

static char *print_object_r(char *r, void *o) {
  uint32_t i, size;
  int type = (int)O_TAG(o);
  int open_par = 1;

  if (print_depth >= MAX_PRINT_DEPTH) {
    if (!print_object_error) {
      fprintf(stderr, "MAX_PRINT_DEPTH reached: likely a recursive object\n");
      print_object_error = 1;
    }
    aputs(r,"<MAX_PRINT_DEPTH reached>");
    aput(r, 0);
    return r;
  }

  print_depth++;

print_tail:
  if (o == No) {
    r = afmt(r, "No");
  } else if (type == T_CLOSURE) {
    //FIXME: check metainfo to see if this object has associated print routine
    r = afmt(r, "#(clsr 0x%llx sz=%d)",  O_CODE(o), O_SIZE(o));
    //r = print_object_r(r, LGET(o,0));
  } else if (type == T_INT) {
    // FIXME: this relies on the fact that shift preserves sign
    r = afmt(r, "%lld", UNFXN(o));
  } else if (type == T_LIST) {
    size = LIST_SIZE(o);
    if (open_par) r = afmt(r, "(");
    for (i = 0; i < size; i++) {
      if (i || !open_par) r = afmt(r, " ");
      r = print_object_r(r, LGET(o,i));
    }
    r = afmt(r, ")");
  } else if (type == T_VIEW) {
    uint32_t start = VIEW_START(o);
    size = VIEW_SIZE(o);
    if (open_par) r = afmt(r, "(");
    for (i = 0; i < size; i++) {
      if (i || !open_par) r = afmt(r, " ");
      r = print_object_r(r, VIEW_LGET(o,start,i));
    }
    r = afmt(r, ")");
  } else if (type == T_CONS) {
    open_par = 0;
    r = afmt(r, "(");
    for (;;) {
      r = print_object_r(r, CAR(o));
      o = CDR(o);
      type = (int)O_TAG(o);
      if (type != T_CONS) goto print_tail;
      r = afmt(r, " ");
    }
  } else if (type == T_FIXTEXT) {
    aput(r, '`');
    char buf[32];
    decode_text(buf, o);
    aputs(r, buf);
    aput(r, '`');
  } else if (type == T_BYTES) {
    size = BYTES_SIZE(o);
    if (open_par) r = afmt(r, "(");
    for (i = 0; i < size; i++) {
      if (i || !open_par) r = afmt(r, " ");
      r = afmt(r, "%d", BYTES_DATA(o)[i]);
    }
    r = afmt(r, ")");
  } else if (type == T_FLOAT) {
    float f;
    STFLT(f,o);
    r = afmt(r, "%.8f", (double)f);
    while (r[alen(r)-1] == '0' && r[alen(r)-2] != '.') apop(r);
  } else if (type == T_TEXT) {
    aput(r, '`');
    size = (int)BIGTEXT_SIZE(o);
    char *p = (char*)BIGTEXT_DATA(o);
    for (i = 0; i < size; i++, p++) aput(r, *p);
    aput(r, '`');
  } else {
    if (type < arrlen(types)) {
      r = afmt(r, "#(%s %p)", types[type].name, o);
    } else {
      r = afmt(r, "#(bad_type(%d) %p)", type, o);
    }
  }

  print_depth--;

  return r;
}

NOINLINE void *bad_argnum(dyn e, uint32_t expected) {
  uint32_t got = LIST_SIZE(e);
  rterr("bad arguments number: %d (expected %d)\n", LIST_SIZE(e), expected);
  return 0;
}


NOINLINE void bad_type(char *expected, int arg_index, char *name) {
  rterr("%s: arg %d is not %s", name, arg_index, expected);
#if 0
  PROLOGUE(FRAME_HEADER_SIZE);
  uint32_t i, nargs = LIST_SIZE(E);
  fprintf(stderr, "arg %d isnt %s, in: %s", arg_index, expected, name);
  for (i = 0; i < nargs; i++) fprintf(stderr, " %s", print_object(getArg(i)));
  fprintf(stderr, "\n");
  print_stack_trace();
  fatal("aborting");
#endif
}



static void fatal_error(void *msg) {
  rterr("%s", print_object(msg));
}

extern int sbc_opc;
extern int64_t sbc_icount;

static int ctx_error_handler(ctx_error_t *info) {
  void *ctx = info->ctx;
  void *ip = ctx_ip(ctx);
  void *sp = ctx_sp(ctx);
  //fprintf(stderr, "icount=%lld, opcode=%d\n", sbc_icount, sbc_opc);
  //fprintf(stderr, "Err at ip=%p sp=%p\n", ip, sp);
  if (info->id == CTXE_ACCESS) {
    uint8_t *p = (uint8_t*)info->mem;
    /*if (stack_bottom <= p && p < stack_base) {
      fprintf(stderr, "stack overflow\n");
    } else*/ if ((uint8_t*)api.heap0 <= p && p < (uint8_t*)api.heap1+HEAP_SIZE) {
      rterr("out of memory");
    } else {
      /* Zero-padded 16-digit hex matches Win/macOS CRT output and
       * keeps `tests/.../run.sh` byte-diffs portable. */
      rterr("segfault at %016llx (heap=%016llx)",
            (unsigned long long)(uintptr_t)p,
            (unsigned long long)(uintptr_t)(uint8_t*)api.heap0);
    }
  } else {
    rterr("%s", info->text);
  }
  return CTXE_ABORT;
}

static void init_metadata(void *metatbl, int count) {
  int i;
  fn_meta_t *ms = (fn_meta_t*)metatbl;
  for (i = 0; i < count; i++) {
    hooks_heap[ms[i].hook].meta = &ms[i];
  }
}

static void init_texts(void *txtbl, int count) {
  int i;
  GC_DISABLE();
  void **ts = (void**)txtbl;
  text_table_t *tt = arraddnptr(text_tables,1);
  tt->size = count;
  tt->items = ts;
  for (i = 0; i < count; i++) {
    TEXT(ts[i],ts[i]);
  }
  api.hgp->dirty |= DRT_TEXT_TABLES;
  GC_ENABLE();
}

static int *resolve_methods(void *metbl, int count) {
  int i;
  char **ms = (char**)metbl;
  int *rs = malloc(sizeof(int)*count);
  for (i = 0; i < count; i++) {
    rs[i] = resolve_method(ms[i]);
  }
  return rs;
}

static void interns(void *types, int count) {
  int i;
  void **ts = (void**)types;
  for (i = 0; i < count; i++) {
    char *name = (char*)ts[i];
    ts[i] = (dyn)intern(name);
  }
}

static void *find_export(void *name, void *exports) {
  if (O_TAG(exports) != T_LIST) {
    fatal("exports ain't a list: %s\n", print_object(exports));
  }

  int nexports = LIST_SIZE(exports);
  for (int i = 0; i < nexports; i++) {
    void *pair = LGET(exports,i);
    if (O_TAG(pair) != T_LIST || LIST_SIZE(pair) != 2) {
      fatal("export contains invalid pair: %s\n", print_object(pair));
    }
    void *export_name = LGET(pair,0);
    if (!IS_TEXT(export_name)) {
      fatal("export contains bad name: %s\n", print_object(pair));
    }
    if (texts_equal(name, export_name)) {
      return LGET(pair,1);
    }
  }

  fatal("Couldn't resolve `%s`\n", print_object(name));
}

static void load_sbcs(void **libs, int nlibs, void **imlibs,
                      int nimlibs, void **im, int nimports) {
  int i;
  void *name;

  //create temporary libs, so gc() could run properly
  void **tlibs = zmalloc(sizeof(void*)*nlibs);
  module_imp_t *mi = arraddnptr(module_imports, 1);
  mi->nlibs = nlibs;
  mi->libs = tlibs;
  mi->nims = 0;
  mi->ims = 0;
  int module_index = alen(module_imports)-1;
  //GC_DISABLE();
  for (i = 0; i < nlibs; i++) {
    tlibs[i] = (void*)load_sbc((char*)libs[i]);
    api.hgp->dirty |= DRT_MODULE_IMPS;
  }
  //GC_ENABLE();
  for (i = 0; i < nlibs; i++) {
    libs[i] = tlibs[i];
  }
  //load_sbc could have resulted into a recursive call to load_sbcs
  //so mi have to be refreshed
  mi = &module_imports[module_index];
  mi->libs = libs;
  free(tlibs);

  GC_DISABLE();
  for (i = 0; i < nimports; i++) {
    //printf("%p: %s\n",libs[(intptr_t)imlibs[i]], (char*)im[i]);
    TEXT(name,(char*)im[i]);
    im[i] = (void*)find_export(name,libs[(intptr_t)imlibs[i]]);
  }

  GC_ENABLE();
  mi->nims = nimports;
  mi->ims = im;
  api.hgp->dirty |= DRT_MODULE_IMPS;
}


void sbc_init_types(sbc_t *sbc, void *tables) {
  tot_entry_t *ts = (tot_entry_t*)tables;
  init_metadata(ts[0].table, ts[0].size); //fmtbl
  init_texts(ts[1].table, ts[1].size);
  interns(ts[2].table, ts[2].size);
  sbc->mt = resolve_methods(ts[3].table, ts[3].size);
  load_sbcs((void**)ts[4].table, ts[4].size
               ,(void**)ts[5].table, ts[5].size
               ,(void**)ts[6].table, ts[6].size);

  //wont need it after init
  free(ts[3].table);
  ts[3].table = 0;

  sbc->tx = ts[1].table;
  sbc->ty = ts[2].table;
  sbc->im = ts[6].table;
}


void init_types() {
  dyn n_int, n_float, n_fn, n_list, n_text, n_no, n_tbl, n_tok;

  resolve_method(""); //reserve the NULL method id
  api.m_ampersand = resolve_method("&");
  api.m_underscore = resolve_method("__");
  api.m_hash = resolve_method("hash");
  api.m_equal = resolve_method("><");
  api.m_ne_ = resolve_method("<>");

  intern("int");
  intern("float");
  intern("_fixtext_");

  intern("_unused0_");
  intern("_unused1_");
  intern("_unused2_");

  intern("_tag_");
  intern("_data_");

  intern("fn");
  intern("_list_");
  intern("_view_");
  intern("_cons_");
  intern("_");
  intern("_text_");
  intern("no");
  intern("list");
  intern("text");
  intern("hard_list");
  intern("_bytes_");
  intern("tbl");
  intern("tok");
  intern("id");

  gc_init_collectors();

  TEXT(n_int, "int");
  TEXT(n_float, "float");
  TEXT(n_fn, "fn");
  TEXT(n_list, "list");
  TEXT(n_text, "text");
  TEXT(n_no, "no");
  TEXT(n_tbl, "tbl");
  TEXT(n_tok, "tok");

  set_type_size_and_name(T_INT, 0, n_int);
  set_type_size_and_name(T_FLOAT, 0, n_float);
  set_type_size_and_name(T_CLOSURE, 0, n_fn);
  set_type_size_and_name(T_LIST, 0, n_list);
  set_type_size_and_name(T_TEXT, 0, n_text);
  set_type_size_and_name(T_FIXTEXT, 0, n_text);
  set_type_size_and_name(T_VIEW, 0, n_list);
  set_type_size_and_name(T_CONS, 0, n_list);
  set_type_size_and_name(T_NO, 0, n_no);
  set_type_size_and_name(T_BYTES, 0, n_list);
  set_type_size_and_name(T_TBL, 0, n_tbl);
  set_type_size_and_name(T_TOK, 7, n_tok);
}

void init_subtypes() {
  add_subtype(T_OBJECT, T_GENERIC_TEXT);
  add_subtype(T_OBJECT, T_GENERIC_LIST);
  add_subtype(T_OBJECT, T_INT);
  add_subtype(T_OBJECT, T_FLOAT);
  add_subtype(T_OBJECT, T_NO);
  add_subtype(T_OBJECT, T_CLOSURE);
  add_subtype(T_OBJECT, T_TBL);
  add_subtype(T_OBJECT, T_TOK);

  add_subtype(T_GENERIC_TEXT, T_FIXTEXT);
  add_subtype(T_GENERIC_TEXT, T_TEXT);

  //hardlist is the type of all lists with O(1) index access
  add_subtype(T_HARD_LIST, T_LIST);
  add_subtype(T_HARD_LIST, T_VIEW);
  add_subtype(T_HARD_LIST, T_BYTES);

  add_subtype(T_GENERIC_LIST, T_HARD_LIST);
  add_subtype(T_GENERIC_LIST, T_CONS);
}

static void init_api(int gen0sz) {
  int i;
  void *paligned;
  
  gen0sz *= PAGE_SIZE;

  //memset(0, sizeof(api_t));

  api.nfi_parg = api.nfi_args;
  for (i = 0; i < NFI_MAX_ARGS; i++) {
    api.nfi_aptrs[i] = &api.nfi_args[i];
  }

  gen0sz /= sizeof(api.hgp->heap[0]);

  api.print_object_f = print_object_f;
  api.gc = gc;
  api.alloc = gc_alloc;
  api.alloc_text = alloc_text;
  api.text_chars = text_chars;

  api.frame = 0;

  api.uwhs = gc_heap.uwhs;
  api.heap0 = gc_heap.heap;
  api.heap1 = gc_heap.heap + HEAP_SIZE;

  //1 is subtracted since some objects consist entirely of gc_head_t
  api.theap0 = gc_heap.theap;
  api.theap1 = gc_heap.theap + HEAP_SIZE;

  api.pgmod = gc_heap.pgmod;
  memset(api.pgmod, PG_CLEAN, NPAGES);

  api.hg0 = gc_gens;
  api.hgp = api.hg0;

  //NOTE: we need a lot of margin space reserved for the final generation
  //because it can overflow up to 2x of its size, during GC, and
  //then it will have to be collected into another heap.
  uint32_t avail = HEAP_SIZE/2;

  uint32_t hgsz = gen0sz;
  for (i = 0; i < MAX_AGE; i++) {
    hg_t *hg = api.hg0+i;
    hg->age = i;
    hg->finalizers = 0;
    hg->magnets = 0;
    if (i >= MAX_AGE-3) {
      hg->parent = 0; //avoid overflow, if GC fails
    } else {
      hg->parent = hg+2;
    }
    if (avail >= hgsz*2) {
      hg->size = hgsz;
      avail -= hgsz;
      hgsz *= 2;
    } else {
      // tail gc_gens all get the max remaining size.
      hg->size = hgsz;
    }
    if (i == 0) {
      hg->base = api.heap0 + HEAP_SIZE;
      hg->top = hg->base;
      hg->ts = hg->top - hg->size;
      hg->heap = api.heap0;
      hg->theap = api.theap0;
    } else if (i == 1) {
      hg->base = api.heap1 + HEAP_SIZE;
      hg->top = hg->base;
      hg->ts = hg->top - hg->size;
      hg->heap = api.heap1;
      hg->theap = api.theap1;
    } else {
      hg->base = api.hg0[i-2].base;
      hg->top = api.hg0[i-2].top;
      hg->ts = hg->top - hg->size;
      hg->heap = api.hg0[i-2].heap;
      hg->theap = api.hg0[i-2].theap;
    }
  }
  api.ngens = i;

  //fprintf(stderr, "genNsz=%d/%d; ngens=%d\n", genNsz/2, HEAP_SIZE, i);

  api.puwh = api.uwhs;
  *api.puwh = 0;

  ctx_set_error_handler(ctx_error_handler);

  _mprotect(ALIGN(CTX_PGSZ,api.heap0), CTX_PGSZ*2, PROT_NONE);
  _mprotect(ALIGN(CTX_PGSZ,api.heap1), CTX_PGSZ*2, PROT_NONE);

#if 0
  fprintf(stderr, "writing to guard page...\n");
  *(int*)ALIGN(CTX_PGSZ,api.heap) = 123;
  fprintf(stderr, "written to guard page!!!\n");
#endif
}

void make_executable(void *ptr, int size) {
  int s = (size+CTX_PGSZ-1+CTX_PGSZ)/CTX_PGSZ*CTX_PGSZ;
  _mprotect(ALIGN0(CTX_PGSZ,ptr), s, PROT_READ|PROT_WRITE|PROT_EXEC);
}



int main(int argc, char **argv) {
  int i;
  void *R;
  char *tmp;

#ifdef __linux__
  /* Match Windows' merged-output interleaving exactly so the test
   * goldens survive a cross-platform diff:
   *
   *   - stdout: line-buffered, so program-side `say`/printf
   *     content flushes per line and stays in chronological order
   *     with stderr writes that happen between them.
   *
   *   - stderr: block-buffered, so the runtime's `fprintf(stderr,…)`
   *     bursts (stack traces especially) accumulate in a buffer
   *     and only emit at exit. On Windows the C runtime does the
   *     same thing implicitly when stderr is a pipe; on glibc
   *     stderr is unbuffered by default which causes stack traces
   *     to interleave AHEAD of in-flight stdout content.
   *
   * Together these reproduce the "stdout drained then stderr"
   * order the Win64 build emits when invoked via `2>&1`. */
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IOFBF, 0);
#endif

#ifndef __linux__
  /* Historical invariant: heap addresses must sort below code
   * addresses so the runtime can fingerprint pointers by range.
   * Holds on Win64 (image base at 0x100000000+) and macOS (same
   * with -image_base 0x100000000). On Linux there's no clean
   * GNU-ld equivalent of -image_base — heap and code both live in
   * the low 4 GiB. Nothing else in the runtime currently depends
   * on this layout, so we skip the check. Revisit if a future
   * encoding scheme needs it (or use -Ttext to pin code high). */
  if ((void*)malloc(1) > (void*)&main) {
    fprintf(stderr, "FIXME: heap is above the code. Edit BLTIN_BASE.\n");
    exit(-1);
  }
#endif

  shdefault(methlut,-1);
  shdefault(typelut,-1);

#if 0
  //obsolete checks. high bits tags aren't used anymore
  if ((uint64_t)&tmp > (uint64_t)GID_MASK) {
    fprintf(stderr, "Stack is is too high! (relink with proper -stack_addr)\n");
    exit(-1);
  }
  if ((uint64_t)(&api_g+1) > (uint64_t)GID_MASK) {
    fprintf(stderr, "Heap is is too high! (relink with proper -image_base)\n");
    exit(-1);
  }
#endif

  //The default Clang stack size is 0x800000. It is located at 0x7ffeefc00000
  //Our stack has size 0xA00000, and therefore ends at 0x7ffeef200000
  //there is also a 4096 bytes guard page at the 0x7ffeef201000
  //fprintf(stderr, "stack_base: %p\n", &argc);
  //fprintf(stderr, "stack_end: %p\n", (uint8_t*)&argc - 0xA00000);
  //exit(-1);

  sif_loader_init();

  //init_api(1);
  //init_api(8);
  //init_api(16);
  //init_api(32);
  //init_api(GEN_ZERO_SIZE);
  /* RT-3: allow the gen0 size to be overridden via env.
   * SYMTA_GEN0_SIZE is interpreted as bytes (decimal, hex with 0x,
   * or octal with leading 0), rounded UP to a page.  Below
   * PAGE_SIZE silently clamps to one page.  Above HEAP_SIZE/2
   * silently clamps to the safe ceiling (init_api would otherwise
   * fall through to the smaller "tail-size" branch). */
  {
    int gen0_pages = GEN_ZERO_SIZE * 10;
    char *gen0_env = getenv("SYMTA_GEN0_SIZE");
    if (gen0_env && *gen0_env) {
      long bytes = strtol(gen0_env, 0, 0);
      if (bytes > 0) {
        long pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        if (pages < 1) pages = 1;
        /* HEAP_SIZE is in 8-byte slots; clamp to a quarter of that
         * in pages (so even the doubling ladder gen1/gen2/... can
         * fit before init_api's avail check stops growing). */
        long max_pages = (HEAP_SIZE / 4) / (PAGE_SIZE / 8);
        if (pages > max_pages) pages = max_pages;
        gen0_pages = (int)pages;
      }
    }
    init_api(gen0_pages);
  }
  //init_api(2560*PAGE_SIZE);

  api.sb = (void**)&tmp;

  init_builtins(argc, argv);


  //prun_tests();

  //api.gc();
  //bugtrap();

  //show_runtime_info();


  char *main_lib = cat(main_path,"sbc");
  add_lib_folder(main_lib);

  if (!fs_is_folder(main_lib)) {
    tmp = fmt("%ssbc.saf", main_path);
    if (fs_is_file(tmp)) {
      //fprintf(stderr, "%s\n  %s\n", main_lib, tmp);
      char *src = cat(main_lib, "/");
      fs_mount(src, tmp, MNT_SAF);
      free(src);
    } else {
      fprintf(stderr, "Missing %s\n", tmp);
      exit(-1);
    }
    arrfree(tmp);
  }

  //tmp = fmt("%s/%s", main_lib, "go");
  //if (!file_exists(tmp))
  {
    //arrfree(tmp);
    tmp = fmt("%s/go.sbc", main_lib);
    if (!fs_is_file(tmp)) {
      fprintf(stderr, "Couldnt load %s\n", tmp);
      exit(-1);
    }
  }
  R = exec_module(tmp);
  arrfree(tmp);

  //fprintf(stderr, "%s\n", print_object(R));

  //fprintf(stderr, "%d\n", (int)(api.hgp->base - api.hgp->top));

  return 0;
}
