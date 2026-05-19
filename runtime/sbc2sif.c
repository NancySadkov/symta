//#define SBC_REMAP
#include "sif.h"
#include "ng.h"



void sbc_dasm_sub(dasm_t *dasm) {
  int opcode;
  uint8_t *pin = dasm->pin;
  sbc_t *sbc = dasm->sbc;
  rlabels_t o2l = dasm->o2l;
  int iofs = pin-sbc->code;
  if (dasm->arglist_i < dasm->arglist_nargs) {
    if (dasm->arglist_bits == 8) {
      opcode = SBC_STARG8;
    } else {
      opcode = SBC_STARG;
    }
  } else {
    opcode = RD8;
  }
  char **as = 0;
  char *head = hmget(sbcasm,opcode);
  if (head) head = afmt(0,"%s", head);
  arrput(as,head);
  switch(opcode) {
  case SBC_NOP: {break;}
  case SBC_SUBR: {
    int lib_id = RD16; //should be 0
    arrput(as, afmt(0,"%d", RD16));
    break;}
  case SBC_LEAVE0:
    as[0] = afmt(0,"%s", hmget(sbcasm,SBC_LEAVE));
    arrput(as, afmt(0,"0"));
    break;
  case SBC_LEAVE:
    arrput(as, afmt(0,"L[%d]",RD16));
    break;
  case SBC_JMP: {
    uint32_t ofs = RD24;
    arrput(as, afmt(0,"%s",REGOFS("l",ofs)));
    break;}
  case SBC_JMP16: {
    as[0] = afmt(0,"jmp");
    uint32_t ofs = pin-sbc->code;
    ofs += (int16_t)RD16;
    arrput(as, afmt(0,"%s",REGOFS("l",ofs)));
    break;}
  case SBC_B: {
    uint32_t cnd = RD16;
    uint32_t ofs = RD24;
    arrput(as, afmt(0,"L[%d]",cnd));
    arrput(as, afmt(0,"%s",REGOFS("l",ofs)));
    break;}
  case SBC_B8: {
    as[0] = afmt(0,"%s", hmget(sbcasm,SBC_B));
    uint32_t cnd = RD8;
    uint32_t ofs = pin-sbc->code;
    ofs += (int16_t)RD16;
    arrput(as, afmt(0,"L[%d]",cnd));
    arrput(as, afmt(0,"%s",REGOFS("l",ofs)));
    break;}
  case SBC_IFFXN: {
    uint32_t cnd = RD8;
    uint32_t ofs = pin-sbc->code;
    ofs += (int16_t)RD16;
    arrput(as, afmt(0,"L[%d]",cnd));
    arrput(as, afmt(0,"%s",REGOFS("l",ofs)));
    break;}
  case SBC_CALL: {
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;}
  case SBC_CALLIR: {
    as[0] = afmt(0,"call");
    arrput(as, "dummy");
    arrput(as, afmt(0,"L[%d]",RD16));
    break;}
  case SBC_CALLT: {
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;}
  case SBC_CALLTIR: {
    as[0] = afmt(0,"%s", hmget(sbcasm,SBC_CALLT));
    arrput(as, "dummy");
    arrput(as, afmt(0,"L[%d]",RD24));
    break;}
  case SBC_MCALL: {
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"mt[%d]",RD16));
    pin += 2; /* RT-7: 2-byte mcache id */
    break;}
  case SBC_MCALLIR: {
    as[0] = afmt(0,"mcall");
    arrput(as, afmt(0,"dummy"));
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"mt[%d]",RD16));
    pin += 2; /* RT-7: 2-byte mcache id */
    break;}
  case SBC_MCALL8: {
    as[0] = afmt(0,"mcall");
    arrput(as, afmt(0,"L[%d]",RD8));
    arrput(as, afmt(0,"L[%d]",RD8));
    arrput(as, afmt(0,"mt[%d]",RD8));
    pin += 2; /* RT-7: 2-byte mcache id */
    break;}
  case SBC_CLOSURE: {
    uint32_t dst = RD16;
    uint32_t idx = RD16;
    uint32_t size = RD8;
    arrput(as, afmt(0,"L[%d]",dst));
    uint32_t ofs = ((uint32_t*)sbc->fntbl)[idx];
    arrput(as, afmt(0,"%s",REGOFS("f",ofs)));
    arrput(as, afmt(0,"%d",size));
    break;}
  case SBC_OBJECT: {
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"ty[%d]",RD16));
    arrput(as, afmt(0,"%d",RD16));
    break;}
  case SBC_CNAS: {
    arrput(as, afmt(0,"%d",RD16));
    break;}
  case SBC_ARGLIST: {
    dasm->arglist_bits = 16;
    dasm->arglist_i = 0;
    dasm->arglist_nargs = RD16;
    arrput(as, afmt(0,"%d",dasm->arglist_nargs));
    break;}
  case SBC_ARGLIST8: {
    as[0] = afmt(0,"arglist");
    dasm->arglist_bits = 8;
    dasm->arglist_i = 0;
    dasm->arglist_nargs = RD8;
    arrput(as, afmt(0,"%d",dasm->arglist_nargs));
    break;}
  case SBC_ARGLIST0:
  case SBC_ARGLIST1:
  case SBC_ARGLIST2:
  case SBC_ARGLIST3:
  case SBC_ARGLIST4:
  case SBC_ARGLIST5: {
    as[0] = afmt(0,"arglist");
    dasm->arglist_bits = 8;
    dasm->arglist_i = 0;
    dasm->arglist_nargs = pin[-1]-SBC_ARGLIST0;
    arrput(as, afmt(0,"%d",dasm->arglist_nargs));
    break;}
  case SBC_STARG: {
    arrput(as, afmt(0,"%d",dasm->arglist_i++));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;}
  case SBC_STARG8: {
    as[0] = afmt(0,"starg");
    arrput(as, afmt(0,"%d",dasm->arglist_i++));
    arrput(as, afmt(0,"L[%d]",RD8));
    break;}
  case SBC_LIST: {
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"%d",RD16));
    break;}
  case SBC_LIST2: {
    /* RT-9: fused size-2 list literal */
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;}
  case SBC_LIST1: {
    /* RT-9: fused size-1 list literal */
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;}
  case SBC_MOVEEMT: {
    as[0] = afmt(0,"mv");
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"Empty"));
    break;}
  case SBC_MOVENO: {
    as[0] = afmt(0,"mv");
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"No"));
    break;}
  case SBC_MOVETX: {
    as[0] = afmt(0,"mv");
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"tx[%d]",RD24));
    break;}
  case SBC_MOVETX8: {
    as[0] = afmt(0,"mv");
    arrput(as, afmt(0,"L[%d]",RD8));
    arrput(as, afmt(0,"tx[%d]",RD8));
    break;}
  case SBC_MOVEMT: { //move method
    as[0] = afmt(0,"mv");
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"mt[%d]",RD24));
    break;}
  case SBC_MOVEMT8: { //move method
    as[0] = afmt(0,"mv");
    arrput(as, afmt(0,"L[%d]",RD8));
    arrput(as, afmt(0,"mt[%d]",RD8));
    break;}
  case SBC_MOVEIM: { //move imported
    as[0] = afmt(0,"mv");
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"im[%d]",RD24));
    break;}
  case SBC_MOVE: { //move local to local
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;}
  case SBC_MOVE8: { //move local to local
    as[0] = afmt(0,"mv");
    arrput(as, afmt(0,"L[%d]",RD8));
    arrput(as, afmt(0,"L[%d]",RD8));
    break;}
  case SBC_STOR: {
    int dst = RD16;
    int src = RD16;
    int index = RD16;
    arrput(as, afmt(0,"L[%d]",dst));
    arrput(as, afmt(0,"%d",index));
    arrput(as, afmt(0,"L[%d]",src));
    break;}
  case SBC_STOR8: {
    as[0] = afmt(0,"st");
    int dst = RD8;
    int src = RD8;
    int index = RD8;
    arrput(as, afmt(0,"L[%d]",dst));
    arrput(as, afmt(0,"%d",index));
    arrput(as, afmt(0,"L[%d]",src));
    break;}
  case SBC_LOAD: {
    int dst = RD16;
    int src = RD16;
    int index = RD16;
    arrput(as, afmt(0,"L[%d]",dst));
    arrput(as, afmt(0,"L[%d]",src));
    arrput(as, afmt(0,"%d",index));
    break;}
  case SBC_LOAD8: {
    as[0] = afmt(0,"ld");
    int dst = RD8;
    int src = RD8;
    int index = RD8;
    arrput(as, afmt(0,"L[%d]",dst));
    arrput(as, afmt(0,"L[%d]",src));
    arrput(as, afmt(0,"%d",index));
    break;}
  case SBC_LD4_0:
  case SBC_LD4_1:
  case SBC_LD4_2:
  case SBC_LD4_3:
  case SBC_LD4_4:
  case SBC_LD4_5:
  case SBC_LD4_6:
  case SBC_LD4_7:
  case SBC_LD4_8:
  case SBC_LD4_9:
  case SBC_LD4_A:
  case SBC_LD4_B:
  case SBC_LD4_C:
  case SBC_LD4_D:
  case SBC_LD4_E:
  case SBC_LD4_F: {
    int index = pin[-1]-SBC_LD4_0;
    as[0] = afmt(0,"ld");
    int opr = RD8;
    int dst = opr&0xF;
    int src = opr>>4;
    arrput(as, afmt(0,"L[%d]",dst));
    arrput(as, afmt(0,"L[%d]",src));
    arrput(as, afmt(0,"%d",index));
    break;}
  case SBC_COPY: {
    int dst = RD16;
    int src = RD16;
    int dindex = RD16;
    int sindex = RD16;
    arrput(as, afmt(0,"L[%d]",dst));
    arrput(as, afmt(0,"%d",dindex));
    arrput(as, afmt(0,"L[%d]",src));
    arrput(as, afmt(0,"%d",sindex));
    break;}
  case SBC_FXNB0: {
    as[0] = afmt(0,"ldfxn");
    arrput(as, afmt(0,"L[%d]",RD8));
    arrput(as, afmt(0,"0"));
    break;}
  case SBC_FXNB8: {
    int dst = RD8;
    uint64_t imm = RD8;
    as[0] = afmt(0,"ldfxn");
    arrput(as, afmt(0,"L[%d]",dst));
    arrput(as, afmt(0,"%lld",(int64_t)(int8_t)(uint8_t)imm));
    break;}
  case SBC_FXNB16: {
    int dst = RD8;
    uint64_t imm = RD16;
    as[0] = afmt(0,"ldfxn");
    arrput(as, afmt(0,"L[%d]",dst));
    arrput(as, afmt(0,"%lld",(int64_t)(int16_t)(uint16_t)imm));
    break;}
  case SBC_FXNB32: {
    int dst = RD8;
    uint64_t imm = RD32;
    as[0] = afmt(0,"ldfxn");
    arrput(as, afmt(0,"L[%d]",dst));
    arrput(as, afmt(0,"%lld",(int64_t)(int32_t)(uint32_t)imm));
    break;}
  case SBC_FXN0: {
    as[0] = afmt(0,"ldfxn");
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"0"));
    break;}
  case SBC_FXN8: {
    int dst = RD16;
    uint64_t imm = RD8;
    as[0] = afmt(0,"ldfxn");
    arrput(as, afmt(0,"L[%d]",dst));
    arrput(as, afmt(0,"%lld",(int64_t)(int8_t)(uint8_t)imm));
    break;}
  case SBC_FXN16: {
    int dst = RD16;
    uint64_t imm = RD16;
    as[0] = afmt(0,"ldfxn");
    arrput(as, afmt(0,"L[%d]",dst));
    arrput(as, afmt(0,"%lld",(int64_t)(int16_t)(uint16_t)imm));
    break;}
  case SBC_FXN32: {
    int dst = RD16;
    uint64_t imm = RD32;
    as[0] = afmt(0,"ldfxn");
    arrput(as, afmt(0,"L[%d]",dst));
    arrput(as, afmt(0,"%lld",(int64_t)(int32_t)(uint32_t)imm));
    break;}
  case SBC_IMMB64:
  case SBC_IMM64: {
    int dst;
    if (opcode == SBC_IMMB64) dst = RD8;
    else dst = RD16;
    uint64_t imm = RD64;
    if (O_TAG(imm) == T_INT) {
      as[0] = afmt(0,"ldfxn");
      int64_t val = UNFXN(imm);
      arrput(as, afmt(0,"L[%d]",dst));
      if (val > 0x7FFFFFFF || val < -0x7FFFFFFF) {
        arrput(as, afmt(0,"%lldLL",val));
      } else {
        arrput(as, afmt(0,"%lld",val));
      }
    } else if (O_TAG(imm) == T_FLOAT) {
      as[0] = afmt(0,"ldflt");
      float fval;
      STFLT(fval,imm);
      arrput(as, afmt(0,"L[%d]",dst));
      arrput(as, afmt(0,"%f",fval));
    } else if (O_TAG(imm) == T_FIXTEXT) {
      as[0] = afmt(0,"mv");
      arrput(as, afmt(0,"L[%d]",dst));
      arrput(as, afmt(0,"FIXTEXT(%lldULL)",O_GID(imm)));
    }
    break;}
  case SBC_FXNTAG:
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;
  case SBC_FXNLISTN:
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;
  case SBC_FXNCANGET:
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;
  case SBC_FXNLGET:
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    pin += 2; /* RT-7: 2-byte mcache id */
    break;
  case SBC_FXNLSET:
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    pin += 2; /* RT-7: 2-byte mcache id */
    break;
  case SBC_FXNLSETIR:
    as[0] = afmt(0,"fxnlset");
    arrput(as, afmt(0,"dummy"));
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    pin += 2; /* RT-7: 2-byte mcache id */
    break;
  case SBC_FXNSIZE:
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;
  case SBC_NEG:
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;
  case SBC_IMMEQ:
  case SBC_IMMNE:
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    pin += 2; /* RT-7: 2-byte mcache id */
    break;
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
  case SBC_IADD:   /* TS-4.1 */
  case SBC_ISUB:
  case SBC_IMUL:
  case SBC_IDIV:
  case SBC_IREM:
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;
  case SBC_TINIT:
    arrput(as, afmt(0,"ty[%d]",RD16));
    arrput(as, afmt(0,"%d",RD16));
    arrput(as, afmt(0,"tx[%d]",RD24));
    break;
  case SBC_TINITI:
    as[0] = afmt(0,"%s", hmget(sbcasm,SBC_TINIT));
    arrput(as, afmt(0,"ty[%d]",RD16));
    arrput(as, afmt(0,"%d",RD16));
    arrput(as, afmt(0,"FIXTEXT(%lldULL)",RD64));
    break;
  case SBC_SUBTYPE:
    arrput(as, afmt(0,"ty[%d]",RD16));
    arrput(as, afmt(0,"ty[%d]",RD16));
    break;
  case SBC_DMET: {
    int tyidx = RD16;
    int mtidx = RD24;
    int handler = RD16;
    arrput(as, afmt(0,"mt[%d]",mtidx));
    arrput(as, afmt(0,"ty[%d]",tyidx));
    arrput(as, afmt(0,"L[%d]",handler));
    break;}
  case SBC_GID:
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;
  case SBC_CURMET:
    arrput(as, afmt(0,"L[%d]",RD16));
    break;
  case SBC_MNAME:
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;
  case SBC_FATAL:
    arrput(as, afmt(0,"L[%d]",RD16));
    break;
  case SBC_GC0:
  case SBC_GC1:
    break;
  case SBC_NLDU1:
  case SBC_NLDU2:
  case SBC_NLDU4:
  case SBC_NLDS1:
  case SBC_NLDS2:
  case SBC_NLDS4:
  {
    char *t;
    switch (opcode) {
    case SBC_NLDU1: t = "uint8_t"; break;
    case SBC_NLDU2: t = "uint16_t"; break;
    case SBC_NLDU4: t = "uint32_t"; break;
    case SBC_NLDS1: t = "int8_t"; break;
    case SBC_NLDS2: t = "int16_t"; break;
    case SBC_NLDS4: t = "int32_t"; break;
    }
    as[0] = afmt(0,"%s", hmget(sbcasm,SBC_NLD));
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"%s", t));
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;}
  case SBC_NSTU1:
  case SBC_NSTU2:
  case SBC_NSTU4:
  case SBC_NSTS1:
  case SBC_NSTS2:
  case SBC_NSTS4:
  {
    char *t;
    switch (opcode) {
    case SBC_NSTU1: t = "uint8_t"; break;
    case SBC_NSTU2: t = "uint16_t"; break;
    case SBC_NSTU4: t = "uint32_t"; break;
    case SBC_NSTS1: t = "int8_t"; break;
    case SBC_NSTS2: t = "int16_t"; break;
    case SBC_NSTS4: t = "int32_t"; break;
    }
    as[0] = afmt(0,"%s", hmget(sbcasm,SBC_NST));
    arrput(as, afmt(0,"%s", t));
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    arrput(as, afmt(0,"L[%d]",RD16));
    break;}
  default: {
    char *name = head ? head : "unk";
    fprintf(stderr, "sbc2sif: bad opcode=%02x (%s)\n", opcode, name);
    exit(-1);
    break;
    }
  }

  if (!as[0]) {
    fprintf(stderr, "sbc2sif: unknown opcode=%02x\n", opcode);
    exit(-1);
  }

  instr_t *ins = &dasm->ins;
  ins->args = as;
  ins->labels = 0;
  ins->row = dasm->row;
  ins->ofs = iofs;
  ins->opcode = shget(asmsbc,as[0]);
  dasm->o2l = o2l;
  dasm->row++;
  dasm->pin = pin;
  //sif_print(&ins);
}



sif_t *sbc2sif(char *filename) {
  int i,j,k;
  
  sbc_t *sbc = sbc_load(filename);
  if (!sbc) return 0;

  
  instr_t *instrs = 0;
  labels_t label2ins = 0;
  sh_new_arena(label2ins); //store key copies

  //offset to label map
  struct { int key; char *value; } *o2l = 0;

#define REGOFS(s,o) \
  (hmget(o2l,o) ?  hmget(o2l,o) : \
  (hmput(o2l,o,afmt(0,"%s%x",s,o)), hmget(o2l,o)))

  hmput(o2l,0,afmt(0,"entry"));

  int arglist_bits = 0;
  int arglist_i = 0;
  int arglist_nargs = 0;

  uint8_t *pin = sbc->code;
  uint8_t *ein = pin+sbc->code_sz;
  for (i = 0; pin < ein; i++) {
    int iofs = pin-sbc->code;
    int opcode;
    if (arglist_i < arglist_nargs) {
      if (arglist_bits == 8) {
        opcode = SBC_STARG8;
      } else {
        opcode = SBC_STARG;
      }
    } else {
      opcode = RD8;
    }
    char **as = 0;
    char *head = hmget(sbcasm,opcode);
    if (head) head = afmt(0,"%s", head);
    arrput(as,head);
    switch(opcode) {
    case SBC_NOP: {break;}
    case SBC_SUBR: {
      int lib_id = RD16; //should be 0
      arrput(as, afmt(0,"%d", RD16));
      break;}
    case SBC_LEAVE0:
      as[0] = afmt(0,"%s", hmget(sbcasm,SBC_LEAVE));
      arrput(as, afmt(0,"0"));
      break;
    case SBC_LEAVE:
      arrput(as, afmt(0,"L[%d]",RD16));
      break;
    case SBC_JMP: {
      uint32_t ofs = RD24;
      arrput(as, afmt(0,"%s",REGOFS("l",ofs)));
      break;}
    case SBC_JMP16: {
      as[0] = afmt(0,"jmp");
      uint32_t ofs = pin-sbc->code;
      ofs += (int16_t)RD16;
      arrput(as, afmt(0,"%s",REGOFS("l",ofs)));
      break;}
    case SBC_B: {
      uint32_t cnd = RD16;
      uint32_t ofs = RD24;
      arrput(as, afmt(0,"L[%d]",cnd));
      arrput(as, afmt(0,"%s",REGOFS("l",ofs)));
      break;}
    case SBC_B8: {
      as[0] = afmt(0,"%s", hmget(sbcasm,SBC_B));
      uint32_t cnd = RD8;
      uint32_t ofs = pin-sbc->code;
      ofs += (int16_t)RD16;
      arrput(as, afmt(0,"L[%d]",cnd));
      arrput(as, afmt(0,"%s",REGOFS("l",ofs)));
      break;}
    case SBC_IFFXN: {
      uint32_t cnd = RD8;
      uint32_t ofs = pin-sbc->code;
      ofs += (int16_t)RD16;
      arrput(as, afmt(0,"L[%d]",cnd));
      arrput(as, afmt(0,"%s",REGOFS("l",ofs)));
      break;}
    case SBC_CALL: {
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;}
    case SBC_CALLIR: {
      as[0] = afmt(0,"call");
      arrput(as, "dummy");
      arrput(as, afmt(0,"L[%d]",RD16));
      break;}
    case SBC_CALLT: {
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;}
    case SBC_CALLTIR: {
      as[0] = afmt(0,"%s", hmget(sbcasm,SBC_CALLT));
      arrput(as, "dummy");
      arrput(as, afmt(0,"L[%d]",RD24));
      break;}
    case SBC_MCALL: {
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"mt[%d]",RD16));
      pin += 2; /* RT-7: 2-byte mcache id */
      break;}
    case SBC_MCALLIR: {
      as[0] = afmt(0,"mcall");
      arrput(as, afmt(0,"dummy"));
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"mt[%d]",RD16));
      pin += 2; /* RT-7: 2-byte mcache id */
      break;}
    case SBC_MCALL8: {
      as[0] = afmt(0,"mcall");
      arrput(as, afmt(0,"L[%d]",RD8));
      arrput(as, afmt(0,"L[%d]",RD8));
      arrput(as, afmt(0,"mt[%d]",RD8));
      pin += 2; /* RT-7: 2-byte mcache id */
      break;}
    case SBC_CLOSURE: {
      uint32_t dst = RD16;
      uint32_t idx = RD16;
      uint32_t size = RD8;
      arrput(as, afmt(0,"L[%d]",dst));
      uint32_t ofs = ((uint32_t*)sbc->fntbl)[idx];
      arrput(as, afmt(0,"%s",REGOFS("f",ofs)));
      arrput(as, afmt(0,"%d",size));
      break;}
    case SBC_OBJECT: {
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"ty[%d]",RD16));
      arrput(as, afmt(0,"%d",RD16));
      break;}
    case SBC_CNAS: {
      arrput(as, afmt(0,"%d",RD16));
      break;}
    case SBC_ARGLIST: {
      arglist_bits = 16;
      arglist_i = 0;
      arglist_nargs = RD16;
      arrput(as, afmt(0,"%d",arglist_nargs));
      break;}
    case SBC_ARGLIST8: {
      as[0] = afmt(0,"arglist");
      arglist_bits = 8;
      arglist_i = 0;
      arglist_nargs = RD8;
      arrput(as, afmt(0,"%d",arglist_nargs));
      break;}
    case SBC_STARG: {
      arrput(as, afmt(0,"%d",arglist_i++));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;}
    case SBC_STARG8: {
      as[0] = afmt(0,"starg");
      arrput(as, afmt(0,"%d",arglist_i++));
      arrput(as, afmt(0,"L[%d]",RD8));
      break;}
    case SBC_LIST: {
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"%d",RD16));
      break;}
    case SBC_LIST2: {
      /* RT-9 */
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;}
    case SBC_LIST1: {
      /* RT-9 */
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;}
    case SBC_MOVEEMT: {
      as[0] = afmt(0,"mv");
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"Empty"));
      break;}
    case SBC_MOVENO: {
      as[0] = afmt(0,"mv");
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"No"));
      break;}
    case SBC_MOVETX: {
      as[0] = afmt(0,"mv");
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"tx[%d]",RD24));
      break;}
    case SBC_MOVETX8: {
      as[0] = afmt(0,"mv");
      arrput(as, afmt(0,"L[%d]",RD8));
      arrput(as, afmt(0,"tx[%d]",RD8));
      break;}
    case SBC_MOVEMT: { //move method
      as[0] = afmt(0,"mv");
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"mt[%d]",RD24));
      break;}
    case SBC_MOVEMT8: { //move method
      as[0] = afmt(0,"mv");
      arrput(as, afmt(0,"L[%d]",RD8));
      arrput(as, afmt(0,"mt[%d]",RD8));
      break;}
    case SBC_MOVEIM: { //move imported
      as[0] = afmt(0,"mv");
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"im[%d]",RD24));
      break;}
    case SBC_MOVE: { //move local to local
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;}
    case SBC_MOVE8: { //move local to local
      as[0] = afmt(0,"mv");
      arrput(as, afmt(0,"L[%d]",RD8));
      arrput(as, afmt(0,"L[%d]",RD8));
      break;}
    case SBC_STOR: {
      int dst = RD16;
      int src = RD16;
      int index = RD16;
      arrput(as, afmt(0,"L[%d]",dst));
      arrput(as, afmt(0,"%d",index));
      arrput(as, afmt(0,"L[%d]",src));
      break;}
    case SBC_STOR8: {
      as[0] = afmt(0,"st");
      int dst = RD8;
      int src = RD8;
      int index = RD8;
      arrput(as, afmt(0,"L[%d]",dst));
      arrput(as, afmt(0,"%d",index));
      arrput(as, afmt(0,"L[%d]",src));
      break;}
    case SBC_LOAD: {
      int dst = RD16;
      int src = RD16;
      int index = RD16;
      arrput(as, afmt(0,"L[%d]",dst));
      arrput(as, afmt(0,"L[%d]",src));
      arrput(as, afmt(0,"%d",index));
      break;}
    case SBC_LOAD8: {
      as[0] = afmt(0,"ld");
      int dst = RD8;
      int src = RD8;
      int index = RD8;
      arrput(as, afmt(0,"L[%d]",dst));
      arrput(as, afmt(0,"L[%d]",src));
      arrput(as, afmt(0,"%d",index));
      break;}
    case SBC_LD4_0:
    case SBC_LD4_1:
    case SBC_LD4_2:
    case SBC_LD4_3:
    case SBC_LD4_4:
    case SBC_LD4_5:
    case SBC_LD4_6:
    case SBC_LD4_7:
    case SBC_LD4_8:
    case SBC_LD4_9:
    case SBC_LD4_A:
    case SBC_LD4_B:
    case SBC_LD4_C:
    case SBC_LD4_D:
    case SBC_LD4_E:
    case SBC_LD4_F: {
      int index = pin[-1]-SBC_LD4_0;
      as[0] = afmt(0,"ld");
      int opr = RD8;
      int dst = opr&0xF;
      int src = opr>>4;
      arrput(as, afmt(0,"L[%d]",dst));
      arrput(as, afmt(0,"L[%d]",src));
      arrput(as, afmt(0,"%d",index));
      break;}
    case SBC_COPY: {
      int dst = RD16;
      int src = RD16;
      int dindex = RD16;
      int sindex = RD16;
      arrput(as, afmt(0,"L[%d]",dst));
      arrput(as, afmt(0,"%d",dindex));
      arrput(as, afmt(0,"L[%d]",src));
      arrput(as, afmt(0,"%d",sindex));
      break;}
    case SBC_FXNB0: {
      as[0] = afmt(0,"ldfxn");
      arrput(as, afmt(0,"L[%d]",RD8));
      arrput(as, afmt(0,"0"));
      break;}
    case SBC_FXNB8: {
      int dst = RD8;
      uint64_t imm = RD8;
      as[0] = afmt(0,"ldfxn");
      arrput(as, afmt(0,"L[%d]",dst));
      arrput(as, afmt(0,"%lld",(int64_t)(int8_t)(uint8_t)imm));
      break;}
    case SBC_FXNB16: {
      int dst = RD8;
      uint64_t imm = RD16;
      as[0] = afmt(0,"ldfxn");
      arrput(as, afmt(0,"L[%d]",dst));
      arrput(as, afmt(0,"%lld",(int64_t)(int16_t)(uint16_t)imm));
      break;}
    case SBC_FXNB32: {
      int dst = RD8;
      uint64_t imm = RD32;
      as[0] = afmt(0,"ldfxn");
      arrput(as, afmt(0,"L[%d]",dst));
      arrput(as, afmt(0,"%lld",(int64_t)(int32_t)(uint32_t)imm));
      break;}
    case SBC_FXN0: {
      as[0] = afmt(0,"ldfxn");
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"0"));
      break;}
    case SBC_FXN8: {
      int dst = RD16;
      uint64_t imm = RD8;
      as[0] = afmt(0,"ldfxn");
      arrput(as, afmt(0,"L[%d]",dst));
      arrput(as, afmt(0,"%lld",(int64_t)(int8_t)(uint8_t)imm));
      break;}
    case SBC_FXN16: {
      int dst = RD16;
      uint64_t imm = RD16;
      as[0] = afmt(0,"ldfxn");
      arrput(as, afmt(0,"L[%d]",dst));
      arrput(as, afmt(0,"%lld",(int64_t)(int16_t)(uint16_t)imm));
      break;}
    case SBC_FXN32: {
      int dst = RD16;
      uint64_t imm = RD32;
      as[0] = afmt(0,"ldfxn");
      arrput(as, afmt(0,"L[%d]",dst));
      arrput(as, afmt(0,"%lld",(int64_t)(int32_t)(uint32_t)imm));
      break;}
    case SBC_IMMB64:
    case SBC_IMM64: {
      int dst;
      if (opcode == SBC_IMMB64) dst = RD8;
      else dst = RD16;
      uint64_t imm = RD64;
      if (O_TAG(imm) == T_INT) {
        as[0] = afmt(0,"ldfxn");
        int64_t val = UNFXN(imm);
        arrput(as, afmt(0,"L[%d]",dst));
        if (val > 0x7FFFFFFF || val < -0x7FFFFFFF) {
          arrput(as, afmt(0,"%lldLL",val));
        } else {
          arrput(as, afmt(0,"%lld",val));
        }
      } else if (O_TAG(imm) == T_FLOAT) {
        as[0] = afmt(0,"ldflt");
        double fval;
        STFLT(fval,imm);
        arrput(as, afmt(0,"L[%d]",dst));
        arrput(as, afmt(0,"%f",fval));
      } else if (O_TAG(imm) == T_FIXTEXT) {
        as[0] = afmt(0,"mv");
        arrput(as, afmt(0,"L[%d]",dst));
        arrput(as, afmt(0,"FIXTEXT(%lldULL)",O_GID(imm)));
      }
      break;}
    case SBC_FXNTAG:
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;
    case SBC_FXNLISTN:
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;
    case SBC_FXNCANGET:
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;
    case SBC_FXNLGET:
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      pin += 2; /* RT-7: 2-byte mcache id */
      break;
    case SBC_FXNLSET:
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      pin += 2; /* RT-7: 2-byte mcache id */
      break;
    case SBC_FXNLSETIR:
      as[0] = afmt(0,"fxnlset");
      arrput(as, afmt(0,"dummy"));
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      pin += 2; /* RT-7: 2-byte mcache id */
      break;
    case SBC_FXNSIZE:
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;
    case SBC_NEG:
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;
    case SBC_IMMEQ:
    case SBC_IMMNE:
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      pin += 2; /* RT-7: 2-byte mcache id */
      break;
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
    case SBC_IADD:   /* TS-4.1 */
    case SBC_ISUB:
    case SBC_IMUL:
    case SBC_IDIV:
    case SBC_IREM:
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;
    case SBC_TINIT:
      arrput(as, afmt(0,"ty[%d]",RD16));
      arrput(as, afmt(0,"%d",RD16));
      arrput(as, afmt(0,"tx[%d]",RD24));
      break;
    case SBC_TINITI:
      as[0] = afmt(0,"%s", hmget(sbcasm,SBC_TINIT));
      arrput(as, afmt(0,"ty[%d]",RD16));
      arrput(as, afmt(0,"%d",RD16));
      arrput(as, afmt(0,"FIXTEXT(%lldULL)",RD64));
      break;
    case SBC_SUBTYPE:
      arrput(as, afmt(0,"ty[%d]",RD16));
      arrput(as, afmt(0,"ty[%d]",RD16));
      break;
    case SBC_DMET: {
      int tyidx = RD16;
      int mtidx = RD24;
      int handler = RD16;
      arrput(as, afmt(0,"mt[%d]",mtidx));
      arrput(as, afmt(0,"ty[%d]",tyidx));
      arrput(as, afmt(0,"L[%d]",handler));
      break;}
    case SBC_GID:
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;
    case SBC_CURMET:
      arrput(as, afmt(0,"L[%d]",RD16));
      break;
    case SBC_MNAME:
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;
    case SBC_FATAL:
      arrput(as, afmt(0,"L[%d]",RD16));
      break;
    case SBC_GC0:
    case SBC_GC1:
      break;
    case SBC_NLDU1:
    case SBC_NLDU2:
    case SBC_NLDU4:
    case SBC_NLDS1:
    case SBC_NLDS2:
    case SBC_NLDS4:
    {
      char *t;
      switch (opcode) {
      case SBC_NLDU1: t = "uint8_t"; break;
      case SBC_NLDU2: t = "uint16_t"; break;
      case SBC_NLDU4: t = "uint32_t"; break;
      case SBC_NLDS1: t = "int8_t"; break;
      case SBC_NLDS2: t = "int16_t"; break;
      case SBC_NLDS4: t = "int32_t"; break;
      }
      as[0] = afmt(0,"%s", hmget(sbcasm,SBC_NLD));
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"%s", t));
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;}
    case SBC_NSTU1:
    case SBC_NSTU2:
    case SBC_NSTU4:
    case SBC_NSTS1:
    case SBC_NSTS2:
    case SBC_NSTS4:
    {
      char *t;
      switch (opcode) {
      case SBC_NSTU1: t = "uint8_t"; break;
      case SBC_NSTU2: t = "uint16_t"; break;
      case SBC_NSTU4: t = "uint32_t"; break;
      case SBC_NSTS1: t = "int8_t"; break;
      case SBC_NSTS2: t = "int16_t"; break;
      case SBC_NSTS4: t = "int32_t"; break;
      }
      as[0] = afmt(0,"%s", hmget(sbcasm,SBC_NST));
      arrput(as, afmt(0,"%s", t));
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      arrput(as, afmt(0,"L[%d]",RD16));
      break;}
    default: {
      char *name = head ? head : "unk";
      fprintf(stderr, "sbc2sif: bad opcode=%02x (%s)\n", opcode, name);
      exit(-1);
      break;
      }
    }

    if (!as[0]) {
      fprintf(stderr, "sbc2sif: unknown opcode=%02x\n", opcode);
      exit(-1);
    }

    instr_t ins;
    ins.args = as;
    ins.labels = 0;
    ins.row = i;
    ins.ofs = iofs;
    ins.opcode = shget(asmsbc,as[0]);
    arrput(instrs, ins);
    //sif_print(&ins);
  }

  uint8_t *totpin = sbc->tot;
  char *tns[7] = {"fm","tx","ty","mt","libs","imlib","im"};

  int row = 0;
  for (i = 0; i < 7; i++) {
    pin = totpin;
    int nentries = RD24;
    int tofs = RD24;
    totpin = pin;
    pin = sbc->tbls+tofs;
    int nargs = 1;
    if (i == 0) nargs = 7;
    for (j = 0; j < nentries; j++) {
      char **as = 0;
      arrput(as, afmt(0, "t"));
      arrput(as, afmt(0, "%s", tns[i]));
      if (i == 0) {
        for (k = 0; k < nargs; k++) {
          if (k==0) {//closure size
            arrput(as, afmt(0, "%d", (int32_t)RD16));
          } else if (k==1) { //nargs
            int nargs = RD16;
            if (nargs == 0xFFFF) nargs = -1;
            arrput(as, afmt(0, "%d", nargs));
          } else if (k==2) { //name
            uint32_t ofs = RD24;
            if (ofs) {
              arrput(as, afmt(0,"%s",REGOFS("b",ofs)));
            } else {
              arrput(as, afmt(0,"0"));
            }
          } else if (k==3) { //fn_meta_t.fn
            int idx = RD16;
            uint32_t ofs = ((uint32_t*)sbc->fntbl)[idx];
            arrput(as, afmt(0,"%s",REGOFS("f",ofs)));
          } else if (k==4) {//row
            arrput(as, afmt(0, "%d", (int32_t)RD16));
          } else if (k==5) {//col
            arrput(as, afmt(0, "%d", (int32_t)RD16));
          }
        }
      } else {
        if (i==5) {
          arrput(as, afmt(0, "%d", (int32_t)RD24));
        } else {
          uint32_t ofs = RD24;
          if (ofs) {
            arrput(as, afmt(0,"%s",REGOFS("b",ofs)));
          } else {
            arrput(as, afmt(0,"0"));
          }
        }
      }
      instr_t ins;
      ins.args = as;
      ins.labels = 0;
      ins.row = row++;
      ins.ofs = -1;
      ins.opcode = SBC_T;
      arrput(instrs, ins);
      //sif_print(&ins);
    }
  }

  //read arbitary byte sequences
  int code_sz = sbc->code_sz;
  pin = sbc->data;
  ein = pin+sbc->data_sz;
  char *clabel = 0;
  char **as = 0;
  arrput(as, afmt(0, "bs"));
  for ( ; ; i++) {
    if (!as) arrput(as, afmt(0, "bs"));
    int iofs = pin-sbc->data + code_sz;
    char *label = hmget(o2l,iofs);
    if (pin == ein || label) {
      if (!clabel) {
        clabel = label;
      } else {
        instr_t ins;
        ins.args = as;
        ins.labels = 0;
        ins.row = row++;
        ins.ofs = -1;
        ins.opcode = SBC_BS;
        arrput(ins.labels, afmt(0,"%s",clabel));
        arrput(instrs, ins);
        //sif_print(&ins);
        clabel = label;
        as = 0;
        arrput(as, afmt(0, "bs"));
      }
    }
    if (pin == ein) break;
    arrput(as,afmt(0,"%d",(int32_t)RD8));
  }

  for (i = 0; i < arrlen(instrs); i++) {
    instr_t *ins = &instrs[i];
    char *label = hmget(o2l,ins->ofs);
    if (!label) continue;
    arrput(ins->labels, afmt(0,"%s",label));
    shput(label2ins,label,i);
  }

  for (j = 0; j < hmlen(o2l); j++) {
    arrfree(o2l[j].value);
  }

  sif_t *sif = malloc(sizeof(sif_t));
  sif->instrs = instrs;
  sif->labels = label2ins;

#if 0
  for (i = 0; i < arrlen(instrs); i++) {
    instr_t *ins = &instrs[i];
    sif_print(ins);
  }
#endif
  
  sbc_free(sbc);

  return sif;
}



#if 0
void sbc_dasm_init(dasm_t *dasm, sbc_t *sbc, uint8_t *pin) {
  dasm->sbc = sbc;
  dasm->pin = pin;
  dasm->o2l = 0;
  dasm->row = 0;
  dasm->arglist_bits = 0;
  dasm->arglist_i = 0;
  dasm->arglist_nargs = 0;
}

void sbc_dasm(sbc_t *sbc, uint8_t *pin, int n) {
  dasm_t dasm;
  sbc_dasm_init(&dasm, sbc, pin);
  for (int i = 0; i < n; i++) {
    sbc_dasm_sub(&dasm);
    if (dasm.ins.opcode == SBC_SUBR) break;
    sif_print(&dasm.ins);
  }
}

void sbc_dasm_fn(uint8_t *pin, int n) {
  int sbc_id = RD16;
  sbc_t *sbc = sbcs[sbc_id];
  int nvars = RD16;
  dasm_t dasm;
  sbc_dasm_init(&dasm, sbc, pin);
  for (int i = 0; i < n; i++) {
    sbc_dasm_sub(&dasm);
    if (dasm.ins.opcode == SBC_SUBR) break;
    sif_print(&dasm.ins);
  }
}

void dasm_hook(hook_t *hook) {   //dasm_hook(&O_HOOK(o));
  if ((void*)hook->handler != (void*)sbc_exec_fn) return;
  printf("optimizing %p (%s)\n", hook->payload, hook->meta->name);
  sbc_dasm_fn(hook->payload, 100);
}
#endif

