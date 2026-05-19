// TS-4.2: typed-int `<<` (<=) and `>>` (>=) via _ilte / _igte.
lte X^int Y^int = X << Y
gte X^int Y^int = X >> Y
say (lte 3 5)        // 1
say (lte 5 5)        // 1
say (lte 7 3)        // 0
say (gte 5 3)        // 1
say (gte 5 5)        // 1
say (gte 3 5)        // 0
