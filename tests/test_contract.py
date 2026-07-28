# -*- coding: utf-8 -*-
"""Tests for larzscript.contract - deploying a .lz program as a state machine."""
import unittest

from larzscript import Money
from larzscript.contract import Contract, ContractError
from larzscript.adapters.credit import CreditSettlement
from larzscript.errors import OutOfGasError, RequireError

SUBSCRIPTION_CONTRACT = """
    wallet treasury
    paywall pro = $9.00 / month to treasury

    fn subscribe_user(user) gas 10 {
        subscribe user to pro
        return user has pro
    }

    fn is_subscribed(user) {
        return user has pro
    }
"""

VAULT_CONTRACT = """
    wallet vault
    let deposits = 0

    fn deposit(who) {
        pay $5.00 from who to vault
        deposits = deposits + 1
        return deposits
    }
"""


class TestContractBasics(unittest.TestCase):
    def test_deploy_and_call_moves_money(self):
        c = Contract(SUBSCRIPTION_CONTRACT)
        alice = c.new_wallet("alice", "$20.00")
        res = c.call("subscribe_user", alice)
        self.assertIs(res.value, True)
        self.assertEqual(c.balance("treasury"), Money(900))
        self.assertEqual(c.balance("alice"), Money(1100))
        self.assertEqual(len(res.ledger), 1)              # one settlement

    def test_state_persists_across_calls(self):
        c = Contract(VAULT_CONTRACT)
        a = c.new_wallet("a", "$50.00")
        self.assertEqual(c.call("deposit", a).value, 1)
        self.assertEqual(c.call("deposit", a).value, 2)   # `deposits` persisted
        self.assertEqual(c.balance("vault"), Money(1000))

    def test_unknown_function_is_an_error(self):
        c = Contract(VAULT_CONTRACT)
        with self.assertRaises(ContractError):
            c.call("nope")

    def test_money_string_parsing(self):
        c = Contract(VAULT_CONTRACT)
        w = c.new_wallet("w", "$12.34")
        self.assertEqual(w.balance, Money(1234))


class TestGasMetering(unittest.TestCase):
    def test_per_call_gas_budget_is_enforced(self):
        c = Contract(SUBSCRIPTION_CONTRACT, gas=5)         # fn needs gas 10
        alice = c.new_wallet("alice", "$20.00")
        with self.assertRaises(OutOfGasError):
            c.call("subscribe_user", alice)

    def test_gas_used_is_reported_per_call(self):
        c = Contract(SUBSCRIPTION_CONTRACT)
        alice = c.new_wallet("alice", "$20.00")
        res = c.call("subscribe_user", alice)
        self.assertEqual(res.gas_used, 10)


class TestDeterministicCommitment(unittest.TestCase):
    def _run(self):
        c = Contract(VAULT_CONTRACT)
        a = c.new_wallet("a", "$50.00")
        c.call("deposit", a)
        c.call("deposit", a)
        return c

    def test_same_calls_same_hash(self):
        self.assertEqual(self._run().state_hash(), self._run().state_hash())

    def test_state_hash_changes_with_state(self):
        c = self._run()
        h1 = c.state_hash()
        c.call("deposit", c.get("a"))
        self.assertNotEqual(h1, c.state_hash())

    def test_state_snapshot_contents(self):
        c = self._run()
        st = c.state()
        self.assertEqual(st["wallets"]["vault"], 1000)
        self.assertEqual(st["vars"]["deposits"], 2)


class TestSettlesThroughARail(unittest.TestCase):
    def test_calls_settle_through_the_credit_backend(self):
        settle = CreditSettlement(balances={"buyer": 5000, "vault": 0})
        c = Contract(VAULT_CONTRACT, settlement=settle)
        buyer = c.new_wallet("buyer", "$50.00")
        c.call("deposit", buyer)
        c.call("deposit", buyer)
        # the pluggable backend saw both settlements
        self.assertEqual(settle.credit_cents("vault"), 1000)
        self.assertEqual(settle.credit_cents("buyer"), 4000)

    def test_declined_settlement_propagates(self):
        settle = CreditSettlement(balances={"buyer": 300, "vault": 0})  # only $3
        c = Contract(VAULT_CONTRACT, settlement=settle)
        buyer = c.new_wallet("buyer", "$3.00")
        from larzscript.errors import SettlementError
        with self.assertRaises(SettlementError):
            c.call("deposit", buyer)                       # needs $5


if __name__ == "__main__":
    unittest.main()
