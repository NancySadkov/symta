#include <ctype.h>

#include "sif.h"
#include "ng.h"

void sif_print(instr_t *ins) {
  FILE *out = stdout;
  for (int i = 0; i < arrlen(ins->labels); i++) {
    fprintf(out, "%s:\n", ins->labels[i]);
  }
  fprintf(out, "%06x", ins->ofs);
  fprintf(out, "  ");
  for (int i = 0; i < arrlen(ins->args); i++) {
    fprintf(out, "%s ", ins->args[i]);
  }
  fprintf(out, "\n");
}


void instr_free(instr_t *ins) {
  int j;
  char **as = ins->args;
  for (j = 0; j < arrlen(as); j++) {
    arrfree(as[j]);
  }
  arrfree(as);
  char **ls = ins->labels;
  for (j = 0; j < arrlen(ls); j++) {
    arrfree(ls[j]);
  }
  arrfree(ls);
}

void sif_free(sif_t *sif) {
  int i,j;
  instr_t *instrs = sif->instrs;
  for (i = 0; i < arrlen(instrs); i++) {
    instr_free(&instrs[i]);
  }
  arrfree(instrs);
  shfree(sif->labels);
  free(sif);
}


instr_t **sif_cmp_normalize(instr_t *instrs) {
  instr_t **ris = 0;
  for (int i = 0; i < arrlen(instrs); i++) {
    instr_t *ins = &instrs[i];
    char **as = ins->args;
    int nargs = arrlen(as);
    if (i == 0 && ins->opcode==SBC_FATAL) continue;
    if (ins->opcode==SBC_IGNORE) continue;
    if (ins->opcode==SBC_LIST && !strcmp(as[1],"dummy")) continue;
    if (ins->opcode==SBC_STOR && !strcmp(as[1],"dummy")) continue;
    switch (ins->opcode) {
    case SBC_BS:
    case SBC_T:
      continue;
      break;
    default:
      arrput(ris, ins);
      break;
    }
  }
  return ris;
}

int sif_cmp(sif_t *a, sif_t *b) {
  int r = 1;
  instr_t **ais = sif_cmp_normalize(a->instrs);
  instr_t **bis = sif_cmp_normalize(b->instrs);
  int al = arrlen(ais);
  int bl = arrlen(bis);
  if (al != bl) {
    fprintf(stderr, "Different number of instructions: %d != %d\n",al, bl);
    r = 0;
  }

  int l = MAX(al,bl);

  for (int i = 0; i < l; i++) {
    instr_t *a = ais[i];
    instr_t *b = bis[i];
    char **aas = a->args;
    char **bas = b->args;
    int aasl = arrlen(aas);
    int basl = arrlen(bas);

    //fprintf(stderr, "%s = %s\n",a->args[0],b->args[0]);
    int asl = MAX(aasl,basl);
    for (int j = 0; j < asl; j++) {
      if (j == 1 && !strcmp(aas[0],"jmp")) continue;
      if (j == 2 && !strcmp(aas[0],"branch")) continue;
      if (j == 2 && !strcmp(aas[0],"closure")) continue;
      if (!strcmp(aas[0],"load_float")) continue;
      if (strcmp(aas[j],bas[j])) {
        fprintf(stderr, "Instructions differ\n");
        sif_print(a);
        sif_print(b);
        r = 0;
        //exit(-1);
        break;
      }
    }
  }
  arrfree(ais);
  arrfree(bis);
  return r;
}



asmsbc_t *asmsbc;
sbcasm_t *sbcasm;


void sif_loader_init() {
  shdefault(asmsbc,-1);
  shput(asmsbc,"nop"       ,SBC_NOP);
  shput(asmsbc,"subr"      ,SBC_SUBR);
  shput(asmsbc,"leave"     ,SBC_LEAVE);
  shput(asmsbc,"jmp"       ,SBC_JMP);
  shput(asmsbc,"b"         ,SBC_B);
  shput(asmsbc,"bz"        ,SBC_BZ);
  shput(asmsbc,"call"      ,SBC_CALL);
  shput(asmsbc,"callt"     ,SBC_CALLT);
  shput(asmsbc,"mcall"     ,SBC_MCALL);
  shput(asmsbc,"closure"   ,SBC_CLOSURE);
  shput(asmsbc,"object"    ,SBC_OBJECT);
  shput(asmsbc,"iffxn"     ,SBC_IFFXN);
  shput(asmsbc,"cnas"      ,SBC_CNAS); //check number of arguments
  shput(asmsbc,"arglist"   ,SBC_ARGLIST);
  shput(asmsbc,"starg"     ,SBC_STARG);
  shput(asmsbc,"list"      ,SBC_LIST);
  shput(asmsbc,"list1"     ,SBC_LIST1);
  shput(asmsbc,"list2"     ,SBC_LIST2);
  shput(asmsbc,"mv"        ,SBC_MOVE);
  shput(asmsbc,"st"        ,SBC_STOR);
  shput(asmsbc,"ld"        ,SBC_LOAD);
  shput(asmsbc,"cp"        ,SBC_COPY);
  shput(asmsbc,"ldfxn"     ,SBC_LDFXN);
  shput(asmsbc,"ldflt"     ,SBC_LDFLT);
  shput(asmsbc,"fxnadd"    ,SBC_FXNADD);
  shput(asmsbc,"fxnsub"    ,SBC_FXNSUB);
  shput(asmsbc,"fxnmul"    ,SBC_FXNMUL);
  shput(asmsbc,"fxndiv"    ,SBC_FXNDIV);
  shput(asmsbc,"fxnrem"    ,SBC_FXNREM);
  // TS-4.1: unboxed int arithmetic.  No tag check, no fallback.
  shput(asmsbc,"iadd"      ,SBC_IADD);
  shput(asmsbc,"isub"      ,SBC_ISUB);
  shput(asmsbc,"imul"      ,SBC_IMUL);
  shput(asmsbc,"idiv"      ,SBC_IDIV);
  shput(asmsbc,"irem"      ,SBC_IREM);
  shput(asmsbc,"fxneq"     ,SBC_IMMEQ);
  shput(asmsbc,"fxnne"     ,SBC_IMMNE);
  shput(asmsbc,"fxnlt"     ,SBC_FXNLT);
  shput(asmsbc,"fxngt"     ,SBC_FXNGT);
  shput(asmsbc,"fxnlte"    ,SBC_FXNLTE);
  shput(asmsbc,"fxngte"    ,SBC_FXNGTE);
  shput(asmsbc,"fxnand"    ,SBC_FXNAND);
  shput(asmsbc,"fxnior"    ,SBC_FXNIOR);
  shput(asmsbc,"fxnxor"    ,SBC_FXNXOR);
  shput(asmsbc,"fxnshl"    ,SBC_FXNSHL);
  shput(asmsbc,"fxnshr"    ,SBC_FXNSHR);
  shput(asmsbc,"fxntag"    ,SBC_FXNTAG);
  shput(asmsbc,"fxnlistn"  ,SBC_FXNLISTN);
  shput(asmsbc,"fxnlget"   ,SBC_FXNLGET);
  shput(asmsbc,"fxnlset"   ,SBC_FXNLSET);
  shput(asmsbc,"fxnsize"   ,SBC_FXNSIZE);
  shput(asmsbc,"abs"      ,SBC_ABS);
  shput(asmsbc,"same"     ,SBC_SAME);
  shput(asmsbc,"vary"     ,SBC_VARY);
  shput(asmsbc,"tinit"    ,SBC_TINIT); //type init
  shput(asmsbc,"subtype"  ,SBC_SUBTYPE);
  shput(asmsbc,"dmet"     ,SBC_DMET);
  shput(asmsbc,"neg"      ,SBC_NEG);
  shput(asmsbc,"not"      ,SBC_NOT);
  shput(asmsbc,"got"      ,SBC_GOT);
  shput(asmsbc,"no"       ,SBC_NO);
  shput(asmsbc,"gid"      ,SBC_GID);
  shput(asmsbc,"inc"      ,SBC_INC);
  shput(asmsbc,"dec"      ,SBC_DEC);
  shput(asmsbc,"curmet"   ,SBC_CURMET); //current method
  shput(asmsbc,"mname"    ,SBC_MNAME); //method name
  shput(asmsbc,"fatal"    ,SBC_FATAL);
  shput(asmsbc,"gc0"      ,SBC_GC0);
  shput(asmsbc,"gc1"      ,SBC_GC1);
  shput(asmsbc,"nfi"      ,SBC_NFI);
  shput(asmsbc,"nfa"      ,SBC_NFA);
  shput(asmsbc,"nfr"      ,SBC_NFR);
  shput(asmsbc,"nld"      ,SBC_NLD);
  shput(asmsbc,"nst"      ,SBC_NST);
  shput(asmsbc,"tcall"    ,SBC_TCALL);
  shput(asmsbc,"tmcall"   ,SBC_TMCALL);
  shput(asmsbc,"ctx"      ,SBC_CTX);
  shput(asmsbc,"lsrc"     ,SBC_LSRC); /* CORE-1: line-source marker */

  shput(asmsbc,"setjmp"   ,SBC_SETJMP);
  shput(asmsbc,"longjmp"  ,SBC_LONGJMP);
  shput(asmsbc,"set_unwind_handler"   ,SBC_SET_UNWIND_HANDLER);
  shput(asmsbc,"remove_unwind_handler",SBC_REMOVE_UNWIND_HANDLER);
  shput(asmsbc,"bs"       ,SBC_BS);
  shput(asmsbc,"t"        ,SBC_T);
  shput(asmsbc,"src"      ,SBC_SRC);
  shput(asmsbc,"deps"     ,SBC_DEPS);
  shput(asmsbc,"export"   ,SBC_EXPORT);
  shput(asmsbc,"incl"     ,SBC_INCL);
  hmdefault(sbcasm,0);

  for (int i = 0; i < shlen(asmsbc); i++) {
    //if (hmget(sbcasm, asmsbc[i].value)) continue;
    hmput(sbcasm,asmsbc[i].value, asmsbc[i].key);
  }
}



sif_t *sif_parse(char *file, int file_size) {
  int i, j;
  char *q;
  char *p = file;
  char *e = file + file_size;
  char *l;
  char **ls = 0;

  while (p < e) {
    l = 0;
    while (p < e && *p != '\n') {
      arrput(l,*p);
      p++;
    }
    p++;
    arrput(l,0);
    arrput(ls, l);
  }

  instr_t *instrs = 0;
  labels_t label2ins = 0;
  sh_new_arena(label2ins); //store key copies

  char **labels = 0;
  for (i = 0; i < arrlen(ls); i++) {
    l = ls[i];
    p = l;
    while (*p == ' ') p++;
    if (*p == ';') while (*p) p++;
    if (!*p) continue; // empty line
    char **as = 0; //arguments
    while (*p) {
      while (*p == ' ') p++;
      char *a = 0;
      for (; *p && *p != ' '; p++) {
        if (*p == ':' && !as) break;
        arrput(a, *p);
      }
      arrput(a, 0);
      int is_label = 0;
      if (*p == ':' && !as) {
        arrput(labels,a);
        p++;
        continue;
      }
      if (!strcmp(a,"P")) {
        arrfree(a);
        aputs(a, "L[0]");
        arrput(a, 0);
      } else if (!strcmp(a,"E")) {
        arrfree(a);
        aputs(a, "L[1]");
        arrput(a, 0);
      }
      arrput(as, a);
    }
    if (!as) continue;

    instr_t ins;
    ins.args = as;
    ins.labels = labels;
    ins.row = i;
    ins.opcode = shget(asmsbc,as[0]);
    for (j = 0; j < arrlen(labels); j++) {
      shput(label2ins,labels[j],arrlen(instrs));
    }
    labels = 0;
    arrput(instrs, ins);
  }

  sif_t *sif = malloc(sizeof(sif_t));
  sif->instrs = instrs;
  sif->labels = label2ins;

  for (i = 0; i < arrlen(ls); i++) arrfree(ls[i]);
  arrfree(ls);

  return sif;
}
