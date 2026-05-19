// TS-3 integration: the bank-account demo's static guards.
// Putting a text literal where the strict checker expects an int
// (cents field) trips the TS-3.1 mismatch at mex time, not at
// the runtime tag check.  Mirrors the "pin sensitive fields"
// pattern in examples/40-bank.s.
type money Cents Curr: cents!Cents currency!Curr

bad_amount = money (_the int "fifty cents") \USD
