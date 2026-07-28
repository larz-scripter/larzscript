# Changelog

## 1.0.0
Pluggable settlement - Larzscript can now settle against a real ledger.
- Every `pay` and `subscribe` routes through a `Settlement` backend instead of
  moving money directly. The default settles in memory (byte-identical to 0.4),
  so all existing programs and tests are unchanged.
- `run(src, settlement=...)` plugs in your own backend. Subclass `Settlement`
  and override `authorize()` (decline a payment before any money moves - an
  on-chain balance check, a fiat funds-hold, KYC, fraud rules) and `record()`
  (persist/broadcast a settled movement). Or wire callbacks with
  `CallbackSettlement(on_authorize=..., on_record=...)` - no subclass needed.
- A declined payment raises `SettlementError` and never partially settles.
- New public API: `Settlement`, `CallbackSettlement`, `SettlementError`.
  Works identically on both backends. See `examples/settlement_backend.py`.
  53 tests.

## 0.4.0
Lists, for-loops, and a few stdlib builtins.
- List literals `[1, 2, 3]`, indexing `xs[i]` (bounds-checked), and `for x in xs { }`
  loops (desugared to let/while/index, so both backends get them free).
- Builtins: `range(n)`, `push(list, item)`; `len` now works on lists too.
- Works on money too: `for item in basket { pay item from customer to shop }`.
- Both backends verified in agreement over list/loop programs. 46 tests.

## 0.3.0
A compiler backend - Larzscript now runs two ways.
- New bytecode compiler (compiler.py) + stack VM (vm.py): `run(src, backend="vm")`
  compiles the AST to bytecode and runs it, versus the default tree-walking
  interpreter (backend="tree").
- Shared runtime (runtime.py) - both backends use the same values and semantics,
  so a program produces IDENTICAL output, ledger, balances and gas either way
  (verified by parity tests over 15 programs).
- CLI `--vm` flag (`larzscript --vm file.lz`). compile_source() exposed. 38 tests.

## 0.2.0
Subscriptions, a `has` operator, and a CLI/REPL.
- paywall NAME = $X / period to WALLET; subscribe WALLET to PAYWALL; WALLET has PAYWALL.
- CLI: larzscript <file.lz> / larzscript repl / python -m larzscript. New len builtin.

## 0.1.0
First release - a working money-native language (tree-walking interpreter).
- First-class Money, wallets, pay ... from ... to ... (ledger), require guardrails,
  fn ... gas N metered functions. Lexer + parser + interpreter, pure Python, zero deps.
