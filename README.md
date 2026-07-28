# larzscript

**A money-native programming language — where payments, wallets, revenue splits
and metering are *keywords*, not a library you bolt on.**

Every language can *call* a payments API. Larzscript makes money part of the
grammar: `$3.50` is a value, a `wallet` is a type, `pay ... from ... to ...` moves
funds and records a ledger, `require` is a guardrail the runtime enforces, and
functions can be **gas-metered** so untrusted or billable code can't run past its
budget. It's the "build software people pay for" idea, turned into syntax.

Pure Python, zero dependencies. Tokenizer → recursive-descent parser →
tree-walking interpreter — all readable.

## Install

```bash
pip install larzscript
```

## A taste

```python
from larzscript import run

program = '''
    wallet customer = $20.00
    wallet platform
    wallet creator
    price premium = $9.00

    fn buy_premium(buyer) {
        require buyer.balance >= premium, "not enough funds"
        pay premium from buyer to platform
        pay premium * 0.8 from platform to creator   # revenue split, in the language
    }

    buy_premium(customer)
    print("creator earned:", creator.balance)        # $7.20
'''

result = run(program)
result.output              # 'creator earned: $7.20'
result.ledger              # the recorded transactions
result.get("creator").balance   # Money(720) -> '$7.20'
```

### Gas-metered execution (fails closed)

```python
src = 'fn scan() gas 500 { return 1 }\nscan()\nscan()\nscan()'
run(src, gas=1200)         # raises OutOfGasError on the 3rd call
```

### Subscriptions & paywalls (new in 0.2)

```
wallet customer = $20.00
wallet platform
paywall pro = $9.00 / month to platform

subscribe customer to pro          # charges $9, pays platform, records the ledger, grants access

fn premium(user) {
    require user has pro, "subscribe to Pro first"   # the runtime enforces the gate
    return "secret content"
}
```

### Run it

```bash
larzscript program.lz     # run a file
larzscript repl           # interactive REPL (state persists across lines)
python -m larzscript program.lz
```

## Lists & loops (new in 0.4)

```
let basket = [$3.50, $12.00, $4.25]     # lists (money too)
for item in basket {                    # for-loops
    pay item from customer to shop
}
print(len(basket), basket[0])           # 3  $3.50
```

Plus `range(n)` and `push(list, item)`.

## Two backends: interpret or compile (new in 0.3)

The same program runs two ways, producing identical results:

```python
from larzscript import run
run(program)                    # tree-walking interpreter (default)
run(program, backend="vm")      # compile to bytecode, run on a stack VM
```

`larzscript --vm file.lz` runs the compiled path. Both share one runtime, so
output, ledger, balances and gas are identical whichever you pick.

## Pluggable settlement (new in 1.0)

This is what makes Larzscript more than a toy. A program never moves money
directly — `pay` and `subscribe` ask the program's **settlement backend** to do
it. The default settles in memory (byte-identical to before), but you can plug
in a real one and run the *same, unchanged program* against it:

```python
from larzscript import run, Settlement, CallbackSettlement, SettlementError

# Quick: attach callbacks — no subclass.
run(program, settlement=CallbackSettlement(
    on_record=lambda txn, kind, memo: audit_log.append(txn)))

# Real: authorize against external state, then broadcast what settles.
class OnChainSettlement(Settlement):
    def authorize(self, src, dst, amount, kind):
        return chain.balance(src.name) >= amount.cents   # decline before money moves
    def record(self, txn, kind, memo):
        chain.submit(txn.src, txn.dst, txn.amount.cents)  # broadcast the settled tx

run(program, settlement=OnChainSettlement())
```

- `authorize()` runs **before** any debit — a declined payment raises
  `SettlementError` and **never partially settles**. This is where an on-chain
  balance check, a fiat funds-hold, KYC, or fraud rules live.
- `record()` runs **after** a successful move — persist or broadcast it to a real
  ledger (a LarzChain transaction, a GemVault fiat charge, an audit log).
- Works identically on both backends. See `examples/settlement_backend.py`.

## Why it exists — the selling point

A new general-purpose language competing with Python or Rust is dead on arrival.
Larzscript wins by owning a niche no mainstream language does: **money as a
first-class concern.**

- **Money-native primitives** — `price`, `wallet`, `pay`, `require`, and metered
  `fn ... gas N`. The runtime *guarantees* an endpoint can't run without payment,
  a function can't exceed its gas, and a split is enforced — because they're
  language semantics, not code you might forget.
- **Pluggable settlement** *(shipped in 1.0)* — `pay`/`subscribe` run through a
  swappable `Settlement` backend that can authorize and record every movement.
  Point it at a payments hub for fiat/card or a chain for on-chain settlement and
  the same program settles for real — the program itself doesn't change.
- **Gas-metering built in** — makes "pay-per-execution" and "run untrusted code
  safely" natural, and makes Larzscript a natural smart-contract language that
  *also* speaks fiat.

## Language at a glance

```
let x = 5                         # variables
fn add(a, b) { return a + b }     # functions, recursion, closures
if x > 3 { ... } else { ... }     # control flow
while x > 0 { x = x - 1 }

price coffee   = $3.50            # money is a value type ($ = cents)
wallet shop                       # a balance you can credit/debit
pay coffee from customer to shop  # moves money, records a ledger entry
require shop.balance >= $10, "min"   # a guardrail the runtime enforces
fn analyze(img) gas 500 { ... }   # metered: each call costs gas
```

## Roadmap

- **v0.1** — tree-walking interpreter, in-memory settlement.
- **v0.2** — subscriptions/paywalls, the `has` operator, and a CLI + REPL.
- **v0.3** — a bytecode compiler + stack VM (the compiled path).
- **v0.4** — lists, for-loops, and stdlib builtins.
- **v1.0 (this release)** — **pluggable settlement**: `pay`/`subscribe` settle
  through a swappable backend that can authorize against, and record to, a real
  ledger (fiat gateway / on-chain), with the program unchanged.
- **Next** — a native standalone build already runs `.lz` with zero Python (see
  `native/`); on-chain settlement adapters that deploy `.lz` as contracts which
  also speak fiat are the ongoing direction.

## Learn to code with Larz

Part of the [Larz stack](https://github.com/larz-scripter) — see the
[Learn to Code platform](https://larzos.com/learn/). Built on the same ideas as
the stack's `larzcalc` (a tiny interpreter) and gas-metered VM.

## Tests

```bash
python -m unittest discover -s tests -v   # 53 tests
```

## License

MIT (c) larz-scripter
