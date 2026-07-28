# -*- coding: utf-8 -*-
"""Deploy a `.lz` program as a contract - a deterministic, settling state machine.

A subscription SaaS as a Larzscript contract: it has a treasury, a Pro paywall,
and functions users call. Every call is gas-metered, settles through a pluggable
backend (here fiat credit), and the whole contract state hashes to a commitment
you could anchor on-chain. Run:

    python3 examples/contract_saas.py
"""
from larzscript.contract import Contract
from larzscript.adapters.credit import CreditSettlement

SOURCE = """
    wallet treasury
    paywall pro = $9.00 / month to treasury
    let signups = 0

    fn join(user) gas 20 {
        require user.balance >= $9.00, "need at least $9.00 to join Pro"
        subscribe user to pro
        signups = signups + 1
        return signups
    }

    fn is_pro(user) {
        return user has pro
    }
"""


def main():
    # Fiat-credit settlement: each subscribe draws down purchased credit.
    settle = CreditSettlement(balances={"treasury": 0, "alice": 2000, "bob": 500})
    c = Contract(SOURCE, settlement=settle)

    alice = c.new_wallet("alice", "$20.00")
    bob = c.new_wallet("bob", "$5.00")

    print("deployed. state hash:", c.state_hash()[:16], "...")

    r = c.call("join", alice)
    print("alice joined -> signup #%d, gas %d, settled %d tx"
          % (r.value, r.gas_used, len(r.ledger)))
    print("  is_pro(alice):", c.call("is_pro", alice).value)
    print("  treasury balance:", c.balance("treasury"))
    print("  state hash now:  ", c.state_hash()[:16], "...")

    # Bob only has $5 - the require guardrail rejects him, nothing settles.
    try:
        c.call("join", bob)
    except Exception as e:
        print("bob rejected:", type(e).__name__, "-", e)

    # Determinism: a fresh deployment replaying the SAME setup and calls commits
    # to the SAME state hash - which is what makes it anchorable on-chain.
    c2 = Contract(SOURCE, settlement=CreditSettlement(
        balances={"treasury": 0, "alice": 2000, "bob": 500}))
    c2.new_wallet("alice", "$20.00")
    c2.new_wallet("bob", "$5.00")
    c2.call("join", c2.get("alice"))
    try:
        c2.call("join", c2.get("bob"))       # rejected, same as before
    except Exception:
        pass
    assert c2.state_hash() == c.state_hash(), "replay must reproduce the state"
    print("\nreplay reproduced the state hash exactly:", c2.state_hash()[:16], "...")
    print("OK - deterministic contract, real settlement, anchorable commitment.")


if __name__ == "__main__":
    main()
