// The macro-internal R gensym of `case` reassigns across
// different arm types (int sentinel 0 vs each arm's body type).
// `case` MUST keep working inside `static` thanks to the
// gensym filter (`__N` suffix) -- propagation skips internal
// registers.  Each arm here returns text; without the filter
// the sideband would type R as text from the first arm and
// then trip on the int sentinel.
sign_of N^int = static:
  case N:
    0 = "zero"
    Else = if N < 0 then "neg" else "pos"
say (sign_of 0)            // zero
say (sign_of -5)           // neg
say (sign_of 12)           // pos
