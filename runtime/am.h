/*****************************

  Adaptive Map

  An adaptive hash table whose internal representation specialises
  to the observed key/value distribution at runtime. One table
  object (an O_TAG=T_TBL heap allocation, 2 cells: base + void)
  can be in any of six modes:

    AM_EMPTY    - no backing; everything misses
    AM_BITMAP0  - bitmap of present integer keys; values are
                  implicitly FXN(0). Set bit -> get returns 0.
    AM_BITMAP1  - bitmap; values implicitly FXN(1).
    AM_INT      - nh_t-derived ih_t hashmap, integer-keyed
                  (AM-6: replaced stb_ds; Robin Hood + 75% load).
    AM_TEXT     - nh_t-derived th_t hashmap, text-keyed
                  (AM-6b: replaced stb_ds; Robin Hood + 75% load
                  + AM-15 hash cache; FNV-1a for BIGTEXT,
                  Murmur3 finaliser for FIXTEXT).
    AM_GENERIC  - nh_t-derived dh_t hashmap, dyn-keyed
                  (any mix of types; Robin Hood + 75% load +
                  AM-15 hash cache).

  Iteration order is unspecified across all hash modes -- the
  table is an unordered hash map, not a sequence. User code
  that needs deterministic output sorts via T.l.s or T.ks.s
  first (see examples/18-tables.s for the pattern). All three
  real-hash modes iterate in Robin Hood / hash order; AM_BITMAP
  modes iterate in increasing-gid order via the bitmap scan.

  Mode transitions happen lazily inside amSet/amGidSet: every
  promotion path widens the underlying type (the inverse demotion
  doesn't exist -- once promoted to AM_INT, a column that drains
  back to all-zero values stays in INT mode until amClear).

  --- void_val contract ---

  AM_VOID(o) holds the value returned for missing keys. Defaults
  to No (set in amNew). The user can change it with `T.setNo X`
  (-> amSetNo).

  amSet / amGidSet treat `value == void_val` as a delete request.
  In particular, this means:
    - Default void_val = No: `T.K = No` deletes K. (Expected.)
    - After `T.setNo 0`: `T.K = 0` also deletes K, even though
      the user "stored a value". This is the AM-7 quirk -- the
      surprise is greatest on BITMAP0 tables, where 0 is *also*
      the implicit stored value. We have no separate "remove key"
      API in Symta today; the convention is to either pick a
      void_val that's not in your value alphabet (the default No
      is usually right), or use T.del K explicitly.

  --- AM_EMPTY initial-mode selection (AM-7b: aligned) ---

  Both amSet (user-facing T.K=V) and amGidSet (component-column
  setter, used by cls fields) pick the same initial mode for an
  empty table:

    value == FXN(0) -> AM_BITMAP0
    value == FXN(1) -> AM_BITMAP1
    other int       -> AM_INT
    text key        -> AM_TEXT
    other key       -> AM_GENERIC

  Observable behaviour is the same across BITMAP and INT modes
  (`T.has K` returns 1, `T.K` returns the stored value); the
  bitmap choice saves ~16 B/key over AM_INT for the dense-zero
  and dense-one cases. Promotion to AM_INT happens on the
  first divergent value (e.g., a third value in a 0/1 table
  becomes AM_INT carrying all prior keys).

  --- iteration / mutation ---

  amL and amKs eagerly build a fresh list of the current
  contents; mutation after the call returns doesn't disturb the
  returned list. amL therefore *implicitly snapshots*; we don't
  need an explicit "iteration in progress" guard. Callers that
  walk the table via NH_FOR/internal pointers (e.g. from inside
  a finalizer) are on their own.

Todo:
* Efficient strings set representation:
  Use a default value 1 or a radix judy tree instead of a string-keyed hash map.


******************************/


#ifndef AM_H
#define AM_H

#include "ng.h"
#include "dh.h"
#include "ih.h"
#include "th.h"
#include "nb.h"
#include "prf.h"

#ifdef AM_IMPLEMENTATION
uint32_t amFinalizerHook;
#undef AM_IMPLEMENTATION
#else
extern uint32_t amFinalizerHook;
#endif


#define AM_ALLOC(r) OBJECT(r, T_TBL, 2)

#define AM_BASE(o) (*((void **)&LGET8((o),0)))
#define AM_VOID(o) (*((void **)&LGET8((o),1)))

#define AM_TYPE(o) (O_CODE(o)&0xFF)
#define AM_SET_TYPE(o,v) O_CODE(o) = ((O_CODE(o)&~(uint32_t)0xFF)|(v))

//the age of the youngest object it holds
#define AM_YOUNGEST(o) (O_CODE(o)>>8)
#define AM_SET_YOUNGEST(o,v) \
  O_CODE(o) = ((O_CODE(o)&~(uint32_t)0xFFFF00)|((v)<<8))


#define AM_OLDER(hm,v) (!IMMEDIATE(v) && AM_YOUNGEST(hm) > O_AGE(v))

#define AM_ATTRACT(hm,v)                \
  if (AM_OLDER(hm, v)) {                \
    int v_age = O_AGE(v);               \
    AM_SET_YOUNGEST(hm,v_age);          \
    arrput(api.hg0[v_age].magnets, hm); \
  }

#define AM_EMPTY   0
#define AM_GENERIC 1
#define AM_TEXT    2
#define AM_INT     3
#define AM_BITMAP0 4
#define AM_BITMAP1 5


/* symta_itbl was the stb_ds int-keyed row; AM-6 replaced
 * it with ih_t (nh_t-template) for AM_INT. symta_stbl
 * was its string-keyed sibling; AM-6b replaced it with th_t
 * for AM_TEXT. AM-pack-v2 then changed the layout *inside* every
 * nh_t instantiation: keys, values, and (when NH_CACHE_HASH)
 * the hash now live in a single packed `nh_slot_t` array inline
 * in the nh_t tail, instead of three separate arrays. Hot path
 * reads one cache line for everything; one malloc per table
 * instead of two or three. AM_BITMAP* still uses its dedicated
 * nb_t (which sits on top of an nh_t-template internally and
 * inherits the packed layout). */


INLINE dyn amGidGet(dyn o, dyn ref) {
  dyn r;
  CASE(AM_TYPE(o))
  GOT(AM_EMPTY) r = AM_VOID(o);
  GOT(AM_INT)
    ih_t *hm = AM_BASE(o);
    dyn key = FXN(O_GID(ref));
    r = ihGet(hm, key);
    if (r == NIL) r = AM_VOID(o);
  GOT(AM_BITMAP0)
    nb_t nb = AM_BASE(o);
    dyn key = FXN(O_GID(ref));
    r = nbGet(nb, UNFXN(key)) ? FXN(0) : AM_VOID(o);
  GOT(AM_BITMAP1)
    nb_t nb = AM_BASE(o);
    dyn key = FXN(O_GID(ref));
    r = nbGet(nb, UNFXN(key)) ? FXN(1) : AM_VOID(o);
  GOT(AM_GENERIC)
    /* Column promoted to GENERIC at some point (mixed key types or
     * a non-int key got written). The keys are stored as full Symta
     * values; look up by the gid-as-int key the same way amGet would.
     * AM-1: fixes the latent UB where amGidGet returned
     * `r` uninitialised on this branch. */
    dh_t *hm = AM_BASE(o);
    r = dhGet(hm, FXN(O_GID(ref)));
    if (r == NIL) r = AM_VOID(o);
  GOT(AM_TEXT)
    /* Text-keyed column being queried by entity GID is a contract
     * violation -- cls fields are integer-keyed by construction.
     * Crash loudly so the bug surfaces at the call site instead of
     * silently returning garbage. (Also AM-1.) */
    fatal("amGidGet: AM_TEXT column queried by integer GID\n");
    r = AM_VOID(o); /* unreachable; keeps the compiler quiet */
  ESAC
  return r;
}

INLINE dyn amRefs(dyn o, uint64_t tag) {
  dyn r;
  GC_DISABLE();
  CASE(AM_TYPE(o))
  GOT(AM_EMPTY)
    r = Empty;
  GOT(AM_TEXT)
    r = Empty;
  GOT(AM_INT)
    ih_t *hm = AM_BASE(o);
    int n = ihN(hm);
    LIST(r,n);
    int j = 0;
    NH_FOR(ih,i,hm) {
      LGET(r,j) = MKIMM(tag,UNFXN(*ihKey(hm,i)));
      j++;
    }
  GOT(AM_GENERIC)
    r = Empty;
  GOT(AM_BITMAP0)
    nb_t nb = AM_BASE(o);
    int n = nbN(nb);
    LIST(r,n);
    int i = 0;
    NB_FOR(id,nb) {
      LGET(r,i) = MKIMM(tag,(uint64_t)id);
      ++i;
    }
  GOT(AM_BITMAP1)
    nb_t nb = AM_BASE(o);
    int n = nbN(nb);
    LIST(r,n);
    int i = 0;
    NB_FOR(id,nb) {
      LGET(r,i) = MKIMM(tag,(uint64_t)id);
      ++i;
    }
  ESAC
  GC_ENABLE();
  return r;
}

INLINE void amGidSet(dyn o, dyn ref, dyn value) {
  AM_ATTRACT(o,value);
  dyn key = FXN(O_GID(ref));
  dyn void_val = AM_VOID(o);
  if (value == void_val) { //we are allowed to delete these
    CASE(AM_TYPE(o))
    GOT(AM_EMPTY)
      return;
    GOT(AM_GENERIC)
      dh_t *hm = AM_BASE(o);
      dhDel(&hm, key);
      AM_BASE(o) = hm;
      return;
    GOT(AM_INT)
      ih_t *hm = AM_BASE(o);
      if (O_TAG(key) == T_INT) {
        ihDel(&hm, key);
        AM_BASE(o) = hm;
      }
      return;
    GOT(AM_BITMAP0)
      if (O_TAG(key) == T_INT) {
        nb_t nb = AM_BASE(o);
        nbDel(&nb, UNFXN(key));
        AM_BASE(o) = nb;
      }
      return;
    GOT(AM_BITMAP1)
      if (O_TAG(key) == T_INT) {
        nb_t nb = AM_BASE(o);
        nbDel(&nb, UNFXN(key));
        AM_BASE(o) = nb;
      }
      return;
    ESAC
  }


  CASE(AM_TYPE(o))
  GOT(AM_EMPTY)
    if (value == FXN(0)) {
      nb_t nb = nbAlloc();
      nbSet(&nb,UNFXN(key));
      AM_BASE(o) = nb;
      AM_SET_TYPE(o, AM_BITMAP0);
    } else if (value == FXN(1)) {
      nb_t nb = nbAlloc();
      nbSet(&nb,UNFXN(key));
      AM_BASE(o) = nb;
      AM_SET_TYPE(o, AM_BITMAP1);
    } else {
      ih_t *hm = ihAlloc();
      ihSet(&hm, key, value);
      AM_BASE(o) = hm;
      AM_SET_TYPE(o, AM_INT);
    }
  GOT(AM_INT)
    ih_t *hm = AM_BASE(o);
    ihSet(&hm, key, value);
    AM_BASE(o) = hm;
  GOT(AM_BITMAP0)
    nb_t nb = AM_BASE(o);
    if (value == FXN(0)) {
      nbSet(&nb,UNFXN(key));
      AM_BASE(o) = nb;
    } else {
      ih_t *hm = ihAlloc();
      NB_FOR(id,nb) ihSet(&hm, FXN(id), FXN(0));
      ihSet(&hm, key, value);
      AM_BASE(o) = hm;
      AM_SET_TYPE(o, AM_INT);
      nbFree(nb); /* AM-14: pre-existing leak -- the old bitmap
                   * pages were never freed on BITMAP→INT
                   * promotion. */
    }
  GOT(AM_BITMAP1)
    nb_t nb = AM_BASE(o);
    if (value == FXN(1)) {
      nbSet(&nb,UNFXN(key));
      AM_BASE(o) = nb;
    } else {
      ih_t *hm = ihAlloc();
      NB_FOR(id,nb) ihSet(&hm, FXN(id), FXN(1));
      ihSet(&hm, key, value);
      AM_BASE(o) = hm;
      AM_SET_TYPE(o, AM_INT);
      nbFree(nb); /* AM-14 */
    }
  ESAC
}

static dyn amClear(uint8_t *bc_) {
  dyn o = LGET(api.args,0);
  CASE(AM_TYPE(o))
  GOT(AM_EMPTY)
  GOT(AM_GENERIC)
    dh_t *hm = AM_BASE(o);
    dhFree(hm);
  GOT(AM_TEXT)
    th_t *hm = AM_BASE(o);
    thFree(hm);
  GOT(AM_INT)
    ih_t *hm = AM_BASE(o);
    ihFree(hm);
  GOT(AM_BITMAP0)
    nb_t nb = AM_BASE(o);
    nbFree(nb);
  GOT(AM_BITMAP1)
    nb_t nb = AM_BASE(o);
    nbFree(nb);
  ESAC
  AM_BASE(o) = 0;
  AM_SET_TYPE(o, AM_EMPTY);
  return 0;
}

INLINE dyn amNew() {
  dyn r;
  GC_DISABLE();
  AM_ALLOC(r);
  AM_VOID(r) = No;
  AM_SET_YOUNGEST(r,api.hgp->age);
  dyn fin;
  if (!amFinalizerHook) amFinalizerHook = sbc_hook(amClear,0);
  CLOSURE(fin, amFinalizerHook, 0);
  gc_set_finalizer(r, fin); //FIXME: this should be a builtin function
  GC_ENABLE();
  return r;
}

INLINE dyn amHas(dyn o, dyn key) { //Key exists
  dyn r;
  CASE(AM_TYPE(o))
  GOT(AM_EMPTY) return 0;
  GOT(AM_TEXT)
    if (!IS_TEXT(key)) return 0;
    th_t *hm = AM_BASE(o);
    r = thGet(hm, key);
  GOT(AM_INT)
    ih_t *hm = AM_BASE(o);
    r = ihGet(hm, key);
  GOT(AM_GENERIC)
    dh_t *hm = AM_BASE(o);
    r = dhGet(hm, key);
  GOT(AM_BITMAP0)
    /* AM-12: bit set ↔ key present (see amSet's
     * BITMAP0 branch: writing 0 calls nbSet, populating the
     * bit). amGet, amGidGet, and AM_BITMAP1's amHas all agree
     * on this semantic. The original `nbGet == 0` was an
     * inversion left over from when BITMAP0 tracked exceptions
     * rather than presence. Latent until tc_void exercised it
     * through `T.has K` on a true BITMAP0 table.
     *
     * AM-13: the T_INT guard rejects non-int keys before they
     * reach nbGet -- otherwise UNFXN(non-int) feeds garbage to
     * the bitmap and can rarely false-positive against a
     * populated bit. (`T.has "hello"` on a B0 table.) */
    if (O_TAG(key) != T_INT) return 0;
    nb_t nb = AM_BASE(o);
    return FXN(nbGet(nb, UNFXN(key)) != 0);
  GOT(AM_BITMAP1)
    if (O_TAG(key) != T_INT) return 0;
    nb_t nb = AM_BASE(o);
    return FXN(nbGet(nb, UNFXN(key)) != 0);
  ESAC
  return FXN(r != NIL);
}

INLINE dyn amGot(dyn o, dyn key) { //key both exists and has value != No
  dyn r;
  CASE(AM_TYPE(o))
  GOT(AM_EMPTY) return 0;
  GOT(AM_TEXT)
    if (!IS_TEXT(key)) return 0;
    th_t *hm = AM_BASE(o);
    r = thGet(hm, key);
  GOT(AM_INT)
    ih_t *hm = AM_BASE(o);
    r = ihGet(hm, key);
  GOT(AM_GENERIC)
    dh_t *hm = AM_BASE(o);
    r = dhGet(hm, key);
  GOT(AM_BITMAP0)
    /* AM-12: same inversion fix as in amHas above.
     * amGot is "present AND value != No"; on BITMAP0 the value
     * is always FXN(0) which is != No, so amGot collapses to
     * the same predicate as amHas. AM-13: same T_INT guard. */
    if (O_TAG(key) != T_INT) return 0;
    nb_t nb = AM_BASE(o);
    int64_t rr = nbGet(nb, UNFXN(key)) != 0;
    return FXN(rr);
  GOT(AM_BITMAP1)
    if (O_TAG(key) != T_INT) return 0;
    nb_t nb = AM_BASE(o);
    int64_t rr = nbGet(nb, UNFXN(key)) != 0;
    return FXN(rr);
  ESAC
  return FXN(r != NIL && r != No);
}

INLINE dyn amGet(dyn o, dyn key) {
  dyn r;
  CASE(AM_TYPE(o))
  GOT(AM_EMPTY) return AM_VOID(o);
  GOT(AM_TEXT)
    if (!IS_TEXT(key)) return AM_VOID(o);
    th_t *hm = AM_BASE(o);
    r = thGet(hm, key);
    if (r == NIL) r = AM_VOID(o);
  GOT(AM_INT)
    ih_t *hm = AM_BASE(o);
    r = ihGet(hm, key);
    if (r == NIL) r = AM_VOID(o);
  GOT(AM_GENERIC)
    dh_t *hm = AM_BASE(o);
    r = dhGet(hm, key);
    if (r == NIL) r = AM_VOID(o);
  GOT(AM_BITMAP0)
    /* AM-13: same T_INT defence as amHas/amGot. */
    if (O_TAG(key) != T_INT) return AM_VOID(o);
    nb_t nb = AM_BASE(o);
    r = nbGet(nb, UNFXN(key)) ? FXN(0) : AM_VOID(o);
  GOT(AM_BITMAP1)
    if (O_TAG(key) != T_INT) return AM_VOID(o);
    nb_t nb = AM_BASE(o);
    r = nbGet(nb, UNFXN(key)) ? FXN(1) : AM_VOID(o);
  ESAC
  return r;
}

INLINE void amSetNo(dyn o, dyn value) {
  AM_VOID(o) = value;
}


INLINE void amSwap(dyn o, dyn m) {
  dyn obase = AM_BASE(o);
  dyn ovoid = AM_VOID(o);
  uint32_t otype = AM_TYPE(o);
  uint32_t oyoungest = AM_YOUNGEST(o);

  AM_BASE(o) = AM_BASE(m);
  AM_VOID(o) = AM_VOID(m);
  AM_SET_TYPE(o,AM_TYPE(m));
  AM_SET_YOUNGEST(o,AM_YOUNGEST(m));

  AM_BASE(m) = obase;
  AM_VOID(m) = ovoid;
  AM_SET_TYPE(m,otype);
  AM_SET_YOUNGEST(m,oyoungest);
}

//FIXME: maps upgraded to a generic map can be
//       replaced with a special type on the next GC.
//       such type won't need a switch.
INLINE void amSet(dyn o, dyn key, dyn value) {
  dyn void_val = AM_VOID(o);
  if (value == void_val) { //we are allowed to delete these
    CASE(AM_TYPE(o))
    GOT(AM_EMPTY)
       return;
    GOT(AM_GENERIC)
      dh_t *hm = AM_BASE(o);
      dhDel(&hm, key);
      AM_BASE(o) = hm;
      return;
    GOT(AM_TEXT)
      th_t *hm = AM_BASE(o);
      if (IS_TEXT(key)) {
        thDel(&hm, key);
        AM_BASE(o) = hm;
      }
      /* AM-11: without this `return`, control falls out
       * of the delete-branch switch, past the `if (value==void_val)`
       * block, and into the regular AM_TEXT insert branch below --
       * which would `thSet` the key right back. */
      return;
    GOT(AM_INT)
      ih_t *hm = AM_BASE(o);
      if (O_TAG(key) == T_INT) {
        ihDel(&hm, key);
        AM_BASE(o) = hm;
      }
      return;
    GOT(AM_BITMAP0)
      if (O_TAG(key) == T_INT) {
        nb_t nb = AM_BASE(o);
        nbDel(&nb, UNFXN(key));
        AM_BASE(o) = nb;
      }
      return;
    GOT(AM_BITMAP1)
      if (O_TAG(key) == T_INT) {
        nb_t nb = AM_BASE(o);
        nbDel(&nb, UNFXN(key));
        AM_BASE(o) = nb;
      }
      return;
    ESAC
  }

  AM_ATTRACT(o,key);
  AM_ATTRACT(o,value);

  CASE(AM_TYPE(o))
  GOT(AM_EMPTY)
    //pick initial type
    if (IS_TEXT(key)) {
      th_t *hm = thAlloc();
      thSet(&hm, key, value);
      AM_BASE(o) = hm;
      AM_SET_TYPE(o, AM_TEXT);
    } else if (O_TAG(key) == T_INT) {
      /* AM-7b: pick BITMAP0/1 for the first FXN(0)/FXN(1) write,
       * matching amGidSet's behaviour. The two paths used to
       * diverge -- amSet preferred AM_INT for FXN(0) on the
       * theory that the user "stored a value", but the
       * observable behaviour is identical between AM_INT and
       * AM_BITMAP0 (T.has K → 1, T.K → 0) and the memory cost
       * difference is ~16 B/key (INT slot) vs ~1 bit/key
       * (bitmap). Users writing T.K = 0 in a loop now get the
       * compact representation by default. */
      if (value == FXN(0)) {
        nb_t nb = nbAlloc();
        nbSet(&nb,UNFXN(key));
        AM_BASE(o) = nb;
        AM_SET_TYPE(o, AM_BITMAP0);
      } else if (value == FXN(1)) {
        nb_t nb = nbAlloc();
        nbSet(&nb,UNFXN(key));
        AM_BASE(o) = nb;
        AM_SET_TYPE(o, AM_BITMAP1);
      } else {
        ih_t *hm = ihAlloc();
        ihSet(&hm, key, value);
        AM_BASE(o) = hm;
        AM_SET_TYPE(o, AM_INT);
      }
    } else {
      dh_t *hm = dhAlloc();
      dhSet(&hm, key, value);
      AM_BASE(o) = hm;
      AM_SET_TYPE(o, AM_GENERIC);
    }
  GOT(AM_GENERIC)
    dh_t *hm = AM_BASE(o);
    dhSet(&hm, key, value);
    AM_BASE(o) = hm;
  GOT(AM_TEXT)
    th_t *hm = AM_BASE(o);
    if (IS_TEXT(key)) {
      thSet(&hm, key, value);
      AM_BASE(o) = hm;
    } else { //convert to dynamic key type hash map
      dh_t *dh = dhAlloc();
      NH_FOR(th,i,hm) {
        dhSet(&dh, *thKey(hm,i), *thVal(hm,i));
      }
      dhSet(&dh, key, value);
      AM_BASE(o) = dh;
      AM_SET_TYPE(o, AM_GENERIC);
      thFree(hm);
    }
  GOT(AM_INT)
    ih_t *hm = AM_BASE(o);
    if (O_TAG(key) == T_INT) {
      ihSet(&hm, key, value);
      AM_BASE(o) = hm;
    } else { //convert to dynamic key type hash map
      dh_t *dh = dhAlloc();
      NH_FOR(ih,i,hm) {
        dhSet(&dh, *ihKey(hm,i), *ihVal(hm,i));
      }
      dhSet(&dh, key, value);
      AM_BASE(o) = dh;
      AM_SET_TYPE(o, AM_GENERIC);
      ihFree(hm);
    }
  GOT(AM_BITMAP0)
    nb_t nb = AM_BASE(o);
    if (O_TAG(key) == T_INT) {
      if (value == FXN(0)) {
        nbSet(&nb,UNFXN(key));
        AM_BASE(o) = nb;
      } else {
        ih_t *hm = ihAlloc();
        NB_FOR(id,nb) ihSet(&hm, FXN(id), FXN(0));
        ihSet(&hm, key, value);
        AM_BASE(o) = hm;
        AM_SET_TYPE(o, AM_INT);
        nbFree(nb); /* AM-14 */
      }
    } else {
      dh_t *dh = dhAlloc();
      NB_FOR(id,nb) dhSet(&dh, FXN(id), FXN(0));
      dhSet(&dh, key, value);
      AM_BASE(o) = dh;
      AM_SET_TYPE(o, AM_GENERIC);
      nbFree(nb);
    }
  GOT(AM_BITMAP1)
    nb_t nb = AM_BASE(o);
    if (O_TAG(key) == T_INT) {
      if (value == FXN(1)) {
        nbSet(&nb,UNFXN(key));
        AM_BASE(o) = nb;
      } else {
        ih_t *hm = ihAlloc();
        NB_FOR(id,nb) ihSet(&hm, FXN(id), FXN(1));
        ihSet(&hm, key, value);
        AM_BASE(o) = hm;
        AM_SET_TYPE(o, AM_INT);
        nbFree(nb); /* AM-14 */
      }
    } else {
      dh_t *dh = dhAlloc();
      NB_FOR(id,nb) dhSet(&dh, FXN(id), FXN(1));
      dhSet(&dh, key, value);
      AM_BASE(o) = dh;
      AM_SET_TYPE(o, AM_GENERIC);
      nbFree(nb);
    }
  ESAC
}

INLINE dyn amDel(dyn o, dyn key) {
  CASE(AM_TYPE(o))
  GOT(AM_EMPTY)
  GOT(AM_GENERIC)
    dh_t *hm = AM_BASE(o);
    dhDel(&hm, key);
    AM_BASE(o) = hm;
  GOT(AM_TEXT)
    th_t *hm = AM_BASE(o);
    if (IS_TEXT(key)) {
      thDel(&hm, key);
      AM_BASE(o) = hm;
    }
  GOT(AM_INT)
    ih_t *hm = AM_BASE(o);
    if (O_TAG(key) == T_INT) {
      ihDel(&hm, key);
      AM_BASE(o) = hm;
    }
  GOT(AM_BITMAP0)
    if (O_TAG(key) == T_INT) {
      nb_t nb = AM_BASE(o);
      nbDel(&nb, UNFXN(key));
      AM_BASE(o) = nb;
    }
  GOT(AM_BITMAP1)
    if (O_TAG(key) == T_INT) {
      nb_t nb = AM_BASE(o);
      nbDel(&nb, UNFXN(key));
      AM_BASE(o) = nb;
    }
  ESAC
  return o;
}

INLINE dyn amN(dyn o) {
  CASE(AM_TYPE(o))
  GOT(AM_EMPTY)
  GOT(AM_GENERIC)
    dh_t *hm = AM_BASE(o);
    return FXN(dhN(hm));
  GOT(AM_TEXT)
    th_t *hm = AM_BASE(o);
    return FXN(thN(hm));
  GOT(AM_INT)
    ih_t *hm = AM_BASE(o);
    return FXN(ihN(hm));
  GOT(AM_BITMAP0)
    nb_t nb = AM_BASE(o);
    return FXN(nbN(nb));
  GOT(AM_BITMAP1)
    nb_t nb = AM_BASE(o);
    return FXN(nbN(nb));
  ESAC
  return FXN(0);
}

INLINE dyn amL(dyn o) {
  dyn r;
  GC_DISABLE();
  CASE(AM_TYPE(o))
  GOT(AM_EMPTY)
    r = Empty;
  GOT(AM_TEXT)
    th_t *hm = AM_BASE(o);
    int n = thN(hm);
    LIST(r,n);
    int j = 0;
    NH_FOR(th,i,hm) {
      dyn kv;
      LIST(kv,2);
      LGET(kv,0) = *thKey(hm,i);
      LGET(kv,1) = *thVal(hm,i);
      LGET(r,j) = kv;
      j++;
    }
  GOT(AM_INT)
    ih_t *hm = AM_BASE(o);
    int n = ihN(hm);
    LIST(r,n);
    int j = 0;
    NH_FOR(ih,i,hm) {
      dyn kv;
      LIST(kv,2);
      LGET(kv,0) = *ihKey(hm,i);
      LGET(kv,1) = *ihVal(hm,i);
      LGET(r,j) = kv;
      j++;
    }
  GOT(AM_GENERIC)
    dh_t *hm = AM_BASE(o);
    LIST(r,dhN(hm));
    int j = 0;
    NH_FOR(dh,i,hm) {
      dyn kv;
      LIST(kv,2);
      LGET(kv,0) = *dhKey(hm,i);
      LGET(kv,1) = *dhVal(hm,i);
      LGET(r,j) = kv;
      j++;
    }
  GOT(AM_BITMAP0)
    nb_t nb = AM_BASE(o);
    int n = nbN(nb);
    LIST(r,n);
    int i = 0;
    NB_FOR(id,nb) {
      dyn kv;
      LIST(kv,2);
      LGET(kv,0) = FXN(id);
      LGET(kv,1) = FXN(0);
      LGET(r,i) = kv;
      ++i;
    }
  GOT(AM_BITMAP1)
    nb_t nb = AM_BASE(o);
    int n = nbN(nb);
    LIST(r,n);
    int i = 0;
    NB_FOR(id,nb) {
      dyn kv;
      LIST(kv,2);
      LGET(kv,0) = FXN(id);
      LGET(kv,1) = FXN(1);
      LGET(r,i) = kv;
      ++i;
    }
  ESAC
  GC_ENABLE();
  return r;
}

INLINE dyn amKs(dyn o) {
  dyn r;
  GC_DISABLE();
  CASE(AM_TYPE(o))
  GOT(AM_EMPTY)
    r = Empty;
  GOT(AM_TEXT)
    th_t *hm = AM_BASE(o);
    int n = thN(hm);
    LIST(r,n);
    int j = 0;
    NH_FOR(th,i,hm) {
      LGET(r,j) = *thKey(hm,i);
      j++;
    }
  GOT(AM_INT)
    ih_t *hm = AM_BASE(o);
    int n = ihN(hm);
    LIST(r,n);
    int j = 0;
    NH_FOR(ih,i,hm) {
      LGET(r,j) = *ihKey(hm,i);
      j++;
    }
  GOT(AM_GENERIC)
    dh_t *hm = AM_BASE(o);
    LIST(r,dhN(hm));
    int j = 0;
    NH_FOR(dh,i,hm) {
      LGET(r,j) = *dhKey(hm,i);
      j++;
    }
  GOT(AM_BITMAP0)
    nb_t nb = AM_BASE(o);
    int n = nbN(nb);
    LIST(r,n);
    int i = 0;
    NB_FOR(id,nb) {
      LGET(r,i) = FXN(id);
      ++i;
    }
  GOT(AM_BITMAP1)
    nb_t nb = AM_BASE(o);
    int n = nbN(nb);
    LIST(r,n);
    int i = 0;
    NB_FOR(id,nb) {
      LGET(r,i) = FXN(id);
      ++i;
    }
  ESAC
  GC_ENABLE();
  return r;
}



#endif
