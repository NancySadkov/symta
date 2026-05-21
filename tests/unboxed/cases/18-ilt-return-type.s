// TS-4.2: typed-int comparisons return int (0/1) statically.
// TS-4.3a lifted the earlier `_the int (X < Y)` workaround --
// the pre-mex `<` peek-through in infer_declared_type now
// returns int directly for typed-param operands.
is_less X^int Y^int = X < Y
result = _the text (is_less 3 5)
