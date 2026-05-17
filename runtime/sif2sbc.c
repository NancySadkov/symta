//#define SBC_REMAP
#include "sif.h"
#include "ng.h"

#include <ctype.h>


static instr_t *curins;

static int refidx(char *s) {
  if (*s == 'd' && !strcmp(s,"dummy")) {
    fprintf(stderr, "refidx got dummy!\n");
    sif_print(curins);
    exit(-1);
  }
  while (*s && *s != '[') s++;
  if (*s) s++;
  return strtol(s,0,10);
}

static int nfi_type_id(char *t) {
  int r = -1;
  if (!strcmp(t,"void")) r = SBC_NFI_VOID;
  else if (!strcmp(t,"int")) r = SBC_NFI_INT;
  else if (!strcmp(t,"ptr")) r = SBC_NFI_PTR;
  else if (!strcmp(t,"text")) r = SBC_NFI_TXT;
  else if (!strcmp(t,"u4")) r = SBC_NFI_U4;
  else if (!strcmp(t,"float")) r = SBC_NFI_FLT;
  else if (!strcmp(t,"double")) r = SBC_NFI_DBL;
  return r;
}



#define SIF_MAX_INSTR_SIZE 11

#define SIF_RELC16 1
#define SIF_RELC24 2
#define SIF_RELC64 3

typedef struct {
  int offset;
  char *label;
  int type;
} reloc_t;

uint8_t *sif2sbc(sif_t *sif) {
  int i,j,k;
  instr_t *instrs = sif->instrs;

  struct { char *key; int value; } *label2fn = 0;
  sh_new_arena(label2fn); //store key copies
  shdefault(label2fn,-1); //should be after sh_new_arena

  int fnrefr;

#define FNREF(l) \
  (((fnrefr=shget(label2fn,l))!=-1) ? fnrefr : \
   (fnrefr=shlen(label2fn), shput(label2fn,l,fnrefr), fnrefr))

  instr_t **bs = 0; //arbitrary byte sequences

  instr_t **fm = 0;
  instr_t **tx = 0;
  instr_t **ty = 0;
  instr_t **mt = 0;
  instr_t **libs = 0;
  instr_t **imlib = 0;
  instr_t **im = 0;
  instr_t **doc = 0;     /* HELP-3: docstring entries from `t doc <sym> <val>` SIF directives. */

  labels_t l2o = 0; //label to offset map
  sh_new_arena(l2o); //store key copies

  reloc_t *rs = 0; //references needing relocation

  int *nrs = 0; //NFI relocs

  /* CORE-1 (side-table form): collected at emit time, written to
   * a dedicated SBC section after nrs.  Each entry is 12 bytes
   * (u32 pc, u32 row, u16 col, u16 pad) and stays sorted by pc
   * because we append in instruction order and code offsets
   * monotonically increase. */
  typedef struct {
    uint32_t pc;
    uint32_t row;
    uint16_t col;
    uint16_t pad;
  } lineno_ent_t;
  lineno_ent_t *linenos = 0;

  char *src_label = 0;

  char *deps_label = 0;
  char *export_label = 0;

  uint8_t *hdr = 0;
  uint8_t *code = 0;
  uint8_t *tbls = 0;

  uint8_t *wb = 0;

#define EMIT_RELC24(l) do { \
  reloc_t r; \
  r.offset = arrlen(wb); \
  r.label = l; \
  r.type = SIF_RELC24; \
  arrput(rs,r); \
  EMIT24(0); \
} while (0)

#define EMIT_RELC16(l) do { \
  reloc_t r; \
  r.offset = arrlen(wb); \
  r.label = l; \
  r.type = SIF_RELC16; \
  arrput(rs,r); \
  EMIT16(0); \
} while (0)


#define EMIT_LABELS(ls) do {\
   for (int j = 0; j < arrlen(ls); j++) { \
    shput(l2o,ls[j], arrlen(wb)); \
  } \
} while(0)

  //ensure entry symbols are referenced
  FNREF("entry");

  if (!arrlen(instrs) || strcmp(instrs[0].args[0],"fatal")) {
    EMIT8(SBC_FATAL); //ensure no valid code is at offset 0
    EMIT24(0);        //this simplifies debugging null-pointer calls
  }
  int instr_count = 0;
  int nfi_argc = 0;
  /* RT-7: per-SBC mcache id counter.  Each cache-bearing opcode
   * (MCALL / MCALLIR / MCALL8 / TMCALL / FXNLGET / FXNLSET /
   * FXNLSETIR / IMMEQ / IMMNE) emits this counter as a 2-byte
   * id in its bytecode and increments it.  At SBC load time the
   * runtime allocates `sbc->mcaches[counter_final]` so each id
   * indexes a distinct D-side slot. */
  uint32_t mcache_counter = 0;
  for (i = 0; i < arrlen(instrs); i++) {
    instr_t *ins = &instrs[i];
    char **as = ins->args;
    int nargs = arrlen(as);
    char **ls = ins->labels;
    if (ins->opcode != SBC_BS) EMIT_LABELS(ls);
    else {
      arrput(bs, ins);
      continue;
    }
    int bofs = arrlen(wb);
    ++instr_count;

    curins = ins;

    //fprintf(stderr, "%d/%d:%s\n", i, (int)arrlen(instrs),as[0]);
    switch(ins->opcode) {
    case SBC_SRC: {
      src_label = as[1];
      break;}
    case SBC_DEPS: {
      deps_label = as[1];
      break;}
    case SBC_EXPORT: {
      export_label = as[1];
      break;}
    case SBC_T: {
      --instr_count;
      if (arrlen(as) < 2) {
        fprintf(stderr, "sif2c: bad table at row %d", ins->row);
        exit(-1);
      }
      if (!strcmp(as[1],"fm")) arrput(fm, ins);
      else if (!strcmp(as[1],"tx")) arrput(tx, ins);
      else if (!strcmp(as[1],"ty")) arrput(ty, ins);
      else if (!strcmp(as[1],"mt")) arrput(mt, ins);
      else if (!strcmp(as[1],"libs")) arrput(libs, ins);
      else if (!strcmp(as[1],"imlib")) {
        //printf("imlib: %s\n", as[2]);
        arrput(imlib, ins);
      } else if (!strcmp(as[1],"im")) arrput(im, ins);
      else if (!strcmp(as[1],"doc")) arrput(doc, ins);
      else {
        fprintf(stderr, "sif2c: bad table type `%s`\n", as[1]);
        exit(-1);
      }
      break;}
    case SBC_IGNORE: { break;}
    case SBC_NOP: {EMIT8(SBC_NOP); break;}
    case SBC_SUBR: {
      int size = strtol(as[1],0,10);
      EMIT8(SBC_SUBR);
      EMIT16(0);
      EMIT16(size);
      break;}
    case SBC_LEAVE: {
      if (!strcmp(as[1],"0")) {
        EMIT8(SBC_LEAVE0);
      } else if (isdigit(as[1][0])) {
        int n = strtol(as[1],0,10);
        EMIT8(SBC_LEAVEN);
        EMIT16(n);
      } else {
        EMIT8(SBC_LEAVE);
        int rval = refidx(as[1]);
        EMIT16(rval);
      }
      break;}
    case SBC_JMP: {
      char *l = as[1];
      int tofs = shget(sif->labels,l);
      int pc = (int)(ins-instrs)-tofs;
      pc *= SIF_MAX_INSTR_SIZE;
      pc += 1;
      if (-0x8000 < pc && pc < 0x7fff) {
        //FIXME: do two passes:
        //       1st estimates maximum instruction size
        //       2nd can use it to emit more precise jumps            
        EMIT8(SBC_JMP16);
        EMIT_RELC16(l);
      } else {
        EMIT8(SBC_JMP);
        EMIT_RELC24(l);
      }
      break;}
    case SBC_B: {
      int cnd = refidx(as[1]); //always branches on a local L variable
      char *l = as[2];
      int tofs = shget(sif->labels,l);
      int pc = (int)(ins-instrs)-tofs;
      pc *= SIF_MAX_INSTR_SIZE;
      pc += 1;
      if (cnd < 256 && -0x7ffe < pc && pc < 0x7fff) { //are these enough?
        EMIT8(SBC_B8);
        EMIT8(cnd);
        EMIT_RELC16(l);
      } else {
        EMIT8(SBC_B);
        EMIT16(cnd);
        EMIT_RELC24(l);
      }
      break;}
    case SBC_CALL: {
      int fval = refidx(as[2]);
      if (!strcmp(as[1],"dummy")) {
        EMIT8(SBC_CALLIR);
        EMIT16(fval);
      } else {
        EMIT8(SBC_CALL);
        int rval = refidx(as[1]);
        EMIT16(rval);
        EMIT16(fval);
      }
      break;}
    case SBC_IFFXN: {
      int cnd = refidx(as[1]); //always branches on a local L variable
      char *l = as[2];
      EMIT8(SBC_IFFXN);
      EMIT8(cnd);
      EMIT_RELC16(l);
      break;}
    case SBC_CALLT: {
      int fval = refidx(as[2]);
      if (!strcmp(as[1],"dummy")) {
        EMIT8(SBC_CALLTIR);
        EMIT16(fval);
      } else {
        EMIT8(SBC_CALLT);
        int rval = refidx(as[1]);
        EMIT16(rval);
        EMIT16(fval);
      }
      break;}
    case SBC_MCALL: {
      int oval = refidx(as[2]);
      int midx = refidx(as[3]);
      if (!strcmp(as[1],"dummy")) {
        EMIT8(SBC_MCALLIR);
        EMIT16(oval);
        EMIT16(midx);
      } else {
        int rval = refidx(as[1]);
        if (rval<256 && oval<256 && midx<256) {
          EMIT8(SBC_MCALL8);
          EMIT8(rval);
          EMIT8(oval);
          EMIT8(midx);
        } else {
          EMIT8(SBC_MCALL);
          EMIT16(rval);
          EMIT16(oval);
          EMIT16(midx);
        }
      }
      EMIT16(mcache_counter++); /* RT-7: 2-byte mcache id */ 
      break;}
    case SBC_TMCALL: {
      int oval = refidx(as[1]);
      int midx = refidx(as[2]);
      EMIT8(SBC_TMCALL);
      EMIT16(oval);
      EMIT16(midx);
      EMIT16(mcache_counter++); /* RT-7: 2-byte mcache id */ 
      break;}
    case SBC_TCALL: {
      int fval = refidx(as[1]);
      EMIT8(SBC_TCALL);
      EMIT16(fval);
      break;}
    case SBC_CLOSURE: {
      int dst = refidx(as[1]);
      int size = strtol(as[3],0,10);
      EMIT8(SBC_CLOSURE);
      EMIT16(dst);
      if (!strcmp(as[2],"0")) {
        EMIT16(0); //local closure
      } else {
        EMIT16(FNREF(as[2]));
      }
      EMIT8(size);
      break;}
    case SBC_OBJECT: {
      int dst = refidx(as[1]);
      int type = refidx(as[2]);
      int size = strtol(as[3],0,10);
      EMIT8(SBC_OBJECT);
      EMIT16(dst);
      EMIT16(type);
      EMIT16(size);
      break;}
    case SBC_CNAS: {
      EMIT8(SBC_CNAS);
      EMIT16(strtol(as[1],0,10));
      break;}
    case SBC_ARGLIST: {
      int nargs = strtol(as[1],0,10);
      int all_below256 = nargs<256;
      if (all_below256) for (j = 0; j < nargs; j++) {
        instr_t *ains = &instrs[i+j+1];
        if (ains->opcode != SBC_STARG) {
          fprintf(stderr, "SBC_ARGLIST: unexpected opcode=%d\n",ains->opcode);
          exit(-1);
        }
        int arg = refidx(ains->args[2]);
        if (arg >= 256) {
          all_below256 = 0;
        }
      }
      if (all_below256) {
        if (nargs < 6) {
          EMIT8(SBC_ARGLIST0+nargs);
        } else {
          EMIT8(SBC_ARGLIST8);
          EMIT8(nargs);
        }
        for (j = 0; j < nargs; j++) {
          instr_t *ains = &instrs[i+j+1];
          int arg = refidx(ains->args[2]);
          EMIT8(arg);
        }
      } else {
        EMIT8(SBC_ARGLIST);
        EMIT16(nargs);
        for (j = 0; j < nargs; j++) {
          instr_t *ains = &instrs[i+j+1];
          int arg = refidx(ains->args[2]);
          EMIT16(arg);
        }
      }
      i += nargs;
      break;}
    case SBC_LIST: {
      if (!strcmp(as[1],"dummy")) {
        //FIXME: avoid generating such opeations
      } else {
        int dst = refidx(as[1]);
        int size = strtol(as[2],0,10);
        EMIT8(SBC_LIST);
        EMIT16(dst);
        EMIT16(size);
      }
      break;}
    case SBC_LIST2: {
      /* RT-9: fused size-2 list allocation.  SIF form:
       *   list2 <dst> <a> <b>
       * Compiler emits this when ssa_list sees Xs.n == 2 and
       * both elements are simple (non-aggregate) values that
       * map straight to register refs. */
      int dst = refidx(as[1]);
      int a   = refidx(as[2]);
      int b   = refidx(as[3]);
      EMIT8(SBC_LIST2);
      EMIT16(dst);
      EMIT16(a);
      EMIT16(b);
      break;}
    case SBC_LIST1: {
      /* RT-9: fused size-1 list allocation.  SIF form:
       *   list1 <dst> <x> */
      int dst = refidx(as[1]);
      int x   = refidx(as[2]);
      EMIT8(SBC_LIST1);
      EMIT16(dst);
      EMIT16(x);
      break;}
    case SBC_MOVE: {
      if (!strcmp(as[2],"Empty")) {
        int dst = refidx(as[1]);
        EMIT8(SBC_MOVEEMT);
        EMIT16(dst);
      } else if (!strcmp(as[2],"No")) {
        int dst = refidx(as[1]);
        EMIT8(SBC_MOVENO);
        EMIT16(dst);
      } else if (!strncmp(as[2],"FIXTEXT",7)) {
        int dst = refidx(as[1]);
        uint64_t imm = strtoull(as[2]+8,0,10);
#ifndef SBC_TRANSITION
        if (dst < 256) {
          if (!(imm>>8)) {
            EMIT8(SBC_FXT8);
            EMIT8(dst);
            EMIT8(imm);
          } else if (!(imm>>16)) {
            EMIT8(SBC_FXT16);
            EMIT8(dst);
            EMIT16(imm);
          } else if (!(imm>>24)) {
            EMIT8(SBC_FXT24);
            EMIT8(dst);
            EMIT24(imm);
          } else if (!(imm>>32)) {
            EMIT8(SBC_FXT32);
            EMIT8(dst);
            EMIT32(imm);
          } else if (!(imm>>40)) {
            EMIT8(SBC_FXT40);
            EMIT8(dst);
            EMIT40(imm);
          } else if (!(imm>>48)) {
            EMIT8(SBC_FXT48);
            EMIT8(dst);
            EMIT48(imm);
          } else if (!(imm>>56)) {
            EMIT8(SBC_FXT56);
            EMIT8(dst);
            EMIT56(imm);
          } else {
            EMIT8(SBC_IMMB64);
            EMIT8(dst);
            EMIT64((uint64_t)FIXTEXT(imm));
          }
        } else
#else
#error "Implement conversion"
#endif
        {
          EMIT8(SBC_IMM64);
          EMIT16(dst);
          EMIT64(FIXTEXT(imm));
        }
      } else if (!strncmp(as[2],"tx",2)) {
        int dst = refidx(as[1]);
        int src = refidx(as[2]);
        if (dst < 256 && src < 256) {
          EMIT8(SBC_MOVETX8);
          EMIT8(dst);
          EMIT8(src);
        } else {
          EMIT8(SBC_MOVETX);
          EMIT16(dst);
          EMIT24(src);
        }
      } else if (!strncmp(as[2],"mt",2)) {
        int dst = refidx(as[1]);
        int src = refidx(as[2]);
        if (dst < 256 && src < 256) {
          EMIT8(SBC_MOVEMT8);
          EMIT8(dst);
          EMIT8(src);
        } else {
          EMIT8(SBC_MOVEMT);
          EMIT16(dst);
          EMIT24(src);
        }
      } else if (!strncmp(as[2],"im",2)) {
        int dst = refidx(as[1]);
        int src = refidx(as[2]);
        EMIT8(SBC_MOVEIM);
        EMIT16(dst);
        EMIT24(src);
      } else {
        int dst = refidx(as[1]);
        int src = refidx(as[2]);
        if (src < 16 && dst < 16) {
          EMIT8(SBC_MOVE4);
          EMIT8(dst|(src<<4));
        } else if (src < 256 && dst < 256) {
          EMIT8(SBC_MOVE8);
          EMIT8(dst);
          EMIT8(src);
        } else {
          EMIT8(SBC_MOVE);
          EMIT16(dst);
          EMIT16(src);
        }
      }
      break;}
    case SBC_STOR: {
      if (!strcmp(as[1],"dummy")) {
        //FIXME: avoid generating such instructions
      } else {
        int dst = refidx(as[1]);
        int index = strtol(as[2],0,10);
        int src = refidx(as[3]);
        if (dst < 16 && src < 16 && index < 16) {
          EMIT8(SBC_ST4_0+index);
          EMIT8((src<<4)|dst);
        } else if (dst < 256 && src < 256 && index < 256) {
          EMIT8(SBC_STOR8);
          EMIT8(dst);
          EMIT8(src);
          EMIT8(index);
        } else {
          EMIT8(SBC_STOR);
          EMIT16(dst);
          EMIT16(src);
          EMIT16(index);
        }
      }
      break;}
    case SBC_LOAD: {
      int dst = refidx(as[1]);
      int src = refidx(as[2]);
      int index = strtol(as[3],0,10);
      if (dst < 16 && src < 16 && index < 16) {
        EMIT8(SBC_LD4_0+index);
        EMIT8((src<<4)|dst);
      } else if (dst < 256 && src < 256 && index < 256) {
        EMIT8(SBC_LOAD8);
        EMIT8(dst);
        EMIT8(src);
        EMIT8(index);
      } else {
        EMIT8(SBC_LOAD);
        EMIT16(dst);
        EMIT16(src);
        EMIT16(index);
      }
      break;}
    case SBC_COPY: {
      int dst = refidx(as[1]);
      int dindex = strtol(as[2],0,10);
      int src = refidx(as[3]);
      int sindex = strtol(as[4],0,10);
      EMIT8(SBC_COPY);
      EMIT16(dst);
      EMIT16(src);
      EMIT16(dindex);
      EMIT16(sindex);
      break;}
    case SBC_LDFXN: {
      if (!strcmp(as[1], "dummy")) break;
      int dst = refidx(as[1]);
      int64_t val = strtoll(as[2],0,10);
      if (dst < 0x100) {
        if (val == 0) {
          EMIT8(SBC_FXNB0);
          EMIT8(dst);
        } else if (-128 <= val && val < 128) {
          EMIT8(SBC_FXNB8);
          EMIT8(dst);
          EMIT8((uint8_t)(int8_t)val);
        } else if (-32768 <= val && val < 32768) {
          EMIT8(SBC_FXNB16);
          EMIT8(dst);
          EMIT16((uint16_t)(int16_t)val);
        } else if (-2147483648 <= val && val < 2147483648) {
          EMIT8(SBC_FXNB32);
          EMIT8(dst);
          EMIT32((uint32_t)(int32_t)val);
        } else {
#ifndef SBC_TRANSITION
          EMIT8(SBC_IMMB64);
          EMIT8(dst);
          EMIT64((uint64_t)FXN(val));
#else
#error "Implement conversion"
#endif
        }
      } else {
        if (val == 0) {
          EMIT8(SBC_FXN0);
          EMIT16(dst);
        } else if (-128 <= val && val < 128) {
          EMIT8(SBC_FXN8);
          EMIT16(dst);
          EMIT8((uint8_t)(int8_t)val);
        } else if (-32768 <= val && val < 32768) {
          EMIT8(SBC_FXN16);
          EMIT16(dst);
          EMIT16((uint16_t)(int16_t)val);
        } else if (-2147483648 <= val && val < 2147483648) {
          EMIT8(SBC_FXN32);
          EMIT16(dst);
          EMIT32((uint32_t)(int32_t)val);
        } else {
#ifndef SBC_TRANSITION
          EMIT8(SBC_IMM64);
          EMIT16(dst);
          EMIT64((uint64_t)FXN(val));
#else
#error "Implement conversion"
#endif
        }
      }
      break;}
    case SBC_FXNTAG: {
      EMIT8(SBC_FXNTAG);
      EMIT16(refidx(as[1]));
      EMIT16(refidx(as[2]));
      break;}
    case SBC_FXNLISTN: {
      EMIT8(SBC_FXNLISTN);
      EMIT16(refidx(as[1]));
      EMIT16(refidx(as[2]));
      break;}
    case SBC_FXNLGET: {
      if (!strcmp(as[1],"dummy")) break;
      EMIT8(SBC_FXNLGET);
      EMIT16(refidx(as[1]));
      EMIT16(refidx(as[2]));
      EMIT16(refidx(as[3]));
      EMIT16(mcache_counter++); /* RT-7: 2-byte mcache id */ 
      break;}
    case SBC_FXNLSET: {
      if (!strcmp(as[1],"dummy")) {
        EMIT8(SBC_FXNLSETIR);
        EMIT16(refidx(as[2]));
        EMIT16(refidx(as[3]));
        EMIT16(refidx(as[4]));
      } else {
        EMIT8(SBC_FXNLSET);
        EMIT16(refidx(as[1]));
        EMIT16(refidx(as[2]));
        EMIT16(refidx(as[3]));
        EMIT16(refidx(as[4]));
      }
      EMIT16(mcache_counter++); /* RT-7: 2-byte mcache id */ 
      break;}
    case SBC_FXNSIZE: {
      EMIT8(SBC_FXNSIZE);
      EMIT16(refidx(as[1]));
      EMIT16(refidx(as[2]));
      break;}
    case SBC_IMMEQ:
    case SBC_IMMNE: {
      if (!strcmp(as[1],"dummy")) break;
      EMIT8(ins->opcode);
      EMIT16(refidx(as[1]));
      EMIT16(refidx(as[2]));
      EMIT16(refidx(as[3]));
      EMIT16(mcache_counter++); /* RT-7: 2-byte mcache id */ 
      break;
    }
    case SBC_FXNADD:
    case SBC_FXNSUB:
    case SBC_FXNMUL:
    case SBC_FXNDIV:
    case SBC_FXNREM:
    case SBC_FXNLT:
    case SBC_FXNGT:
    case SBC_FXNLTE:
    case SBC_FXNGTE:
    case SBC_FXNAND:
    case SBC_FXNIOR:
    case SBC_FXNXOR:
    case SBC_FXNSHL:
    case SBC_FXNSHR:
    case SBC_SAME:
    case SBC_VARY:
    {
      if (!strcmp(as[1],"dummy")) break;
      EMIT8(ins->opcode);
      EMIT16(refidx(as[1]));
      EMIT16(refidx(as[2]));
      EMIT16(refidx(as[3]));
      break;
    }
    case SBC_LDFLT: {
      int dst = refidx(as[1]);
      dyn val;
      LDFLT(val,strtod(as[2],0));
      EMIT8(SBC_IMM64);
      EMIT16(dst);
      EMIT64((uint64_t)val);
      break;}
    case SBC_TINIT: {
      //char tya[16]; refarr(tya,as[1]); //always ty
      int tyi = refidx(as[1]);
      int tsize = strtol(as[2],0,10);
      char *name = as[3];
      if (!strncmp(name,"FIXTEXT",7)) {
        uint64_t imm = strtoull(name+8,0,10);
        EMIT8(SBC_TINITI);
        EMIT16(tyi);
        EMIT16(tsize);
        EMIT64(FIXTEXT(imm));
      } else {
        int txref = refidx(as[1]);
        EMIT8(SBC_TINIT);
        EMIT16(tyi);
        EMIT16(tsize);
        EMIT24(txref);
      }
      break;}
    case SBC_SUBTYPE: {
      int tsuper = refidx(as[1]);
      int tsub = refidx(as[2]);
      EMIT8(SBC_SUBTYPE);
      EMIT16(tsuper);
      EMIT16(tsub);
      break;}
    case SBC_DMET: {
      int mtidx = refidx(as[1]);
      int tyidx = refidx(as[2]);
      int handler = refidx(as[3]);
      EMIT8(SBC_DMET);
      EMIT16(tyidx);
      EMIT24(mtidx);
      EMIT16(handler);
      break;}
    case SBC_ABS:
    case SBC_NEG:
    case SBC_INC:
    case SBC_DEC:
    case SBC_NOT:
    case SBC_GOT:
    case SBC_NO:
    case SBC_GID: {
      EMIT8(ins->opcode);
      EMIT16(refidx(as[1]));
      EMIT16(refidx(as[2]));
      break;}
    case SBC_CURMET: {
      EMIT8(SBC_CURMET);
      EMIT16(refidx(as[1]));
      break;}
    case SBC_MNAME: {
      EMIT8(SBC_MNAME);
      EMIT16(refidx(as[1]));
      EMIT16(refidx(as[2]));
      break;}
    case SBC_FATAL: {
      EMIT8(SBC_FATAL);
      EMIT16(refidx(as[1]));
      break;}
    case SBC_GC0: {EMIT8(SBC_GC0); break;}
    case SBC_GC1: {EMIT8(SBC_GC1); break;}
    case SBC_NFA: {
      if (!nfi_argc) {
        arrput(nrs, arrlen(wb)); //register the start of a function
        ++nfi_argc;
      }
      char *dst = as[1];
      char *type = as[2];
      int nfi_tid = nfi_type_id(type);
      if (nfi_tid == -1) {
        fprintf(stderr, "NFI: bad arg type = %s\n", type);
        exit(-1);
      }
      EMIT8(nfi_tid);
      EMIT16(refidx(as[1]));
      break;}
    case SBC_NFI: { //native function invocation
      if (!nfi_argc) arrput(nrs, arrlen(wb)); //no arguments case
      nfi_argc = 0;
      EMIT8(SBC_NFI);
      EMIT16(refidx(as[1]));
      EMIT16(arrlen(nrs)-1); //index into the trampouline table
      break;}
    case SBC_NFR: { //return value for native function
      char *dst = as[1];
      char *type = as[2];
      int nfi_tid = nfi_type_id(type);
      if (nfi_tid == -1) {
        fprintf(stderr, "NFI: bad result type = %s\n", type);
        exit(-1);
      }
      EMIT8(nfi_tid);
      if (!strcmp(dst,"dummy")) {
        EMIT16(0);
      } else {
        EMIT16(refidx(dst));
      }
      break;}
    case SBC_NLD: {
      int dst = refidx(as[1]);
      char *t = as[2];
      int ptr = refidx(as[3]);
      int ofs = refidx(as[4]);
      if (!strcmp(t,"uint8_t") || !strcmp(t,"u1")) EMIT8(SBC_NLDU1);
      else if (!strcmp(t,"int8_t") || !strcmp(t,"s1")) EMIT8(SBC_NLDS1);
      else if (!strcmp(t,"uint16_t") || !strcmp(t,"u2")) EMIT8(SBC_NLDU2);
      else if (!strcmp(t,"int16_t") || !strcmp(t,"s2")) EMIT8(SBC_NLDS2);
      else if (!strcmp(t,"uint32_t") || !strcmp(t,"u4")) EMIT8(SBC_NLDU4);
      else if (!strcmp(t,"int32_t") || !strcmp(t,"s4")) EMIT8(SBC_NLDS4);
      else {
        fprintf(stderr, "SBC_NLD: bad type = %s\n", t);
        exit(-1);
      }
      EMIT16(dst);
      EMIT16(ptr);
      EMIT16(ofs);
      break;}
    case SBC_NST: {
      char *t = as[1];
      int ptr = refidx(as[2]);
      int ofs = refidx(as[3]);
      int val = refidx(as[4]);
      if (!strcmp(t,"uint8_t") || !strcmp(t,"u1")) EMIT8(SBC_NSTU1);
      else if (!strcmp(t,"int8_t") || !strcmp(t,"s1")) EMIT8(SBC_NSTS1);
      else if (!strcmp(t,"uint16_t") || !strcmp(t,"u2")) EMIT8(SBC_NSTU2);
      else if (!strcmp(t,"int16_t") || !strcmp(t,"s2")) EMIT8(SBC_NSTS2);
      else if (!strcmp(t,"uint32_t") || !strcmp(t,"u4")) EMIT8(SBC_NSTU4);
      else if (!strcmp(t,"int32_t") || !strcmp(t,"s4")) EMIT8(SBC_NSTS4);
      else {
        fprintf(stderr, "SBC_NST: bad type = %s\n", t);
        exit(-1);
      }
      EMIT16(ptr);
      EMIT16(ofs);
      EMIT16(val);
      break;}
    case SBC_SETJMP: {
      EMIT8(SBC_CTX);
      EMIT8(0); //save context
      EMIT16(refidx(as[1])); //dst
      break;}
    case SBC_LONGJMP: {
      EMIT8(SBC_CTX);
      EMIT8(1); //restore cotext
      EMIT16(refidx(as[1])); //saved context
      EMIT16(refidx(as[2])); //returned value
      break;}
    case SBC_SET_UNWIND_HANDLER: {
      /* CORE-2: push a finalizer closure onto api.puwh.  Lowered
       * to SBC_CTX sub-type 2 so the dispatch lives next to the
       * existing btland (0) / btjump (1) handlers. */
      EMIT8(SBC_CTX);
      EMIT8(2);
      EMIT16(refidx(as[1])); //handler register
      break;}
    case SBC_REMOVE_UNWIND_HANDLER: {
      EMIT8(SBC_CTX);
      EMIT8(3);
      break;}
    case SBC_LSRC: {
      /* CORE-1 (side-table form): no bytecode is emitted -- the
       * interpreter never dispatches a per-line opcode.  Instead
       * we record (current code offset, row, col) into a parallel
       * sorted table that print_stack_trace binary-searches.
       * Keeps the hot dispatch path cache-clean while preserving
       * accurate stack-trace positions.
       *
       * --instr_count compensates for the unconditional ++ at the
       * top of the loop: a lineno marker isn't an executable
       * instruction, so it must not bump the counter that
       * JMP/branch offset estimates depend on. */
      lineno_ent_t e;
      e.pc = (uint32_t)arrlen(wb);
      e.row = (uint32_t)strtol(as[1],0,10);
      e.col = (uint16_t)strtol(as[2],0,10);
      e.pad = 0;
      arrput(linenos, e);
      --instr_count;
      break;}
    default: {
      fprintf(stderr, "sif2sbc: bad operator `%s`\n", as[0]);
      exit(-1);
      break;}
    }
  }
  
  //fprintf(stderr, ":::: %d < %d\n", instr_count*4, (int)arrlen(wb));

  //emit arbitrary bytes data
  int code_sz = arrlen(wb);
  for (i = 0; i < arrlen(bs); i++) {
    instr_t *ins = bs[i];
    char **as = ins->args;
    int nargs = arrlen(as);
    char **ls = ins->labels;
    EMIT_LABELS(ls);
    for (int j = 1; j < nargs; j++) {
      int b = strtol(as[j],0,10);
      EMIT8(b);
    }
  }

  while (arrlen(wb)&0x7) EMIT8(0); //pad it to retain alignment
  
  int data_sz = arrlen(wb)-code_sz;

  code = wb;

  //patch relocations
  for (i = 0; i < arrlen(rs); i++) {
    reloc_t *r = &rs[i];
    int ofs = shget(l2o,r->label);
    if (r->type == SIF_RELC24) {
      code[r->offset] = ofs&0xff;
      code[r->offset+1] = (ofs>>8)&0xff;
      code[r->offset+2] = (ofs>>16)&0xff;
    } else if (r->type == SIF_RELC16) {
      uint16_t rofs = (uint16_t)(int16_t)(ofs - r->offset - 2);
      code[r->offset] = rofs&0xff;
      code[r->offset+1] = (rofs>>8)&0xff;
    } else {
      fprintf(stderr, "FIXME: reloc_t->type = %d\n", r->type);
    }
  }
  wb = 0;
  //emit functions table
  for (i = 0; i < shlen(label2fn); i++) {
    int ofs = shget(l2o,label2fn[i].key);
    EMIT24(ofs);
  }

  uint8_t *fntbl = wb;
  wb = 0;

  instr_t **ts[7] = {fm,tx,ty,mt,libs,imlib,im};
  int tos[7];
  for (i = 0; i < 7; i++) {
    instr_t **t = ts[i];
    long nentries = arrlen(t);
    tos[i] = arrlen(wb);
    for (j = 0; j < nentries; j++) {
      instr_t *ins = t[j];
      char **as = ins->args;
      int nargs = arrlen(as)-2;
      if (i == 0) {
        for (k = 0; k < nargs; k++) {
          char *a = as[k+2];
          if (k==0) { //nargs
            int fnargs = strtoll(a,0,10);
            if (fnargs < 0) {
              EMIT16(0xFFFF);
            } else {
              EMIT16(fnargs);
            }
          } else if (k==1) { //name string
            int ofs = shget(l2o,a);
            EMIT24(ofs);
          } else if (k==2) { //fn index
            if (!strcmp(a,"0")) {
              EMIT16(0); //local closure
            } else {
              EMIT16(FNREF(a));
            }
          } else if (k==3) { //row
            EMIT16(strtoll(a,0,10));
          } else if (k==4) { //col
            int col = strtoll(a,0,10);
            if (nargs != 6) col |= SBC_DEFAULT_SRC;
            EMIT16(col);
          } else if (k==5) { //src file
            int ofs = shget(l2o,a);
            EMIT24(ofs);
          }
        }
      } else if (i == 5) {
        char *a = as[2];
        EMIT16(strtoll(a,0,10));
      } else {
        char *a = as[2];
        int ofs = shget(l2o,a);
        EMIT24(ofs);
      }
    }
  }

  int nrs_ofs = arrlen(wb);
  int nrs_sz = arrlen(nrs);

  //native functions relocations
  for (i = 0; i < nrs_sz; i++) {
    EMIT24(nrs[i]);
  }

  /* CORE-1: emit the lineno side table after nrs.  Layout per
   * entry matches the on-disk format documented in sif.h's
   * sbc_t.lineno_table comment: u32 pc, u32 row, u16 col, u16 pad. */
  int lineno_ofs = arrlen(wb);
  int lineno_sz = arrlen(linenos);
  for (i = 0; i < lineno_sz; i++) {
    EMIT32(linenos[i].pc);
    EMIT32(linenos[i].row);
    EMIT16(linenos[i].col);
    EMIT16(linenos[i].pad);
  }

  /* HELP-3: emit the docstring section after lineno.  Each entry
   * is six bytes: 3-byte symbol-name offset (into data) followed
   * by 3-byte docstring-text offset (into data).  Both strings
   * live in the existing data section -- the runtime helper can
   * resolve them without a copy. */
  int doc_ofs = arrlen(wb);
  int doc_sz = arrlen(doc);
  for (i = 0; i < doc_sz; i++) {
    instr_t *ins = doc[i];
    char **as = ins->args;
    /* args: ["t", "doc", "<sym-label>", "<val-label>"] */
    if (arrlen(as) < 4) {
      fprintf(stderr, "sif2c: `t doc` needs 2 labels at row %d\n", ins->row);
      exit(-1);
    }
    int sym_ofs = shget(l2o, as[2]);
    int val_ofs = shget(l2o, as[3]);
    EMIT24(sym_ofs);
    EMIT24(val_ofs);
  }

  tbls = wb;
  wb = 0;

  /* tot_sz counts the (count, offset) pairs in the trailing tot:
   * 7 lookup tables + nrs + linenos + RT-7 mcache + HELP-3 docs.
   * The RT-7 mcache entry's offset field doubles as a format flag
   * for stage 2 -- see the EMIT24 pair near the bottom of this
   * function.  The HELP-3 docs entry (count, offset) points to
   * a flat array of (sym_ofs:24, val_ofs:24) pairs, both offsets
   * referring into the data section. */
  int tot_sz = 7 + 3 + 1;

  /* Format identification: 4-byte magic + 2-byte revision.
   * `sbc_new` checks both before touching anything else.  See
   * `runtime/sif.h` for the revision history. */
  EMIT32(SBC_MAGIC);
  EMIT16(SBC_REVISION);
  int src_text_ofs = shget(l2o,src_label);
  EMIT24(src_text_ofs);
  int deps_text_ofs = deps_label ? shget(l2o,deps_label) : 0;
  EMIT24(deps_text_ofs);
  int export_text_ofs = export_label ? shget(l2o,export_label) : 0;
  EMIT24(export_text_ofs);
  EMIT16(tot_sz);
  EMIT24(code_sz);
  EMIT24(data_sz);
  EMIT24(arrlen(tbls));
  EMIT24(arrlen(fntbl));
  int tot_ofs = arrlen(wb);
  for (i = 0; i < 7; i++) {
    EMIT24(arrlen(ts[i]));
    EMIT24(tos[i]);
  }
  EMIT24(nrs_sz);
  EMIT24(nrs_ofs);
  /* CORE-1: (count, offset) pair for the lineno side table.
   * Count is entries, not bytes; each entry is 12 bytes. */
  EMIT24(lineno_sz);
  EMIT24(lineno_ofs);
  /* RT-7: count = number of D-side mcache slots.  Mcaches live
   * in a runtime-malloc'd array, not in the SBC file, so the
   * "offset" field is repurposed as a format flag:
   *   0 = legacy 8-byte cache slot (pre-RT-7 8 SBC_NOPs, or
   *       RT-7 stage 1's 2-byte id + 6 SBC_NOP filler)
   *   1 = compact 2-byte cache slot (just the uint16_t id)
   * The runtime reads this into `sbc->cache_slot_size` so
   * MCACHE_SKIP / MCACHE_CALL advance pin correctly. */
  EMIT24(mcache_counter);
  EMIT24(1); /* RT-7 stage 2: compact 2-byte cache slots */
  /* HELP-3: docstring section -- count + offset into tbls. */
  EMIT24(doc_sz);
  EMIT24(doc_ofs);


  hdr = wb;
  wb = 0;
  
  uint8_t *out = 0;
  aputa(out,hdr);
  aputa(out,code);
  aputa(out,tbls);
  aputa(out,fntbl);

  arrfree(fm);
  arrfree(tx);
  arrfree(ty);
  arrfree(mt);
  arrfree(libs);
  arrfree(imlib);
  arrfree(im);
  
  shfree(l2o);
  shfree(label2fn);
  arrfree(rs);
  arrfree(nrs);
  arrfree(linenos);
  arrfree(doc);
  arrfree(hdr);
  arrfree(code);
  arrfree(tbls);
  arrfree(fntbl);
  arrfree(bs);

  return out;
}


