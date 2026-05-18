// `_unsafe T E` skips the static check.  No compile error
// even though "hello" isn't an int.  At runtime the value is
// reinterpreted as int (welcome to UB).
X _unsafe int "hello"
// We don't actually use X here because UB.  Just confirm no
// compile-time error.
say "unsafe ok"
