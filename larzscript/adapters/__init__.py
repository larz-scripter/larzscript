# -*- coding: utf-8 -*-
"""Optional settlement adapters for Larzscript.

The core language is settlement-agnostic: `pay`/`subscribe` go through a
:class:`larzscript.runtime.Settlement` backend. These adapters are ready-made
backends for real money rails. They are pure-stdlib (still zero third-party
dependencies) and are imported explicitly, so core larzscript stays untouched:

    from larzscript.adapters.credit import CreditSettlement
    from larzscript.adapters.gemvault import GemVaultSettlement

`CreditSettlement` is the reusable primitive: payments settle instantly against
pre-funded per-wallet credit, and credit is topped up out of band by a real
checkout a human completes. `GemVaultSettlement` wires that to the GemVault
payment hub (card / PayPal / crypto) without hardcoding any secret.
"""
