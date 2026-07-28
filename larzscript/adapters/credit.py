# -*- coding: utf-8 -*-
"""Credit-based settlement — settle Larzscript payments against pre-funded credit.

This is the fiat analogue of on-chain settlement. There, a wallet is funded with
LARZ and `pay` draws it down; here, a wallet holds **credit** (in cents) bought
out of band via a real checkout (card / PayPal / crypto), and `pay` draws that
down. So a program's payments settle **instantly and never trigger a charge** -
the only place real money moves is the top-up checkout, which a human completes.

    from larzscript import run
    from larzscript.adapters.credit import CreditSettlement

    settle = CreditSettlement(balances={"customer": 5000})   # $50.00 credit
    run(program, settlement=settle)          # pays draw down the credit
    settle.balances()                        # {'customer': 38.0, 'store': 12.0}

To sell credit through a gateway, pass one (see :mod:`larzscript.adapters.gemvault`):

    settle = CreditSettlement(gateway=gw)
    url = settle.checkout("customer", 50)    # hosted checkout the buyer pays
    ...                                       # later, in your webhook handler:
    settle.handle_webhook(headers, raw_body) # verifies + credits the wallet
"""
import uuid

from larzscript.runtime import Settlement, Transaction, Money
from larzscript.errors import SettlementError


class CheckoutGateway(object):
    """The interface a checkout provider implements for :class:`CreditSettlement`.

    * ``create_checkout(reference, cents, email=None)`` -> a hosted checkout URL
      the buyer pays. ``reference`` is an opaque id that must round-trip back in
      the webhook so the payment can be matched to its checkout.
    * ``verify_webhook(headers, raw_body)`` -> ``{"reference", "cents", "txid"}``
      for a genuine, signature-verified payment, or ``None`` to reject it.
    """

    def create_checkout(self, reference, cents, email=None):   # pragma: no cover
        raise NotImplementedError

    def verify_webhook(self, headers, raw_body):               # pragma: no cover
        raise NotImplementedError


class CreditSettlement(Settlement):
    """A settlement backend that moves pre-funded credit between wallets."""

    def __init__(self, gateway=None, balances=None):
        self.gateway = gateway
        self._credit = {}                 # wallet name -> cents
        self.settled = []                 # Transactions settled (internal moves)
        self.pending = {}                 # reference -> {"wallet", "cents"}
        self.seen_txids = set()           # webhook idempotency
        for name, cents in (balances or {}).items():
            self._credit[name] = int(cents)

    # -- credit management ------------------------------------------------- #
    def credit_cents(self, name):
        return self._credit.get(name, 0)

    def credit(self, name, cents):
        """Add ``cents`` of credit to ``name`` (call this once a top-up clears)."""
        self._credit[name] = self.credit_cents(name) + int(cents)
        return self._credit[name]

    def balances(self):
        """A {name: dollars} snapshot of every wallet's credit."""
        return {n: c / 100.0 for n, c in self._credit.items()}

    # -- top-up (the only place a real charge happens) --------------------- #
    def checkout(self, name, usd, email=None):
        """Create a real gateway checkout to buy ``usd`` of credit for ``name``.

        Returns the hosted checkout URL the buyer pays. Nothing is charged until
        they complete it and the gateway confirms via :meth:`handle_webhook`.
        """
        if self.gateway is None:
            raise SettlementError("no checkout gateway configured")
        cents = int(round(float(usd) * 100))
        reference = uuid.uuid4().hex
        url = self.gateway.create_checkout(reference, cents, email=email)
        if not url:
            raise SettlementError("gateway did not return a checkout URL")
        self.pending[reference] = {"wallet": name, "cents": cents}
        return url

    def handle_webhook(self, headers, raw_body):
        """Verify a gateway webhook and credit the wallet. Call from your HTTP
        handler. Returns True if a payment was newly applied, False otherwise
        (bad signature, unknown/duplicate payment)."""
        if self.gateway is None:
            raise SettlementError("no checkout gateway configured")
        info = self.gateway.verify_webhook(headers, raw_body)
        if not info:
            return False
        txid = info.get("txid")
        if txid and txid in self.seen_txids:
            return False                  # already processed (idempotent)
        return self.confirm(info.get("reference"),
                            cents=info.get("cents"), txid=txid)

    def confirm(self, reference, cents=None, txid=None):
        """Mark a pending checkout paid and credit its wallet. Usually reached
        via :meth:`handle_webhook`, but callable directly (e.g. in tests)."""
        pend = self.pending.pop(reference, None)
        if pend is None:
            return False
        if txid:
            self.seen_txids.add(txid)
        self.credit(pend["wallet"], cents if cents is not None else pend["cents"])
        return True

    # -- the settlement seam ----------------------------------------------- #
    def transfer(self, src, dst, amount, src_label=None, dst_label=None,
                 kind="pay", memo=None):
        # Authorize against the payer's credit identity (its declared name),
        # not the pay-site label (e.g. a function parameter).
        have = self.credit_cents(src.name)
        if have < amount.cents:
            raise SettlementError(
                "settlement declined: '%s' has %s credit, needs %s"
                % (src.name, Money(have), amount))
        self._credit[src.name] = have - amount.cents
        self._credit[dst.name] = self.credit_cents(dst.name) + amount.cents
        # keep the in-program wallet balances in step with the credit ledger
        src.debit(amount)
        dst.credit(amount)
        txn = Transaction(src_label if src_label is not None else src.name,
                          dst_label if dst_label is not None else dst.name,
                          amount)
        self.settled.append(txn)
        return txn
