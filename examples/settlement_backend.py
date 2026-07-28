# -*- coding: utf-8 -*-
"""Pluggable settlement (Larzscript v1.0).

A Larzscript program never moves money directly - `pay` and `subscribe` ask the
program's *settlement backend* to do it. The default backend settles in memory.
This example shows how you plug in a real one: the same `.lz` program, run three
ways, without changing a single line of the program.

Run:  python3 examples/settlement_backend.py
"""
from larzscript import run, Settlement, CallbackSettlement, SettlementError

# The SAME program every time. Notice it knows nothing about "backends".
PROGRAM = """
    wallet customer = $50.00
    wallet store

    price plan   = $12.00
    price top_up = $8.00

    pay plan   from customer to store
    pay top_up from customer to store

    print("customer left:", customer.balance)
    print("store took:", store.balance)
"""


# --------------------------------------------------------------------------- #
# 1. Default: settle in memory (the historical behaviour).
# --------------------------------------------------------------------------- #
print("=== 1. in-memory (default) ===")
r = run(PROGRAM)
print(r.output)
print("ledger:", [(t.src, t.dst, str(t.amount)) for t in r.ledger])


# --------------------------------------------------------------------------- #
# 2. Attach an audit log with a callback - no subclassing needed.
# --------------------------------------------------------------------------- #
print("\n=== 2. audit log via CallbackSettlement ===")
audit = []
run(PROGRAM, settlement=CallbackSettlement(
    on_record=lambda txn, kind, memo: audit.append(
        "%s: %s -> %s %s" % (kind, txn.src, txn.dst, txn.amount))))
for line in audit:
    print("  AUDIT", line)


# --------------------------------------------------------------------------- #
# 3. A real gateway adapter: authorize against an external balance and
#    "broadcast" each settled payment. This is exactly the shape a LarzChain
#    on-chain adapter or a GemVault fiat adapter takes.
# --------------------------------------------------------------------------- #
print("\n=== 3. gateway adapter (authorize + broadcast) ===")


class GatewaySettlement(Settlement):
    """Pretends to be a real payment rail. It holds the *true* off-program
    balance of each wallet and refuses any payment it can't cover, then
    'broadcasts' the ones it settles."""

    def __init__(self, balances):
        self.external = dict(balances)      # e.g. on-chain / bank balances
        self.broadcasts = []

    def authorize(self, src, dst, amount, kind):
        return self.external.get(src.name, 0) >= amount.cents

    def record(self, txn, kind, memo):
        self.external[txn.src] -= txn.amount.cents
        self.external[txn.dst] = self.external.get(txn.dst, 0) + txn.amount.cents
        ref = "0x%06x" % (len(self.broadcasts) + 1)
        self.broadcasts.append(ref)
        print("  BROADCAST %s  %s -> %s  %s" % (ref, txn.src, txn.dst, txn.amount))


gw = GatewaySettlement({"customer": 1500, "store": 0})   # only $15.00 on-chain
try:
    run(PROGRAM, settlement=gw)
except SettlementError as e:
    # $12 clears, then $8 is declined (only $3 left externally) -> the program
    # stops with the guardrail, and NO partial settlement happened.
    print("  DECLINED:", e)
print("  external balances after:", gw.external)
