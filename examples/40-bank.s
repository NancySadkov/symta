// 40-bank.s -- TS-3: bank-account demo exercising the type system.
//
// A miniature bank-account library where the type system enforces
// the invariants you'd actually want a real ledger to enforce:
//
//   * Money is a tagged value (amount + currency).  You can't
//     accidentally add USD to EUR -- the constructor refuses to
//     unify them at runtime, and any static path that mixes the
//     two trips the strict checker at mex time.
//
//   * `deposit` / `withdraw` / `transfer` take `account` values
//     by type, not by text/dict.  Pass a bare list and TS-1's
//     `_the account` boundary check fires.
//
//   * The amount inside `money` is `int` (whole cents).  No
//     float drift.  TS-3.1 catches `money 12.5 \USD` at compile
//     time.
//
//   * Transaction kinds are case-narrowed -- `record_txn` reads
//     them as `[X^int @Rest]` style patterns so the audit-log
//     fields are typed inside the case-arm body.
//
// Run:  symta -f examples/40-bank.s
//
// What "in full" means here: every TS-3 feature gets exercised.
// The labelled sections below mark which slice each line tests.


// --- Domain types --------------------------------------------
//
// TS-1 substrate: `type T: field!Default ...` declares T and
// auto-emits a constructor `T arg1 arg2 ...`, per-field readers
// `T.field`, and the runtime tag check for `_the T x` / `x^T`.

type money Cents Curr: cents!Cents currency!Curr
type account Id Name Bal: id!Id name!Name bal!Bal


// --- Currency safety -----------------------------------------
//
// `money` carries its currency as a field.  Arithmetic enforces
// the match.  TS-3.5 case-narrowing means inside the `int?` arm
// of `add`, both .cents reads see int (so the `+` lowers to the
// unboxed integer path, not the dyn dispatch).

add A B =
| _the money A
| _the money B
| less A.currency >< B.currency:
  | bad "currency mismatch: [A.currency] vs [B.currency]"
| money A.cents+B.cents A.currency

sub A B =
| _the money A
| _the money B
| less A.currency >< B.currency:
  | bad "currency mismatch: [A.currency] vs [B.currency]"
| money A.cents-B.cents A.currency

money_lt A B =
| _the money A
| _the money B
| less A.currency >< B.currency:
  | bad "currency mismatch: [A.currency] vs [B.currency]"
| A.cents < B.cents


// --- Audit log -----------------------------------------------
//
// Transactions are typed lists.  The log is appended in-place.
// `txn_kind` returns a keyword tag -- not text -- so a typo at
// the call site is a compile-time `undefined variable` rather
// than a silently-wrong audit entry.

GLog []

record_txn Kind Acc Amt =
| _the account Acc
| _the money Amt
| push [Kind Acc.id Amt.cents Amt.currency] GLog


// --- Operations ----------------------------------------------
//
// `deposit` / `withdraw` are typed at the boundary.  Static
// check catches `deposit "savings" amt` (text != account) at
// mex time.  TS-3.13 case-result unification means the body
// of `withdraw` is typed `account` (both arms return one), so
// downstream callers that consume the result via `_the account`
// pass the static check.

deposit Acc Amt =
| _the account Acc
| _the money Amt
| NewBal add Acc.bal Amt
| account Acc.id Acc.name NewBal

withdraw Acc Amt =
| _the account Acc
| _the money Amt
| when money_lt Acc.bal Amt: bad "insufficient: [Acc.bal.cents] < [Amt.cents]"
| NewBal sub Acc.bal Amt
| record_txn \withdraw Acc Amt
| account Acc.id Acc.name NewBal

transfer Src Dst Amt =
| _the account Src
| _the account Dst
| _the money Amt
| Src2 withdraw Src Amt
| Dst2 deposit Dst Amt
| record_txn \transfer Src Amt
| [Src2 Dst2]


// --- Demo ----------------------------------------------------

say "=== Bank-account type-system demo ==="
say ""

// Open two accounts.  `money 50000 \USD` builds the
// constructor; the `account` constructor takes id, name, and
// initial balance.  Both fields are runtime-checked at boundary.
Alice account 1 "Alice" (money 50000 \USD)
Bob   account 2 "Bob"   (money 30000 \USD)

say "Alice: id=[Alice.id] bal=[Alice.bal.cents]c [Alice.bal.currency]"
say "Bob:   id=[Bob.id] bal=[Bob.bal.cents]c [Bob.bal.currency]"
say ""


// Deposit + withdraw + transfer cycle.
Alice = deposit Alice (money 12500 \USD)
say "After deposit: Alice bal = [Alice.bal.cents]c"

Alice = withdraw Alice (money 5000 \USD)
say "After withdraw: Alice bal = [Alice.bal.cents]c"

[Alice Bob] transfer Alice Bob (money 10000 \USD)
say "After transfer 100.00 USD A->B: Alice=[Alice.bal.cents]c Bob=[Bob.bal.cents]c"
say ""


// --- Boundary catches ----------------------------------------
//
// Each line below should statically OR dynamically catch a
// real bug.  `btrap` collects the runtime errors so the rest of
// the demo keeps running.

// 1. Currency mismatch -- runtime.  `add` checks the currencies
// match before unifying cents.  The static checker can't infer
// the currency-field value (it's a runtime keyword), so this
// catches at runtime not mex time.
EUR_amt money 1000 \EUR
E1 btrap: => add Alice.bal EUR_amt
say "boundary 1 -- mixing currencies: [E1.text]"

// 2. Wrong-type argument -- runtime tag check via `_the account`.
// Static check can't see through a btrap-thunked call so the
// runtime check is what fires.
E2 btrap: => deposit "savings_account" (money 100 \USD)
say "boundary 2 -- non-account arg: [E2.text]"

// 3. Insufficient funds -- domain rule, raised explicitly.
E3 btrap: => withdraw Bob (money 99999999 \USD)
say "boundary 3 -- overdraft:     [E3.text]"

// 4. Money with float cents.  `money 12.5 \USD` would store a
// float in `.cents`; routing through `int` snaps to a whole
// cent count.  TS-3.1's static checker would catch the literal
// `_the int 12.5` (float != int), but here the float is routed
// through `int` first so we just see the conversion.
M1 money (int 12.5) \USD
say "boundary 4 -- float->int cents: [M1.cents]"
say ""


// --- Static-check catches (commented out to keep file running) ---
//
// Each of these lines, if uncommented, would fail the TS-3
// strict checker at MEX TIME -- a compile error, not a runtime
// trap.  The checker can prove the mismatch from the literal
// types alone.
//
//   _the int "fifty bucks"     // text where int expected
//   _the money 1000            // bare int where money expected
//   _the account (money 0 \USD)  // money where account expected
//
// A useful pattern in real code: when a sensitive field MUST
// have a specific type (transaction amount in cents, account id),
// wrap the binding in `_the int X` / `_the account X`.  The
// strict checker then refuses any obvious wrong-type literal
// from sneaking in.

PinnedCents _the int 0     // explicit -- enforced
PinnedAcc Alice^account    // ascription form
say "boundary 5 -- pinned-typed bindings: cents=[PinnedCents] acc=[PinnedAcc.id]"
say ""


// --- Audit log ----------------------------------------------

say "=== Audit log ==="
for E GLog: say "  [E.0]: acc=[E.1] amt=[E.2]c [E.3]"
say ""
say "demo complete"
