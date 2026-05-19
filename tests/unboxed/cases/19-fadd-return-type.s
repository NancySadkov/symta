// TS-4.2: typed-float arithmetic returns float statically.
// As with case 10/18, infer_declared_type doesn't peek through
// the pre-mex `+` form; explicit `_the float` makes TS-3.8 fire.
fadd_two X^float Y^float = _the float (X + Y)
result = _the int (fadd_two 1.5 2.5)
