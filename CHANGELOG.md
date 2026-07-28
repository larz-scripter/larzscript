# Changelog

## 0.1.0
First release - a working money-native language (tree-walking interpreter).
- Values: numbers, strings, booleans, nil, and first-class Money ($1.50 = cents).
- Wallets with credit/debit; pay ... from ... to ... records a ledger; require
  guardrails; fn ... gas N metered functions that fail closed on an out-of-gas.
- let/assign, if/else, while, functions + recursion, closures, print/money builtins.
- Lexer + recursive-descent parser + interpreter, all pure Python, zero deps.
- run(source, gas=None) -> Interpreter (inspect .ledger, .output, .get(name),
  .gas_used). 25 tests + example .lz programs.
