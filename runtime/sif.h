#ifndef SIF_H
#define SIF_H

#include "common.h"

/* SBC file format identification.  Emitted at the very first
 * bytes of every SBC; the runtime's `sbc_new` reads them and
 * rejects mismatches up-front with a clear "this isn't a
 * symta SBC" or "wrong revision -- recompile" message.
 *
 * SBC_MAGIC: 32-bit little-endian "SymB" (Symta Bytecode).
 *   The capital S, lowercase y/m, and B-for-bytecode make it
 *   distinct from random binary file headers and stable
 *   forever.  Identifies the file as ours.
 *
 * SBC_REVISION: 16-bit format revision.  Bumped every time the
 *   on-disk layout changes in a way the runtime needs to know
 *   about.  Future runtimes can branch on this to support
 *   multiple revisions if backwards compat is ever desired;
 *   for now the runtime accepts only the current revision and
 *   prints "recompile" otherwise.
 *
 * Revision history:
 *   1 -- initial.  Post-RT-7-stage-2, post-SBC_LSRC-drop,
 *        post-lineno-side-table.  Header layout is
 *        [magic:4][rev:2][src:3][deps:3][export:3]
 *        [tot_sz:2][code_sz:3][data_sz:3][tbls_sz:3]
 *        [fntbl_sz:3] followed by tot_sz tot entries.
 *
 * Bumps should increment SBC_REVISION here and add an entry
 * to the history.  The runtime check in `sbc_new` keeps the
 * compiled binary self-describing. */
#define SBC_MAGIC    0x426D7953u  /* "SymB" -- bytes S, y, m, B little-endian */
#define SBC_REVISION 1

enum {
/*00*/ SBC_NOP,
/*01*/ SBC_SUBR,
/*02*/ SBC_LEAVE,
/*03*/ SBC_LEAVE0,
/*04*/ SBC_JMP,
/*05*/ SBC_JMP16,
/*06*/ SBC_B,
/*07*/ SBC_B8,
/*08*/ SBC_BZ,
/*09*/ SBC_CALL,
/*0A*/ SBC_CALLIR,  //ignore return value version
/*0B*/ SBC_CALLT,   //call tagged
/*0C*/ SBC_CALLTIR,
/*0D*/ SBC_MCALL,
/*0E*/ SBC_MCALLIR, //ignore return value version
/*0F*/ SBC_MCALL8,
/*10*/ SBC_CLOSURE,
/*11*/ SBC_OBJECT,
/*12*/ SBC_LIST,
/*13*/ SBC_IFFXN,
/*14*/ SBC_CNAS,
/*15*/ SBC_ARGLIST,
/*16*/ SBC_ARGLIST8,
/*17*/ SBC_STARG,
/*18*/ SBC_STARG8,
/*19*/ SBC_MOVE,
/*1A*/ SBC_MOVE8,
/*1B*/ SBC_MOVEEMT,  //move empty list
/*1C*/ SBC_MOVENO,
/*1D*/ SBC_MOVETX,   //move text
/*1E*/ SBC_MOVETX8,
/*1F*/ SBC_MOVEMT,   //move methods table
/*20*/ SBC_MOVEMT8,
/*21*/ SBC_MOVEIM,   //move import
/*22*/ SBC_STOR,
/*23*/ SBC_STOR8,
/*24*/ SBC_LOAD,
/*25*/ SBC_LOAD8,
/*26*/ SBC_COPY,
/*27*/ SBC_FXNB0,
/*28*/ SBC_FXNB8,
/*29*/ SBC_FXNB16,
/*2A*/ SBC_FXNB32,
/*2B*/ SBC_FXN0,
/*2C*/ SBC_FXN8,
/*2D*/ SBC_FXN16,
/*2E*/ SBC_FXN32,
/*2F*/ SBC_IMMB64,
/*30*/ SBC_IMM64,
/*31*/ SBC_FXNTAG,
/*32*/ SBC_FXNLISTN,
/*33*/ SBC_ABS,
/*34*/ SBC_FXNLGET,
/*35*/ SBC_FXNLSET,
/*36*/ SBC_FXNLSETIR,
/*37*/ SBC_FXNSIZE,
/*38*/ SBC_NEG,
/*39*/ SBC_FXNADD,
/*3A*/ SBC_FXNSUB,
/*3B*/ SBC_FXNMUL,
/*3C*/ SBC_FXNDIV,
/*3D*/ SBC_FXNREM,
/*3E*/ SBC_IMMEQ,
/*3F*/ SBC_IMMNE,
/*40*/ SBC_FXNLT,
/*41*/ SBC_FXNGT,
/*42*/ SBC_FXNLTE,
/*43*/ SBC_FXNGTE,
/*44*/ SBC_FXNAND,
/*45*/ SBC_FXNIOR,
/*46*/ SBC_FXNXOR,
/*47*/ SBC_FXNSHL,
/*48*/ SBC_FXNSHR,
/*49*/ SBC_NLDU1, //foreign get unsigned
/*4A*/ SBC_NLDU2,
/*4B*/ SBC_NLDU4,
/*4C*/ SBC_NSTU1, //foreign set unsigned
/*4D*/ SBC_NSTU2,
/*4E*/ SBC_NSTU4,
/*4F*/ SBC_NLDS1, //foreign get signed
/*50*/ SBC_NLDS2,
/*51*/ SBC_NLDS4,
/*52*/ SBC_NSTS1, //foreign set signed
/*53*/ SBC_NSTS2,
/*54*/ SBC_NSTS4,
/*55*/ SBC_TINIT, //type init
/*56*/ SBC_TINITI, //type init for immediate text
/*57*/ SBC_SUBTYPE,
/*58*/ SBC_DMET,
/*59*/ SBC_GID,
/*5A*/ SBC_CURMET, // currently executing method
/*5B*/ SBC_MNAME,
/*5C*/ SBC_FATAL,
/*5D*/ SBC_GC0, //turn gc on
/*5E*/ SBC_GC1, //turn gc off

//opcodes for pushing native arguments
/*5F*/ SBC_NFI, //native function function invocation
/*60*/ SBC_NFI_VOID,
/*61*/ SBC_NFI_INT,
/*62*/ SBC_NFI_PTR,
/*63*/ SBC_NFI_TXT,
/*64*/ SBC_NFI_U4,
/*65*/ SBC_NFI_FLT,
/*66*/ SBC_NFI_DBL,
/*67*/ SBC_TCALL,  //tail call
/*68*/ SBC_TMCALL, //method tail call
/*69*/ SBC_LEAVEN, //leave constant value
/*6A*/ SBC_LD4_0,
/*6B*/ SBC_LD4_1,
/*6C*/ SBC_LD4_2,
/*6D*/ SBC_LD4_3,
/*6E*/ SBC_LD4_4,
/*6F*/ SBC_LD4_5,
/*70*/ SBC_LD4_6,
/*71*/ SBC_LD4_7,
/*72*/ SBC_LD4_8,
/*73*/ SBC_LD4_9,
/*74*/ SBC_LD4_A,
/*75*/ SBC_LD4_B,
/*76*/ SBC_LD4_C,
/*77*/ SBC_LD4_D,
/*78*/ SBC_LD4_E,
/*79*/ SBC_LD4_F,
/*7A*/ SBC_ST4_0,
/*7B*/ SBC_ST4_1,
/*7C*/ SBC_ST4_2,
/*7D*/ SBC_ST4_3,
/*7E*/ SBC_ST4_4,
/*7F*/ SBC_ST4_5,
/*80*/ SBC_ST4_6,
/*81*/ SBC_ST4_7,
/*82*/ SBC_ST4_8,
/*83*/ SBC_ST4_9,
/*84*/ SBC_ST4_A,
/*85*/ SBC_ST4_B,
/*86*/ SBC_ST4_C,
/*87*/ SBC_ST4_D,
/*88*/ SBC_ST4_E,
/*89*/ SBC_ST4_F,
/*8A*/ SBC_ARGLIST0,
/*8B*/ SBC_ARGLIST1,
/*8C*/ SBC_ARGLIST2,
/*8D*/ SBC_ARGLIST3,
/*8E*/ SBC_ARGLIST4,
/*8F*/ SBC_ARGLIST5,
/*90*/ SBC_FXT8,
/*91*/ SBC_FXT16,
/*92*/ SBC_FXT24,
/*93*/ SBC_FXT32,
/*94*/ SBC_FXT40,
/*95*/ SBC_FXT48,
/*96*/ SBC_FXT56,
/*97*/ SBC_MOVE4,
/*98*/ SBC_NLD, //native load
/*99*/ SBC_NST, //native store
/*9A*/ SBC_CTX, //context manipulation

//virtual opcodes
/*9B*/ SBC_NOT,
/*9C*/ SBC_GOT,
/*9D*/ SBC_NO,
/*9E*/ SBC_INC,
/*9F*/ SBC_DEC,
/*A0*/ SBC_UNUSEDA0,    /* was SBC_LSRC (CORE-1 v1).  CORE-1 v2
                          (commit 0b9e2c4) moved linenos to a
                          side table so no LSRC bytes are ever
                          emitted; the SIF-side `SBC_LSRC` is
                          now a virtual opcode (see below). */
/*A1*/ SBC_LIST2,        /* RT-9: fused size-2 list allocation.
                          Single opcode replaces SBC_LIST 2 +
                          SBC_ST4_0 + SBC_ST4_1.  Cuts dispatch
                          count for [A B] literals from 3 to 1
                          (size-2 LISTs are 24.6% of all alloc
                          traffic; ~130 M per ./game compile). */
/*A2*/ SBC_LIST1,        /* RT-9: fused size-1 list allocation.
                          Single opcode replaces SBC_LIST 1 +
                          SBC_ST4_0 for [X] literals.  Size-1
                          LISTs are 65.5% of alloc traffic
                          (348 M per ./game compile); SBC_LIST1
                          only optimises the LITERAL subset
                          (runtime `_listn 1` from `dup N` still
                          goes through SBC_FXNLISTN). */
/*A3*/ SBC_IADD,         /* TS-4.1: unboxed int add; both ops known-int
                          * at compile time, no tag check, no MCALL
                          * fallback.  Compiler emits this when the
                          * static checker proves both operands have
                          * type "int".  Same wire shape as SBC_FXNADD
                          * (op + 3*16-bit regs). */
/*A4*/ SBC_ISUB,
/*A5*/ SBC_IMUL,
/*A6*/ SBC_IDIV,
/*A7*/ SBC_IREM,

/*A8*/ SBC_SAME, //1 if handles equal
/*A9*/ SBC_VARY, //0 if handles equal

/* TS-4.2: unboxed-int comparison opcodes.  Same wire shape as the
 * arith family; no tag check or MCALL fallback.  Result is 0 or 1
 * (already FXN-tagged).  A future x86 backend lowers these to a
 * literal CMP + SETcc sequence.  Placed AFTER SAME/VARY so those
 * keep their committed wire-byte values (A8/A9) and existing
 * sbc/ files load with the new runtime without renumbering. */
/*AA*/ SBC_ILT,
/*AB*/ SBC_IGT,
/*AC*/ SBC_ILTE,
/*AD*/ SBC_IGTE,
/* TS-4.2: unboxed-float arithmetic.  Operands are known to be
 * float-tagged (struct-tagged 64-bit IEEE754).  Same wire shape;
 * runtime uses STFLT/LDFLT for unbox/rebox.  An x86 backend
 * lowers these to ADDSS / SUBSS / MULSS / DIVSS on xmm regs. */
/*AE*/ SBC_FADD,
/*AF*/ SBC_FSUB,
/*B0*/ SBC_FMUL,
/*B1*/ SBC_FDIV,

/*B2*/ SBC_END,


  SBC_BS,
  SBC_T,      //table
  SBC_LDFXN,  //virtual opcode, which compiles to SBC_IMM
  SBC_LDFLT,  //virtual opcode, which compiles to SBC_IMM
  SBC_NFA,    //native function argument
  SBC_NFR,    //native function result
  SBC_SRC,    //file source
  SBC_DEPS,   //file deps
  SBC_EXPORT, //file exports
  SBC_INCL,   //associated .h header
  SBC_SETJMP,
  SBC_LONGJMP,
  /* CORE-2: try/finally via the existing api.puwh stack.  Both
   * virtual ops lower to SBC_CTX with a sub-type byte. */
  SBC_SET_UNWIND_HANDLER,
  SBC_REMOVE_UNWIND_HANDLER,
  /* CORE-1 v2: compile-time-only marker that captures source
   * position.  sif2sbc consumes each `lsrc` instruction into a
   * per-SBC sorted side table (see `sbc_t.lineno_table`) and
   * emits no bytecode for it -- print_stack_trace recovers
   * (row, col) from the table by binary-searching on
   * `frame->pin`.  Never appears in the runtime dispatch
   * stream. */
  SBC_LSRC,
  SBC_IGNORE
};


#define SBC_DEFAULT_SRC 0x8000

typedef struct instr_t {
  int opcode;    //interger opcode
  char **args;   //arguments to this instruction
  char **labels; //labels referencing this instruction
  int row;       //row in the sif file, this instruction originates from
  int ofs;       //offset in the SBC file
} instr_t;

typedef struct { char *key; int value; } *labels_t;

typedef struct sif_t { //Parsed Symta Instructions File
//FIXME: some code to free the sif file after use.
  instr_t *instrs;
  labels_t labels;
} sif_t;

/* RT-7: mcache slots no longer live inline in the bytecode
 * stream.  Each per-call-site cache used to be an 8-byte
 * `mcache_t` written into the SBC code section right after the
 * opcode's args -- which meant every MCALL / FXNLGET / FXNLSET /
 * IMMEQ / IMMNE / TMCALL site paid 6 bytes of code-section
 * padding (8 actual cache bytes, 2 saved by the new format)
 * AND every cache write touched a D-side line that was
 * otherwise read-only-prefetched as instruction stream data.
 *
 * Now the bytecode carries a 2-byte `uint16_t` id at each site;
 * the runtime allocates `sbc->mcaches[sbc->mcache_cnt]` at SBC
 * load and the id indexes into that array.  Code-section size
 * shrinks by 6 bytes per cache-bearing opcode (~9 % of total
 * code on call-heavy modules); cache writes go to a clean
 * D-side region that the L1 prefetcher and the bytecode dispatch
 * loop don't fight over. */
/* RT-6b: mcache wire format -- carries (tid, mid, fn) directly
 * instead of a pointer into the (now retired) stable method-node
 * arena.  16 bytes per slot, fits in a single cache line for any
 * normal mcache layout.  The dispatch hot path becomes:
 *   tid_match = (mce->tid == O_TAG(o));
 *   mid_match = (mce->mid == m);
 *   if (hit) CALL(mce->fn) else fill via get_method_for_tag.
 * Pre-RT-6b SBCs that depended on the 8-byte node-pointer
 * mcache slot still re-execute correctly because mcache_cnt
 * (RT-7 wire-format field) only describes how many slots to
 * allocate -- the slot size is a runtime-internal detail. */
typedef struct {
  uint32_t tid;
  uint32_t mid;
  dyn fn;
} mcache_t;

#define SBC_MCACHE

typedef struct sbc_t {
  char *filename;
  uint8_t *file; //FIXME: file body can be attached to the end of sbc_t
  uint8_t *code;
  int code_sz;
  uint8_t *data;
  int data_sz;
  uint8_t *tbls;
  int tbls_sz;
  uint8_t *fntbl;
  int fntbl_sz;
  uint8_t *tot;
  uint32_t setup;
  uint32_t entry;
  uint64_t *hooks; //array of offsets into hooks_base
  uint8_t *nrs; //native functions relocations
  int nrs_sz;
  /* CORE-1 (side-table form): per-SBC lineno table.  Each entry
   * is 12 bytes -- (uint32 pc_offset_in_code, uint32 row,
   * uint16 col, uint16 pad) -- sorted by pc.  print_stack_trace
   * binary-searches for the largest pc <= frame->pin to recover
   * a per-instruction position without paying for an interleaved
   * SBC_LSRC opcode on the hot dispatch path. */
  uint8_t *lineno_table;
  int lineno_sz;
  /* RT-7: D-side mcache slots, indexed by the uint16_t id
   * embedded in the bytecode at each cache-bearing opcode.
   * Allocated by `sbc_prepare` once `mcache_cnt` is known. */
  mcache_t *mcaches;
  uint32_t mcache_cnt;
  /* HELP-3: docstring section.  Each entry is six bytes:
   * 3-byte symbol-name offset into `data`, then 3-byte
   * docstring offset into `data`.  Both strings are
   * NUL-terminated where they sit; lookups can return
   * a `char *` directly without copying. */
  uint8_t *doc_table;
  uint32_t doc_sz;
  tot_entry_t rtot[7]; //relocated tot
  dyn *tx;
  dyn *ty;
  int *mt;
  dyn *libs;
  dyn *imlibs;
  dyn *im;
  int ready; //is this symta bytecode module ready for execution?
  void **nfi_trmps; //nfi trampoulines
  void *nfi_ctx; //nfi context
  int src_file_string; //offset of the name of the src filed used to build sbc
  int deps_string;
  int export_string;
} sbc_t;

typedef struct { int key; char *value; } *rlabels_t;

typedef struct dasm_t { //disassembler state
  sbc_t *sbc;
  uint8_t *pin;
  instr_t ins;
  rlabels_t o2l;
  int row;
  int arglist_bits;
  int arglist_i;
  int arglist_nargs;
} dasm_t;

void sif_loader_init();
void instr_free(instr_t *ins);
void sif_print(instr_t *ins);
sif_t *sif_parse(char *file, int file_size);
void sif_free(sif_t *sif);
uint8_t *sif2sbc(sif_t *sif);
sif_t *sbc2sif(char *filename);
dyn sbc_exec_file(char *path);
dyn sbc_exec(sbc_t *sbc);
sbc_t *sbc_new(uint8_t *pin, int64_t size, char *path);
sbc_t *sbc_load(char *path);
void sbc_free(sbc_t *sbc);
void sbc_dasm(sbc_t *sbc, uint8_t *pin, int n);
void sbc_dasm_sub(dasm_t *dasm);
int sbc_load_metadata(char *filename, char **src, char **deps, char **export);
dyn sbc_exec_fn(uint8_t *bytecode);
void sbc_dasm_fn(uint8_t *pin, int n);

void sbc_init_types(sbc_t *sbc, void *tables);

typedef struct { char *key; int value; } asmsbc_t;

typedef struct { int key; char *value; } sbcasm_t;

extern asmsbc_t *asmsbc;
extern sbcasm_t *sbcasm;

//#define SBC_TRANSITION


#endif