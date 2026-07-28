# Changelog

## 0.2.0
Subscriptions, a `has` operator, and a CLI/REPL.
- paywall NAME = $X / period to WALLET - declare a subscription product.
- subscribe WALLET to PAYWALL - charge the subscriber, pay the payee, record the
  ledger, and grant access.
- WALLET has PAYWALL - a boolean the runtime evaluates from the subscription
  registry (great inside `require ... has ...` gates).
- CLI: `larzscript <file.lz>` and `larzscript repl` (also `python -m larzscript`).
- New `len` builtin. 32 tests + a subscription example.

## 0.1.0
First release - a working money-native language (tree-walking interpreter).
- First-class Money ($1.50 = cents), wallets, pay ... from ... to ... (ledger),
  require guardrails, fn ... gas N metered functions (fail closed).
- let/assign, if/else, while, functions + recursion + closures, print/money.
- Lexer + recursive-descent parser + interpreter, pure Python, zero deps.
