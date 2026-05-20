#include <ctype.h>
#include <float.h>

#include "sif.h"
#include "ng.h"
#include "fs.h"
#include "sffi/sffi.h"
#include "jit.h"

/* RT-1: threaded dispatch (computed gotos) is on by default for
 * compilers that support GCC's address-of-label extension --
 * gcc, clang, MinGW.  The earlier measurement that flagged it as
 * ~8 % slower was tainted by Windows EcoQoS background throttling
 * and agent-CPU competition (both addressed in bench/bench.md's
 * workaround pattern); a clean rebench is owed before judging it
 * again, but the literature unanimously favours threaded dispatch
 * and "default on" matches every other JITless interpreter we
 * compare against.
 *
 * MSVC and any other compiler without computed-goto support
 * falls through to the portable switch dispatch.  Build with
 * `-DSBC_NO_THREADED_DISPATCH` to force the switch even on
 * GCC / Clang.  See sbc_exec_fn below for the dispatch macros. */
#if !defined(SBC_NO_THREADED_DISPATCH) && !defined(SBC_THREADED_DISPATCH)
  #if defined(__GNUC__) || defined(__clang__)
    #define SBC_THREADED_DISPATCH 1
  #endif
#endif

#define MCACHE_DIV 4

sbc_t *sbc_new(uint8_t *pin, int64_t size, char *path) {
  sbc_t *sbc = malloc(sizeof(sbc_t));
  sbc->file = pin;

  /* Format identification: 4-byte magic "SymB" followed by a
   * 2-byte revision.  Both are mandatory and validated up-front
   * so future format changes can fail at load with a clear
   * diagnostic instead of a misaligned read or a crash deep
   * in the dispatch loop. */
  if (size < 6) {
    fprintf(stderr, "sbc_load: %s is too short (%lld B) to be an SBC.\n"
           ,path, (long long)size);
    free(sbc);
    return 0;
  }
  uint32_t magic = (uint32_t)RD32;
  if (magic != SBC_MAGIC) {
    fprintf(stderr, "sbc_load: %s isn't an SBC file "
            "(magic=0x%08x, expected 0x%08x).\n",
            path, magic, SBC_MAGIC);
    free(sbc);
    return 0;
  }
  uint32_t rev = (uint32_t)RD16;
  if (rev != SBC_REVISION) {
    fprintf(stderr, "sbc_load: %s has format revision %u, "
            "this runtime supports revision %u -- recompile.\n",
            path, rev, SBC_REVISION);
    free(sbc);
    return 0;
  }
  sbc->src_file_string = RD24;
  sbc->deps_string = RD24;
  sbc->export_string = RD24;
  uint32_t tot_sz = RD16;
  sbc->code_sz = RD24;
  sbc->data_sz = RD24;
  sbc->tbls_sz = RD24;
  sbc->fntbl_sz = RD24;
  sbc->tot = pin;
  pin += tot_sz*3*2;

  sbc->code = pin;
  pin += sbc->code_sz;
  sbc->data = pin;
  pin += sbc->data_sz;
  sbc->tbls = pin;
  pin += sbc->tbls_sz;
  sbc->fntbl = pin;
  pin += sbc->fntbl_sz;
  
  pin = sbc->tot + 3*2*7;
  sbc->nrs_sz = RD24;
  sbc->nrs = sbc->tbls + RD24;
  /* CORE-1: per-SBC lineno table (count, offset) right after nrs
   * in tot.  Only present when tot_sz >= 9 (SBC compiled with
   * side-table support); older SBCs leave both fields zero. */
  if (tot_sz >= 9) {
    sbc->lineno_sz = RD24;
    uint32_t lineno_ofs = RD24;
    sbc->lineno_table = sbc->lineno_sz ? (sbc->tbls + lineno_ofs) : 0;
  } else {
    sbc->lineno_sz = 0;
    sbc->lineno_table = 0;
  }
  /* RT-7: D-side mcache count.  The 10th tot entry's "offset"
   * field is a format flag that must be 1 for the compact
   * 2-byte cache slot format.  The runtime no longer supports
   * the legacy 8-byte slot formats (pre-RT-7 8 SBC_NOPs, or
   * RT-7 stage 1's 2-byte id + 6 SBC_NOP filler); any SBC
   * without `tot_sz >= 10 && mcache_fmt == 1` is rejected. */
  if (tot_sz >= 10) {
    sbc->mcache_cnt = (uint32_t)RD24;
    uint32_t mcache_fmt = (uint32_t)RD24;
    if (mcache_fmt != 1) {
      fprintf(stderr, "sbc_load: %s has legacy cache slot "
              "format (fmt=%u) -- recompile.\n", path, mcache_fmt);
      free(sbc);
      return 0;
    }
  } else {
    fprintf(stderr, "sbc_load: %s has tot_sz=%u, requires >= 10 "
            "for RT-7 stage-2 cache format -- recompile.\n",
            path, tot_sz);
    free(sbc);
    return 0;
  }
  sbc->mcaches = 0; /* allocated lazily in sbc_prepare */
  /* HELP-3: docstring section (tot[10]).  Optional -- older SBCs
   * compiled before the format extension leave both fields zero. */
  if (tot_sz >= 11) {
    sbc->doc_sz = (uint32_t)RD24;
    uint32_t doc_ofs = (uint32_t)RD24;
    sbc->doc_table = sbc->doc_sz ? (sbc->tbls + doc_ofs) : 0;
  } else {
    sbc->doc_sz = 0;
    sbc->doc_table = 0;
  }
  /* NATIVE/IA64 section (tot[11]).  Optional -- present only on
   * SBCs emitted by an AOT-capable writer.  Count is the number
   * of functions described in the section (matches fntbl_sz/3
   * when populated).  Offset points to the section header inside
   * tbls.  The section header begins with magic "IA64" (4 bytes)
   * + version (2 bytes); sbc_prepare validates these on adopt. */
  if (tot_sz >= 12) {
    sbc->ia64_sz = (uint32_t)RD24;
    uint32_t ia64_ofs = (uint32_t)RD24;
    sbc->ia64_table = sbc->ia64_sz ? (sbc->tbls + ia64_ofs) : 0;
  } else {
    sbc->ia64_sz = 0;
    sbc->ia64_table = 0;
  }

  sbc->filename = strdup(path);
  sbc->mt = 0;
  sbc->hooks = 0;
  sbc->ready = 0;
  sbc->nfi_ctx = 0;
  sbc->nfi_trmps = 0;
  //fprintf(stderr, "%s:\n  %s\n", path,sbc->code+sbc->export_string);
  return sbc;
}

sbc_t *sbc_load(char *path) {
  int64_t size;
  uint8_t *pin = fs_get(&size, path);
  if (!pin) return 0;
  return sbc_new(pin, size, path);
}

void sbc_free(sbc_t *sbc) {
  if (sbc->ready) {
    free(sbc->mt);
    if (sbc->hooks) free(sbc->hooks);
    /* RT-7: D-side mcache array. */
    if (sbc->mcaches) free(sbc->mcaches);
    for (int i = 0; i < 7; i++) {
      //FIXME: ensure it gets unloaded from the runtime
      if (sbc->rtot[i].table) free(sbc->rtot[i].table);
    }
    /* sffi sigs were allocated one-per-FFI at sbc_prepare. Release
     * them here.  nfi_ctx is a vestigial pointer field on sbc_t
     * that's always NULL under sffi; we keep the field for ABI
     * compatibility with code that still reads it. */
    if (sbc->nfi_trmps) {
      for (int i = 0; i < arrlen(sbc->nfi_trmps); i++) {
        sffi_free((sffi_sig_t *)sbc->nfi_trmps[i]);
      }
      arrfree(sbc->nfi_trmps);
    }
  }
  if (sbc->filename) free(sbc->filename);
  free(sbc->file);
  free(sbc);
}

int sbc_load_metadata(char *filename, char **src, char **deps, char **export) {
  sbc_t *sbc = sbc_load(filename);
  if (!sbc) return 0;
  *src = strdup(sbc->code+sbc->src_file_string);
  *deps = strdup(sbc->code+sbc->deps_string);
  *export = strdup(sbc->code+sbc->export_string);
  sbc_free(sbc);
  return 1;
}

#ifdef NATIVE_HOOKS
//basically curries the 2nd argument of a 2-argument function
uint8_t *emit_hook(uint8_t *dst, void *target, void *payload) {
  //OSX functions use RDI to pass 1st argument, and RSI to pass 2nd
  uint8_t *wb = dst;
  EMIT8(0x48); //mov rax, fn
  EMIT8(0xB8);
  EMIT64((uint64_t*)target);

  EMIT8(0x48); //mov rsi, payload
  EMIT8(0xBE);
  EMIT64((uint64_t*)payload);

#if 0
  EMIT8(0x48); //mov rdi, payload
  EMIT8(0xBF);
  EMIT64((uint64_t*)payload);
#endif

#if 0
  //following can be useful, if we instead prefix the payload to the buf
  //or put the curry code before the interpreted function
  EMIT8(0xE8); //push ip
  EMIT32(0);
  EMIT8(0x5F);  //pop rdi
#endif

  EMIT8(0xFF);  //jmp rax
  EMIT8(0xE0);

  return wb;
}
#endif

#define MAX_HOOKS (1024*64)

hook_t hooks_heap[MAX_HOOKS];
hook_t *hooks_ptr = hooks_heap+2; //hook 0 is local scope closures

uint32_t sbc_hook(psf_t fn, uint8_t *payload) {
  uint32_t r = hooks_ptr-hooks_heap;

  FAIL(hooks_ptr+1 >= hooks_heap+MAX_HOOKS , "Hooks heap overflow\n");

  hooks_ptr->handler = fn;
  hooks_ptr->payload = payload;
  hooks_ptr->meta = 0;
  hooks_ptr++;
  return r;
}

void sbc_emit_hooks(sbc_t *sbc, int sbc_id) {
  uint8_t *pin = sbc->fntbl;
  int nfns = sbc->fntbl_sz/3;
  sbc->hooks = malloc(sizeof(uint64_t)*nfns);
  for (int i = 0; i < nfns; i++) {
    uint32_t ofs = RD24;
    uint8_t *fptr = sbc->code + ofs;
    fptr[1] = sbc_id&0xFF;      //patch subr
    fptr[2] = (sbc_id>>8)&0xFF;
    sbc->hooks[i] = sbc_hook(&sbc_exec_fn, fptr+1);
  }
  
  //make_executable(sbc->hooks_base,nfns*sizeof(hook_t));
}


void sbc_init_tables(sbc_t *sbc) {
  int i,j,k;
  tot_entry_t *rtot = sbc->rtot;

  uint8_t *totpin = sbc->tot;
  uint8_t *pin;
  char *tns[7] = {"fm","tx","ty","mt","libs","imlib","im"};
  int code_sz = sbc->code_sz;
  for (i = 0; i < 7; i++) {
    pin = totpin;
    int nentries = RD24;
    int tofs = RD24;
    totpin = pin;
    pin = sbc->tbls+tofs;
    rtot[i].size = nentries;
    if (i == 0) { //fnmeta?
      fn_meta_t *t = malloc(sizeof(fn_meta_t)*nentries);
      rtot[i].table = t;
      for (j = 0; j < nentries; j++) {
        uint32_t ofs;
        int64_t nargs = RD16;
        if (nargs == 0xFFFF) nargs = -1;
        t[j].nargs = FXN(nargs);
  
        ofs = RD24;
        if (ofs) t[j].name = sbc->data + ofs-code_sz;
        else t[j].name = 0;

        int idx = RD16;
        t[j].hook = (uint32_t)sbc->hooks[idx];


        //FIXME: upper col bit could specify if 
        //       src differs from the sbc->src_file_string
        t[j].row = RD16;
        t[j].col = RD16;

        if (t[j].col&SBC_DEFAULT_SRC) {
          ofs = sbc->src_file_string;
          t[j].col &= ~SBC_DEFAULT_SRC;
        } else {
          ofs = RD24;
        }
        if (ofs) t[j].origin = sbc->data + ofs-code_sz;
        else t[j].origin = 0;
      }
    } else {
      dyn *t = malloc(sizeof(dyn)*nentries);
      rtot[i].table = t;
      for (j = 0; j < nentries; j++) {
        if (i != 5) {
          uint32_t ofs = RD24;
          if (ofs) t[j] = sbc->data + ofs-code_sz;
          else t[j] = 0;
        } else {
          t[j] = (dyn)(uint64_t)RD16; //library index
        }
      }
    }
  }

  sbc_init_types(sbc, rtot);
}

#define SBCS_MAX 1024
/* Exposed to bltin.c so print_stack_trace can locate the SBC that
 * owns a frame's saved pin (for CORE-1 lineno lookup). */
int sbcs_loaded = 0;
sbc_t *sbcs[SBCS_MAX];

/* SBC_NFI_* opcode → sffi_type. Returns -1 on unrecognised. */
static int sbc2sffi(int id) {
  switch (id) {
  case SBC_NFI_VOID: return SFFI_VOID;
  case SBC_NFI_INT:  return SFFI_I32;
  case SBC_NFI_PTR:  return SFFI_PTR;
  case SBC_NFI_TXT:  return SFFI_PTR;  /* char* — same ABI as void* */
  case SBC_NFI_U4:   return SFFI_U32;
  case SBC_NFI_FLT:  return SFFI_F32;
  case SBC_NFI_DBL:  return SFFI_F64;
  }
  return -1;
}

void sbc_prepare(sbc_t *sbc) {
  int sbc_id = sbcs_loaded;
  if (sbc_id == SBCS_MAX) {
    fprintf(stderr, "sbc_prepare: sbcs[] overflow\n");
    exit(-1);
  }
  sbcs[sbcs_loaded++] = sbc;
  sbc_emit_hooks(sbc, sbc_id);

  /* sffi has no per-SBC context.  Keep the vestigial nfi_ctx
   * field NULL; nothing else reads it. */
  sbc->nfi_ctx = 0;

  /* Walk the per-FFI argument-type stream and bind a sffi sig for
   * each. Stored in sbc->nfi_trmps[] so SBC_NFI invocations can
   * grab the sig by index. */
  uint8_t *pin = sbc->nrs;
  for (int i = 0; i < sbc->nrs_sz; i++) {
    sffi_type arg_types[SFFI_MAX_ARGS];
    int n_args = 0;
    int ofs = RD24;
    uint8_t *p = sbc->code + ofs;
    uint8_t *e = sbc->code + sbc->code_sz;
    while (*p != SBC_NFI && p < e) {
      int t = sbc2sffi(*p);
      if (t < 0 || n_args >= SFFI_MAX_ARGS) {
        fprintf(stderr, "sbc_prepare: bad SBC_NFI arg type 0x%02x "
                        "in %s\n", *p, sbc->filename);
        exit(-1);
      }
      arg_types[n_args++] = (sffi_type)t;
      p += 3;
    }
    p += 5;
    int rt = sbc2sffi(*p);
    if (rt < 0) {
      fprintf(stderr, "sbc_prepare: bad SBC_NFI return type 0x%02x "
                      "in %s\n", *p, sbc->filename);
      exit(-1);
    }
    sffi_sig_t *sig = sffi_bind((sffi_type)rt, n_args, arg_types);
    if (!sig) {
      fprintf(stderr, "sbc_prepare: sffi_bind failed for FFI %d "
                      "in %s\n", i, sbc->filename);
      exit(-1);
    }
    arrput(sbc->nfi_trmps, sig);
  }

  sbc_init_tables(sbc);

  /* RT-7: allocate the per-SBC D-side mcache array.  Calloc so
   * every slot starts with `node == NULL`, matching the
   * MCACHE_CALL miss path's "cold cache" condition.  We always
   * allocate at least one slot so pre-RT-7 SBCs -- which have
   * `mcache_cnt == 0` because their tot_sz didn't include the
   * RT-7 entry -- can still execute (every call site reads
   * `id=0` from the leading two SBC_NOP bytes of the old cache
   * padding and indexes that single slot, megamorphic and
   * slow but correct). */
  uint32_t slots = sbc->mcache_cnt ? sbc->mcache_cnt : 1;
  sbc->mcaches = calloc(slots, sizeof(mcache_t));
  if (!sbc->mcaches) {
    fprintf(stderr, "sbc_prepare: out of memory for %u mcache slots\n"
           ,slots);
    exit(-1);
  }

  /* Step 5d: optional JIT translatability audit.  Gated on the
   * env var so a default `make test-drift` run doesn't print
   * extra noise.  Use `SYMTA_JIT_AUDIT=1 ./symta.exe ...` to see
   * per-loaded-SBC stats on stderr. */
  if (getenv("SYMTA_JIT_AUDIT")) {
    fprintf(stderr, "jit-audit: %s -- ", sbc->filename);
    sbc_jit_audit(sbc);
  }

  /* Step 5k: install JIT'd hooks if SYMTA_JIT_RUN is set.
   * Separate flag from SYMTA_JIT_AUDIT so the audit can be run
   * (measurement only) without exposing dispatch to JIT'd code.
   * Setting both runs audit first, then install. */
  if (getenv("SYMTA_JIT_RUN")) {
    sbc_jit_install(sbc);
  }

  sbc->ready = 1;
}

#if 1
#define CHKREG(x) 
#else
#define CHKREG(x) \
  if ((x)>=nvars) do { \
    fprintf(stderr,"%s:%d: regs overflow, %d >= %d\n"\
      ,__FILE__,__LINE__,x,nvars); \
    fprintf(stderr,"sbc file:%06x: %s\n", (int)(pin-sbc->code), sbc->filename); \
    volatile int x = 0; \
    volatile int y = 1; \
    y /= x; \
    exit(-1); \
  } while(0)
#endif


//#define MCACHE_PROFILE

#ifdef MCACHE_PROFILE
int64_t mcache_misses = 0;
int64_t mcache_hits = 0;
#define MCACHE_HIT do { \
    if ((++mcache_hits % 100000) == 0) \
      printf("hits %lld: %06x\n", mcache_hits, ip); \
 } while (0);
#define MCACHE_MISS do { \
    if ((++mcache_misses % 100000) == 0) \
      printf("misses %lld: %06x\n", mcache_misses, ip); \
 } while (0);
#else
#define MCACHE_HIT
#define MCACHE_MISS
#endif

/*
  FIXME: implement a probation scheme, where mcalls whose object type changes
         frequently get patched with normal MCALLS.
         i.e. maintain ration of hits to misses.
*/

/* RT-7: the cache used to sit inline in the bytecode (8 bytes
 * of `mcache_t` after each cache-bearing opcode's args), and
 * `mce->node = node` was a write into the bytecode stream
 * itself.  Now the cache lives in a separate D-side
 * `sbc->mcaches[]` array, indexed by a single `uint16_t` id
 * stored at each cache site -- 2 bytes total. */
#define MCACHE_SKIP pin += 2

#ifdef SBC_MCACHE

//#define MCACHE_CALL(k,o,_mid) MCALL(k,o,_mid)
/* The cache hit check has to validate the cached entry belongs
 * to the same `(method_id, type_id)` pair as the current call.
 * Pre-RT-7 SBCs run with `mcache_cnt == 0` -> a single fallback
 * mcache slot that every cache site collides on, so a stale
 * cached entry from a different `m` could otherwise satisfy a
 * tid-only check and dispatch the wrong method.  For RT-7-
 * compiled SBCs each site has its own slot, so `mce->mid == m`
 * is always true on a hit -- the extra comparison costs ~1
 * cycle per call and the branch predicts cleanly.
 *
 * RT-6b: the cache now carries `(tid, mid, fn)` inline (16 B,
 * one cache line) instead of a `method_node_t*` indirection.
 * Hit path is one load of fn; miss path probes the per-type
 * slot table via get_method_for_tag and writes the triple. */
#define MCACHE_CALL(k,o,_mid) do {            \
    int m = _mid;                            \
    api.method = m;                         \
    dyn oo = o;                             \
    uint32_t _mid_idx = (uint32_t)RD16;     \
    mcache_t *mce = &sbc->mcaches[_mid_idx];\
    uint32_t tid = O_TAG(o);                \
    dyn mfn;                                \
    if (mce->tid != tid || mce->mid != (uint32_t)m) { \
      mfn = get_method_for_tag(m,tid);      \
      mce->tid = tid;                       \
      mce->mid = (uint32_t)m;               \
      mce->fn = mfn;                        \
      MCACHE_MISS                           \
    } else {                                \
      mfn = mce->fn;                        \
      MCACHE_HIT                            \
    }                                       \
    CALL(k,mfn);                            \
  } while (0)
#else
#define MCACHE_CALL(k,o,mid) MCALL(k,o,mid)
#endif



int sbcd = 0; //SBC debug

int sbc_opc;
int64_t sbc_icount;

/* RT-1: bytecode dispatch -- switch(RD8) vs computed gotos.
 *
 * Define SBC_THREADED_DISPATCH at build time (e.g. via -D) to
 * compile the threaded variant: one indirect branch per opcode
 * (`goto *dt[*pin++]`) instead of one centralised indirect in
 * the switch's jump table. Empirically ~10-25 % faster on tight
 * inner loops because each opcode gets its own branch-predictor
 * entry for the *next* opcode (the predictor learns common
 * opcode-to-opcode transitions like `FXNADD -> ST_4_x`). See
 * benchmark/rt/ for measurements.
 *
 * The case bodies are byte-identical between modes; the OP()
 * and BREAK macros bridge the syntactic difference (case label
 * vs goto label, break vs goto-next). The nested switch inside
 * SBC_NFI's return-type handling is left as a real switch in
 * both modes -- BREAK inside it does the right thing in either
 * dispatch flavour (exits via inner break, then outer break or
 * jumps to next outer opcode directly).
 *
 * Default is the portable switch dispatch; threaded mode needs
 * GCC's computed-goto extension (works on gcc/clang/MinGW; not
 * MSVC -- but Symta ships on w64devkit so that's a non-issue).
 */
#ifdef SBC_THREADED_DISPATCH
  #define OP(op)    L_##op:
  #define BREAK     goto *dt[*pin++]
  #define DEFAULT   L_default:
#else
  #define OP(op)    case op:
  #define BREAK     break
  #define DEFAULT   default:
#endif

dyn sbc_exec_fn(uint8_t *pin) {
#if 0
  if (pin[-1] != SBC_SUBR) {
    fatal("sbc_exec_fn: first opcode is not SUBR");
  }
#endif

  //FIXME: it would be more efficient to store the sbc_t * pointer
  sbc_t *sbc = sbcs[RD16];

  int nvars = RD16;
  SUBR(nvars);

#ifdef SBC_THREADED_DISPATCH
  /* Per-opcode jump table.  Holes default to L_default (the
   * `bad opcode` handler).  Order doesn't matter, but we keep
   * it close to the enum order in sif.h for readability. */
  static void *const dt[256] = {
    [0 ... 255] = &&L_default,
    [SBC_NOP] = &&L_SBC_NOP,
    [SBC_LEAVE0] = &&L_SBC_LEAVE0,
    [SBC_LEAVEN] = &&L_SBC_LEAVEN,
    [SBC_LEAVE] = &&L_SBC_LEAVE,
    [SBC_JMP] = &&L_SBC_JMP,
    [SBC_JMP16] = &&L_SBC_JMP16,
    [SBC_B] = &&L_SBC_B,
    [SBC_B8] = &&L_SBC_B8,
    [SBC_IFFXN] = &&L_SBC_IFFXN,
    [SBC_CALL] = &&L_SBC_CALL,
    [SBC_CALLIR] = &&L_SBC_CALLIR,
    [SBC_CALLT] = &&L_SBC_CALLT,
    [SBC_CALLTIR] = &&L_SBC_CALLTIR,
    [SBC_TCALL] = &&L_SBC_TCALL,
    [SBC_MCALL] = &&L_SBC_MCALL,
    [SBC_MCALLIR] = &&L_SBC_MCALLIR,
    [SBC_MCALL8] = &&L_SBC_MCALL8,
    [SBC_TMCALL] = &&L_SBC_TMCALL,
    [SBC_CLOSURE] = &&L_SBC_CLOSURE,
    [SBC_OBJECT] = &&L_SBC_OBJECT,
    [SBC_CNAS] = &&L_SBC_CNAS,
    [SBC_ARGLIST] = &&L_SBC_ARGLIST,
    [SBC_ARGLIST8] = &&L_SBC_ARGLIST8,
    [SBC_ARGLIST0] = &&L_SBC_ARGLIST0,
    [SBC_ARGLIST1] = &&L_SBC_ARGLIST1,
    [SBC_ARGLIST2] = &&L_SBC_ARGLIST2,
    [SBC_ARGLIST3] = &&L_SBC_ARGLIST3,
    [SBC_ARGLIST4] = &&L_SBC_ARGLIST4,
    [SBC_ARGLIST5] = &&L_SBC_ARGLIST5,
    [SBC_LIST] = &&L_SBC_LIST,
    [SBC_MOVEEMT] = &&L_SBC_MOVEEMT,
    [SBC_MOVENO] = &&L_SBC_MOVENO,
    [SBC_MOVETX] = &&L_SBC_MOVETX,
    [SBC_MOVETX8] = &&L_SBC_MOVETX8,
    [SBC_MOVEMT] = &&L_SBC_MOVEMT,
    [SBC_MOVEMT8] = &&L_SBC_MOVEMT8,
    [SBC_MOVEIM] = &&L_SBC_MOVEIM,
    [SBC_MOVE] = &&L_SBC_MOVE,
    [SBC_MOVE8] = &&L_SBC_MOVE8,
    [SBC_MOVE4] = &&L_SBC_MOVE4,
    [SBC_STOR] = &&L_SBC_STOR,
    [SBC_STOR8] = &&L_SBC_STOR8,
    [SBC_ST4_0] = &&L_SBC_ST4_0,
    [SBC_ST4_1] = &&L_SBC_ST4_1,
    [SBC_ST4_2] = &&L_SBC_ST4_2,
    [SBC_ST4_3] = &&L_SBC_ST4_3,
    [SBC_ST4_4] = &&L_SBC_ST4_4,
    [SBC_ST4_5] = &&L_SBC_ST4_5,
    [SBC_ST4_6] = &&L_SBC_ST4_6,
    [SBC_ST4_7] = &&L_SBC_ST4_7,
    [SBC_ST4_8] = &&L_SBC_ST4_8,
    [SBC_ST4_9] = &&L_SBC_ST4_9,
    [SBC_ST4_A] = &&L_SBC_ST4_A,
    [SBC_ST4_B] = &&L_SBC_ST4_B,
    [SBC_ST4_C] = &&L_SBC_ST4_C,
    [SBC_ST4_D] = &&L_SBC_ST4_D,
    [SBC_ST4_E] = &&L_SBC_ST4_E,
    [SBC_ST4_F] = &&L_SBC_ST4_F,
    [SBC_LOAD] = &&L_SBC_LOAD,
    [SBC_LOAD8] = &&L_SBC_LOAD8,
    [SBC_LD4_0] = &&L_SBC_LD4_0,
    [SBC_LD4_1] = &&L_SBC_LD4_1,
    [SBC_LD4_2] = &&L_SBC_LD4_2,
    [SBC_LD4_3] = &&L_SBC_LD4_3,
    [SBC_LD4_4] = &&L_SBC_LD4_4,
    [SBC_LD4_5] = &&L_SBC_LD4_5,
    [SBC_LD4_6] = &&L_SBC_LD4_6,
    [SBC_LD4_7] = &&L_SBC_LD4_7,
    [SBC_LD4_8] = &&L_SBC_LD4_8,
    [SBC_LD4_9] = &&L_SBC_LD4_9,
    [SBC_LD4_A] = &&L_SBC_LD4_A,
    [SBC_LD4_B] = &&L_SBC_LD4_B,
    [SBC_LD4_C] = &&L_SBC_LD4_C,
    [SBC_LD4_D] = &&L_SBC_LD4_D,
    [SBC_LD4_E] = &&L_SBC_LD4_E,
    [SBC_LD4_F] = &&L_SBC_LD4_F,
    [SBC_COPY] = &&L_SBC_COPY,
    [SBC_FXNB0] = &&L_SBC_FXNB0,
    [SBC_FXNB8] = &&L_SBC_FXNB8,
    [SBC_FXNB16] = &&L_SBC_FXNB16,
    [SBC_FXNB32] = &&L_SBC_FXNB32,
    [SBC_FXN0] = &&L_SBC_FXN0,
    [SBC_FXN8] = &&L_SBC_FXN8,
    [SBC_FXN16] = &&L_SBC_FXN16,
    [SBC_FXN32] = &&L_SBC_FXN32,
    [SBC_IMMB64] = &&L_SBC_IMMB64,
    [SBC_IMM64] = &&L_SBC_IMM64,
    [SBC_FXT8] = &&L_SBC_FXT8,
    [SBC_FXT16] = &&L_SBC_FXT16,
    [SBC_FXT24] = &&L_SBC_FXT24,
    [SBC_FXT32] = &&L_SBC_FXT32,
    [SBC_FXT40] = &&L_SBC_FXT40,
    [SBC_FXT48] = &&L_SBC_FXT48,
    [SBC_FXT56] = &&L_SBC_FXT56,
    [SBC_FXNTAG] = &&L_SBC_FXNTAG,
    [SBC_FXNLISTN] = &&L_SBC_FXNLISTN,
    [SBC_FXNLGET] = &&L_SBC_FXNLGET,
    [SBC_FXNLSET] = &&L_SBC_FXNLSET,
    [SBC_FXNLSETIR] = &&L_SBC_FXNLSETIR,
    [SBC_FXNSIZE] = &&L_SBC_FXNSIZE,
    [SBC_ABS] = &&L_SBC_ABS,
    [SBC_NEG] = &&L_SBC_NEG,
    [SBC_FXNADD] = &&L_SBC_FXNADD,
    [SBC_FXNSUB] = &&L_SBC_FXNSUB,
    [SBC_FXNMUL] = &&L_SBC_FXNMUL,
    [SBC_FXNDIV] = &&L_SBC_FXNDIV,
    [SBC_FXNREM] = &&L_SBC_FXNREM,
    [SBC_IMMEQ] = &&L_SBC_IMMEQ,
    [SBC_IMMNE] = &&L_SBC_IMMNE,
    [SBC_FXNLT] = &&L_SBC_FXNLT,
    [SBC_FXNGT] = &&L_SBC_FXNGT,
    [SBC_FXNLTE] = &&L_SBC_FXNLTE,
    [SBC_FXNGTE] = &&L_SBC_FXNGTE,
    [SBC_FXNAND] = &&L_SBC_FXNAND,
    [SBC_FXNIOR] = &&L_SBC_FXNIOR,
    [SBC_FXNXOR] = &&L_SBC_FXNXOR,
    [SBC_FXNSHL] = &&L_SBC_FXNSHL,
    [SBC_FXNSHR] = &&L_SBC_FXNSHR,
    [SBC_SAME] = &&L_SBC_SAME,
    [SBC_VARY] = &&L_SBC_VARY,
    [SBC_LIST2] = &&L_SBC_LIST2,
    [SBC_LIST1] = &&L_SBC_LIST1,
    [SBC_TINIT] = &&L_SBC_TINIT,
    [SBC_TINITI] = &&L_SBC_TINITI,
    [SBC_SUBTYPE] = &&L_SBC_SUBTYPE,
    [SBC_DMET] = &&L_SBC_DMET,
    [SBC_INC] = &&L_SBC_INC,
    [SBC_DEC] = &&L_SBC_DEC,
    [SBC_NOT] = &&L_SBC_NOT,
    [SBC_GOT] = &&L_SBC_GOT,
    [SBC_NO] = &&L_SBC_NO,
    [SBC_GID] = &&L_SBC_GID,
    [SBC_CURMET] = &&L_SBC_CURMET,
    [SBC_MNAME] = &&L_SBC_MNAME,
    [SBC_FATAL] = &&L_SBC_FATAL,
    [SBC_GC0] = &&L_SBC_GC0,
    [SBC_GC1] = &&L_SBC_GC1,
    [SBC_NFI_INT] = &&L_SBC_NFI_INT,
    [SBC_NFI_PTR] = &&L_SBC_NFI_PTR,
    [SBC_NFI_TXT] = &&L_SBC_NFI_TXT,
    [SBC_NFI_U4] = &&L_SBC_NFI_U4,
    [SBC_NFI_FLT] = &&L_SBC_NFI_FLT,
    [SBC_NFI_DBL] = &&L_SBC_NFI_DBL,
    [SBC_NFI] = &&L_SBC_NFI,
    [SBC_NLDU1] = &&L_SBC_NLDU1,
    [SBC_NLDU2] = &&L_SBC_NLDU2,
    [SBC_NLDU4] = &&L_SBC_NLDU4,
    [SBC_NLDS1] = &&L_SBC_NLDS1,
    [SBC_NLDS2] = &&L_SBC_NLDS2,
    [SBC_NLDS4] = &&L_SBC_NLDS4,
    [SBC_NSTU1] = &&L_SBC_NSTU1,
    [SBC_NSTU2] = &&L_SBC_NSTU2,
    [SBC_NSTU4] = &&L_SBC_NSTU4,
    [SBC_NSTS1] = &&L_SBC_NSTS1,
    [SBC_NSTS2] = &&L_SBC_NSTS2,
    [SBC_NSTS4] = &&L_SBC_NSTS4,
    [SBC_CTX] = &&L_SBC_CTX,
    /* TS-4.1/4.2: typed unboxed-arithmetic opcodes are placed at
     * the END of the dispatch table so their goto-label addresses
     * cluster on their own cache lines.  The interleaved layout
     * (originally grouped next to SBC_FXNSHR) made the typed-int
     * hot path share a 64-byte line with the FXN fallbacks; the
     * generic-mexed code that runs untyped arithmetic was then
     * paying L1d misses against the typed code.  The same
     * separation principle applies to the OP() handler bodies
     * further down -- see end of switch. */
    [SBC_IADD] = &&L_SBC_IADD,
    [SBC_ISUB] = &&L_SBC_ISUB,
    [SBC_IMUL] = &&L_SBC_IMUL,
    [SBC_IDIV] = &&L_SBC_IDIV,
    [SBC_IREM] = &&L_SBC_IREM,
    [SBC_ILT]  = &&L_SBC_ILT,
    [SBC_IGT]  = &&L_SBC_IGT,
    [SBC_ILTE] = &&L_SBC_ILTE,
    [SBC_IGTE] = &&L_SBC_IGTE,
    [SBC_FADD] = &&L_SBC_FADD,
    [SBC_FSUB] = &&L_SBC_FSUB,
    [SBC_FMUL] = &&L_SBC_FMUL,
    [SBC_FDIV] = &&L_SBC_FDIV,
  };
  goto *dt[*pin++];
#else
  for (;;) {
  switch(RD8) {
#endif
  OP(SBC_NOP) {BREAK;}
  OP(SBC_LEAVE0) return (dyn)0;
  OP(SBC_LEAVEN) {return FXN((int64_t)RD16);}
  OP(SBC_LEAVE) {
    int src = RD16;
    //fprintf(stderr, "leave: %d\n", src);
    CHKREG(src);
    return L[src];}
  OP(SBC_JMP) {pin = sbc->code+RD24; BREAK;}
  OP(SBC_JMP16) {
    int16_t diff = (int16_t)RD16;
    pin += diff;
    BREAK;}
  OP(SBC_B) {
    uint32_t cnd = RD16;
    uint32_t ofs = RD24;
    CHKREG(cnd);
    if (L[cnd]) pin = sbc->code+ofs;
    BREAK;}
  OP(SBC_B8) {
    uint32_t cnd = RD8;
    int16_t diff = (int16_t)RD16;
    CHKREG(cnd);
    if (L[cnd]) pin += diff;
    BREAK;}
  OP(SBC_IFFXN) {
    uint32_t cnd = RD8;
    int16_t diff = (int16_t)RD16;
    CHKREG(cnd);
    if (!O_TAG(L[cnd])) pin += diff;
    BREAK;}
  /* CORE-1: every Symta-level CALL site saves `pin` into the
   * caller's frame so a later stack trace can binary-search the
   * caller's lineno table for the (row, col) of the call.  Pin
   * here is just past the encoded args (the call instruction is
   * the last instruction before pin), which is the position the
   * interpreter will resume at when the callee returns.  Lineno
   * lookup tolerates this by taking "the most recent entry at or
   * before pin". */
  OP(SBC_CALL) {
    int dst = RD16;
    int fn = RD16;
    CHKREG(dst);
    CHKREG(fn);
    api.frame->pin = pin;
    CALL(L[dst],L[fn]);
    BREAK;}
  OP(SBC_CALLIR) {
    int fn = RD16;
    dyn dummy;
    CHKREG(fn);
    api.frame->pin = pin;
    CALL(dummy,L[fn]);
    BREAK;}
  OP(SBC_CALLT) {
    int dst = RD16;
    int fn = RD16;
    CHKREG(dst);
    CHKREG(fn);
    api.frame->pin = pin;
    CALL_TAGGED(L[dst],L[fn]);
    BREAK;}
  OP(SBC_CALLTIR) {
    int fn = RD16;
    dyn dummy;
    CHKREG(fn);
    api.frame->pin = pin;
    CALL_TAGGED(dummy,L[fn]);
    BREAK;}
  OP(SBC_TCALL) {
    int fn = RD16;
    dyn *r;
    CHKREG(fn);
    api.frame->pin = pin;
    CALL(r,L[fn]);
    //FIXME: instead check that thunk handler is sbc_exec_fn and jump to the
    //       beginning
    return r;
    BREAK;}
  OP(SBC_MCALL) {
    int dst = RD16;
    int obj = RD16;
    int met = RD16;
    CHKREG(dst);
    CHKREG(obj);
    api.frame->pin = pin;
    MCACHE_CALL(L[dst],L[obj],sbc->mt[met]);
    BREAK;}
  OP(SBC_MCALLIR) {
    dyn dummy;
    int obj = RD16;
    int met = RD16;
    CHKREG(obj);
    api.frame->pin = pin;
    MCACHE_CALL(dummy,L[obj],sbc->mt[met]);
    BREAK;}
  OP(SBC_MCALL8) {
    int dst = RD8;
    int obj = RD8;
    int met = RD8;
    CHKREG(dst);
    CHKREG(obj);
    api.frame->pin = pin;
    MCACHE_CALL(L[dst],L[obj],sbc->mt[met]);
    BREAK;}
  OP(SBC_TMCALL) {
    int obj = RD16;
    int met = RD16;
    dyn *r;
    CHKREG(obj);
    api.frame->pin = pin;
    MCACHE_CALL(r,L[obj],sbc->mt[met]);
    //FIXME: instead check that thunk handler is sbc_exec_fn and jump to the
    //       beginning
    return r;
    BREAK;}
  OP(SBC_CLOSURE) {
    uint32_t dst = RD16;
    uint32_t idx = RD16;
    uint32_t size = RD8;
    CHKREG(dst);
    void *fn = (void*)sbc->hooks[idx];
    CLOSURE(L[dst],fn,size);
    BREAK;}
  OP(SBC_OBJECT) {
    int dst = RD16;
    int tid = RD16;
    int size = RD16;
    CHKREG(dst);
    if (size) {
      OBJECT(L[dst],sbc->ty[tid],size);
    } else {
      L[dst] = MKIMM(sbc->ty[tid],0);
    }
    BREAK;}
  OP(SBC_CNAS) {
    uint32_t expected = RD16;
    CHECK_NARGS(expected);
    BREAK;}
  OP(SBC_ARGLIST) {
    int size = RD16;
    ARGLIST(size);
    for (int j = 0; j < size; j++) {
      int src = RD16;
      CHKREG(src);
      STARG(j,L[src]);
    }
    BREAK;}
  OP(SBC_ARGLIST8) {
    int size = RD8;
    ARGLIST(size);
    for (int j = 0; j < size; j++) {
      int src = RD8;
      CHKREG(src);
      STARG(j,L[src]);
    }
    BREAK;}
  OP(SBC_ARGLIST0) {
    ARGLIST(0);
    BREAK;}
  OP(SBC_ARGLIST1) {
    int a;
    ARGLIST(1);
    a = RD8;
    CHKREG(a);
    STARG(0,L[a]);
    BREAK;}
  OP(SBC_ARGLIST2) {
    int a;
    ARGLIST(2);
    a = RD8;
    CHKREG(a);
    STARG(0,L[a]);
    a = RD8;
    CHKREG(a);
    STARG(1,L[a]);
    BREAK;}
  OP(SBC_ARGLIST3) {
    int a;
    ARGLIST(3);
    a = RD8;
    CHKREG(a);
    STARG(0,L[a]);
    a = RD8;
    CHKREG(a);
    STARG(1,L[a]);
    a = RD8;
    CHKREG(a);
    STARG(2,L[a]);
    BREAK;}
  OP(SBC_ARGLIST4) {
    int a;
    ARGLIST(4);
    a = RD8;
    CHKREG(a);
    STARG(0,L[a]);
    a = RD8;
    CHKREG(a);
    STARG(1,L[a]);
    a = RD8;
    CHKREG(a);
    STARG(2,L[a]);
    a = RD8;
    CHKREG(a);
    STARG(3,L[a]);
    BREAK;}
  OP(SBC_ARGLIST5) {
    int a;
    ARGLIST(5);
    a = RD8;
    CHKREG(a);
    STARG(0,L[a]);
    a = RD8;
    CHKREG(a);
    STARG(1,L[a]);
    a = RD8;
    CHKREG(a);
    STARG(2,L[a]);
    a = RD8;
    CHKREG(a);
    STARG(3,L[a]);
    a = RD8;
    CHKREG(a);
    STARG(4,L[a]);
    BREAK;}
  OP(SBC_LIST) {
    uint32_t dst = RD16;
    uint32_t size = RD16;
    CHKREG(dst);
    LIST(L[dst],size);
    BREAK;}
  OP(SBC_LIST2) {
    /* RT-9: fused size-2 list allocation.  Replaces the 3-opcode
     * sequence SBC_LIST 2 + SBC_ST4_0 + SBC_ST4_1 used to emit
     * `[A B]` literals.  Args: dst u16, a u16, b u16 (8 B total
     * vs 4 + 2 + 2 = 8 B for the old sequence -- code size
     * unchanged, but dispatch count drops 3:1, and for the
     * 130 M size-2 LISTs in a ./game compile that's ~260 M
     * fewer opcode dispatches). */
    uint32_t dst = RD16;
    uint32_t a = RD16;
    uint32_t b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    LIST(L[dst], 2);
    LGET(L[dst], 0) = L[a];
    LGET(L[dst], 1) = L[b];
    BREAK;}
  OP(SBC_LIST1) {
    /* RT-9: fused size-1 list allocation.  Replaces SBC_LIST 1 +
     * SBC_ST4_0 for `[X]` literals.  Args: dst u16, x u16. */
    uint32_t dst = RD16;
    uint32_t x = RD16;
    CHKREG(dst); CHKREG(x);
    LIST(L[dst], 1);
    LGET(L[dst], 0) = L[x];
    /* RT-9 measurement: count what element type ends up in
     * size-1 LISTs.  Tells us which `T_LIST1_<tag>` tagged-
     * immediate variants would be worth implementing. */
    {
      uint64_t etag = O_TAG(L[x]);
      if (etag < 32) alloc_stats.list1_elem_tag[etag]++;
    }
    BREAK;}
  OP(SBC_MOVEEMT) {
    int dst = RD16;
    CHKREG(dst);
    L[dst] = Empty;
    BREAK;}
  OP(SBC_MOVENO) {
    int dst = RD16;
    CHKREG(dst);
    L[dst] = No;
    BREAK;}
  OP(SBC_MOVETX) {
    int dst = RD16;
    int src = RD24;
    CHKREG(dst);
    L[dst] = sbc->tx[src];
    BREAK;}
  OP(SBC_MOVETX8) {
    int dst = RD8;
    int src = RD8;
    CHKREG(dst);
    L[dst] = sbc->tx[src];
    BREAK;}
  OP(SBC_MOVEMT) { //move method
    int dst = RD16;
    int src = RD24;
    CHKREG(dst);
    L[dst] = FXN(sbc->mt[src]);
    BREAK;}
  OP(SBC_MOVEMT8) { //move method
    int dst = RD8;
    int src = RD8;
    CHKREG(dst);
    L[dst] = FXN(sbc->mt[src]);
    BREAK;}
  OP(SBC_MOVEIM) { //move imported
    int dst = RD16;
    int src = RD24;
    CHKREG(dst);
    L[dst] = sbc->im[src];
    BREAK;}
  OP(SBC_MOVE) { //move local to local
    int dst = RD16;
    int src = RD16;
    CHKREG(dst);
    CHKREG(src);
    L[dst] = L[src];
    BREAK;}
  OP(SBC_MOVE8) { //move local to local
    int dst = RD8;
    int src = RD8;
    CHKREG(dst);
    CHKREG(src);
    L[dst] = L[src];
    BREAK;}
  OP(SBC_MOVE4) { //move local to local
    int opr = RD8;
    int dst = opr&0xF;
    int src = opr>>4;
    CHKREG(dst);
    CHKREG(src);
    L[dst] = L[src];
    BREAK;}
  OP(SBC_STOR) {
    int dst = RD16;
    int src = RD16;
    int index = RD16;
    CHKREG(dst);
    CHKREG(src);
    STOR(L[dst],index,L[src]);
    BREAK;}
  OP(SBC_STOR8) {
    int dst = RD8;
    int src = RD8;
    int index = RD8;
    CHKREG(dst);
    CHKREG(src);
    STOR(L[dst],index,L[src]);
    BREAK;}
  OP(SBC_ST4_0)
  OP(SBC_ST4_1)
  OP(SBC_ST4_2)
  OP(SBC_ST4_3)
  OP(SBC_ST4_4)
  OP(SBC_ST4_5)
  OP(SBC_ST4_6)
  OP(SBC_ST4_7)
  OP(SBC_ST4_8)
  OP(SBC_ST4_9)
  OP(SBC_ST4_A)
  OP(SBC_ST4_B)
  OP(SBC_ST4_C)
  OP(SBC_ST4_D)
  OP(SBC_ST4_E)
  OP(SBC_ST4_F) {
    int index = pin[-1]-SBC_ST4_0;
    int opr = RD8;
    int dst = opr&0xF;
    int src = opr>>4;
    CHKREG(dst);
    CHKREG(src);
    STOR(L[dst],index,L[src]);
    BREAK;}
  OP(SBC_LOAD) {
    int dst = RD16;
    int src = RD16;
    int index = RD16;
    CHKREG(dst);
    CHKREG(src);
    LOAD(L[dst],L[src],index);
    BREAK;}
  OP(SBC_LOAD8) {
    int dst = RD8;
    int src = RD8;
    int index = RD8;
    CHKREG(dst);
    CHKREG(src);
    LOAD(L[dst],L[src],index);
    BREAK;}
  OP(SBC_LD4_0)
  OP(SBC_LD4_1)
  OP(SBC_LD4_2)
  OP(SBC_LD4_3)
  OP(SBC_LD4_4)
  OP(SBC_LD4_5)
  OP(SBC_LD4_6)
  OP(SBC_LD4_7)
  OP(SBC_LD4_8)
  OP(SBC_LD4_9)
  OP(SBC_LD4_A)
  OP(SBC_LD4_B)
  OP(SBC_LD4_C)
  OP(SBC_LD4_D)
  OP(SBC_LD4_E)
  OP(SBC_LD4_F) {
    int index = pin[-1]-SBC_LD4_0;
    int opr = RD8;
    int dst = opr&0xF;
    int src = opr>>4;
    CHKREG(dst);
    CHKREG(src);
    LOAD(L[dst],L[src],index);
    BREAK;}
  OP(SBC_COPY) {
    int dst = RD16;
    int src = RD16;
    int dindex = RD16;
    int sindex = RD16;
    CHKREG(dst);
    CHKREG(src);
    COPY(L[dst],dindex,L[src],sindex);
    BREAK;}
  OP(SBC_FXNB0) {
    int dst = RD8;
    CHKREG(dst);
    L[dst] = 0;
    BREAK;}
  OP(SBC_FXNB8) {
    int dst = RD8;
    uint64_t imm = RD8;
    CHKREG(dst);
    L[dst] = FXN((int64_t)(int8_t)(uint8_t)imm);
    BREAK;}
  OP(SBC_FXNB16) {
    int dst = RD8;
    uint64_t imm = RD16;
    CHKREG(dst);
    L[dst] = FXN((int64_t)(int16_t)(uint16_t)imm);
    BREAK;}
  OP(SBC_FXNB32) {
    int dst = RD8;
    uint64_t imm = RD32;
    CHKREG(dst);
    L[dst] = FXN((int64_t)(int32_t)(uint32_t)imm);
    BREAK;}
  OP(SBC_FXN0) {L[RD16] = 0; BREAK;}
  OP(SBC_FXN8) {
    int dst = RD16;
    uint64_t imm = RD8;
    CHKREG(dst);
    L[dst] = FXN((int64_t)(int8_t)(uint8_t)imm);
    BREAK;}
  OP(SBC_FXN16) {
    int dst = RD16;
    uint64_t imm = RD16;
    CHKREG(dst);
    L[dst] = FXN((int64_t)(int16_t)(uint16_t)imm);
    BREAK;}
  OP(SBC_FXN32) {
    int dst = RD16;
    uint64_t imm = RD32;
    CHKREG(dst);
    L[dst] = FXN((int64_t)(int32_t)(uint32_t)imm);
    BREAK;}
  OP(SBC_IMMB64) {
    int dst = RD8;
    CHKREG(dst);
#ifndef SBC_TRANSITION
    L[dst] = (dyn)RD64;
#else
    uint64_t imm = RD64;
    uint64_t tag = O_TAG(imm);
    if (tag == T_INT) {
      int64_t val = UNFXN(imm);
      L[dst] = FXN(val);
    } else if (tag == T_FLOAT) {
      float fval;
      STFLT(fval,imm);
      LDFLT(L[dst], fval);
    } else { // T_FIXTEXT
      char tmp[16];
      fixtext_decode(tmp, (dyn)imm);
      TEXT(L[dst], tmp);
    }
#endif
    BREAK;}
  OP(SBC_IMM64) {
    int dst = RD16;
    CHKREG(dst);
#ifndef SBC_TRANSITION
    L[dst] = (dyn)RD64;
#else
    uint64_t imm = RD64;
    uint64_t tag = O_TAG(imm);
    if (tag == T_INT) {
      int64_t val = UNFXN(imm);
      L[dst] = FXN(val);
    } else if (tag == T_FLOAT) {
      float fval;
      STFLT(fval,imm);
      LDFLT(L[dst], fval);
    } else { // T_FIXTEXT
      char tmp[16];
      fixtext_decode(tmp, (dyn)imm);
      TEXT(L[dst], tmp);
    }
#endif
    BREAK;}
  OP(SBC_FXT8) {
    int dst = RD8;
    uint64_t imm = RD8;
    CHKREG(dst);
    L[dst] = (dyn)FIXTEXT((uint64_t)imm);
    BREAK;}
  OP(SBC_FXT16) {
    int dst = RD8;
    uint64_t imm = RD16;
    CHKREG(dst);
    L[dst] = (dyn)FIXTEXT((uint64_t)imm);
    BREAK;}
  OP(SBC_FXT24) {
    int dst = RD8;
    uint64_t imm = RD24;
    CHKREG(dst);
    L[dst] = (dyn)FIXTEXT((uint64_t)imm);
    BREAK;}
  OP(SBC_FXT32) {
    int dst = RD8;
    uint64_t imm = RD32;
    CHKREG(dst);
    L[dst] = (dyn)FIXTEXT((uint64_t)imm);
    BREAK;}
  OP(SBC_FXT40) {
    int dst = RD8;
    uint64_t imm = RD40;
    CHKREG(dst);
    L[dst] = (dyn)FIXTEXT((uint64_t)imm);
    BREAK;}
  OP(SBC_FXT48) {
    int dst = RD8;
    uint64_t imm = RD48;
    CHKREG(dst);
    L[dst] = (dyn)FIXTEXT((uint64_t)imm);
    BREAK;}
  OP(SBC_FXT56) {
    int dst = RD8;
    uint64_t imm = RD56;
    CHKREG(dst);
    L[dst] = (dyn)FIXTEXT((uint64_t)imm);
    BREAK;}
  OP(SBC_FXNTAG) {
    int dst = RD16;
    int src = RD16;
    CHKREG(dst);
    CHKREG(src);
    L[dst] = FXN(O_TAG(L[src]));
    BREAK;}
  OP(SBC_FXNLISTN) {
    int dst = RD16;
    int src = RD16;
    CHKREG(dst); CHKREG(src);
    FXNLISTN(L[dst],L[src]);
    BREAK;}
  OP(SBC_FXNLGET) {
    int dst = RD16;
    int src = RD16;
    int index = RD16;
    CHKREG(dst); CHKREG(src); CHKREG(index);
    dyn ss = L[src];
    dyn ii = L[index];
    if (TAGIS(T_LIST,ss) && TAGIS(T_INT,ii)
       && (uint64_t)ii < (uint64_t)FXN(LIST_SIZE(ss))) {
      FXNLGET(L[dst],ss,ii);
      MCACHE_SKIP;
    } else {
      ARGLIST2(L[src],L[index]);
      MCACHE_CALL(L[dst],L[src],m_get);
    }
    BREAK;}
  OP(SBC_FXNLSET) {
    int dst = RD16;
    int src = RD16;
    int index = RD16;
    int val = RD16;
    CHKREG(dst); CHKREG(src); CHKREG(index); CHKREG(val);
    dyn ss = L[src];
    dyn ii = L[index];
    if (TAGIS(T_LIST,ss) && TAGIS(T_INT,ii)
       && (uint64_t)ii < (uint64_t)FXN(LIST_SIZE(ss))) {
      FXNLSET(L[dst],ss,ii,L[val]);
      MCACHE_SKIP;
    } else {
      ARGLIST3(L[src],L[index],L[val]);
      MCACHE_CALL(L[dst],L[src],m_set);
    }
    BREAK;}
  OP(SBC_FXNLSETIR) {
    dyn dummy;
    int src = RD16;
    int index = RD16;
    int val = RD16;
    CHKREG(src); CHKREG(index); CHKREG(val);
    dyn ss = L[src];
    dyn ii = L[index];
    if (TAGIS(T_LIST,ss) && TAGIS(T_INT,ii)
       && (uint64_t)ii < (uint64_t)FXN(LIST_SIZE(ss))) {
      FXNLSET(dummy,L[src],L[index],L[val]);
      MCACHE_SKIP;
    } else {
      ARGLIST3(L[src],L[index],L[val]);
      MCACHE_CALL(dummy,L[src],m_set);
    }
    BREAK;}
  OP(SBC_FXNSIZE) {
    int dst = RD16;
    int src = RD16;
    CHKREG(dst);
    CHKREG(src);
    L[dst] = FXN(LIST_SIZE(L[src]));
    BREAK;}
  OP(SBC_ABS) {
    int dst = RD16; int a = RD16; 
    CHKREG(dst); CHKREG(a);
    dyn aa = L[a];
    if (TAGIS(T_INT, aa)) {
      int64_t val = UNFXN(aa);
      if (val < 0) aa = FXN(-val);
      L[dst] = aa;
    } else if (TAGIS(T_FLOAT, aa)) {
      float fa, fb;
      STFLT(fa,aa);
      if (fa < 0.0f) LDFLT(aa,-fa);
      L[dst] = aa;
    } else {
      ARGLIST1(L[a]);
      MCALL(L[dst],L[a],m_abs);
    }
    BREAK;}
  OP(SBC_NEG) {
    int dst = RD16; int a = RD16; 
    CHKREG(dst); CHKREG(a);
    dyn aa = L[a];
    if (TAGIS(T_INT, aa)) {
      FXNNEG(L[dst],L[a]);
    } else {
      ARGLIST1(L[a]); //cant use aa and bb here, cuz GC could have run
      MCALL(L[dst],L[a],m_neg);
    }
    BREAK;}
  OP(SBC_FXNADD) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    dyn aa = L[a];
    if (TAGIS(T_INT, aa)) {
      dyn bb = L[b];
      if (TAGIS(T_INT, bb)) FXNADD(L[dst],aa,bb);
      else {
        float fa, fb;
        fa = (float)UNFXN(aa);
        STFLT(fb,bb);
        LDFLT(L[dst],fa+fb);
      }
    } else {
      ARGLIST2(L[a],L[b]); //cant use aa and bb here, cuz GC could have run
      MCALL(L[dst],L[a],m_add);
    }
    BREAK;}
  OP(SBC_FXNSUB) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    dyn aa = L[a];
    if (TAGIS(T_INT, aa)) {
      dyn bb = L[b];
      if (TAGIS(T_INT, bb)) FXNSUB(L[dst],aa,bb);
      else {
        float fa, fb;
        fa = (float)UNFXN(aa);
        STFLT(fb,bb);
        LDFLT(L[dst],fa-fb);
      }
    } else {
      ARGLIST2(L[a],L[b]);
      MCALL(L[dst],L[a],m_sub);
    }
    BREAK;}
  OP(SBC_FXNMUL) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    dyn aa = L[a];
    if (TAGIS(T_INT, aa)) {
      dyn bb = L[b];
      if (TAGIS(T_INT, bb)) FXNMUL(L[dst],aa,bb);
      else {
        float fa, fb;
        fa = (float)UNFXN(aa);
        STFLT(fb,bb);
        LDFLT(L[dst],fa*fb);
      }
    } else {
      ARGLIST2(L[a],L[b]);
      MCALL(L[dst],L[a],m_mul);
    }
    BREAK;}
  OP(SBC_FXNDIV) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    dyn aa = L[a];
    if (TAGIS(T_INT, aa)) {
      dyn bb = L[b];
      if (TAGIS(T_INT, bb)) FXNDIV(L[dst],aa,bb);
      else {
        float fa, fb;
        fa = (float)UNFXN(aa);
        STFLT(fb,bb);
        if (fb == 0.0) fb = FLT_MIN;
        LDFLT(L[dst],fa/fb);
      }
    } else {
      ARGLIST2(L[a],L[b]);
      MCALL(L[dst],L[a],m_div);
    }
    BREAK;}
  OP(SBC_FXNREM) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    FXNREM(L[dst],L[a],L[b]);
    dyn aa = L[a];
    if (TAGIS(T_INT, aa)) {
      dyn bb = L[b];
      if (TAGIS(T_INT, bb)) FXNREM(L[dst],aa,bb);
      else {
        float fa, fb, r;
        fa = (float)UNFXN(aa);
        STFLT(fb,bb);
        if (fb == 0.0) fb = FLT_MIN;
        r = fa/fb;
        LDFLT(L[dst],(r-(float)(int64_t)r)*fb);
      }
    } else {
      ARGLIST2(L[a],L[b]);
      MCALL(L[dst],L[a],m_rem);
    }
    BREAK;}
  OP(SBC_IMMEQ) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    if (TAGIS(T_INT, L[a])) {
      IMMEQ(L[dst],L[a],L[b]);
      MCACHE_SKIP;
    } else if ((TAGIS(T_TEXT, L[a]) || TAGIS(T_FIXTEXT, L[a]))
            && (TAGIS(T_TEXT, L[b]) || TAGIS(T_FIXTEXT, L[b]))) {
      /* OP-5c: text-vs-text fast path.  `case X [\foo @Rest]:`
       * patterns reduce to a head-vs-literal-keyword IMMEQ.
       * For FIXTEXT operands the existing `><` macro peephole
       * emits SBC_SAME (identity); for BIGTEXT (or mixed) the
       * fallback used to go through MCACHE_CALL m_eq.  Call
       * `texts_equal` directly here -- skips the mcache lookup
       * and the per-call frame setup. */
      L[dst] = FXN(texts_equal(L[a], L[b]));
      MCACHE_SKIP;
    } else {
      ARGLIST2(L[a],L[b]);
      MCACHE_CALL(L[dst],L[a],m_eq);
    }
    BREAK;}
  OP(SBC_IMMNE) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    if (TAGIS(T_INT, L[a])) {
      IMMNE(L[dst],L[a],L[b]);
      MCACHE_SKIP;
    } else if ((TAGIS(T_TEXT, L[a]) || TAGIS(T_FIXTEXT, L[a]))
            && (TAGIS(T_TEXT, L[b]) || TAGIS(T_FIXTEXT, L[b]))) {
      /* OP-5c: text-vs-text fast path for `<>` / inequality. */
      L[dst] = FXN(!texts_equal(L[a], L[b]));
      MCACHE_SKIP;
    } else {
      ARGLIST2(L[a],L[b]);
      MCACHE_CALL(L[dst],L[a],m_ne);
    }
    BREAK;}
  OP(SBC_FXNLT) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    if (TAGIS(T_INT, L[a]) && TAGIS(T_INT, L[b])) {
      FXNLT(L[dst],L[a],L[b]);
    } else {
      ARGLIST2(L[a],L[b]);
      MCALL(L[dst],L[a],m_lt);
    }
    BREAK;}
  OP(SBC_FXNGT) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    if (TAGIS(T_INT, L[a]) && TAGIS(T_INT, L[b])) {
      FXNGT(L[dst],L[a],L[b]);
    } else {
      ARGLIST2(L[a],L[b]);
      MCALL(L[dst],L[a],m_gt);
    }
    BREAK;}
  OP(SBC_FXNLTE) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    if (TAGIS(T_INT, L[a]) && TAGIS(T_INT, L[b])) {
      FXNLTE(L[dst],L[a],L[b]);
    } else {
      ARGLIST2(L[a],L[b]);
      MCALL(L[dst],L[a],m_lte);
    }
    BREAK;}
  OP(SBC_FXNGTE) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    if (TAGIS(T_INT, L[a]) && TAGIS(T_INT, L[b])) {
      FXNGTE(L[dst],L[a],L[b]);
    } else {
      ARGLIST2(L[a],L[b]);
      MCALL(L[dst],L[a],m_gte);
    }
    BREAK;}
  OP(SBC_FXNAND) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    if (TAGIS(T_INT, L[a]) && TAGIS(T_INT, L[b])) {
      FXNAND(L[dst],L[a],L[b]);
    } else {
      ARGLIST2(L[a],L[b]);
      MCALL(L[dst],L[a],m_and);
    }
    BREAK;}
  OP(SBC_FXNIOR) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    if (TAGIS(T_INT, L[a]) && TAGIS(T_INT, L[b])) {
      FXNIOR(L[dst],L[a],L[b]);
    } else {
      ARGLIST2(L[a],L[b]);
      MCALL(L[dst],L[a],m_ior);
    }
    BREAK;}
  OP(SBC_FXNXOR) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    if (TAGIS(T_INT, L[a]) && TAGIS(T_INT, L[b])) {
      FXNXOR(L[dst],L[a],L[b]);
    } else {
      ARGLIST2(L[a],L[b]);
      MCALL(L[dst],L[a],m_xor);
    }
    BREAK;}
  OP(SBC_FXNSHL) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    if (TAGIS(T_INT, L[a]) && TAGIS(T_INT, L[b])) {
      FXNSHL(L[dst],L[a],L[b]);
    } else {
      ARGLIST2(L[a],L[b]);
      MCALL(L[dst],L[a],m_shl);
    }
    BREAK;}
  OP(SBC_FXNSHR) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    if (TAGIS(T_INT, L[a]) && TAGIS(T_INT, L[b])) {
      FXNSHR(L[dst],L[a],L[b]);
    } else {
      ARGLIST2(L[a],L[b]);
      MCALL(L[dst],L[a],m_shr);
    }
    BREAK;}
  OP(SBC_SAME) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    IMMEQ(L[dst],L[a],L[b]);
    BREAK;}
  OP(SBC_VARY) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    IMMNE(L[dst],L[a],L[b]);
    BREAK;}
  OP(SBC_TINIT) {
    int type = RD16;
    int size = RD16;
    int name = RD24;
    set_type_size_and_name((int64_t)sbc->ty[type],size,sbc->tx[name]);
    BREAK;}
  OP(SBC_TINITI) {
    int tag = RD16;
    int size = RD16;
    dyn name = (dyn)RD64; //FIXTEXT READ
    set_type_size_and_name((int64_t)sbc->ty[tag],size,name);
    BREAK;}
  OP(SBC_SUBTYPE) {
    int super = RD16;
    int sub = RD16;
    add_subtype((int)(int64_t)sbc->ty[super],(int)(int64_t)sbc->ty[sub]);
    BREAK;}
  OP(SBC_DMET) {
    int tyidx = RD16;
    int mtidx = RD24;
    int handler = RD16;
    add_method((int)(int64_t)sbc->ty[tyidx], sbc->mt[mtidx], L[handler]);
    BREAK;}
  OP(SBC_INC) {
    int dst = RD16; int a = RD16; 
    CHKREG(dst); CHKREG(a);
    dyn aa = L[a];
    if (TAGIS(T_INT, aa)) {
      FXNADD(L[dst],aa,FXN(1));
    } else {
      ARGLIST1(L[a]); //cant use aa and bb here, cuz GC could have run
      MCALL(L[dst],L[a],m_inc);
    }
    BREAK;}
  OP(SBC_DEC) {
    int dst = RD16; int a = RD16; 
    CHKREG(dst); CHKREG(a);
    dyn aa = L[a];
    if (TAGIS(T_INT, aa)) {
      FXNSUB(L[dst],aa,FXN(1));
    } else {
      ARGLIST1(L[a]); //cant use aa and bb here, cuz GC could have run
      MCALL(L[dst],L[a],m_dec);
    }
    BREAK;}
  OP(SBC_NOT) {
    int dst = RD16;
    int src = RD16;
    CHKREG(dst);
    CHKREG(src);
    L[dst] = L[src] ? FXN(0) : FXN(1);
    BREAK;}
  OP(SBC_GOT) {
    int dst = RD16;
    int src = RD16;
    CHKREG(dst);
    CHKREG(src);
    L[dst] = L[src]!=No ? FXN(1) : FXN(0);
    BREAK;}
  OP(SBC_NO) {
    int dst = RD16;
    int src = RD16;
    CHKREG(dst);
    CHKREG(src);
    L[dst] = L[src]==No ? FXN(1) : FXN(0);
    BREAK;}
  OP(SBC_GID) {
    int dst = RD16;
    int src = RD16;
    CHKREG(dst);
    CHKREG(src);
    L[dst] = STRIP_TAG(L[src]);
    BREAK;}
  OP(SBC_CURMET) {
    int dst = RD16;
    CHKREG(dst);
    THIS_METHOD(L[dst]);
    BREAK;}
  OP(SBC_MNAME) {
    int dst = RD16;
    int src = RD16;
    CHKREG(dst);
    CHKREG(src);
    L[dst] = get_method_name(UNFXN(L[src]));
    BREAK;}
  OP(SBC_FATAL) {
    int msg = RD16;
    CHKREG(msg);
    FATAL(L[msg]);
    BREAK;}
  OP(SBC_GC0) { GC_DISABLE(); BREAK;}
  OP(SBC_GC1) { GC_ENABLE(); BREAK;}
  OP(SBC_NFI_INT) {
    int r = RD16;
    CHKREG(r);
    dyn src = L[r];
    FFI_ARG_int(tmp, src);
    *(int*)api.nfi_parg = tmp;
    ++api.nfi_parg;
    BREAK;}
  OP(SBC_NFI_PTR) {
    int r = RD16;
    CHKREG(r);
    dyn src = L[r];
    FFI_ARG_ptr(tmp, src);
    *(void**)api.nfi_parg = tmp;
    ++api.nfi_parg;
    BREAK;}
  OP(SBC_NFI_TXT) {
    int r = RD16;
    CHKREG(r);
    dyn src = L[r];
    FFI_ARG_text(tmp, src);
    *(char**)api.nfi_parg = tmp;
    ++api.nfi_parg;
    BREAK;}
  OP(SBC_NFI_U4) {
    int r = RD16;
    CHKREG(r);
    dyn src = L[r];
    FFI_ARG_u4(tmp, src);
    *(uint32_t*)api.nfi_parg = tmp;
    ++api.nfi_parg;
    BREAK;}
  OP(SBC_NFI_FLT) {
    int r = RD16;
    CHKREG(r);
    dyn src = L[r];
    FFI_ARG_float(tmp, src);
    *(float*)api.nfi_parg = tmp;
    ++api.nfi_parg;
    BREAK;}
  OP(SBC_NFI_DBL) {
    int r = RD16;
    CHKREG(r);
    dyn src = L[r];
    FFI_ARG_double(tmp, src);
    *(double*)api.nfi_parg = tmp;
    ++api.nfi_parg;
    BREAK;}
  OP(SBC_NFI) {
    int pfn = RD16; //native function pointer
    int tri = RD16; //trampoline index
    CHKREG(pfn);
    void *target = NFI_DECPTR(L[pfn]);
    /* sffi_call returns the raw bit-pattern of the return register
     * (rax for int/ptr, xmm0 for float/double). The switch below
     * reinterprets per the declared return type. */
    uint64_t retval = sffi_call(
        (const sffi_sig_t *)sbc->nfi_trmps[tri],
        target, api.nfi_args);
    api.nfi_parg = api.nfi_args;
    int rtid = RD8;
    int dst = RD16;
    CHKREG(dst);
    if (dst) switch (rtid) {
    case SBC_NFI_VOID: L[dst] = No;
    case SBC_NFI_INT: {
      FFI_FROM_int(L[dst],*(int*)&retval);
      BREAK;}
    case SBC_NFI_PTR: {
      FFI_FROM_ptr(L[dst],*(void**)&retval);
      BREAK;}
    case SBC_NFI_TXT: {
      FFI_FROM_text(L[dst],*(char**)&retval);
      BREAK;}
    case SBC_NFI_U4: {
      FFI_FROM_u4(L[dst],*(uint32_t*)&retval);
      BREAK;}
    case SBC_NFI_FLT: {
      FFI_FROM_float(L[dst],*(float*)&retval);
      BREAK;}
    case SBC_NFI_DBL: {
      FFI_FROM_double(L[dst],*(double*)&retval);
      BREAK;}
    }
    BREAK;}
  OP(SBC_NLDU1) {
    int dst = RD16; int ptr = RD16; int ofs = RD16;
    CHKREG(dst); CHKREG(ptr); CHKREG(ofs);
    FFI_GET(L[dst],uint8_t,L[ptr],L[ofs]);
    BREAK;}
  OP(SBC_NLDU2) {
    int dst = RD16; int ptr = RD16; int ofs = RD16;
    CHKREG(dst); CHKREG(ptr); CHKREG(ofs);
    FFI_GET(L[dst],uint16_t,L[ptr],L[ofs]);
    BREAK;}
  OP(SBC_NLDU4) {
    int dst = RD16; int ptr = RD16; int ofs = RD16;
    CHKREG(dst); CHKREG(ptr); CHKREG(ofs);
    FFI_GET(L[dst],uint32_t,L[ptr],L[ofs]);
    BREAK;}
  OP(SBC_NLDS1) {
    int dst = RD16; int ptr = RD16; int ofs = RD16;
    CHKREG(dst); CHKREG(ptr); CHKREG(ofs);
    FFI_GET(L[dst],int8_t,L[ptr],L[ofs]);
    BREAK;}
  OP(SBC_NLDS2) {
    int dst = RD16; int ptr = RD16; int ofs = RD16;
    CHKREG(dst); CHKREG(ptr); CHKREG(ofs);
    FFI_GET(L[dst],int16_t,L[ptr],L[ofs]);
    BREAK;}
  OP(SBC_NLDS4) {
    int dst = RD16; int ptr = RD16; int ofs = RD16;
    CHKREG(dst); CHKREG(ptr); CHKREG(ofs);
    FFI_GET(L[dst],int32_t,L[ptr],L[ofs]);
    BREAK;}
  OP(SBC_NSTU1) {
    int ptr = RD16; int ofs = RD16; int val = RD16;
    CHKREG(val); CHKREG(ptr); CHKREG(ofs);
    FFI_SET(uint8_t,L[ptr],L[ofs],L[val]);
    BREAK;}
  OP(SBC_NSTU2) {
    int ptr = RD16; int ofs = RD16; int val = RD16;
    CHKREG(val); CHKREG(ptr); CHKREG(ofs);
    FFI_SET(uint16_t,L[ptr],L[ofs],L[val]);
    BREAK;}
  OP(SBC_NSTU4) {
    int ptr = RD16; int ofs = RD16; int val = RD16;
    CHKREG(val); CHKREG(ptr); CHKREG(ofs);
    FFI_SET(uint32_t,L[ptr],L[ofs],L[val]);
    BREAK;}
  OP(SBC_NSTS1) {
    int ptr = RD16; int ofs = RD16; int val = RD16;
    CHKREG(val); CHKREG(ptr); CHKREG(ofs);
    FFI_SET(int8_t,L[ptr],L[ofs],L[val]);
    BREAK;}
  OP(SBC_NSTS2) {
    int ptr = RD16; int ofs = RD16; int val = RD16;
    CHKREG(val); CHKREG(ptr); CHKREG(ofs);
    FFI_SET(int16_t,L[ptr],L[ofs],L[val]);
    BREAK;}
  OP(SBC_NSTS4) {
    int ptr = RD16; int ofs = RD16; int val = RD16;
    CHKREG(val); CHKREG(ptr); CHKREG(ofs);
    FFI_SET(int32_t,L[ptr],L[ofs],L[val]);
    BREAK;}
#define CTX_BTLAND 0
#define CTX_BTJUMP 1
#define CTX_SET_UNWIND_HANDLER 2
#define CTX_REMOVE_UNWIND_HANDLER 3
  OP(SBC_CTX) {
    int type = RD8;
    if (type == CTX_BTLAND) {
      int dst = RD16;
      CHKREG(dst);
      int entry;

      jmp_state * volatile js_; //volatile to prevent compiler from caching it
      GC_DISABLE();
      api.jmp_return = alloc_bytes(sizeof(jmp_state));
      GC_ENABLE();
      js_ = (jmp_state*)BYTES_DATA(api.jmp_return);
      //js_->uwh = (dyn)123;
      js_->uwh = api.puwh;
      js_->frame = api.frame;
      js_->ip = pin;
      jmp_buf jb;
      entry = setjmp(jb);
      js_->anchor = &jb;
      entry = !entry;
      if (!entry) {
        js_ = (jmp_state*)BYTES_DATA(LGET(api.jmp_return,0));
        api.jmp_return = LGET(api.jmp_return,1);
      }
      pin = js_->ip;

      dyn *r;
      GC_DISABLE();
      LIST(r,2);
      LSET(r,0,FXN(entry));
      LSET(r,1,api.jmp_return);
      GC_ENABLE();
      L[dst] = r;
      api.jmp_return = 0;
    } else if (type == CTX_BTJUMP) {
      int state = RD16;
      int value = RD16;
      CHKREG(state);
      CHKREG(value);
      btjump(L[state],L[value]);
    } else if (type == CTX_SET_UNWIND_HANDLER) {
      /* CORE-2: push a finalizer closure onto api.puwh so it fires
       * if the protected region unwinds via btjump.  Mirrors the
       * SET_UNWIND_HANDLER macro in symta.h. */
      int h = RD16;
      CHKREG(h);
      ++api.puwh;
      *api.puwh = L[h];
    } else if (type == CTX_REMOVE_UNWIND_HANDLER) {
      /* CORE-2: pop the most recent handler on normal-exit from
       * the protected region (the `fin` macro emits a tail call
       * to the handler immediately after this op for the
       * no-unwind case). */
      *api.puwh = 0;
      --api.puwh;
    } else {
      fprintf(stderr, "SBC_CTX: bad type=%d\n", type);
    }
    BREAK;}
  /* ============================================================
   * TS-4 typed unboxed-arithmetic handlers.  Placed at the END
   * of the switch so the generated machine code for these hot
   * paths gets its OWN i-cache footprint -- the untyped FXN
   * fallbacks above don't pull these in on every line fill, and
   * a workload that's heavy on typed arithmetic doesn't keep
   * evicting the FXN code.  Same for the goto-label dispatch
   * table earlier in the file.
   *
   * All handlers share the same wire shape as SBC_FXNADD
   * (op + 3*16-bit regs), so an x86 backend can lower them to
   * native single-instruction sequences with the FXN bit-tag
   * conventions:
   *
   *   IADD/ISUB/IMUL          -> ADD / SUB / IMUL (no tag check)
   *   IDIV/IREM               -> IDIV (trap-on-zero via SEH)
   *   ILT/IGT/ILTE/IGTE       -> CMP + SETcc
   *   FADD/FSUB/FMUL/FDIV     -> ADDSS / SUBSS / MULSS / DIVSS
   * ============================================================ */
  OP(SBC_IADD) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    FXNADD(L[dst], L[a], L[b]);
    BREAK;}
  OP(SBC_ISUB) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    FXNSUB(L[dst], L[a], L[b]);
    BREAK;}
  OP(SBC_IMUL) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    FXNMUL(L[dst], L[a], L[b]);
    BREAK;}
  OP(SBC_IDIV) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    /* No explicit zero check -- the Windows SEH handler in
     * w64/ctx.c catches EXCEPTION_INT_DIVIDE_BY_ZERO and routes
     * it through Symta's `bad`-handling path the same way
     * FXNDIV's bare `a/b` does. */
    FXNDIV(L[dst], L[a], L[b]);
    BREAK;}
  OP(SBC_IREM) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    FXNREM(L[dst], L[a], L[b]);
    BREAK;}
  OP(SBC_ILT) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    FXNLT(L[dst], L[a], L[b]);
    BREAK;}
  OP(SBC_IGT) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    FXNGT(L[dst], L[a], L[b]);
    BREAK;}
  OP(SBC_ILTE) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    FXNLTE(L[dst], L[a], L[b]);
    BREAK;}
  OP(SBC_IGTE) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    FXNGTE(L[dst], L[a], L[b]);
    BREAK;}
  OP(SBC_FADD) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    float fa, fb;
    STFLT(fa, L[a]);
    STFLT(fb, L[b]);
    LDFLT(L[dst], fa + fb);
    BREAK;}
  OP(SBC_FSUB) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    float fa, fb;
    STFLT(fa, L[a]);
    STFLT(fb, L[b]);
    LDFLT(L[dst], fa - fb);
    BREAK;}
  OP(SBC_FMUL) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    float fa, fb;
    STFLT(fa, L[a]);
    STFLT(fb, L[b]);
    LDFLT(L[dst], fa * fb);
    BREAK;}
  OP(SBC_FDIV) {
    int dst = RD16; int a = RD16; int b = RD16;
    CHKREG(dst); CHKREG(a); CHKREG(b);
    float fa, fb;
    STFLT(fa, L[a]);
    STFLT(fb, L[b]);
    /* Match SBC_FXNDIV's float-fallback: avoid IEEE inf/nan
     * by substituting FLT_MIN when the divisor is exactly 0. */
    if (fb == 0.0f) fb = FLT_MIN;
    LDFLT(L[dst], fa / fb);
    BREAK;}
  /* SBC_LSRC was the CORE-1 v1 per-line opcode.  CORE-1 v2
   * (commit 0b9e2c4) moved line/col into a sorted side table
   * that print_stack_trace binary-searches by `frame->pin`;
   * sif2sbc records each lsrc into that table and emits no
   * bytecode for it.  RT-7 stage 2 (commit ba09259) rejects
   * any pre-stage-2 SBC at load anyway, so this opcode never
   * shows up in the dispatch stream and falls through to
   * DEFAULT (bad-opcode) if a malformed file ever did. */
  DEFAULT {
#if 1
    int opcode = pin[-1];
    fprintf(stderr, "sbc_exec_fn: bad opcode=%x\n", opcode);
#else
    char *head = 0;
    int oi = opcode;
    while (!head) {
      head = hmget(sbcasm,oi);
      oi--;
    }
    char *name = head ? head : "unk";
    fprintf(stderr, "sbc_exec_fn: bad opcode=%x (%s)\n", opcode, name);
#endif
    exit(-1);
    BREAK;}
#ifndef SBC_THREADED_DISPATCH
  }}
#endif
  return (dyn)0;
}

#undef OP
#undef BREAK
#undef DEFAULT

dyn sbc_exec(sbc_t *sbc) {
  sbc_prepare(sbc);

  void *entry = (void*)sbc->hooks[0];
  OPEN_FRAME(0);
  PROLOGUE(FRAME_HEADER_SIZE);

  dyn centry=0;
  dyn r;

  GC_DISABLE();
  CLOSURE(centry,entry,0);
  GC_ENABLE();

  CALL(r,centry); // run the module toplevel

  CLOSE_FRAME;

  return r;
}

dyn sbc_exec_file(char *path) {
  sbc_t *sbc = sbc_load(path);
  if (!sbc) fatal("Couldnt load %s\n", path);
  return sbc_exec(sbc);
}


