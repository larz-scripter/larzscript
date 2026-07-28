# -*- coding: utf-8 -*-
"""GemVault checkout gateway for Larzscript credit settlement.

Wires :class:`larzscript.adapters.credit.CreditSettlement` to the GemVault
payment hub, so a wallet's credit can be topped up with a real card / PayPal /
crypto checkout. Pure stdlib (urllib + hmac), and **no secret is hardcoded** -
the base URL, app name, merchant token and webhook secret are all passed in.

    from larzscript.adapters.gemvault import GemVaultSettlement

    settle = GemVaultSettlement(
        base_url="https://gemvault.example",
        app_name="larzscript",
        mkt_token=os.environ["GV_MKT_TOKEN"],
        webhook_secret=os.environ["GV_WEBHOOK_SECRET"],
        return_url="https://myapp.example/paid")

    url = settle.checkout("customer", 50)     # hosted checkout the buyer pays

Then, in your webhook endpoint (GemVault POSTs the confirmed payment):

    settle.handle_webhook(request.headers, request.get_data())  # credits wallet

GemVault protocol (observed from the estate's live integrations):
  * card   -> POST {base}/api/mkt/dodo/checkout       {app,uid,amount,email,return_url} -> {checkout_url}
  * paypal -> POST {base}/api/mkt/paypal/create-order {app,uid,amount_usd,return_url,cancel_url} -> {approve_url}
  * webhook (GemVault -> you): {cl_email:<uid>, usd_amount, tx_hash, coin},
    signed with header X-GV-Signature = HMAC-SHA256(webhook_secret, raw_body).
The ``uid`` we send is the checkout ``reference``; it round-trips back as
``cl_email`` so the payment maps to the wallet that is being topped up.
"""
import hashlib
import hmac
import json
import urllib.request

from larzscript.adapters.credit import CheckoutGateway, CreditSettlement


def _headers_get(headers, name):
    """Case-insensitively read a header from a dict or a mapping-like object."""
    if hasattr(headers, "get"):
        v = headers.get(name)
        if v is not None:
            return v
    low = name.lower()
    try:
        items = headers.items()
    except AttributeError:
        items = []
    for k, v in items:
        if str(k).lower() == low:
            return v
    return None


class GemVaultGateway(CheckoutGateway):
    """A live GemVault checkout client (card by default; PayPal optional)."""

    def __init__(self, base_url, app_name, mkt_token, webhook_secret,
                 method="dodo", return_url="", cancel_url="", timeout=25):
        self.base_url = base_url.rstrip("/")
        self.app_name = app_name
        self.mkt_token = mkt_token
        self.webhook_secret = webhook_secret
        self.method = method                 # "dodo" (card) or "paypal"
        self.return_url = return_url
        self.cancel_url = cancel_url or return_url
        self.timeout = timeout

    # -- HTTP -------------------------------------------------------------- #
    def _post(self, path, payload):
        data = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            self.base_url + path, data=data, method="POST",
            headers={"Content-Type": "application/json",
                     "X-MKT-Token": self.mkt_token})
        with urllib.request.urlopen(req, timeout=self.timeout) as resp:
            body = resp.read().decode("utf-8")
        return json.loads(body or "{}")

    # -- CheckoutGateway --------------------------------------------------- #
    def create_checkout(self, reference, cents, email=None):
        usd = cents / 100.0
        if self.method == "paypal":
            j = self._post("/api/mkt/paypal/create-order", {
                "app": self.app_name, "uid": reference, "amount_usd": usd,
                "return_url": self.return_url, "cancel_url": self.cancel_url})
            return j.get("approve_url")
        j = self._post("/api/mkt/dodo/checkout", {
            "app": self.app_name, "uid": reference, "amount": usd,
            "email": email or "", "return_url": self.return_url})
        return j.get("checkout_url")

    def verify_webhook(self, headers, raw_body):
        if isinstance(raw_body, str):
            raw_body = raw_body.encode("utf-8")
        sig = _headers_get(headers, "X-GV-Signature") or ""
        if not (self.webhook_secret and sig):
            return None
        expected = hmac.new(self.webhook_secret.encode(), raw_body,
                            hashlib.sha256).hexdigest()
        if not hmac.compare_digest(sig, expected):
            return None
        try:
            data = json.loads(raw_body.decode("utf-8") or "{}")
        except ValueError:
            return None
        reference = str(data.get("cl_email") or "").strip()
        try:
            cents = int(round(float(data.get("usd_amount") or 0) * 100))
        except (TypeError, ValueError):
            cents = 0
        txid = (data.get("tx_hash") or "").strip() or None
        if not reference:
            return None
        return {"reference": reference, "cents": cents, "txid": txid}


class GemVaultSettlement(CreditSettlement):
    """:class:`CreditSettlement` pre-wired to a :class:`GemVaultGateway`."""

    def __init__(self, base_url, app_name, mkt_token, webhook_secret,
                 method="dodo", return_url="", cancel_url="", balances=None):
        gateway = GemVaultGateway(base_url, app_name, mkt_token, webhook_secret,
                                  method=method, return_url=return_url,
                                  cancel_url=cancel_url)
        CreditSettlement.__init__(self, gateway=gateway, balances=balances)


class MockGemVaultGateway(CheckoutGateway):
    """An offline stand-in for :class:`GemVaultGateway` - same wire behaviour
    (including the X-GV-Signature HMAC) but no network, for demos and tests.
    Call :meth:`sign_webhook` to produce the headers+body GemVault would POST.
    """

    def __init__(self, webhook_secret="test-secret", base_url="https://mock.gv"):
        self.webhook_secret = webhook_secret
        self.base_url = base_url.rstrip("/")
        self.created = {}                   # reference -> cents

    def create_checkout(self, reference, cents, email=None):
        self.created[reference] = cents
        return "%s/pay/%s" % (self.base_url, reference)

    def sign_webhook(self, reference, cents=None, txid="tx_mock"):
        """Build the signed (headers, raw_body) GemVault would send on payment."""
        if cents is None:
            cents = self.created.get(reference, 0)
        body = json.dumps({"cl_email": reference, "usd_amount": cents / 100.0,
                           "tx_hash": txid, "coin": "DODO"}).encode("utf-8")
        sig = hmac.new(self.webhook_secret.encode(), body,
                       hashlib.sha256).hexdigest()
        return {"X-GV-Signature": sig}, body

    def verify_webhook(self, headers, raw_body):
        if isinstance(raw_body, str):
            raw_body = raw_body.encode("utf-8")
        sig = _headers_get(headers, "X-GV-Signature") or ""
        expected = hmac.new(self.webhook_secret.encode(), raw_body,
                            hashlib.sha256).hexdigest()
        if not (sig and hmac.compare_digest(sig, expected)):
            return None
        data = json.loads(raw_body.decode("utf-8") or "{}")
        return {"reference": str(data.get("cl_email") or ""),
                "cents": int(round(float(data.get("usd_amount") or 0) * 100)),
                "txid": (data.get("tx_hash") or "").strip() or None}
