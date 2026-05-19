// TS-4.2: typed-int comparisons return int (0/1) statically.
// As with case 10, infer_declared_type doesn't yet peek through
// the pre-mex `<` operator -- explicit `_the int` wraps the
// result so TS-3.8 fn-return registration fires.  Mismatch on
// the caller side then catches at mex time.
is_less X^int Y^int = _the int (X < Y)
result = _the text (is_less 3 5)
