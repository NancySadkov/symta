// TS-4.2: typed-float arithmetic returns float statically.
// TS-4.3a lifted the earlier `_the float (X + Y)` workaround.
fadd_two X^float Y^float = X + Y
result = _the int (fadd_two 1.5 2.5)
