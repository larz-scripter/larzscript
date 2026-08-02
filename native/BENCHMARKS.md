# Benchmarks

Real, measured numbers — not a guess, not marketing. Every result below was
produced by actually running the commands shown, on this machine, on this
date. Re-run them yourself; the scripts are inline.

**Machine:** 8-core aarch64 (ARM), Linux. **Larzscript:** native v1.26.0,
built with `cc -O2 -o larzscript native/larzscript.c`. **Reference:** Python
3.6.9 (`python3`), the same box. Wall-clock time via `time`, single runs (not
a statistical study across many trials) — treat the exact figures as
ballpark, the *relative* story as the real finding.

## The honest headline

**The tree-walking interpreter is not built for raw speed - it's built for
zero-build-step scripting.** For pure CPU-bound computation it's slower than
Python. **`--emit-c` is one extra command and changes that completely** -
compiled Larzscript beats Python by double digits on every task tested here.
If you're prototyping, run the interpreter. If you need the numbers, compile.

## Method

Three tasks, each run three ways: the Larzscript interpreter (default),
Larzscript compiled (`larzscript --emit-c file.lz` piped into `cc -O2`), and
Python3 as a familiar reference point.

```bash
larzscript program.lz                      # interpreted
larzscript --emit-c program.lz > program.c && cc -O2 -o program program.c && ./program   # compiled
python3 program.py                         # reference
```

### 1. `fib(30)` — recursive, function-call-heavy

```
fn fib(n) { if n < 2 { return n } return fib(n - 1) + fib(n - 2) }
print(fib(30))
```

| Runner | Time | vs interpreted | vs Python |
|---|---|---|---|
| Larzscript, interpreted | 8.464s | 1x | 7.1x slower |
| Python 3.6.9 | 1.200s | 7.1x faster | — |
| **Larzscript, compiled** | **0.072s** | **117.6x faster** | **16.7x faster** |

### 2. Sum 1..20,000,000 — tight numeric loop

```
let total = 0
let i = 0
while i < 20000000 { total = total + i; i = i + 1 }
print(total)
```

| Runner | Time | vs interpreted | vs Python |
|---|---|---|---|
| Larzscript, interpreted | 20.220s | 1x | 2.0x slower |
| Python 3.6.9 | 10.022s | 2.0x faster | — |
| **Larzscript, compiled** | **0.553s** | **36.6x faster** | **18.1x faster** |

### 3. 200,000 real `pay()` transactions — the actual differentiator

```
wallet customer = $1000000.00
wallet shop
price item = $0.01
let i = 0
while i < 200000 { pay item from customer to shop; i = i + 1 }
print(shop.balance)   # $2000.00
```

No other mainstream language has a directly equivalent primitive to compare
against - this is the operation Larzscript exists to make a keyword instead
of a library call, so there's no fair Python translation to benchmark
against here.

| Runner | Time | Real transactions/sec |
|---|---|---|
| Larzscript, interpreted | 0.236s | ~847,000/sec |
| **Larzscript, compiled** | **0.015s** | **~13,300,000/sec** |

## A real, known gap this surfaced

Printing a `wallet` value directly (`print(shop)`) in `--emit-c` output
currently prints `nil` instead of the formatted `<wallet shop: $2000.00>` the
interpreter shows - the wallet-object `__str__` formatter isn't wired into
the C codegen yet. `shop.balance` (a plain `Money` value) prints correctly
either way, and the underlying money arithmetic is identical and correct in
both modes (verified: `--emit-c` output matches the interpreter's real ledger
state exactly, just the top-level wallet print is unformatted). Filed as a
real, narrow compiler gap, not hidden - `--emit-c` is documented as covering
"a general-purpose subset" of the language, and this is exactly the kind of
thing that subset boundary means in practice.

## Reproduce this yourself

```bash
git clone https://github.com/larz-scripter/larzscript
cd larzscript
cc -O2 -o larzscript native/larzscript.c
# then run any of the three programs above with `time`, interpreted and compiled
```
