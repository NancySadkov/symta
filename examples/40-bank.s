// 40-bank.s -- TS-3: bank-account demo exercising the type system.
//
// A miniature bank-account library where the type system enforces
// the invariants a real ledger would actually want:
//
//   * `money` is a tagged value (amount + currency).  You can't
//     accidentally add USD to EUR -- the operations refuse to
//     unify them at runtime, and any static path that mixes the
//     two trips the strict checker at mex time.
//
//   * `deposit` / `withdraw` / `transfer` take `account` and
//     `money` by *typed parameter* -- the `Acc^account` syntax
//     at the def site is both a runtime tag check at entry AND
//     a static narrow over the body, so `Acc.bal` is known to
//     be a money inside without ceremony.
//
//   * The amount inside `money` is `int` (whole cents).  No
//     float drift.  TS-3.1 catches `_the int 12.5` at compile
//     time; mixing a text where cents is expected fails too.
//
// Run:  symta -f examples/40-bank.s


// --- Domain types --------------------------------------------
//
// `type T A B ...: f1!A f2!B ...` declares T and auto-emits a
// positional constructor + per-field readers + the runtime tag
// check (`_the T x` / `x^T` work for any user type).

type money Cents Curr: cents!Cents currency!Curr
type account Id Name Bal: id!Id name!Name bal!Bal


// --- Currency safety -----------------------------------------
//
// `Acc^account` and `Amt^money` are the boundary -- runtime tag
// check at entry, static narrow over the body.  Once inside,
// `Acc.bal` and `Amt.cents` are typed by the constructor's
// field declarations.  No `_the` peppered through the body --
// the parameter ascription IS the contract.

add A^money B^money =
  less A.currency >< B.currency:
    bad "currency mismatch: [A.currency] vs [B.currency]"
  money A.cents+B.cents A.currency

sub A^money B^money =
  less A.currency >< B.currency:
    bad "currency mismatch: [A.currency] vs [B.currency]"
  money A.cents-B.cents A.currency

money_lt A^money B^money =
  less A.currency >< B.currency:
    bad "currency mismatch: [A.currency] vs [B.currency]"
  A.cents < B.cents


// --- Audit log -----------------------------------------------
//
// `Kind` is a tagged keyword (`\withdraw`, `\transfer`) so a
// typo at the call site is a compile-time `undefined variable`
// rather than a silently-wrong audit entry.  Boundary check
// is on the call data, not the kind.

GLog []

record_txn Kind Acc^account Amt^money =
  push [Kind Acc.id Amt.cents Amt.currency] GLog


// --- Operations ----------------------------------------------
//
// Same shape: type the boundary at the def, body is straight
// business logic.  Strict-mode TS-3.13 case-result unification
// means the body's case-expression participates in TS-3.8
// fn-return registration when arms agree.

deposit Acc^account Amt^money =
  NewBal add Acc.bal Amt
  record_txn \deposit Acc Amt
  account Acc.id Acc.name NewBal

withdraw Acc^account Amt^money =
  when money_lt Acc.bal Amt:
    bad "insufficient: [Acc.bal.cents] < [Amt.cents]"
  NewBal sub Acc.bal Amt
  record_txn \withdraw Acc Amt
  account Acc.id Acc.name NewBal

transfer Src^account Dst^account Amt^money =
  Src2 withdraw Src Amt
  Dst2 deposit Dst Amt
  record_txn \transfer Src Amt
  [Src2 Dst2]


// --- Demo ----------------------------------------------------

say "=== Bank-account type-system demo ==="
say ""

Alice account 1 "Alice" (money 50000 \USD)
Bob   account 2 "Bob"   (money 30000 \USD)

say "Alice: id=[Alice.id] bal=[Alice.bal.cents]c [Alice.bal.currency]"
say "Bob:   id=[Bob.id] bal=[Bob.bal.cents]c [Bob.bal.currency]"
say ""


Alice = deposit Alice (money 12500 \USD)
say "After deposit: Alice bal = [Alice.bal.cents]c"

Alice = withdraw Alice (money 5000 \USD)
say "After withdraw: Alice bal = [Alice.bal.cents]c"

[Alice Bob] transfer Alice Bob (money 10000 \USD)
say "After transfer 100.00 USD A->B: Alice=[Alice.bal.cents]c Bob=[Bob.bal.cents]c"
say ""


// --- Boundary catches ----------------------------------------
//
// Each line below catches a real bug.  `btrap` collects runtime
// errors so the rest of the demo keeps running.

EUR_amt money 1000 \EUR
E1 btrap: => add Alice.bal EUR_amt
say "boundary 1 -- mixing currencies: [E1.text]"

E2 btrap: => deposit "savings_account" (money 100 \USD)
say "boundary 2 -- non-account arg:   [E2.text]"

E3 btrap: => withdraw Bob (money 99999999 \USD)
say "boundary 3 -- overdraft:         [E3.text]"

E4 btrap: => add Alice.bal (money Alice.bal.cents Alice.bal.currency).cents
say "boundary 4 -- non-money arg:     [E4.text]"

say ""


// --- Static-check catches (commented out to keep file running) ---
//
// Each line below, if uncommented, fails the strict checker at
// MEX TIME -- a compile error, not a runtime trap:
//
//   _the int "fifty bucks"          // text where int expected
//   _the money 1000                 // bare int where money expected
//   _the account (money 0 \USD)     // money where account expected
//
// Useful pattern: pin a sensitive binding with `^T`; obvious
// wrong-type literals can't sneak in.

PinnedCents 0^int
PinnedAcc Alice^account
say "boundary 5 -- pinned bindings:   cents=[PinnedCents] acc=[PinnedAcc.id]"
say ""


// --- Audit log ----------------------------------------------

say "=== Audit log ==="
for E GLog: say "  [E.0]: acc=[E.1] amt=[E.2]c [E.3]"
say ""
say "demo complete"
