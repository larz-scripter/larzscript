# -*- coding: utf-8 -*-
"""Settle a Larzscript program against fiat credit — checkout, no silent charges.

The same money-native program settles instantly against pre-funded credit; the
only place real money moves is a checkout the buyer completes. This demo uses a
mock GemVault gateway (offline, but with the real signed-webhook handshake) so
you can run it as-is:

    python3 examples/fiat_settlement.py

Swap MockGemVaultGateway for GemVaultSettlement(base_url=..., mkt_token=...,
webhook_secret=...) to sell real credit via card / PayPal / crypto.
"""
from larzscript import run
from larzscript.adapters.credit import CreditSettlement
from larzscript.adapters.gemvault import MockGemVaultGateway

PROGRAM = """
    wallet customer = $50.00
    wallet store
    wallet creator
    price plan = $12.00

    fn buy(buyer) {
        require buyer.balance >= plan, "not enough funds"
        pay plan from buyer to store
        pay plan * 0.25 from store to creator
    }

    buy(customer)
    print("in-program store:", store.balance)
    print("in-program creator:", creator.balance)
"""


def main():
    gateway = MockGemVaultGateway(webhook_secret="s3cr3t")
    settle = CreditSettlement(gateway=gateway)

    # 1. The buyer tops up $50 of credit through a real checkout.
    url = settle.checkout("customer", 50, email="buyer@example.com")
    print("1. checkout created (buyer pays this):", url)
    print("   credit so far:", settle.balances())         # nothing charged yet

    # 2. GemVault confirms the payment -> our webhook credits the wallet.
    ref = next(iter(settle.pending))
    headers, body = gateway.sign_webhook(ref)             # what GemVault POSTs
    applied = settle.handle_webhook(headers, body)        # verifies X-GV-Signature
    print("2. webhook applied:", applied, "-> credit:", settle.balances())

    # 3. The program now settles instantly against that credit. No charge here.
    print("3. running the program:")
    result = run(PROGRAM, settlement=settle)
    print("  ", result.output.replace("\n", "\n   "))
    print("   final credit:", settle.balances())

    assert settle.balances() == {"customer": 38.0, "store": 9.0, "creator": 3.0}
    print("\nOK - fiat credit settled the program; the only charge was the checkout.")


if __name__ == "__main__":
    main()
