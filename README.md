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

## Why it exists — the selling point

A new general-purpose language competing with Python or Rust is dead on arrival.
Larzscript wins by owning a niche no mainstream language does: **money as a
first-class concern.**

- **Money-native primitives** — `price`, `wallet`, `pay`, `require`, and metered
  `fn ... gas N`. The runtime *guarantees* an endpoint can't run without payment,
  a function can't exceed its gas, and a split is enforced — because they're
  language semantics, not code you might forget.
- **Pluggable settlement** — the in-memory ledger implements a
  credit/debit/record interface. Swap in a real backend (a payments hub for
  fiat/card, or a chain for on-chain settlement) and the same program settles for
  real.
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

- **v0.1 (this release)** — tree-walking interpreter, in-memory settlement.
- **v0.2** — subscriptions/`paywall` and periods, more built-ins, a REPL + CLI.
- **v1.0** — compile to bytecode for a gas-metered VM, and deploy `.lz` programs
  as on-chain contracts.

## Learn to code with Larz

Part of the [Larz stack](https://github.com/larz-scripter) — see the
[Learn to Code platform](https://larzos.com/learn/). Built on the same ideas as
the stack's `larzcalc` (a tiny interpreter) and gas-metered VM.

## Tests

```bash
python -m unittest discover -s tests -v   # 25 tests
```

## License

MIT (c) larz-scripter
