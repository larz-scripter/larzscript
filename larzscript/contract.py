# -*- coding: utf-8 -*-
"""Larzscript contracts - deploy a `.lz` program as a deterministic state machine.

A *contract* is a Larzscript program used as a persistent, callable object: the
top level deploys it (defines its wallets, prices, paywalls and functions), and
then external **calls** invoke its functions over time. Because Larzscript is
deterministic and I/O-free, a contract's state is a pure function of its ordered
calls - so it is **replayable** and its state hashes to a compact commitment you
can anchor on-chain.

This is the buildable core of "deploy `.lz` as a contract that also speaks
fiat": the contract's logic runs deterministically, every ``pay``/``subscribe``
settles through the pluggable settlement backend (on-chain LARZ *or* fiat
credit), and ``state_hash()`` is an anchorable commitment to its state.

    from larzscript.contract import Contract

    c = Contract('''
        wallet treasury
        paywall pro = $9.00 / month to treasury

        fn subscribe_user(user) {
            subscribe user to pro
            return user has pro
        }
    ''')

    alice = c.new_wallet("alice", "$20.00")     # give the contract an account
    c.call("subscribe_user", alice)             # a metered, settling call
    c.balance("treasury")                       # Money(900)
    c.state_hash()                              # sha256 commitment to the state

Full *in-consensus* execution (every node re-running the contract) is a separate
frontier; this gives deterministic state + real settlement + an anchorable
commitment without changing any chain's consensus.
"""
import hashlib
import json

from larzscript import parse
from larzscript.interpreter import Interpreter, _Function
from larzscript.runtime import Wallet, Money, Paywall, stringify
from larzscript.errors import LarzScriptError, LarzNameError


class ContractError(LarzScriptError):
    """A contract was called incorrectly (unknown function, bad target, ...)."""


class CallResult(object):
    """The effects of one contract call: return value, printed output, the
    ledger entries it settled, and the gas it burned."""

    __slots__ = ("value", "output", "ledger", "gas_used")

    def __init__(self, value, output, ledger, gas_used):
        self.value = value
        self.output = output              # list[str]
        self.ledger = ledger              # list[Transaction]
        self.gas_used = gas_used

    def __repr__(self):
        return ("CallResult(value=%s, gas_used=%d, settled=%d)"
                % (stringify(self.value), self.gas_used, len(self.ledger)))


class Contract(object):
    """A deployed Larzscript program you can call and commit to."""

    def __init__(self, source, gas=None, settlement=None):
        # Deployment runs the top level once (unmetered) to define the
        # contract's wallets, prices, paywalls and functions.
        self.interp = Interpreter(gas=None, settlement=settlement)
        self.interp.run(parse(source))
        self.default_gas = gas             # per-call gas budget (None = unlimited)
        self.calls = []                    # history: (fn_name, result) for replay/audit

    # -- accounts ---------------------------------------------------------- #
    def new_wallet(self, name, balance="$0.00"):
        """Create and register a wallet the contract can transact with, funded
        with ``balance`` (a Money or a "$X.YZ" string). Returns the Wallet."""
        bal = self._to_money(balance)
        w = Wallet(name, bal)
        self.interp.globals.define(name, w)
        return w

    def get(self, name):
        return self.interp.get(name)

    def balance(self, name):
        w = self.interp.get(name)
        if not isinstance(w, Wallet):
            raise ContractError("'%s' is not a wallet" % name)
        return w.balance

    def wallets(self):
        return {n: v for n, v in self.interp.globals.vars.items()
                if isinstance(v, Wallet)}

    # -- calling ----------------------------------------------------------- #
    def call(self, fn_name, *args, **kw):
        """Invoke exported function ``fn_name`` with ``args`` (Larzscript values
        or Wallet objects). ``gas=`` overrides the per-call budget. Returns a
        :class:`CallResult`; state changes persist across calls."""
        gas = kw.pop("gas", self.default_gas)
        if kw:
            raise TypeError("unexpected keyword arguments: %s" % ", ".join(kw))
        try:
            fn = self.interp.get(fn_name)
        except LarzNameError:
            raise ContractError("no contract function named '%s'" % fn_name)
        if not isinstance(fn, _Function):
            raise ContractError("'%s' is not a callable contract function" % fn_name)

        out0 = len(self.interp._out)
        led0 = len(self.interp.ledger)
        gas_before = self.interp.gas_used
        self.interp.gas = gas              # arm a fresh budget for this call
        value = self.interp._call(fn, list(args))

        result = CallResult(
            value,
            self.interp._out[out0:],
            self.interp.ledger[led0:],
            self.interp.gas_used - gas_before)
        self.calls.append((fn_name, result))
        return result

    # -- state & commitment ------------------------------------------------ #
    def state(self):
        """A canonical, JSON-serializable snapshot of the contract's mutable
        state: wallet balances (cents), scalar globals, and active subscriptions."""
        wallets, variables = {}, {}
        for name, v in self.interp.globals.vars.items():
            if isinstance(v, Wallet):
                wallets[name] = v.balance.cents
            elif isinstance(v, Money):
                variables[name] = {"money": v.cents}
            elif isinstance(v, bool):
                variables[name] = v
            elif isinstance(v, (int, float, str)):
                variables[name] = v
            # functions, builtins and paywalls are deployment code, not state
        subs = sorted([list(s) for s in self.interp.subscriptions])
        return {"wallets": wallets, "vars": variables, "subscriptions": subs}

    def state_hash(self):
        """A sha256 commitment to :meth:`state` - stable across runs, so it can
        be anchored on-chain as proof of the contract's current state."""
        blob = json.dumps(self.state(), sort_keys=True, separators=(",", ":"))
        return hashlib.sha256(blob.encode("utf-8")).hexdigest()

    # -- helpers ----------------------------------------------------------- #
    @staticmethod
    def _to_money(value):
        if isinstance(value, Money):
            return value
        if isinstance(value, str):
            s = value.strip().lstrip("$")
            neg = s.startswith("-")
            s = s.lstrip("-")
            if "." in s:
                whole, frac = s.split(".", 1)
                frac = (frac + "00")[:2]
            else:
                whole, frac = s, "00"
            cents = int(whole or 0) * 100 + int(frac or 0)
            return Money(-cents if neg else cents)
        raise ContractError("expected Money or a \"$X.YZ\" string, got %r" % (value,))
