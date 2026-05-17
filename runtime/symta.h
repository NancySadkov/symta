#ifndef SYMTA_H
#define SYMTA_H


#include <stdio.h>
#include <stdint.h>
#include <setjmp.h>


#define BIT(n) ((uint64_t)1<<(n))
#define MSK(n) (BIT(n)-1)


#define SYMTA_DEBUG 1

#ifdef WINDOWS

#define NFI_BADPTR(p) ((uint64_t)(p)>>63)
#define NFI_ENCPTR(p) (dyn)((uint64_t)(p)>>1)
#define NFI_DECPTR(p) (void*)((uint64_t)(p)<<1)

#else
//FIXME: the bellow code assumes that all exported symbols are aligned
//       with the lowest bit zero. That is true for OSX.
#define NFI_BADPTR(p) ((uint64_t)(p)&1)
#define NFI_ENCPTR(p) (dyn)(p) 
#define NFI_DECPTR(p) (void*)(p) 

#define NATIVE_HOOKS 1

#if defined __MACH__
#define MACH_X84_64  1
#endif

#endif

#define FAIL(x,s) if (x) fatal(s "\n");
#define FATAL(msg) fatal(msg);

#define CRASH (*(void*volatile*)0 = 0)

//dynamic type (aka top type, aka universal type, aka super type)
typedef void *dyn;


//lowest tag bit determines if the value is on heap flag
#define GID_BITS ((uint64_t)48)
#define TAG_BITS ((uint64_t)16)
#define FLG_BITS 1

#define TAG_MASK (MSK(TAG_BITS-FLG_BITS)<<FLG_BITS)

#define GID_SHFT TAG_BITS
#define GID_MASK (MSK(GID_BITS)<<GID_SHFT)


#define ALIGN_BITS 3
#define ALIGN_MASK MSK(ALIGN_BITS)

//assumes page size to be 4096 and alloc unit to be 8 byte
#define PAGE_BITS 12
#define PAGE_SIZE (1<<PAGE_BITS)
#define PAGE_SHIFT (PAGE_BITS-ALIGN_MASK)


#define T_INT     0
#define T_FLOAT   1
#define T_FIXTEXT 2
#define T_TAG     6
//#define T_DATA    7
#define T_CLOSURE 8
#define T_LIST    9
#define T_VIEW    10
#define T_CONS    11
#define T_OBJECT  12
#define T_TEXT    13
#define T_NO      14
#define T_GENERIC_LIST 15
#define T_GENERIC_TEXT 16
#define T_HARD_LIST    17
#define T_BYTES        18
#define T_TBL          19
#define T_TOK          20
#define T_ID           21

#define T_HEAP 1

#define STRIP_TAG(o) (dyn)((uint64_t)(o)&GID_MASK)

#define O_GID(o) ((uint64_t)(o)>>GID_SHFT)

#define HEAP_BASE (api_g.heap0)
#define HEAP_INDEX(o) (uint64_t)((void**)(o)-HEAP_BASE)
#define PTRENC(o) (HEAP_INDEX(o)<<GID_SHFT)
#define PTRDEC(o) ((uint64_t)(HEAP_BASE+O_GID(o)))

#define O_PTR(o) PTRDEC(o)
#define O_HDR(o) (*((gc_head_t*)O_PTR(o) - 1))

#define O_ONHEAP(o) ((uint64_t)(o)&T_HEAP)
#define IMMEDIATE(o) (!O_ONHEAP(o))

#define O_TAG(o) (((uint64_t)(o)&TAG_MASK)>>FLG_BITS)
#define TAGIS(tag,o) (O_TAG(o) == (tag))

#define O_THEAP(o) api.theap0[O_GID(o)-1]
#define O_PAGE(o) api.pgmod[O_GID(o)>>PAGE_SHIFT]
#define O_RELOC(o) (*(void**)&O_HDR(o))
#define O_AGE(o) O_THEAP(o)
#define O_HG(o) (api.hg0 + O_AGE(o))



#define O_CODE(o) O_HDR(o).code
#define O_HOOK(o) hooks_heap[(uint64_t)O_CODE(o)]
#define O_META(o) O_HOOK(o).meta
#define O_SIZE(o) O_HDR(o).size

#define TAGIFY(o,tag) ((dyn)((uint64_t)(o)|((uint64_t)(tag)<<FLG_BITS)))

#define HEAPREF(o) ((dyn)((uint64_t)(o)|T_HEAP))

//value for non-existing key,value pair (mostly used by T_TBL code)
#define NIL HEAPREF(0)


#define MKIMM(tag,gid)  TAGIFY((uint64_t)(gid)<<GID_SHFT,(tag))

#define No        MKIMM(T_NO,0)
#define FIXTEXT(x) MKIMM(T_FIXTEXT,x)

// sign preserving shifts
#define ASHL(x,count) ((x)*(1<<(count)))
#define ASHR(x,count) ((x)/(1<<(count)))


#define FXN(x) ((dyn)ASHL((int64_t)(x),GID_SHFT))
#define UNFXN(x) ASHR((int64_t)(x),GID_SHFT)

#define FIXTEXT_CHAR_BITS 8
#define FIXTEXT_MAX_CHARS (GID_BITS/FIXTEXT_CHAR_BITS)
#define FIXTEXT_LIM (FIXTEXT_BITS+GID_SHFT)

#define FIXTEXT_BITS (FIXTEXT_MAX_CHARS*FIXTEXT_CHAR_BITS)

#define FIXTEXT_CHAR_MASK ((uint8_t)((uint64_t)(1<<FIXTEXT_CHAR_BITS)-1))
#define FIXTEXT_CHAR_IMASK ((uint8_t)(~FIXTEXT_CHAR_MASK))


#define LIST_SIZE(o) O_SIZE(o)

#define IMMEQ(dst,a,b) dst = FXN((dyn)(a) == (dyn)(b))
#define IMMNE(dst,a,b) dst = FXN((int64_t)(a) != (int64_t)(b))

#define FXNNEG(dst,o) dst = (dyn)(-(int64_t)(o))
#define FXNADD(dst,a,b) dst = (dyn)((int64_t)(a) + (int64_t)(b))
#define FXNSUB(dst,a,b) dst = (dyn)((int64_t)(a) - (int64_t)(b))
#define FXNMUL(dst,a,b) dst = (dyn)(UNFXN(a) * (int64_t)(b))
#define FXNDIV(dst,a,b) dst = FXN((int64_t)(a) / (int64_t)(b))
#define FXNREM(dst,a,b) dst = (dyn)((int64_t)(a) % (int64_t)(b))


#define FXNLT(dst,a,b) dst = FXN((int64_t)(a) < (int64_t)(b))
#define FXNGT(dst,a,b) dst = FXN((int64_t)(a) > (int64_t)(b))
#define FXNLTE(dst,a,b) dst = FXN((int64_t)(a) <= (int64_t)(b))
#define FXNGTE(dst,a,b) dst = FXN((int64_t)(a) >= (int64_t)(b))
#define FXNAND(dst,a,b) dst = (dyn)((uint64_t)(a) & (uint64_t)(b))
#define FXNIOR(dst,a,b) dst = (dyn)((uint64_t)(a) | (uint64_t)(b))
#define FXNXOR(dst,a,b) dst = (dyn)((uint64_t)(a) ^ (uint64_t)(b))
#define FXNSHL(dst,a,b) dst = (dyn)((int64_t)(a)<<UNFXN(b))
#define FXNSHR(dst,a,b) dst = (dyn)(((int64_t)(a)>>UNFXN(b))&GID_MASK)
#define FXNLISTN(dst,size) LIST(dst,UNFXN(size))
#define FXNLGET(dst,xs,i) dst = (dyn)LGET((xs),UNFXN(i))
#define FXNLSET(dst,xs,i,v) LSET((xs),UNFXN(i),(v))


#define LDFLT(dst,x) { \
    float f_ = (float)(x); \
    uint64_t t_ = (*(uint32_t*)&f_); \
    dst = MKIMM(T_FLOAT,t_); \
  }

#define STFLT(dst,x) do { \
    uint32_t t_ = (uint32_t)O_GID(x); \
    dst = *(float*)&t_; \
  } while (0)

//Object's header inside the managed heap
//During GC if age=GC_MOVED,
//It is a 64-bit pointer to the object's new location
typedef struct {
  uint32_t size; //size of the object (not counting this header)
  uint32_t code; //point's to the closure's machine code or entity's id
} __attribute__((packed)) gc_head_t;


typedef uint8_t theap_t;

typedef struct hg_t hg_t;

typedef struct {
  dyn obj;
  dyn fn;
} gc_finalizer_t;

struct hg_t { //heap generation
  void **top;    //this generations's heap top
  void **ts;     //threshold pointer (when GC should be called)
  void **base;   //pointer to this gens's heap base, used by GC
  uint32_t size; //the size of this generation
  void **heap;
  theap_t *theap;
  int age; //age/id used to determine which object belongs to which generation.
  hg_t *parent;   //parent generatioon, which shares the same heap
  uint32_t dirty; //dirty flags. determine what to collect.
  gc_finalizer_t *finalizers;
  dyn *magnets; //old objects which ref younger objects
};

typedef struct frame_t frame_t;
/* RT-8b/c: frame header layout post-callee-allocation.
 *
 * With RT-8b the locals array always sits immediately after the
 * `frame_t` header in the same struct-hack block.  That means
 * the old `vars` pointer-back-to-locals field is entirely
 * redundant -- locals are at `(void**)(frm + 1)` (with a small
 * adjustment so the compiler-level address arithmetic agrees:
 * see `FRAME_LOCALS_OFS` / `FRAME_LOCALS()` below).  Dropping
 * `vars` shrinks the header from 40 bytes to 32 bytes, removes
 * one store per CALL prologue, and keeps the whole header in
 * the first half of a cache line.
 *
 * `nvars` stays -- the GC walk needs the local-array length
 * and there's no compact way to recover it from the
 * struct-hack pointer alone. */
struct frame_t {
  dyn clsr;
  frame_t *prev;
  int nvars;
  /* CORE-1: bytecode pointer at this frame's most recent
   * suspension (call to the next frame).  Set by SBC_CALL /
   * MCALL / etc. just before OPEN_FRAME.  print_stack_trace
   * binary-searches the owning SBC's lineno side table for this
   * pin to recover (row, col).  Cost is one store per
   * Symta-level CALL site, vs the per-source-line SBC_LSRC
   * dispatch the in-bytecode design would have charged the hot
   * loop (40-100% on call-heavy paths). */
  uint8_t *pin;
};

/* Locals start immediately after the frame header.  PROLOGUE
 * uses the same arithmetic as `L_blk_ + FRAME_PREFIX_SLOTS`;
 * GC walkers and any debug code that wants to enumerate a
 * frame's locals call this. */
#define FRAME_LOCALS(frm) ((void**)((frame_t*)(frm) + 1))

//maximum number of arguments to a native function
#define NFI_MAX_ARGS 32

typedef struct api_t api_t;
struct api_t {
  /* ---- HOT cache line (RT-8a, May 2026).
   *
   * These eight fields are touched on the inner runtime loops:
   *   - args   : every CALL with arguments (PROLOGUE, ARGLIST)
   *   - frame  : every CALL/RET (OPEN_FRAME / CLOSE_FRAME /
   *              PROLOGUE writes nvars+vars; root scan reads)
   *   - hgp    : every allocation (gc_alloc's current generation)
   *   - heap0  : every O_GID / HEAP_INDEX -- pointer-to-index math
   *              for the world's address space
   *   - theap0 : every O_AGE lookup (read on each closure call,
   *              GC trace, write barrier)
   *   - pgmod  : every old<-young store (write-barrier's
   *              page-dirty bit; co-located with theap0 since the
   *              barrier touches both)
   *   - method : every MCALL (current method-id, written by the
   *              MCALL macro and read by THIS_METHOD)
   *   - puwh   : every btland (top unwind handler -- not as hot
   *              as the above six but still warm and useful to
   *              keep in line 0 since slots are otherwise free)
   *
   * Order chosen so the layout is stable under MSVC, gcc, clang
   * (only 8-byte pointers / 8-byte dyn, plus one 4-byte int with
   * a 4-byte sibling so there's no padding hole). */
  void *args;
  frame_t *frame;
  hg_t *hgp;
  void **heap0;
  theap_t *theap0;
  uint8_t *pgmod;
  int method;
  int gc_disable;        /* hot enough -- GC_DISABLE / GC_ENABLE
                            fire on every internal GC_DISABLE()
                            section; not pure-fast-path but close */
  void **puwh;

  /* ---- WARM second line: btrap/btland state, runtime constants
   * that show up on common but non-innermost paths. */
  /* RT-8b: closure-pending slot.  OPEN_FRAME used to stash the
   * called-closure dyn into a caller-stack `frame_t frm_;`, then
   * the callee's PROLOGUE read it back via `api.frame->clsr`.
   * That split the frame metadata (caller's stack) from the
   * locals array (callee's stack) across two cache lines per
   * Symta-level call.  After RT-8b, OPEN_FRAME writes the
   * closure here, the callee's PROLOGUE allocates a combined
   * (frame_t + L[]) block on its own stack, and copies the
   * pending closure into it -- frame metadata and locals end
   * up adjacent (often the same cache line for small fsize),
   * which speeds the GC root scan (one line per Symta frame
   * instead of two) and the per-call PROLOGUE store sequence. */
  dyn clsr_pending;
  dyn jmp_return;
  dyn error_handler;
  void *empty_;          /* the canonical No value; appears in
                            comparisons but typically inlined to
                            constants by the compiler */
  hg_t *hg0;             /* base of generation array; O_HG = hg0
                            + age */
  int ngens;
  /* (4-byte hole here on LP64) */
  void **heap1;
  theap_t *theap1;
  void **uwhs;

  /* ---- COLD: setup constants, infrequent function pointers,
   * FFI scratch space. */
  int m_ampersand;
  int m_underscore;
  int m_hash;
  int m_equal;
  /* `<>` (ne) method id -- needed at init_types time so the C-side
   * default identity comparator on T_OBJECT can be registered, and
   * by add_method's redefinition special-case (latest wins). */
  int m_ne_;
  char* (*print_object_f)(void *object);   /* error/trace path */
  void *(*alloc)(uint32_t tag, uint32_t size);   /* one-time init */
  void (*gc)();          /* explicit-gc builtin (RT-2) */
  void (*lset)(void *base, uint64_t ofs, void *value);
  void *(*alloc_text)(char *s);
  char *(*text_chars)(void *text);
  void **sb;             /* stack bottom -- set at startup */
  char *sbuf;            /* string buffer -- error printer */
  void *vlist;           /* gc() scratch */
  uint64_t nfi_args[NFI_MAX_ARGS];
  uint64_t *nfi_parg;
  void *nfi_aptrs[NFI_MAX_ARGS];
};


typedef dyn (*psf_t)(uint8_t *bc_);

typedef struct fn_meta_t { //function metadata
  void *nargs; // number of arguments (-1 = any)
  void *name;  // function name text (anonymous, when name is 0)
  uint64_t hook;
  int32_t row;
  int32_t col;
  void *origin;  // user-provided metadata
}  __attribute__((packed)) fn_meta_t;


typedef struct {
  psf_t handler;
  uint8_t *payload; //usually bytecode, which handler understands
  fn_meta_t *meta;
} __attribute__((packed)) hook_t;


#define Empty     api.empty_

#define OBJECT(dst,tag,count) \
  dst = gc_alloc((uint32_t)(uint64_t)tag,count)



/* RT-5: closures inline a copy of (handler, payload) at the two
 * slots right after their `count` user slots, so the CALL hot
 * path can dispatch from the closure object itself instead of
 * indirecting through `hooks_heap[O_CODE]` (a ~1.5 MB global
 * table that any non-monomorphic closure-call stream hits
 * cache-cold).  O_SIZE keeps reporting `count` (the
 * compiler-visible capture-count) so gc_closure's `for i < size`
 * trace skips the inlined fields cleanly; only the underlying
 * allocation is bumped by two.
 *
 * Invariant: for any live closure `c`,
 *   slots[O_SIZE(c)+0] == hooks_heap[O_CODE(c)].handler
 *   slots[O_SIZE(c)+1] == hooks_heap[O_CODE(c)].payload
 * Three places in bltin.c mutate a closure's hook or size after
 * construction (`unstub_method`, `nativize_method`, `set_meta_`).
 * Each must call CLOSURE_SYNC_HOOK afterwards to re-establish the
 * invariant, otherwise CALL will read stale handler/payload from
 * the closure's inline slots and dispatch to the wrong function.
 *
 * Closures grow by 2 slots; nativize_method's `O_SIZE = 1` resize
 * is safe because the original allocation reserved size+2 slots
 * and the check `O_SIZE(met) < 1` guards the call. */
#define CLOSURE(dst,code,count) do { \
  /* Snapshot both args first.  `code` is often `O_CODE(other)` \
   * which reads from the heap; if the subsequent OBJECT call \
   * triggers a young-gen GC, the source object may relocate and \
   * a second evaluation of `code` would deref stale memory. */ \
  uint32_t _cnt  = (uint32_t)(uint64_t)(count); \
  uint32_t _code = (uint32_t)(uint64_t)(code); \
  OBJECT(dst, T_CLOSURE, _cnt + 2); \
  O_HDR(dst).size = _cnt; \
  O_CODE(dst) = _code; \
  hook_t *_hk = &hooks_heap[_code]; \
  void **_slots = (void**)O_PTR(dst); \
  _slots[_cnt    ] = (void*)_hk->handler; \
  _slots[_cnt + 1] = _hk->payload; \
  /* The two inlined slots hold function pointers, NOT heap \
   * refs.  Clear theap so a stale TG_DIRTY flag left over from \
   * the slot's previous occupant doesn't trick gc_older_gens \
   * into dereferencing the function pointer as a young-gen heap \
   * reference. */ \
  uint32_t _gid = (uint32_t)O_GID(dst); \
  api.theap0[_gid + _cnt    ] = 0; \
  api.theap0[_gid + _cnt + 1] = 0; \
} while (0)

/* RT-5: re-establish the inline (handler, payload) invariant
 * after a runtime mutation of either O_CODE(c) or O_SIZE(c).
 * Caller must ensure the allocation backing `c` still has room
 * for the two inline slots at slots[O_SIZE(c)] and
 * slots[O_SIZE(c)+1] (true by construction for closures created
 * via CLOSURE, since allocation = O_SIZE+2). */
#define CLOSURE_SYNC_HOOK(c) do { \
  uint32_t _sz = (uint32_t)O_SIZE(c); \
  void **_sl = (void**)O_PTR(c); \
  hook_t *_h2 = &hooks_heap[(uint32_t)O_CODE(c)]; \
  _sl[_sz    ] = (void*)_h2->handler; \
  _sl[_sz + 1] = _h2->payload; \
  uint32_t _g2 = (uint32_t)O_GID(c); \
  api.theap0[_g2 + _sz    ] = 0; \
  api.theap0[_g2 + _sz + 1] = 0; \
} while (0)

#define LIST(dst,size) OBJECT(dst, T_LIST, (uint32_t)(uint64_t)size);

#define ARGLIST(size) LIST(api.args,size)
#define STARG(i,arg) LGET(api.args,i) = arg


typedef struct tot_entry_t { //table of tables entry
  int64_t size;
  void *table;
}  __attribute__((packed)) tot_entry_t;

#define IS_LIST(o) (O_TAG(o) == T_LIST)

#define print_object(object) api.print_object_f(object)


#define LDFXN(dst,x) dst = FXN(x);
//FIXME: TEXT is not used by the current SIF code, move it to runtime.h
#define TEXT(dst,x) dst = alloc_text((char*)(x))
#define THIS_METHOD(dst) dst = FXN(api.method);


// P (parent) holds points to closure of current function
// E (environment) holds pointer to arglist of current function
#define P L[0]
#define E L[1]

/* RT-8b: combined frame_t + L[] block.  `frame_t` used to live
 * on the caller's C stack (allocated by `OPEN_FRAME`), with
 * `L[]` on the callee's C stack (allocated here).  The two were
 * always in different C stack frames, so the GC root scan paid
 * a cache miss per Symta frame to chase `frame->vars` across
 * the gap.  Now both live on the callee's stack, adjacent in
 * one C local: `frame_t` followed immediately by `fsize` slots
 * of `L[]`.  For typical `fsize` <= 4 the whole frame is one
 * cache line.  The caller hands the closure across via
 * `api.clsr_pending` (a warm-line field of `api_t`) which the
 * prologue copies into the just-built frame header.  Sized
 * `1 + fsize` void-pointer slots, since `sizeof(frame_t) == 40`
 * fits in 5 pointer slots on LP64 but the cleanest portable
 * trick is a struct-hack: one void* block, the head of which
 * is reinterpreted as `frame_t *`. */
#define FRAME_PREFIX_SLOTS \
  ((sizeof(frame_t) + sizeof(void*) - 1) / sizeof(void*))

#define PROLOGUE(fsize) \
  void *L_blk_[FRAME_PREFIX_SLOTS + (fsize)]; \
  frame_t *frm_ = (frame_t*)L_blk_; \
  void **L = L_blk_ + FRAME_PREFIX_SLOTS; \
  frm_->prev = api.frame; \
  frm_->clsr = api.clsr_pending; \
  frm_->pin = 0; \
  frm_->nvars = (fsize); \
  api.frame = frm_; \
  do { \
    void **p_ = L+2; \
    void **e_ = L+(fsize); \
    while (p_ < e_) *p_++ = 0; \
  } while(0); \
  P = frm_->clsr; \
  E = api.args;

#define FRAME_HEADER_SIZE 2

#define SUBR(fsize) PROLOGUE(fsize)

/* RT-8b: OPEN_FRAME no longer allocates a `frame_t` on the
 * caller's stack.  Instead it stages the called-closure dyn in
 * `api.clsr_pending`; the callee's PROLOGUE picks it up and
 * stores it into the combined frame block it allocates on its
 * own stack.  CALL is responsible for saving/restoring
 * `api.frame` around the inner dispatch, because the callee's
 * frame_t (in callee stack space) is gone by the time control
 * returns here. */
#define OPEN_FRAME(f) api.clsr_pending = (f);

/* RT-8b: CLOSE_FRAME now reads the saved `prev` from the
 * locally-allocated `frm_` (set by PROLOGUE), unwinding the
 * frame at end-of-function rather than at the caller side.
 * The CALL macro doesn't use CLOSE_FRAME any more -- it snapshots
 * api.frame around the inner dispatch -- but the explicit
 * `sbc_exec` driver does, since it's the outermost frame for an
 * entire Symta module run. */
#define CLOSE_FRAME api.frame = frm_->prev;


#define ARGLIST1(a) do {\
  ARGLIST(1); \
  STARG(0,a); \
 } while (0)

#define ARGLIST2(a,b) do {\
  ARGLIST(2); \
  STARG(0,a); \
  STARG(1,b); \
 } while (0)

#define ARGLIST3(a,b,c) do {\
  ARGLIST(3); \
  STARG(0,a); \
  STARG(1,b); \
  STARG(2,c); \
 } while (0)


extern hook_t hooks_heap[];

/* RT-5: dispatch via the closure's inline (handler, payload)
 * slots instead of indirecting through hooks_heap.  Reads land
 * on the closure object itself -- the same cache line as the
 * captures we just walked to set up the call -- so the call no
 * longer pays a vtable-style miss into the side table.
 *
 * RT-8b: also snapshots `api.frame` so we can restore it after
 * the callee (whose `frame_t` lives on its own C stack frame,
 * see PROLOGUE) returns and that stack is unwound.  The save is
 * one register-width local; the unwind store is the same one
 * CLOSE_FRAME used to do, just sourced from the snapshot.  (We
 * don't invoke CLOSE_FRAME here because it would refer to a
 * different `frm_` -- the *enclosing* function's, which we don't
 * want to unwind.) */
#define CALL(k,f) { \
  OPEN_FRAME(f); \
  void **_fp = (void**)O_PTR(f); \
  uint32_t _fz = (uint32_t)O_SIZE(f); \
  psf_t _h = (psf_t)_fp[_fz]; \
  frame_t *_saved_frame = api.frame; \
  k = _h((uint8_t*)_fp[_fz + 1]); \
  api.frame = _saved_frame; \
}

#define GC_DISABLE() ++api.gc_disable
#define GC_ENABLE() --api.gc_disable


#define MCALL(k,o,mid) do {      \
    int m = mid;  \
    api.method = m;              \
    dyn mfn = get_method(m,o);   \
    CALL(k,mfn);                 \
  } while (0)


//used when it is unknown if type is a closure
//fixme: `o` should be pased inside api to avoid ARGLIST alloc
#define CALL_TAGGED(k,o) \
  { \
    if (TAGIS(T_CLOSURE,o)) { \
      CALL(k,o); \
    } else { \
      dyn as = api.args; \
      GC_DISABLE(); \
      ARGLIST(2); \
      GC_ENABLE(); \
      STARG(0,o); \
      STARG(1,as); \
      MCALL(k,o,api.m_ampersand); \
    } \
  }


#define RETURN(o) return (void*)(o);


#define LGET(base,off) (((void**)O_PTR(base))[off])
#define LSET(base,ofs,value) lsetm(base, ofs, value)

#define LOAD(dst,src,src_off) dst = LGET(src,src_off)
#define STOR(dst,off,src) LSET(dst,off,src)
#define COPY(dst,dst_off,src,src_off) LSET(dst,dst_off,LGET(src,src_off))
#define MOVE(dst,src) dst = (src)


#define LGET1(base,off) (*(uint8_t*)(O_PTR(base)+(off)))
#define LGET4(base,off) (*(uint32_t*)(O_PTR(base)+(off)*4))
#define LGET8(base,off) (*(uint64_t*)(O_PTR(base)+(off)*8))


#define SET_UNWIND_HANDLER(r,h) ++api.puwh; *api.puwh = h;
#define REMOVE_UNWIND_HANDLER(r) *api.puwh = 0; --api.puwh;

typedef struct {
  jmp_buf *anchor;
  void *uwh; //top of the unwind handlers stack
  frame_t *frame;
  uint8_t *ip; //instruction pointer
} jmp_state;

#define CHECK_NARGS(expected) \
  if (LIST_SIZE(E) != (uint32_t)expected) bad_argnum(E, expected);

#define FFI_ARG_int(dst,src) \
  if (O_TAG(src) != T_INT) bad_type("int", 0, 0); \
  int dst = (int)UNFXN(src);
#define FFI_FROM_int(dst,src) dst = FXN((int64_t)src);

#define FFI_ARG_u4(dst,src) \
  if (O_TAG(src) != T_INT) bad_type("int", 0, 0); \
  uint32_t dst = (uint32_t)UNFXN(src);
#define FFI_FROM_u4(dst,src) dst = FXN((int64_t)src);

#define FFI_ARG_double(dst,src) \
  double dst; \
  { \
    float _x; \
    STFLT(_x,src); \
    dst = (double)(_x); \
  }

#define FFI_FROM_double(dst,src) LDFLT(dst,(float)(src));

#define FFI_ARG_float(dst,src) float dst; STFLT(dst,src); 

#define FFI_FROM_float(dst,src) LDFLT(dst,(float)(src));

#define FFI_ARG_text(dst,src) char *dst = api.text_chars(src);
#define FFI_FROM_text(dst,src) \
  GC_DISABLE(); \
  dst = alloc_text(src); \
  GC_ENABLE();

#define FFI_ARG_ptr(dst,src) void *dst = (void*)(src);
#define FFI_FROM_ptr(dst,src) dst = (void*)(src);

#define FFI_GET(dst,type,ptr,off) dst = FXN(((type*)(ptr))[UNFXN(off)]);
#define FFI_SET(type,ptr,off,val) ((type*)(ptr))[UNFXN(off)] = (type)UNFXN(val);


#endif //SYMTA_H
