# -*- coding: utf-8 -*-
"""Shared runtime for Larzscript - values and semantics used by BOTH backends.

The tree-walking interpreter and the bytecode VM both use these value types
(Money, Wallet, Paywall, Transaction), the same truthiness/stringify rules, and
the same binary/unary operator semantics - which is what guarantees the two
backends produce identical results.
"""

from larzscript.errors import (LarzNameError, LarzTypeError, LarzRuntimeError,
                               MoneyError, SettlementError)

__all__ = ["Money", "Wallet", "Transaction", "Paywall", "Builtin", "Environment",
           "Settlement", "CallbackSettlement",
           "is_num", "truthy", "stringify", "binop", "unary", "index",
           "make_builtins"]


# --------------------------------------------------------------------------- #
#  Values
# --------------------------------------------------------------------------- #

class Money(object):
    """An amount of money, stored as an integer number of cents."""

    __slots__ = ("cents",)

    def __init__(self, cents):
        self.cents = int(round(cents))

    def __add__(self, other):
        return Money(self.cents + other.cents)

    def __sub__(self, other):
        return Money(self.cents - other.cents)

    def __eq__(self, other):
        return isinstance(other, Money) and self.cents == other.cents

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash(("Money", self.cents))

    def __str__(self):
        c = abs(self.cents)
        body = "$%d.%02d" % (c // 100, c % 100)
        return "-" + body if self.cents < 0 else body

    def __repr__(self):
        return "Money(%d)" % self.cents


class Wallet(object):
    """A named balance you can credit and debit (over-debiting raises)."""

    __slots__ = ("name", "balance")

    def __init__(self, name, balance=None):
        self.name = name
        self.balance = balance if balance is not None else Money(0)

    def credit(self, amount):
        self.balance = self.balance + amount

    def debit(self, amount):
        if amount.cents > self.balance.cents:
            raise MoneyError("wallet '%s' has insufficient funds: balance %s, needs %s"
                             % (self.name, self.balance, amount))
        self.balance = self.balance - amount

    def __repr__(self):
        return "Wallet(%r, %s)" % (self.name, self.balance)


class Paywall(object):
    """A subscription product: a price, a period, and the wallet that's paid."""

    __slots__ = ("name", "price", "period", "payee")

    def __init__(self, name, price, period, payee):
        self.name = name
        self.price = price
        self.period = period
        self.payee = payee

    def __repr__(self):
        return "Paywall(%r, %s / %s)" % (self.name, self.price, self.period)


class Transaction(object):
    """A recorded money movement between two wallets."""

    __slots__ = ("src", "dst", "amount")

    def __init__(self, src, dst, amount):
        self.src = src
        self.dst = dst
        self.amount = amount

    def __repr__(self):
        return "Transaction(%s -> %s: %s)" % (self.src, self.dst, self.amount)


class Builtin(object):
    """A built-in function (print, money, len)."""

    __slots__ = ("name", "fn")

    def __init__(self, name, fn):
        self.name = name
        self.fn = fn

    def __repr__(self):
        return "<builtin %s>" % self.name


# --------------------------------------------------------------------------- #
#  Settlement backend (the pluggable seam under `pay` / `subscribe`)
# --------------------------------------------------------------------------- #

class Settlement(object):
    """Mediates every money movement a program makes.

    This is the seam that makes Larzscript more than a toy: ``pay`` and
    ``subscribe`` don't move money directly, they ask the program's settlement
    backend to do it. The default backend settles *in memory* - it debits the
    source wallet, credits the destination, and returns a :class:`Transaction`
    for the ledger - which is exactly the historical behaviour.

    A real deployment plugs in its own backend by subclassing this (or using
    :class:`CallbackSettlement`) and overriding two hooks:

    * :meth:`authorize` - return ``False`` (or raise) to *decline* a payment
      before any money moves. This is where an on-chain balance check, a fiat
      gateway's funds hold, KYC, or fraud rules live.
    * :meth:`record` - called *after* a successful in-memory move, to persist
      or broadcast it to a real ledger (a LarzChain transaction, a GemVault
      fiat charge, an audit log).

    Because authorization happens before any debit, a declined payment leaves
    wallet balances untouched - there is never a partial settlement.
    """

    def authorize(self, src, dst, amount, kind):
        """Return True to allow this transfer. Override to gate on external
        state. ``kind`` is ``"pay"`` or ``"subscribe"``. Default: allow all."""
        return True

    def record(self, txn, kind, memo):
        """Called after a successful transfer. Override to persist/broadcast the
        movement to a real ledger. ``memo`` carries the paywall name for a
        subscription (else None). Default: do nothing (in-memory only)."""
        pass

    def transfer(self, src, dst, amount, src_label=None, dst_label=None,
                 kind="pay", memo=None):
        """Move ``amount`` (Money) from wallet ``src`` to wallet ``dst``.

        Returns the :class:`Transaction` to append to the ledger. The
        ``*_label`` names are how the wallets were referred to in the statement
        (so the ledger reads the way the source code did); they default to the
        wallets' own names. Raises :class:`SettlementError` if declined.
        """
        if not self.authorize(src, dst, amount, kind):
            raise SettlementError(
                "settlement declined: %s from '%s' to '%s'"
                % (amount, src_label if src_label is not None else src.name,
                   dst_label if dst_label is not None else dst.name))
        src.debit(amount)
        dst.credit(amount)
        txn = Transaction(src_label if src_label is not None else src.name,
                          dst_label if dst_label is not None else dst.name,
                          amount)
        self.record(txn, kind, memo)
        return txn


class CallbackSettlement(Settlement):
    """A settlement backend wired to plain callables - the quickest way to
    attach Larzscript to a real system without writing a subclass.

    ``on_authorize(src, dst, amount, kind) -> bool`` gates each payment (it may
    also raise its own error); ``on_record(txn, kind, memo)`` is called after a
    successful move. Either may be omitted.
    """

    __slots__ = ("_on_authorize", "_on_record")

    def __init__(self, on_authorize=None, on_record=None):
        self._on_authorize = on_authorize
        self._on_record = on_record

    def authorize(self, src, dst, amount, kind):
        if self._on_authorize is None:
            return True
        return bool(self._on_authorize(src, dst, amount, kind))

    def record(self, txn, kind, memo):
        if self._on_record is not None:
            self._on_record(txn, kind, memo)


# --------------------------------------------------------------------------- #
#  Environment (name -> value, with lexical parent chain)
# --------------------------------------------------------------------------- #

class Environment(object):
    def __init__(self, parent=None):
        self.vars = {}
        self.parent = parent

    def get(self, name):
        env = self
        while env is not None:
            if name in env.vars:
                return env.vars[name]
            env = env.parent
        raise LarzNameError("'%s' is not defined" % name)

    def assign(self, name, value):
        env = self
        while env is not None:
            if name in env.vars:
                env.vars[name] = value
                return
            env = env.parent
        raise LarzNameError("cannot assign to undefined '%s' (use 'let')" % name)

    def define(self, name, value):
        self.vars[name] = value


# --------------------------------------------------------------------------- #
#  Semantics (shared by both backends)
# --------------------------------------------------------------------------- #

def is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def truthy(v):
    if v is None:
        return False
    if isinstance(v, bool):
        return v
    if isinstance(v, Money):
        return v.cents != 0
    if is_num(v):
        return v != 0
    if isinstance(v, (str, list)):
        return len(v) != 0
    return True


def stringify(v):
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, Money):
        return str(v)
    if isinstance(v, Wallet):
        return "<wallet %s: %s>" % (v.name, v.balance)
    if isinstance(v, Paywall):
        return "<paywall %s: %s/%s>" % (v.name, v.price, v.period)
    if isinstance(v, list):
        return "[" + ", ".join(stringify(e) for e in v) + "]"
    return str(v)


def index(obj, idx):
    """Index into a list or string (0-based, bounds-checked)."""
    if isinstance(obj, (list, str)):
        if not is_num(idx) or int(idx) != idx:
            raise LarzTypeError("index must be a whole number")
        idx = int(idx)
        if idx < 0 or idx >= len(obj):
            raise LarzRuntimeError("index %d out of range (length %d)" % (idx, len(obj)))
        return obj[idx]
    raise LarzTypeError("cannot index %s" % stringify(obj))


def unary(op, value):
    if op == "not":
        return not truthy(value)
    if op == "-":
        if isinstance(value, Money):
            return Money(-value.cents)
        if is_num(value):
            return -value
        raise LarzTypeError("cannot negate %s" % stringify(value))
    raise LarzTypeError("unknown unary operator %r" % op)   # pragma: no cover


def binop(op, left, right):
    if op == "==":
        return left == right
    if op == "!=":
        return left != right

    both_money = isinstance(left, Money) and isinstance(right, Money)
    both_num = is_num(left) and is_num(right)
    both_str = isinstance(left, str) and isinstance(right, str)

    if op == "+":
        if both_money or both_num or both_str:
            return left + right
        raise LarzTypeError("cannot add %s and %s" % (stringify(left), stringify(right)))
    if op == "-":
        if both_money or both_num:
            return left - right
        raise LarzTypeError("cannot subtract %s from %s" % (stringify(right), stringify(left)))
    if op == "*":
        if isinstance(left, Money) and is_num(right):
            return Money(left.cents * right)
        if is_num(left) and isinstance(right, Money):
            return Money(right.cents * left)
        if both_num:
            return left * right
        raise LarzTypeError("cannot multiply %s and %s" % (stringify(left), stringify(right)))
    if op == "/":
        if isinstance(left, Money) and is_num(right):
            if right == 0:
                raise MoneyError("cannot divide money by zero")
            return Money(left.cents / right)
        if both_num:
            if right == 0:
                raise LarzRuntimeError("division by zero")
            return left / right
        raise LarzTypeError("cannot divide %s by %s" % (stringify(left), stringify(right)))
    if op == "%":
        if both_num:
            if right == 0:
                raise LarzRuntimeError("division by zero")
            return left % right
        raise LarzTypeError("cannot take %s %% %s" % (stringify(left), stringify(right)))

    if op in ("<", "<=", ">", ">="):
        lk, rk = _order_key(left), _order_key(right)
        if lk is None or rk is None or lk[0] != rk[0]:
            raise LarzTypeError("cannot compare %s and %s" % (stringify(left), stringify(right)))
        a, b = lk[1], rk[1]
        if op == "<":
            return a < b
        if op == "<=":
            return a <= b
        if op == ">":
            return a > b
        return a >= b
    raise LarzTypeError("unknown operator %r" % op)   # pragma: no cover


def _order_key(v):
    if isinstance(v, Money):
        return ("money", v.cents)
    if is_num(v):
        return ("num", v)
    if isinstance(v, str):
        return ("str", v)
    return None


# --------------------------------------------------------------------------- #
#  Built-in functions (shared by both backends; print writes to engine._out)
# --------------------------------------------------------------------------- #

def make_builtins(engine):
    """Return the {name: callable} builtins bound to an engine's output buffer."""

    def _print(*args):
        engine._out.append(" ".join(stringify(a) for a in args))
        return None

    def _money(dollars):
        if not is_num(dollars):
            raise LarzTypeError("money() expects a number of dollars")
        return Money(dollars * 100)

    def _len(value):
        if isinstance(value, (str, list)):
            return len(value)
        raise LarzTypeError("len() expects a string or list")

    def _push(lst, item):
        if not isinstance(lst, list):
            raise LarzTypeError("push() expects a list")
        lst.append(item)
        return None

    def _range(n):
        if not is_num(n):
            raise LarzTypeError("range() expects a number")
        return list(range(int(n)))

    return {"print": _print, "money": _money, "len": _len,
            "push": _push, "range": _range}
