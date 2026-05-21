// Typed params + static.  Bin_op routes to _iadd, etc.
add X^int Y^int = static (X + Y)
say (add 3 5)              // 8
mul X^float Y^float = static (X * Y)
say (mul 2.5 4.0)          // 10.0
