# -*- coding: utf-8 -*-
"""Tests for the fiat/credit settlement adapters.

Covers the reusable CreditSettlement (pre-funded credit) and the GemVault
checkout flow via a MockGemVaultGateway that reproduces the real X-GV-Signature
HMAC handshake - so the checkout -> signed-webhook -> credit -> settle path is
exercised end to end, offline, on both backends.
"""
import json
import unittest

from larzscript import run
from larzscript.errors import SettlementError
from larzscript.adapters.credit import CreditSettlement
from larzscript.adapters.gemvault import (GemVaultGateway, GemVaultSettlement,
                                          MockGemVaultGateway)

BACKENDS = ("tree", "vm")


class TestCreditSettlement(unittest.TestCase):
    def test_prefunded_credit_settles_payments(self):
        for backend in BACKENDS:
            s = CreditSettlement(balances={"customer": 5000})   # $50.00
            run("wallet customer = $50.00\nwallet store\n"
                "pay $12.00 from customer to store\n",
                backend=backend, settlement=s)
            self.assertEqual(s.credit_cents("customer"), 3800)
            self.assertEqual(s.credit_cents("store"), 1200)
            self.assertEqual(s.balances(), {"customer": 38.0, "store": 12.0})

    def test_identity_not_label(self):
        for backend in BACKENDS:
            s = CreditSettlement(balances={"customer": 2000})
            run("wallet customer = $20.00\nwallet shop\n"
                "fn buy(buyer) { pay $5.00 from buyer to shop }\nbuy(customer)\n",
                backend=backend, settlement=s)
            self.assertEqual(s.credit_cents("customer"), 1500)
            self.assertEqual(s.credit_cents("shop"), 500)

    def test_insufficient_credit_is_declined_and_moves_nothing(self):
        for backend in BACKENDS:
            s = CreditSettlement(balances={"customer": 500})    # only $5.00
            with self.assertRaises(SettlementError):
                run("wallet customer = $5.00\nwallet shop\n"
                    "pay $9.00 from customer to shop\n",
                    backend=backend, settlement=s)
            self.assertEqual(s.credit_cents("customer"), 500)
            self.assertEqual(s.credit_cents("shop"), 0)

    def test_checkout_requires_a_gateway(self):
        with self.assertRaises(SettlementError):
            CreditSettlement().checkout("customer", 50)


class TestGemVaultCheckoutFlow(unittest.TestCase):
    def _settlement(self):
        gw = MockGemVaultGateway(webhook_secret="s3cr3t")
        return CreditSettlement(gateway=gw), gw

    def test_checkout_then_signed_webhook_credits_the_wallet(self):
        s, gw = self._settlement()
        url = s.checkout("customer", 50)                # buyer would pay this
        self.assertIn("/pay/", url)
        self.assertEqual(s.credit_cents("customer"), 0)  # nothing charged yet
        ref = next(iter(s.pending))
        headers, body = gw.sign_webhook(ref)            # GemVault confirms payment
        self.assertTrue(s.handle_webhook(headers, body))
        self.assertEqual(s.credit_cents("customer"), 5000)   # $50 credited
        # now the program settles against the purchased credit
        run("wallet customer = $50.00\nwallet store\n"
            "pay $30.00 from customer to store\n", settlement=s)
        self.assertEqual(s.balances(), {"customer": 20.0, "store": 30.0})

    def test_bad_signature_is_rejected(self):
        s, gw = self._settlement()
        s.checkout("customer", 50)
        ref = next(iter(s.pending))
        headers, body = gw.sign_webhook(ref)
        headers["X-GV-Signature"] = "0" * 64            # tamper
        self.assertFalse(s.handle_webhook(headers, body))
        self.assertEqual(s.credit_cents("customer"), 0)

    def test_duplicate_webhook_is_idempotent(self):
        s, gw = self._settlement()
        s.checkout("customer", 25)
        ref = next(iter(s.pending))
        headers, body = gw.sign_webhook(ref, txid="tx_abc")
        self.assertTrue(s.handle_webhook(headers, body))
        # re-deliver the same paid checkout: reference already consumed AND txid seen
        self.assertFalse(s.handle_webhook(headers, body))
        self.assertEqual(s.credit_cents("customer"), 2500)

    def test_real_gateway_verifies_its_own_signature(self):
        # The production GemVaultGateway must accept a correctly-signed webhook.
        gw = GemVaultGateway(base_url="https://gv.example", app_name="larzscript",
                             mkt_token="t", webhook_secret="whsec")
        import hashlib, hmac
        body = json.dumps({"cl_email": "ref123", "usd_amount": 12.0,
                           "tx_hash": "txX"}).encode()
        sig = hmac.new(b"whsec", body, hashlib.sha256).hexdigest()
        info = gw.verify_webhook({"X-GV-Signature": sig}, body)
        self.assertEqual(info, {"reference": "ref123", "cents": 1200, "txid": "txX"})
        # wrong secret -> rejected
        self.assertIsNone(gw.verify_webhook({"X-GV-Signature": "bad"}, body))

    def test_gemvault_settlement_wires_the_gateway(self):
        s = GemVaultSettlement(base_url="https://gv.example", app_name="larzscript",
                               mkt_token="t", webhook_secret="whsec",
                               balances={"customer": 1000})
        self.assertIsInstance(s.gateway, GemVaultGateway)
        self.assertEqual(s.credit_cents("customer"), 1000)


if __name__ == "__main__":
    unittest.main()
