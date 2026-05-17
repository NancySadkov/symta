#ifndef SRT_COMMON_H
#define SRT_COMMON_H

// On Linux we deliberately DON'T push hidden visibility: the symta
// runtime is linked as an executable (not a shared library), and
// `push(hidden)` here would cascade through the system headers
// pulled in by symta.h / ng.h below, marking libc symbols (exit,
// malloc, stderr, ...) as hidden undefined references. ld then
// refuses to satisfy them from libc.so.* — "hidden symbol X isn't
// defined". On Windows + macOS the original push is preserved
// because their toolchains either tolerate this or rely on
// -fvisibility=hidden in CFLAGS for the same effect.
#if defined(__GNUC__) && !defined(__linux__)
#pragma GCC visibility push(hidden)
#endif

#include "symta.h"
#include "ng.h"

#define alen(a)   arrlen(a)
#define aput(a,v) arrput(a,v)
#define apop(a)   arrpop(a)
#define alast(a)  arrlast(a)
#define adup(a)   arrdup(a)
#define aputs(a,b) for(char *p_ = b; *p_; p_++) aput(a,*p_)
#define aputa(a,b) \
  (arrsetlen(a, arrlen(a)+arrlen(b)) \
  ,memcpy(a+arrlen(a)-arrlen(b), b, sizeof(*b)*arrlen(b)) \
  ,a)

//what static items are dirty
#define DRT_METHOD_NAMES   0x001
#define DRT_TYPE_METHODS   0x002
#define DRT_TYPES          0x004
#define DRT_LIB_EXPORTS    0x008
#define DRT_TEXT_TABLES    0x010
#define DRT_MODULE_IMPS    0x020

#define DRT_BUILTINS  ( DRT_METHOD_NAMES \
                      | DRT_TYPE_METHODS \
                      | DRT_TYPES        \
                      | DRT_LIB_EXPORTS  \
                      | DRT_TEXT_TABLES  \
                      | DRT_MODULE_IMPS  \
                      )


uint32_t sbc_hook(psf_t fn, uint8_t *payload);
#define BUILTIN_CLOSURE(dst,code) { CLOSURE(dst, code, 0); }

#define IS_FIXTEXT(o) (O_TAG(o) == T_FIXTEXT)
#define IS_BIGTEXT(o) (O_TAG(o) == T_TEXT)
#define IS_TEXT(o) (IS_FIXTEXT(o) || IS_BIGTEXT(o))

#define BIGTEXT_SIZE(o) O_SIZE(o)
#define BIGTEXT_DATA(o) ((char*)&O_CODE(o))

#define C_ANY(o,arg_index,meta)

#define C_FN(o,arg_index,meta) \
  if (O_TAG(o) != T_CLOSURE) \
    bad_type("fn", arg_index, meta)

#define C_INT(o,arg_index,meta) \
  if (O_TAG(o) != T_INT) \
    bad_type("int", arg_index, meta)

#define C_FLOAT(o,arg_index,meta) \
  if (O_TAG(o) != T_FLOAT) \
    bad_type("float", arg_index, meta)

#define C_TEXT(o,arg_index,meta) \
  if (!IS_TEXT(o)) \
    bad_type("text", arg_index, meta)

#define C_BYTES(o,arg_index,meta) \
  if (O_TAG(o) != T_BYTES) \
    bad_type("bytes", arg_index, meta)


#define BUILTIN_SETUP(sname,name,nargs) \
  static void *b_##name(uint8_t *bc_); \
  static fn_meta_t meta_b_##name[1] = \
    {{FXN(nargs),sname,(uint64_t)b_##name,0,0,"builtin"}}; \
  static void setup_b_##name() { \
    meta_b_##name[0].hook = sbc_hook(b_##name,0); \
    hooks_heap[meta_b_##name[0].hook].meta = meta_b_##name; \
  }

#define getArg(i) LGET(E,i)

#define BLTIN_PROLOGUE(name) PROLOGUE(FRAME_HEADER_SIZE)

//FIXME: these vars should be pointers to stack

#define BUILTIN0(sname, name) \
  BUILTIN_SETUP(sname,name,0) \
  static dyn b_##name(uint8_t *bc_) { \
  BLTIN_PROLOGUE(#name); \
  dyn A, R; \
  CHECK_NARGS(0);
#define BUILTIN1(sname,name,a_check,a) \
  BUILTIN_SETUP(sname,name,1) \
  static dyn b_##name(uint8_t *bc_) { \
  BLTIN_PROLOGUE(#name); \
  dyn A, R, a; \
  CHECK_NARGS(1); \
  a = getArg(0); \
  a_check(a, 0, sname);
#define BUILTIN2(sname,name,a_check,a,b_check,b) \
  BUILTIN_SETUP(sname,name,2) \
  static dyn b_##name(uint8_t *bc_) { \
  BLTIN_PROLOGUE(#name); \
  dyn A, R, a, b; \
  CHECK_NARGS(2); \
  a = getArg(0); \
  a_check(a, 0, sname); \
  b = getArg(1); \
  b_check(b, 1, sname);
#define BUILTIN3(sname,name,a_check,a,b_check,b,c_check,c) \
  BUILTIN_SETUP(sname,name,3) \
  static dyn b_##name(uint8_t *bc_) { \
  BLTIN_PROLOGUE(#name); \
  dyn A, R, a, b, c; \
  CHECK_NARGS(3); \
  a = getArg(0); \
  a_check(a, 0, sname); \
  b = getArg(1); \
  b_check(b, 1, sname); \
  c = getArg(2); \
  c_check(c, 2, sname);
#define BUILTIN4(sname,name,a_check,a,b_check,b,c_check,c,d_check,d) \
  BUILTIN_SETUP(sname,name,4) \
  static dyn b_##name(uint8_t *bc_) { \
  BLTIN_PROLOGUE(#name); \
  dyn A, R, a, b, c, d; \
  CHECK_NARGS(4); \
  a = getArg(0); \
  a_check(a, 0, sname); \
  b = getArg(1); \
  b_check(b, 1, sname); \
  c = getArg(2); \
  c_check(c, 2, sname); \
  d = getArg(3); \
  d_check(d, 3, sname);
#define BUILTIN_VARARGS(sname,name) \
  BUILTIN_SETUP(sname,name,-1) \
  static dyn b_##name(uint8_t *bc_) { \
  BLTIN_PROLOGUE(#name); \
  dyn A, R;

#define RETURNS(r) return (dyn)(r); }



//A macro to convert 64-bit function address into a 32-bit hashable one
//Used for meta-data store puproses
#ifdef WINDOWS
#include "w64/compat.h"
#include "w64/mman.h"
#elif defined __MACH__
#include "unix/compat.h"
#include "osx/compat.h"
#include <sys/mman.h>
#define _mprotect mprotect
#else /*assume linux*/
#include "unix/compat.h"
#include "linux/compat.h"
#include <sys/mman.h>
#define _mprotect mprotect
#endif

#define CAR(x) O_RELOC(x)
#define CDR(x) LGET(x,0)  
#define CONS(dst, a, b) \
  OBJECT(dst, T_CONS, 1); \
  CAR(dst) = a; \
  CDR(dst) = b;


#define VIEW_START(o) O_CODE(o)
#define VIEW_SIZE(o) O_SIZE(o)
#define VIEW_BASE(o) LGET(o,0)
#define VIEW_SHARED(base) O_ONHEAP(base)
#define VIEW_MARK_SHARED(base) HEAPREF(base);
#define VIEW_STRIP_SHARED(base) (dyn)((uint64_t)(base) & ~T_HEAP)

//size threshold below which it is more efficient to use plain list
//best value is machine-dependent
#define VIEW_TS 8

#define VIEW(dst,base,start,size) \
  OBJECT(dst, T_VIEW, 1); \
  VIEW_START(dst) = (uint32_t)(start); \
  VIEW_SIZE(dst) = (uint32_t)(size); \
  VIEW_BASE(dst) = base;
#define VIEW_LGET(o,start,i) LGET(VIEW_BASE(o), (start)+(i))


#define BYTES_SIZE(o) O_SIZE(o)
#define BYTES_DATA(o) ((uint8_t*)&O_HDR(o) + 4)


/* RT-6: per-type method slot.  The dispatch table used to be a
 * fixed 512-bucket head-pointer array (`method_node_t
 * *methods[512]` = 4096 bytes per type) with chained
 * `method_node_t` chains for collisions.  4 KB per type, only a
 * fraction of which was populated, and every MCALL miss touched
 * three disjoint cache lines (type header + far-away
 * `methods[hid]` + chain node).
 *
 * RT-6 swapped the bucket array for a packed open-addressed
 * probe table sized to the actual method count per type
 * (initial cap 8, doubled on >= 75 % load).  Empty slots are
 * marked by `mid == 0`; the `""` (null) method id reserved at
 * `init_types()` time guarantees no real method ever uses 0.
 * RT-6 stored each slot as `(mid, node*)` and kept a stable
 * `method_pages` arena so the cached node pointers held by
 * `mcache_t` survived any per-type table resize.
 *
 * RT-6b folds `fn` directly into the slot.  The mcache wire
 * format (sif.h: `mcache_t`) carries `(tid, mid, fn)` instead
 * of a node pointer, so the stable-arena indirection is no
 * longer load-bearing.  Dispatch hot path drops one cache-line
 * load (was: mcache → node → fn; now: mcache.fn directly).
 *
 * Typical small types now hold ~16-64 slots = 256-1024 bytes;
 * heavy types (T_OBJECT plus everything that inherits from it)
 * top out around 256 slots = 4 KB, no worse than before.  The
 * footprint summed across the ~30 base types drops by 4-8×. */
typedef struct {
  uint32_t mid;       /* 0 = empty slot */
  uint32_t _pad;      /* alignment */
  dyn fn;             /* RT-6b: handler closure (was method_node_t*) */
} method_slot_t;

#define INITIAL_METHOD_CAP 8

#define END_TAG (-1)
typedef struct type_t type_t;
struct type_t {
  intptr_t size; // number of data slots in type
  dyn sink_fn;   // sink method handler: `type.__ Method Args = @Body`
  char *name;
  void *sname;   // name in symta's format
  int super;     // parent type
  int subtypes;  // child types
  int next;      // next subtype of this super type
  int id;
  /* RT-6: open-addressed probe table; see method_slot_t above. */
  uint32_t method_cap;    // capacity (power of 2)
  uint32_t method_count;  // entries used
  method_slot_t *methods; // dynamically allocated, calloc'd
};


typedef struct text_table_t {
  int size;
  void **items;
} text_table_t;


typedef struct module_imp_t {
  int nlibs;
  void **libs;
  int nims;
  void **ims;
} module_imp_t;



//maximum number of heap generations
//16 allows for the 2048 first generation
#define MAX_AGE 15

//used during GC to mark objects moved to the older generation
#define GC_MOVED MAX_AGE

//size in uint64_t of a semi heap
#define HEAP_SIZE (32*1024*1024)

#define FULL_HEAP_SIZE (HEAP_SIZE*2)

//size in pages of the generation 0
#define GEN_ZERO_SIZE 64

#define NPAGES (((HEAP_SIZE*2)>>PAGE_SHIFT)+1)

//page could reference younger generation(s)
#define PG_DIRTY 0x80

//page has no younger gen references
#define PG_CLEAN 0xFF

//////////////////////////////////////
//Tag heap flags

//this address points to an object
//#define TG_OBJECT 0x60

//this address could reference younger generation
#define TG_DIRTY  0x80

// maximum number of unwind handlers
#define MAX_UWHS (512)

//Compiler can't be trusted to lay global data in the same order
//it appears in source code, but Symta's GC needs exact order.
//So the heap related global gets assembled into a single struct.
typedef struct gc_heap_t {
  hg_t gc_gens[MAX_AGE];
  void *uwhs[MAX_UWHS]; //unwind handlers

  //actual heap data
  void *heap[FULL_HEAP_SIZE];

  //tags for each heap alloc unit.
  theap_t theap[FULL_HEAP_SIZE];

  //holds id of the youngest generation, referenced by this page
  //used by gc() to quickly trace modifications.
  uint8_t pgmod[NPAGES];
} gc_heap_t;

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) < (b) ? (b) : (a))

typedef void *(*collector_t)(void *o);
extern collector_t *collectors;
#define SET_COLLECTOR(type,handler) collectors[type] = handler;

extern api_t api_g; //FIXME: all routines should use local versions
#define api (api_g)

extern type_t *types;

/* RT-9 measurement: per-tag allocation counters incremented by
 * gc_alloc().  Always on (one uint64_t add per allocation, ~1-2
 * cycles).  Printed by `rtstat`.  Used to answer "how much does
 * CONS dominate?" empirically before committing to a specific
 * RT-9 implementation strategy. */
/* RT-9 caller-PC attribution: open-addressed hash from
 * (bytecode pin) -> alloc count.  16 K entries gives < 50 %
 * load factor for typical workloads (a `./game/` compile uses
 * ~5 K distinct SBC_LIST sites).  Saturates silently on
 * overflow -- the top-N emitters are what we want, and they
 * land first. */
typedef struct {
  void *pin;
  uint64_t count;
} alloc_pin_count_t;
#define ALLOC_ATTRIB_BUCKETS 16384

typedef struct {
  uint64_t by_tag[32];  /* indexed by O_TAG; tags top out at ~22 */
  uint64_t list_size_bucket[16]; /* [0]=size 0, [1]=1, ..., [14]=14,
                                    [15]=15+ (saturating bucket) */
  alloc_pin_count_t pin_counts[ALLOC_ATTRIB_BUCKETS]; /* T_LIST only */
  /* RT-9: element-tag distribution for size-1 LIST allocations.
   * Indexed by O_TAG of LGET(list, 0).  Counts which tags would
   * benefit from a `T_LIST1_<tag>` tagged-immediate encoding
   * that puts the size-1 list entirely in the dyn payload (no
   * heap alloc).  Captured at the size-1-store moment in
   * SBC_LIST1 + SBC_FXNLSET (when target is a freshly-allocated
   * size-1 LIST). */
  uint64_t list1_elem_tag[32];
} alloc_stats_t;
extern alloc_stats_t alloc_stats;

extern char *main_path;
extern void *main_args;
extern dyn single_chars[];
extern void **method_names;
extern dyn sink;  //default sink method handler (RT-6b: was method_node_t*)
extern text_table_t *text_tables;
extern module_imp_t *module_imports;
typedef struct { char *key; void *value; } *lib_expts_t;
extern lib_expts_t lib_expts;

void *tag_of(void *o);
void *fixtext_encode(char *p);
int fixtext_decode(char *dst, void *r);
void *alloc_textdata(int l);
void *alloc_bigtext(char *s, int l);
void *bytes_to_text(uint8_t *bytes, int size);
void *alloc_bytes(int l);
void *text_to_bytes(dyn o);
void *alloc_text(char *s);
char *decode_text(char *out, dyn o);
int texts_equal(dyn a, dyn b);
int fixtext_size(void *o);
int text_size(void *o);
char *text_chars(dyn text);
char *text_to_cstring(dyn text);


dyn get_method(int method_id, dyn object);
dyn get_method_for_tag(int method_id, int tag); /* RT-6b: returns fn for mcache fill */
NOINLINE void print_stack_trace();
NOINLINE void fatal(char *fmt, ...);
NOINLINE void rterr_(char *msg);
#define rterr(...) rterr_(fmt(__VA_ARGS__))

void *load_sbc(char *name);
dyn sbc_exec_fn(uint8_t *bytecode);
void add_lib_folder(char *folder);
void set_type_size_and_name(int tag, int size, void *name);
void add_subtype(int tag, int subtag);
void add_method(int type_id, int method_id, void *handler);
#define ADD_CORE_SINK -1
void add_method_r(int depth, int type_id
                 ,int method_id, void *handler);
int resolve_method(char *name);
dyn get_method_name(uint32_t method_id);
NOINLINE intptr_t intern(char *name);
void init_types();
void init_builtin_methods();
void init_builtin_functions();
void init_subtypes();
void init_root_sink();
void install_meta_dispatch(int tag);
void init_builtins(int argc, char **argv);

void *gc_alloc(uint32_t tag, uint32_t size);
void gc();
void gc_init_collectors();
void *gc_custom(void *o);
void gc_set_finalizer(dyn obj, dyn fn);

NOINLINE void *bad_argnum(dyn e, uint32_t expected);
NOINLINE void bad_type(char *expected, int arg_index, char *name);


void make_executable(void *ptr, int size);

void btjump(dyn land, dyn value);


dyn tokenize(dyn orig, dyn o);
#define token(...) token_(__VA_ARGS__)
dyn token_(dyn type, dyn val, dyn row, dyn col, dyn orig);

#define GC_LSET_DIRTY(p,value) do { \
  void **p_ = p; \
  uint32_t index = (uint32_t)HEAP_INDEX(p_); \
  uint8_t *page = &api.pgmod[index>>PAGE_SHIFT]; \
  int vage = O_AGE(value); \
  if (*page > vage) *page = vage; \
  api.theap0[index] |= TG_DIRTY; \
} while(0)

#define GC_OLDER(base, value) (!IMMEDIATE(value) && O_AGE(base) > O_AGE(value))

#define lsetm(base, ofs, value) do {                 \
  void **lsetp_ = (void**)O_PTR(base) + (ofs);            \
  *lsetp_ = (value);                                      \
  if (GC_OLDER(base, value)) {  \
    GC_LSET_DIRTY(lsetp_,value);                          \
  }                                                       \
} while (0)



extern int main(int argc, char **argv);
#define BLTIN_BASE ((void*)&main)


extern dyn dbgv; //resides in main.c

extern int m_add;
extern int m_sub;
extern int m_mul;
extern int m_div;
extern int m_rem;
extern int m_eq;
extern int m_ne;
extern int m_lt;
extern int m_gt;
extern int m_lte;
extern int m_gte;
extern int m_and;
extern int m_xor;
extern int m_ior;
extern int m_shl;
extern int m_shr;
extern int m_neg;
extern int m_inc;
extern int m_dec;
extern int m_get;
extern int m_set;
extern int m_abs;


#endif //SRT_COMMON_H
