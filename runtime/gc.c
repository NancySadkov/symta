/*

- Align generations's top by page size, after each GC,
  so we can use the page table to store age (i.e. quickly find the generation.)

*/

#include <stdarg.h>

#include "common.h"
#include "am.h"
#include "meta_table.h"

static int gc_cycle = 0;

#define GC_AGE (api.hgp-1)->age

#define GC_REC3(dst,o,mover) { \
  void *oo_ = (void*)(o); \
  uint32_t hg_ = O_AGE(oo_); \
  if (hg_ != GC_AGE) { \
    if (hg_ == GC_MOVED) { \
      dst = O_RELOC(oo_); /*already moved*/ \
    } else { \
      dst = oo_; /*older gen*/  \
    } \
  } else { \
    dst = mover; \
  } \
}

#define GC_REC2(dst,o) \
  GC_REC3(dst,o,collectors[O_TAG(oo_)](oo_))


#define GC_REC(dst,o) {  \
  void *o_ = (void*)(o); \
  if (IMMEDIATE(o_)) {   \
    dst = o_;            \
  } else {               \
    GC_REC2(dst,o_);     \
  }                      \
}

//place redirection to the new location of the object
//if O_AGE(o) is not GCGen, then it is already moved
#define GC_REDIR(o,p) O_AGE(o) = GC_MOVED; O_RELOC(o) = p;


static void *gc_immediate(void *o) {
  return o;
}


#define GCDEF(fn) void *fn(void *o) {void *so_ = o;
#define GCEND(r) return (r);}
//#define GCEND(r) O_THEAP(so_) = 0; return (r);}
#include "gc_types.h"
#undef GCDEF
#undef GCEND


//the range is inclusive since some objects consist entire of gc_head_t
#define INSIDE_HG(g,p) ((g)->top <= (void**)(p) && (void**)(p) <= (g)->base)

#if 0
static int gcprint_lv = 0;

static void gcprint(char *fmt, ...) {
  if (!gcprint_lv) return;
  for (int i = 0; i < gcprint_lv*2; i++) fprintf(stderr, "-");
  va_list ap;
  va_start(ap,fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}
#endif


//FIXME: this part can be optimized by maintain a per generation dirty flag
//       set when builtins are modified.
static void gc_builtins(hg_t *src, hg_t *dst) {
  int i, j;

  GC_REC(sink->fn,sink->fn);
  GC_REC(api.empty_,api.empty_);
  GC_REC(main_args,main_args);
  GC_REC(api.jmp_return,api.jmp_return);

  GC_REC(api.args,api.args);

  GC_REC(api.error_handler,api.error_handler);



  for (frame_t *frm = api.frame; frm; frm = frm->prev) {
    GC_REC(frm->clsr, frm->clsr);
    /* RT-8b/c: locals sit immediately after the frame header in
     * the same struct-hack block, so we synthesise the locals
     * pointer here instead of reading a now-removed `vars`
     * field. */
    void **pv = FRAME_LOCALS(frm);
    void **ev = pv + frm->nvars;
    for (; pv < ev; pv++) { //local variables for each active call
      GC_REC(*pv, *pv);
    }
  }

  if (src->dirty&DRT_METHOD_NAMES) {
    dst->dirty |= DRT_METHOD_NAMES;
    int len = arrlen(method_names);
    for (i = 0; i < len; i++) {
      GC_REC(method_names[i], method_names[i]);
    }
  }

  if (src->dirty&DRT_TYPE_METHODS) {
    dst->dirty |= DRT_TYPE_METHODS;
    for (i = 0; i < nmethods; i++) {
      method_node_t *page = method_pages[i>>METHODS_PAGE_BITS];
      method_node_t *m = page + (i&METHODS_PAGE_MASK);
      GC_REC(m->fn, m->fn);
    }
  }

  if (src->dirty&DRT_TYPES) {
    dst->dirty |= DRT_TYPES;
    for (i = 0; i < arrlen(types); i++) {
      type_t *t = &types[i];
      //FIXME: no need to GC_REC t->sink->fn
      //       since it points a place which already gets gc()'d
      GC_REC(t->sink->fn, t->sink->fn);
      GC_REC(t->sname, t->sname);
    }
  }

  if (src->dirty&DRT_LIB_EXPORTS) {
    dst->dirty |= DRT_LIB_EXPORTS;
    for (i = 0; i < shlen(lib_expts); i++) {
      GC_REC(lib_expts[i].value, lib_expts[i].value);
    }
  }

  if (src->dirty&DRT_TEXT_TABLES) {
    dst->dirty |= DRT_TEXT_TABLES;
    for (i = 0; i < arrlen(text_tables); i++) {
      text_table_t *tt = &text_tables[i];
      for (j = 0; j < tt->size; j++) {
        GC_REC(tt->items[j], tt->items[j]);
      }
    }
  }

  if (src->dirty&DRT_MODULE_IMPS) {
    dst->dirty |= DRT_MODULE_IMPS;
    for (i = 0; i < arrlen(module_imports); i++) {
      module_imp_t *mi = &module_imports[i];
      for (j = 0; j < mi->nlibs; j++) {
        GC_REC(mi->libs[j], mi->libs[j]);
      }
      for (j = 0; j < mi->nims; j++) {
        GC_REC(mi->ims[j], mi->ims[j]);
      }
    }
  }

  src->dirty = 0;
}

extern int bug;

static void gc_older_gens(hg_t *src, hg_t *dst) {
  /*
    There are two alternate solutions, beside the page table
    - First is maintaining a list of modified places,
      and adding to it on each modification.
      Such list unfortunately have a tendency to grow fast, incuring
      additional GCs.
    - Another solution could be using tombstones: https://wiki.c2.com/?TombStone
      Instead of linking directly to actual object, link to the descriptor in an
      object table. Tombstones also alow using 32bit intergers to references
      objects outside of range, and to easily serialize heap content to disk.
      Unfortunately Tombstones lead to a fragmented memory accesses,
      and additional cache miss, due to the superfluous pointer dereference.
  */
  uint8_t *pgmod = api.pgmod;
  uint8_t *theap = api.theap0;
  void **heap = api.heap0;
  for (uint32_t pg = 0; pg < NPAGES; pg++) {
    if (pgmod[pg] > src->age) continue;
    pgmod[pg] = PG_CLEAN;
    uint32_t index = pg << PAGE_SHIFT;
    uint32_t end = index + PAGE_SIZE;
    for (; index < end; index++) {
      uint32_t tag = theap[index];
      if (!(tag&TG_DIRTY)) continue;
      theap[index] &= ~TG_DIRTY;
      void **pp = heap+index;
      void *p = *pp;
      if (IMMEDIATE(p)) continue;
      if (!INSIDE_HG(src,O_PTR(p))) {
        //FIXME: in some cases resetting these dirty flags can be avoided
        //       by checking it is not a pointer to a younger generation.
        //       pgmod can hold a generation index for each heap.
        theap[index] |= TG_DIRTY;
        pgmod[pg] = dst->age;
        continue;
      }
      GC_REC(*pp,p);
      if (!INSIDE_HG(dst,pp)) {
        theap[index] |= TG_DIRTY;
        pgmod[pg] = dst->age;
      }
    }
  }
}

#define GC_IS_MOVED(o) (!IMMEDIATE(o) && O_AGE(o) == GC_MOVED)

void gc_finalizers(hg_t *src, hg_t *dst) {
  int n = arrlen(src->finalizers);
  gc_finalizer_t *tofin = 0;
  for (int i = 0; i < n; i++) {
    gc_finalizer_t *fin = &src->finalizers[i];
    if (GC_IS_MOVED(fin->obj)) {
      GC_REC(fin->obj, fin->obj);
      GC_REC(fin->fn, fin->fn);
      arrput(dst->finalizers, *fin);
    } else {
      arrput(tofin, *fin);
    }
  }
  arrfree(src->finalizers);
  src->finalizers = 0;

  //do it in a separate loop to break circular dependencies
  //when finalizable objs ref each other.
  n = arrlen(tofin);
  for (int i = 0; i < n; i++) {
    gc_finalizer_t *fin = &tofin[i];
    //ensure both fn and obj point to consistent objects
    GC_REC(fin->obj, fin->obj);
    GC_REC(fin->fn, fin->fn);
    ARGLIST(1);
    STARG(0,fin->obj);
    dyn r;
    CALL(r, fin->fn);
  }
  arrfree(tofin);
}

void gc_magnets(hg_t *src, hg_t *dst) {
  int n = arrlen(src->magnets);
  for (int i = 0; i < n; i++) {
    dyn o = src->magnets[i];
    tbl_gc_internals(o);
    AM_SET_YOUNGEST(o,dst->age); //FIXME: determine youngest for values
    if (O_AGE(o) > dst->age) {
      arrput(dst->magnets, o);
    }
  }
  arrfree(src->magnets);
  src->magnets = 0;
}


void gc_hg(hg_t *src, hg_t *dst) {
  src->ts = 0; //unlock src threshold (is it still required?)

  void *sts = dst->ts; //save threshold
  dst->ts = 0; //unlock dst threshold

#if 0
  ++gcprint_lv;
  //if (dbg_lv) ++gcprint_lv;
  //if (gc_cycle >= 1597) ++gcprint_lv;

  //gcprint("-----------------------\n");
  gcprint("gc[%d] age=%ld\n", gc_cycle, src-api.hg0);
  
  //gcprint("%p < %p < %p\n", &api, stack_top, api.gcjmp);

#endif
#if 0
  gcprint("src_hg:%p<%p\n", src->top, src->base);
  gcprint("dst_hg:%p<%p\n", dst->top, dst->base);
  gcprint("src_size:%ld/%d\n", src->base-src->top, src->size);
  gcprint("dst_size:%ld/%d (%d)\n", dst->base-dst->top, dst->size, HEAP_SIZE);
#endif

  hg_t *shgp = api.hgp;
  api.hgp = dst;

  uint32_t dirty = src->dirty;

  gc_builtins(src, dst);

  gc_older_gens(src, dst);

  for (void **ph = api.uwhs; ph <= api.puwh; ph++) {
    GC_REC(*ph, *ph);
  }

  if (src->magnets) {
    gc_magnets(src, dst);
  }

  if (src->finalizers) {
    gc_finalizers(src, dst);
  }

  /* Weak meta table: drop entries whose key wasn't reached and
   * forward survivors.  Must run AFTER all the regular GC passes
   * (so we have an accurate alive/dead distinction) and BEFORE the
   * src-heap wipe below.  Inlined here because the GC_REC machinery
   * isn't exported from this TU.
   *
   * Backing store is `ih_t` (identity-hashed, not `dh_t` content-
   * hashed -- see meta_table.c for why). */
  if (meta_table_g) {
    ih_t *old = (ih_t *)meta_table_g;
    ih_t *fresh = ihAlloc();
    NH_FOR(ih, i, old) {
      dyn key = *ihKey(old, i);
      dyn val = *ihVal(old, i);
      if (IMMEDIATE(key)) {
        /* Shouldn't happen -- meta_set_ rejects immediates -- but
         * keep the entry alive if something else got it in. */
        if (!IMMEDIATE(val)) GC_REC(val, val);
        ihSet(&fresh, key, val);
        continue;
      }
      uint32_t age = O_AGE(key);
      if (age == GC_MOVED) {
        /* Alive.  Forward both key and value to the new gen. */
        dyn new_key = (dyn)O_RELOC(key);
        if (!IMMEDIATE(val)) GC_REC(val, val);
        ihSet(&fresh, new_key, val);
      } else if (age == src->age) {
        /* Dead -- drop entry. */
      } else {
        /* Older generation -- this collection didn't touch the key.
         * Value might still need forwarding though. */
        if (!IMMEDIATE(val)) GC_REC(val, val);
        ihSet(&fresh, key, val);
      }
    }
    free(old);
    meta_table_g = fresh;
  }

  api.hgp = shgp;

  dst->ts = sts; //lock dst threshold


  //clear object start markers and their age
  memset(&src->theap[src->top - src->heap], 0, src->base - src->top);

  //zero memory, so all newly allocated data will have predictable values
  for (void **p = src->top; p < src->base; p++) *p = 0;

  if (dst->top < dst->ts) {
    gc_hg(dst,dst+1);
  }

  if (src->parent) src->base = src->parent->top;
  //else this generation owns the entire heap
  src->top = src->base;
  src->ts = src->base - src->size;

#if 0
  if (gcprint_lv) {
#if 0
    gcprint("etop: %p\n", src->top);
    gcprint("edst_sz:%ld/%d\n", dst->base-dst->top, dst->size);
    gcprint("edst_base:%p\n", dst->base);
    gcprint("edst_top :%p\n", dst->top);
#endif
    gcprint("end\n");
    --gcprint_lv;
    if (!gcprint_lv) gcprint("\n\n");
  }
#endif
}

int bug = 0;

void gc() {
#if 0
  if (bug) {
    fprintf(stderr, "gc_beg: %s\n", print_object((void*)0x10f901efbll));
  }
#endif

  //fprintf(stderr, "begin gc\n");
  gc_hg(api.hgp, api.hgp+1);
  gc_cycle++;
  //fprintf(stderr, "end gc\n");
#if 0
  if (bug) {
    fprintf(stderr, "gc_end: %s\n", print_object((void*)0x10f901efbll));
  }
#endif
}

void *gc_alloc(uint32_t tag, uint32_t size) {
  hg_t *hgp = api.hgp;

  void *r = hgp->top - size;
  gc_head_t *h = (gc_head_t*)r - 1;
  if ((void**)h < hgp->ts && hgp->top < hgp->base && !api.gc_disable) {
    gc();
    return gc_alloc(tag, size);
  }
  uintptr_t tmp_h = (uintptr_t)h;
  hgp->top = (void**)tmp_h;
  h->size = size;
  //hgp->theap[((void**)r - hgp->heap)] |= TG_OBJECT; //FIXNE: not required
  r = HEAPREF(TAGIFY(PTRENC(r), tag));
  O_AGE(r) = hgp->age;
  return r;
}

void gc_init_collectors() {
  SET_COLLECTOR(T_INT, gc_immediate);
  SET_COLLECTOR(T_FLOAT, gc_immediate);
  SET_COLLECTOR(T_FIXTEXT, gc_immediate);
  SET_COLLECTOR(T_CLOSURE, gc_closure);
  SET_COLLECTOR(T_LIST, gc_list);
  SET_COLLECTOR(T_VIEW, gc_view);
  SET_COLLECTOR(T_CONS, gc_cons);
  SET_COLLECTOR(T_TEXT, gc_text);
  SET_COLLECTOR(T_BYTES, gc_bytes);
  SET_COLLECTOR(T_TBL, gc_tbl);
}


void gc_set_finalizer(dyn obj, dyn fn) {
  /* Reject non-closure `fn` up front.  In Symta a bare name like
   * `fire` (top-level fn) or `&named ...` resolves to a TEXT
   * immediate -- the *symbol*, not a callable -- because there's
   * no implicit "function reference" path; only an inline closure
   * (`| X => ...`) produces a T_CLOSURE.  Passing a symbol used to
   * defer the failure to GC time: gc_finalizers' CALL(r, fin->fn)
   * dereferenced `(gc_head_t*)(HEAP_BASE + O_GID(text) - 1)` and
   * segfaulted on the random "header" it read.  Catch it here so
   * the error points at the set_finalizer call site. */
  if (!TAGIS(T_CLOSURE, fn)) {
    rterr_(fmt("set_finalizer: fn must be a closure (got %s); use "
               "an inline `| X => ...` wrapper",
               print_object(fn)));
    return;
  }
  gc_finalizer_t fin;
  fin.obj = obj;
  fin.fn = fn;
  arrput(api.hgp->finalizers, fin);
}
