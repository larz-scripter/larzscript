# larzscript

**A real, standalone programming language — general-purpose, and the first with
money as a first-class citizen.** Payments, wallets, revenue splits and metering
are *keywords*, not a library you bolt on.

Larzscript is its own language, implemented in C and shipped as a single
**statically-linked binary with zero dependencies** (no Python, no runtime, no
pip). You write `.lz` files and run them — just like any other language.

> **The official Larzscript is the native standalone** (`native/`). The Python
> package under `larzscript/` is now a **legacy reference implementation**, kept
> for study — new work targets the native binary so there is one language, not two.

## Get it & run it

**One-line install** (Linux x86_64 / ARM64):

```bash
curl -fsSL https://raw.githubusercontent.com/larz-scripter/larzscript/main/install.sh | sh
```

**Windows** (PowerShell) — installs `larzscript.exe` and adds it to your PATH,
so `larzscript` works in any new cmd/PowerShell window, just like Python:

```powershell
irm https://raw.githubusercontent.com/larz-scripter/larzscript/main/install.ps1 | iex
```

Or grab a binary from [Releases](https://github.com/larz-scripter/larzscript/releases),
or build it — one C file, no dependencies:

```bash
cc -O2 -o larzscript native/larzscript.c     # or: make -C native
./larzscript program.lz                        # run a file
./larzscript -e "print(1 + 2)"                 # a one-liner
./larzscript repl                              # interactive (multi-line) REPL
./larzscript fmt program.lz                    # canonical auto-formatter
./larzscript tools/larzdoc.lz module.lz        # generate Markdown API docs
```

**Docs & site:** [larz-scripter.github.io/larzscript](https://larz-scripter.github.io/larzscript/) · [language reference](native/LANGUAGE.md) · [browser playground](https://larzos.com/larzscript/)

## Packages

Larzscript has a package manager (`larzpkg`) that installs libraries into
`~/.larzscript/lib`, where `import` finds them:

```bash
larzscript ~/.larzscript/larzpkg.lz install json
larzscript ~/.larzscript/larzpkg.lz list
```

```
import "http" as http
import "json" as json
let repo = json.parse(http.get("https://api.github.com/repos/larz-scripter/larzscript"))
print(repo["full_name"])                       # larz-scripter/larzscript
```

Available packages:

| Package | What it does |
|---|---|
| **json** | parse & stringify JSON (pure Larzscript) |
| **http** | HTTP client (get/post/status/download, via curl) |
| **csv** | parse & write CSV, handles quoted fields |
| **args** | command-line argument parsing |
| **color** | ANSI terminal colors |
| **test** | a tiny test framework (assert/report) |
| **time** | time & duration helpers (`humanize`, stopwatch) |
| **string** | string helpers (center, wrap, reverse, snake, ...) |
| **random** | a small seeded PRNG (deterministic) |
| **fs** | filesystem helpers (read/write/ls/copy/...) |
| **base64** | base64 encode/decode (pure Larzscript) |
| **cli** | build CLI tools (subcommands + help) |
| **html** | build HTML safely (auto-escaping) |
| **table** | render tabular data as an aligned ASCII table |
| **log** | leveled logging with timestamps |
| **mathx** | small math helpers (mean, fib, primes) |
| **greet** | a tiny example package |

They compose — e.g. `json.parse(http.get(url))`, or CSV → list-of-dicts → JSON.
Add yours by PR-ing a line to [`packages/registry.txt`](packages/registry.txt).

**Larzscript compiles to native code.** `larzscript --emit-c program.lz` emits C; `tools/larzc program.lz` gcc's it into a native binary that runs ~130x faster than the interpreter (a general-purpose subset today). This is the path toward the LarzOS kernel being written in Larzscript itself.

## LarzOS — an OS written in Larzscript

There's an operating system taking shape in [`os/`](os/): its init, shell
(`larzsh`) and utilities are written **entirely in Larzscript**, and it's
money-native at the core — compute is metered in a built-in wallet that fails
closed. Boot the Stage 0 userland (on Linux) today:

```bash
larzscript os/init.lz     # provisions the system and drops you into larzsh
```

The [roadmap](os/ROADMAP.md) lifts the same Larzscript userland onto a
freestanding kernel and, ultimately, real laptops and servers.

## A taste — general-purpose *and* money-native

```
# It's a real general-purpose language.
fn fib(n) {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}
let words = "the cat the dog the bird".split(" ")
let counts = {}
for w in words { counts[w] = counts.get(w, 0) + 1 }
print(f"fib(10) = {fib(10)}, counts = {counts}")

# And money is part of the grammar.
wallet customer = $20.00
wallet platform
wallet creator
price premium = $9.00

fn buy(buyer) {
    require buyer.balance >= premium, "not enough funds"
    pay premium from buyer to platform
    pay premium * 0.8 from platform to creator     # revenue split, in the language
}
buy(customer)
print(f"creator earned: {creator.balance}")        # creator earned: $7.20
```

See **[native/README.md](native/README.md)** for the full language reference.

### Gas-metered execution (fails closed)

```
fn scan() gas 500 { return 1 }
scan()
scan()
scan()          # OutOfGasError: out of gas calling 'scan'  (with gas budget 1200)
```

### Subscriptions & paywalls

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

### Ready-made rails (`larzscript.adapters`)

Two backends ship ready to use (opt-in imports; core stays zero-dependency):

- **Fiat / credit** — `CreditSettlement` settles payments instantly against
  pre-funded per-wallet credit; the only real charge is a top-up **checkout a
  human completes**, so a program never triggers a silent charge.
  `GemVaultSettlement` sells that credit via the GemVault hub (card / PayPal /
  crypto), verifying the signed webhook. See `examples/fiat_settlement.py`.

  ```python
  from larzscript.adapters.credit import CreditSettlement
  settle = CreditSettlement(balances={"customer": 5000})   # $50.00 of credit
  run(program, settlement=settle)                           # pays draw it down
  ```

- **On-chain** — `LarzChainSettlement` (in the
  [LarzChain](https://github.com/larz-scripter/larzchain) package) settles every
  payment as a real signed LARZ transaction.

## Contracts (new in 1.2)

Deploy a `.lz` program as a persistent, callable **contract**. Because the
language is deterministic and I/O-free, a contract's state is a pure function of
its ordered calls — so it's replayable and hashes to a commitment you can anchor
on-chain, while every payment settles through your chosen rail.

```python
from larzscript.contract import Contract

c = Contract('''
    wallet treasury
    paywall pro = $9.00 / month to treasury
    fn join(user) gas 20 {
        require user.balance >= $9.00, "need $9.00 to join"
        subscribe user to pro
    }
''')

alice = c.new_wallet("alice", "$20.00")
c.call("join", alice)          # metered, settling call; state persists
c.balance("treasury")          # Money(900)
c.state_hash()                 # sha256 commitment — anchor it on-chain
```

Full *in-consensus* execution is a separate frontier; this gives deterministic
state + real settlement + an anchorable commitment with no consensus changes.

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

The **native standalone** (`native/`) is the official implementation and where
all development happens:

- **native v1.0** — grew from money-native core into a real general-purpose
  language: dictionaries, element assignment, `break`/`continue`, a full standard
  library, line-numbered errors.
- **native v1.1** — `try`/`catch`/`throw`, f-strings, slicing, lambdas +
  `map`/`filter`/`reduce`, and the `**` `//` `in` operators.
- **Next** — string interpolation extras, modules/`import`, a real allocator,
  and tooling (formatter, spec, editor support). Money-native primitives
  (`pay`/`require`/`paywall`/`subscribe`) remain first-class throughout.

### Legacy Python reference (`larzscript/`)

The original implementation was a pure-Python interpreter/VM plus settlement
adapters (in-memory, fiat via GemVault, on-chain via
[LarzChain](https://github.com/larz-scripter/larzchain)). It's **kept as a
reference** but is no longer the standard — the native binary is. Its docs and
tests live under `larzscript/` and `tests/`.

## Learn to code with Larz

Part of the [Larz stack](https://github.com/larz-scripter) — see the
[Learn to Code platform](https://larzos.com/learn/).

## Tests

```bash
sh native/run_tests.sh                    # the official native language: 14 tests
python -m unittest discover -s tests -v   # legacy Python reference: 53 tests
```

## License

MIT (c) larz-scripter
