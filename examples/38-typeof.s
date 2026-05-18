// 38-typeof.s -- TS-2: runtime type introspection.
//
//   typeof X         -> text naming X's type ("int", "list", ...)
//                       multi-tag tags are normalised: any of
//                       T_LIST | T_VIEW | T_CONS | T_BYTES -> "list";
//                       T_TEXT | T_FIXTEXT -> "text".
//   subtype_of X T   -> 1 if X is of type T, 0 otherwise.
//                       T may be a text ("int") or tag-symbol (\int).
//                       Walks the runtime super chain (TS-2.1):
//                       `subtype_of 5 \_` is 1 (int <: _),
//                       `subtype_of [1] \hard_list` is 1
//                       (T_LIST <: T_HARD_LIST).
//   parents_of_ X    -> list of type names from X's own type
//                       up to the root.
//
// Run:  symta -f examples/38-typeof.s


// --- typeof: name for the type tag of a value ----------------

A typeof 5;        say "typeof int   = [A]"   // int
A typeof 1.5;      say "typeof float = [A]"   // float
A typeof "hi";     say "typeof text  = [A]"   // text
A typeof No;       say "typeof no    = [A]"   // no


// --- typeof normalises the four list tags --------------------

L1 [1 2 3]             // T_LIST
L2 L1.pre 0            // T_CONS (head/tail cons cell)
L3 L1[0:2]             // T_VIEW (zero-copy slice)
say "T_LIST  = [typeof L1]"   // list
say "T_CONS  = [typeof L2]"   // list
say "T_VIEW  = [typeof L3]"   // list


// --- typeof on a user-defined type ---------------------------

type point x y
P point 3 4
say "typeof point = [typeof P]"   // point


// --- subtype_of: type-equality at the typename granularity --

A subtype_of 5 \int;     say "5 :> int   = [A]"   // 1
A subtype_of 5 \float;   say "5 :> float = [A]"   // 0
A subtype_of "ab" \text; say "'ab' :> text= [A]"  // 1
A subtype_of L1 \list;   say "L_FLAT :> list = [A]" // 1
A subtype_of L2 \list;   say "L_CONS :> list = [A]" // 1
A subtype_of L3 \list;   say "L_VIEW :> list = [A]" // 1
A subtype_of P \point;   say "P :> point = [A]"   // 1
A subtype_of P \list;    say "P :> list  = [A]"   // 0


// --- subtype_of accepts text or tag-symbol -------------------

A subtype_of 5 "int";    say "text-arg  = [A]"    // 1
A subtype_of 5 \int;     say "tag-arg   = [A]"    // 1


// --- TS-2.1: inheritance walk via runtime super chain --------

A subtype_of 5 \_;       say "5 :> _ (root) = [A]"      // 1
A subtype_of L1 \hard_list; say "L_FLAT :> hard_list = [A]" // 1
A subtype_of "ab" \_fixtext_; say "'ab' :> _fixtext_ = [A]"  // 1 (short text is fixtext)

A parents_of_ 5;         say "parents_of 5 = [A]"      // (int _)
A parents_of_ [1 2 3];   say "parents_of list = [A]"   // (_list_ hard_list list _)
