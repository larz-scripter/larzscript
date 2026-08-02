# Legacy Python reference implementation

> **This describes `larzscript/`, the original pure-Python interpreter/VM.**
> It's kept for study, not the standard — [the native binary](README.md) is.
> Version markers below (0.3, 0.4, 1.0, 1.2, ...) are this package's own
> history and predate the native rewrite; they're unrelated to the native
> binary's version number.

## Two backends: interpret or compile

The same program runs two ways, producing identical results:

```python
from larzscript import run
run(program)                    # tree-walking interpreter (default)
run(program, backend="vm")      # compile to bytecode, run on a stack VM
```

Both share one runtime, so output, ledger, balances and gas are identical
whichever you pick.

## Pluggable settlement

A program never moves money directly — `pay` and `subscribe` ask the
program's **settlement backend** to do it. The default settles in memory
(byte-identical to before), but you can plug in a real one and run the *same,
unchanged program* against it:

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

## Contracts

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

## Tests

```bash
python -m unittest discover -s tests -v
```
