GCDEF(gc_closure)
  int i, size;
  hg_t *hg;
  void *p, *q;
  void **pp, **oo;
  void *fixed_size, *dummy;
  size = O_SIZE(o);
  //fprintf(stderr, "beg closure: %d\n", size);
  CLOSURE(p, O_CODE(o), size);
  GC_REDIR(o,p);
  pp = (void**)&LGET(p,0);
  oo = (void**)&LGET(o,0);
  for (i = 0; i < size; i++) {
    GC_REC2(pp[i],oo[i]);
  }
  //fprintf(stderr, "end closure\n");
GCEND(p)

GCDEF(gc_list)
  uint32_t i, size;
  void *p;
  void **pp, **oo;
  size = LIST_SIZE(o);
  LIST(p, size);
  GC_REDIR(o,p);
  pp = &LGET(p,0);
  oo = &LGET(o,0);
  for (i = 0; i < size; i++) {
    GC_REC(pp[i], oo[i]);
  }
GCEND(p)

GCDEF(gc_view)
  uint32_t i;
  dyn p;
  dyn *pp, *oo;
  dyn base = VIEW_BASE(o);
  uint32_t start = VIEW_START(o);
  uint32_t size = VIEW_SIZE(o);

  //if base is older than the view, we can reuse it
  //FIXME: find the best age threshold for it
  //       theoretically it should depend on the HG size
  if (O_AGE(o) < 1 && VIEW_SHARED(base)) {
    VIEW(p, 0, start, size);
    GC_REDIR(o,p); //place redir befor GC'ing base, because loops
    GC_REC(base, base);
    VIEW_BASE(p) = base;
    goto end;
  }

  //otherwise view is old enough to become a list
  LIST(p, size);
  GC_REDIR(o,p);
  oo = &LGET(base, start);
  pp = &LGET(p,0);

  for (i = 0; i < size; i++) {
    GC_REC(pp[i], oo[i]);
  }
end:
GCEND(p)

#if 1
GCDEF(gc_cons)
again:;
  void *p, *r;
  CONS(p, 0, 0);
  r = p;
  dyn car = CAR(o);
  dyn cdr = CDR(o);
  GC_REDIR(o,p);
  GC_REC(CAR(p), car);
  o = cdr;
  //GC them in one loop, so stack space wont be an issue.
  while (!IMMEDIATE(o) && O_TAG(o) == T_CONS) {
    void *oo_ = (void*)(o);
    uint32_t hg_ = O_AGE(oo_);
    if (hg_ != GC_AGE) {
      if (hg_ == GC_MOVED) {
        o = O_RELOC(oo_); /*already moved*/
        break;
      } else {
        o = oo_; /*older gen*/
        break;
      }
    }

    void *prev = p;
    CONS(p, 0, 0);
    CDR(prev) = p;
    //fprintf(stderr, "%s\n", print_object(CAR(o)));
    dyn car = CAR(o);
    dyn cdr = CDR(o);
    GC_REDIR(o,p);
    GC_REC(CAR(p), car);
    o = cdr;
  }
  GC_REC(CDR(p), o);
GCEND(r)
#else
//this simpler code can overflow the stack!!!
GCDEF(gc_cons)
  void *p;
  CONS(p, 0, 0);
  dyn car = CAR(o);
  dyn cdr = CDR(o);
  GC_REDIR(o,p);
  GC_REC(CAR(p), car);
  GC_REC(CDR(p), cdr);
GCEND(p)
#endif

GCDEF(gc_text)
  void *p;
  p = alloc_bigtext(BIGTEXT_DATA(o), BIGTEXT_SIZE(o));
  GC_REDIR(o,p);
GCEND(p)

GCDEF(gc_bytes)
  void *p;
  int size = (int)BYTES_SIZE(o);
  p = alloc_bytes(size);
  memcpy(BYTES_DATA(p), BYTES_DATA(o), size);
  GC_REDIR(o,p);
GCEND(p)

//We don't really need to GC the maps whose values and keys are immediate
//should we track the values?
static void tbl_gc_internals(dyn o) {
  CASE(AM_TYPE(o))
  GOT(AM_EMPTY)
  GOT(AM_GENERIC)
    dh_t *hm = AM_BASE(o);
    NH_FOR(dh,i,hm) {
      GC_REC(*dhKey(hm,i),*dhKey(hm,i));
      GC_REC(*dhVal(hm,i),*dhVal(hm,i));
    }
  GOT(AM_TEXT)
    /* AM-6b: text keys are dyns stored directly in th_t. Mark
     * both keys and values; otherwise a BIGTEXT key whose only
     * reference is the slot would get reclaimed. */
    th_t *hm = AM_BASE(o);
    NH_FOR(th,i,hm) {
      GC_REC(*thKey(hm,i),*thKey(hm,i));
      GC_REC(*thVal(hm,i),*thVal(hm,i));
    }
  GOT(AM_INT)
    ih_t *hm = AM_BASE(o);
    NH_FOR(ih,i,hm) {
      dyn *t = ihVal(hm,i);
      GC_REC(*t,*t);
    }
  GOT(AM_BITMAP0)
  GOT(AM_BITMAP1)
  ESAC
}

GCDEF(gc_tbl)
  dyn r;
  AM_ALLOC(r);
  AM_BASE(r) = AM_BASE(o);
  AM_SET_TYPE(r,AM_TYPE(o));
  AM_SET_YOUNGEST(r,api.hgp->age);
  GC_REDIR(o,r);
  GC_REC(AM_VOID(r), AM_VOID(o));
  tbl_gc_internals(r);
GCEND(r)


GCDEF(gc_custom)
  int i, size;
  void *p;
  void **pp, **oo;
  uint64_t tag = O_TAG(o);
  size = types[tag].size;
  OBJECT(p, tag, size);
  O_CODE(p) = O_CODE(o);
  GC_REDIR(o,p);
  pp = (void**)&LGET(p,0);
  oo = (void**)&LGET(o,0);
  for (i = 0; i < size; i++) {
    GC_REC(pp[i], oo[i]);
  }
GCEND(p)
