# larzscript

**A real, standalone programming language — general-purpose, and the first with
money as a first-class citizen.** Payments, wallets, revenue splits and metering
are *keywords*, not a library you bolt on.

Larzscript is its own language, implemented in C and shipped as a single
**statically-linked binary with zero dependencies** (no Python, no runtime, no
pip). You write `.lz` files and run them — just like any other language.

## Get it & run it

**Linux / macOS** (one-line install, x86_64 or ARM64/Apple Silicon):

```bash
curl -fsSL https://raw.githubusercontent.com/larz-scripter/larzscript/main/install.sh | sh
```

**Windows** (PowerShell) — installs `larzscript.exe` and adds it to your PATH,
so `larzscript` works in any new cmd/PowerShell window, just like Python:

```powershell
irm https://raw.githubusercontent.com/larz-scripter/larzscript/main/install.ps1 | iex
```

Both installers put a single self-contained binary on your PATH - no Python,
no separate runtime, nothing else to install. Later, get the newest release
with the same command from any of the three platforms:

```bash
larzscript update
```

Or grab a binary directly from [Releases](https://github.com/larz-scripter/larzscript/releases)
(`larzscript-linux-x86_64`, `larzscript-linux-aarch64`, `larzscript-macos-x86_64`,
`larzscript-macos-arm64`, `larzscript-windows-x86_64.exe`), or build it from
source — one C file, no dependencies, on any platform with a C compiler:

```bash
cc -O2 -o larzscript native/larzscript.c     # or: make -C native
./larzscript program.lz                        # run a file
./larzscript -e "print(1 + 2)"                 # a one-liner
./larzscript repl                              # interactive (multi-line) REPL
./larzscript fmt program.lz                    # canonical auto-formatter
./larzscript tools/larzdoc.lz module.lz        # generate Markdown API docs
```

**Docs & site:** [larz-scripter.github.io/larzscript](https://larz-scripter.github.io/larzscript/) · [language reference](native/LANGUAGE.md) · [browser playground](https://larzos.com/larzscript/) · [cookbook](https://larzos.com/stack/cookbook/)

## Why it exists — the selling point

A new general-purpose language competing with Python or Rust is dead on arrival.
Larzscript wins by owning a niche no mainstream language does: **money as a
first-class concern.**

- **Money-native primitives** — `price`, `wallet`, `pay`, `require`, and metered
  `fn ... gas N`. The runtime *guarantees* a payment can't partially settle and
  a split is enforced — because they're language semantics, not code you might
  forget to write.
- **Paywalls and subscriptions as keywords** — `paywall`/`subscribe` charge,
  record a ledger entry, and grant access in one statement, enforced by
  `require ... has ...` at the call site.
- **Reads like English where it matters** — `unless x is at least $1.00 { ... }`,
  `for i from 1 to 3 { ... }` — natural-language guardrails around the money
  logic, not just symbols.
- **Compiles to real native code** — `--emit-c` emits portable C; a compiled
  program runs tens of times faster than the interpreter on CPU-bound work (see
  [benchmarks](native/BENCHMARKS.md) for real, measured numbers — not a guess).

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

# And it reads like English, not just symbols.
unless customer.balance is at least $1.00 {
    say "customer is running low"
}
for i from 1 to 3 { say "reminder " + str(i) }
```

*(Every example on this page was run against the current release before being
committed here - not illustrative pseudocode.)*

### Paywalls and subscriptions

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

### Gas-metered functions

```
fn scan() gas 500 { return 1 }
```

`gas N` annotates a function's cost and every call is tracked (`gas_used` in
the runtime). Real enforcement - a call failing closed once a budget is
exhausted - is wired up when Larzscript is embedded as a host language (this
is how [LarzOS](os/) meters compute at the kernel level, failing closed on a
program that overspends). The standalone CLI doesn't yet expose a way to set
an enforced budget from a plain script - tracked as an open interpreter gap,
not shipped as if it already works end to end.

### Two ways to run: interpret or compile

```bash
larzscript program.lz            # tree-walking interpreter (default) - instant, no build step
larzscript --emit-c program.lz   # emit portable C - compile it yourself for a fast native binary
```

Both share one runtime - output, ledger, balances and gas are identical
either way; `--emit-c` just skips re-parsing and re-walking the AST on every
run. (The legacy Python package has a separate bytecode-VM backend of its
own - see [LEGACY.md](LEGACY.md) - unrelated to this.)

## Packages — 160+ zero-dependency libraries

```bash
larzscript pkg install json
larzscript pkg list
```

```
import "http" as http
import "json" as json
let repo = json.parse(http.get("https://api.github.com/repos/larz-scripter/larzscript"))
print(repo["full_name"])                       # larz-scripter/larzscript
```

They compose — e.g. `json.parse(http.get(url))`, or a full ops-alerting
pipeline out of four separately-installable packages. Browse all of them,
with a real tested example on every page, at
**[larzos.com/stack/](https://larzos.com/stack/)** - or see 8 real multi-package
recipes solved end to end in the **[cookbook](https://larzos.com/stack/cookbook/)**.

A representative slice: `money`/`ledger`/`invoice`/`escrow` (payments),
`crypto`/`jwt`/`apikey`/`acl` (security), `http`/`webhook`/`dispatch`/`imap`
(networking, all built on `curl` - no bundled runtime), `neural`/`genetic`/`raytrace`
(from-scratch AI/graphics), `db`/`sql`/`cache` (data). **Publish your own** -
host it yourself, no write access to anything here needed:
`larzscript pkg publish <your-git-url>`. See
[`packages/PUBLISHING.md`](packages/PUBLISHING.md).

## Built with it

- **[larzscript-budget](https://github.com/larz-scripter/larzscript-budget)** -
  a real personal budget tracker CLI. Budget categories *are* `wallet`s; every
  expense is a real `pay`; every spending limit is a real `require` guardrail
  the runtime enforces, not application code you could forget to write. Real
  GitHub Actions CI, green.
- **[larzpulse](https://github.com/larz-scripter/larzpulse)** - a real HTTP
  uptime monitor CLI, and a showcase of *composing* the package ecosystem
  rather than writing a monolith: the whole tool is five single-purpose
  packages (`http`, `monitor`, `statuscard`, `email`, `smtp`) wired together,
  no bespoke alerting logic of its own. Real GitHub Actions CI, green.

(Building something real with Larzscript? Open a PR adding it here.)

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
fn analyze(img) gas 500 { ... }   # gas-annotated: see "Gas-metered functions" above
```

See **[native/LANGUAGE.md](native/LANGUAGE.md)** for the full language reference.

## Legacy Python reference

The original implementation was a pure-Python interpreter/VM plus a pluggable
settlement-backend framework (in-memory, fiat via GemVault, on-chain via
[LarzChain](https://github.com/larz-scripter/larzchain)) and a deterministic
contracts layer. It's **kept for study, not the standard** - the native binary
above is. Its docs, API and code examples now live in
**[LEGACY.md](LEGACY.md)**, so they don't get mixed up with the current
language's own version history.

## Learn to code with Larz

Part of the [Larz stack](https://github.com/larz-scripter) — see the
[Learn to Code platform](https://larzos.com/learn/).

## Tests

```bash
sh native/run_tests.sh                    # the official native language
python -m unittest discover -s tests -v   # legacy Python reference
```

## License

MIT (c) larz-scripter
