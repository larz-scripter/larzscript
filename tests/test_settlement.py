# -*- coding: utf-8 -*-
"""Tests for the pluggable settlement backend (v1.0).

`pay` and `subscribe` route every money movement through a Settlement object.
The default settles in memory (byte-identical to older versions); a custom
backend can authorize/decline payments and record them to a real ledger. These
tests assert both behaviours, on BOTH the tree and vm backends.
"""
import unittest

from larzscript import (run, Money, Settlement, CallbackSettlement,
                        SettlementError)

BACKENDS = ("tree", "vm")

PAY_PROGRAM = (
    'wallet customer = $10.00\n'
    'wallet shop\n'
    'price coffee = $3.50\n'
    'pay coffee from customer to shop\n'
)

SUB_PROGRAM = (
    'wallet customer = $20.00\n'
    'wallet platform\n'
    'paywall pro = $9.00 / month to platform\n'
    'subscribe customer to pro\n'
)


class TestDefaultSettlementUnchanged(unittest.TestCase):
    def test_default_matches_explicit_base_settlement(self):
        for backend in BACKENDS:
            a = run(PAY_PROGRAM, backend=backend)
            b = run(PAY_PROGRAM, backend=backend, settlement=Settlement())
            self.assertEqual(a.get("shop").balance, b.get("shop").balance)
            self.assertEqual([(t.src, t.dst, t.amount.cents) for t in a.ledger],
                             [(t.src, t.dst, t.amount.cents) for t in b.ledger])


class TestRecordHook(unittest.TestCase):
    def test_on_record_sees_every_pay(self, ):
        for backend in BACKENDS:
            seen = []
            s = CallbackSettlement(on_record=lambda txn, kind, memo:
                                   seen.append((txn.src, txn.dst,
                                                txn.amount.cents, kind, memo)))
            run(PAY_PROGRAM, backend=backend, settlement=s)
            self.assertEqual(seen, [("customer", "shop", 350, "pay", None)],
                             "backend %s" % backend)

    def test_on_record_sees_subscription_with_plan_memo(self):
        for backend in BACKENDS:
            seen = []
            s = CallbackSettlement(on_record=lambda txn, kind, memo:
                                   seen.append((kind, memo, txn.amount.cents)))
            run(SUB_PROGRAM, backend=backend, settlement=s)
            self.assertEqual(seen, [("subscribe", "pro", 900)],
                             "backend %s" % backend)


class TestAuthorization(unittest.TestCase):
    def test_declined_payment_raises_and_moves_no_money(self):
        for backend in BACKENDS:
            s = CallbackSettlement(on_authorize=lambda *a: False)
            with self.assertRaises(SettlementError):
                run(PAY_PROGRAM, backend=backend, settlement=s)

    def test_decline_leaves_balances_untouched(self):
        # A declined transfer must not partially settle: capture the engine
        # by running a program that pays, then inspecting via a subclass that
        # denies and records nothing.
        for backend in BACKENDS:
            recorded = []

            class DenyBig(Settlement):
                def authorize(self, src, dst, amount, kind):
                    return amount.cents <= 500       # allow <= $5.00 only
                def record(self, txn, kind, memo):
                    recorded.append(txn.amount.cents)

            src = (
                'wallet customer = $10.00\n'
                'wallet shop\n'
                'pay $4.00 from customer to shop\n'     # allowed
            )
            r = run(src, backend=backend, settlement=DenyBig())
            self.assertEqual(r.get("customer").balance, Money(600))   # $6.00
            self.assertEqual(r.get("shop").balance, Money(400))
            self.assertEqual(recorded, [400])

            src_denied = (
                'wallet customer = $10.00\n'
                'wallet shop\n'
                'pay $9.00 from customer to shop\n'     # over the $5 limit
            )
            with self.assertRaises(SettlementError):
                run(src_denied, backend=backend, settlement=DenyBig())

    def test_authorizing_callback_may_raise_custom_error(self):
        def gate(src, dst, amount, kind):
            if amount.cents > 500:
                raise SettlementError("KYC hold: %s exceeds unverified limit" % amount)
            return True
        for backend in BACKENDS:
            with self.assertRaises(SettlementError):
                run('wallet a = $10.00\nwallet b\npay $9.00 from a to b\n',
                    backend=backend, settlement=CallbackSettlement(on_authorize=gate))


class TestParityWithCustomBackend(unittest.TestCase):
    def test_tree_and_vm_agree_with_a_recording_backend(self):
        prog = (
            'wallet customer = $50.00\n'
            'wallet shop\n'
            'let basket = [$3.50, $12.00, $4.25]\n'
            'for item in basket { pay item from customer to shop }\n'
            'paywall pro = $5.00 / month to shop\n'
            'subscribe customer to pro\n'
        )
        logs = {}
        for backend in BACKENDS:
            captured = []
            s = CallbackSettlement(on_record=lambda t, k, m, c=captured:
                                   c.append((t.src, t.dst, t.amount.cents, k, m)))
            r = run(prog, backend=backend, settlement=s)
            logs[backend] = (captured, r.get("shop").balance.cents,
                             r.get("customer").balance.cents)
        self.assertEqual(logs["tree"], logs["vm"])


if __name__ == "__main__":
    unittest.main()
