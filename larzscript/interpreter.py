# -*- coding: utf-8 -*-
"""The Larzscript interpreter - a tree-walking evaluator with money as a value.

The money-native ideas live here: :class:`Money` (a first-class value in cents),
:class:`Wallet` (a balance you credit/debit), the ``pay`` statement recording a
:class:`Transaction` in a ledger, ``require`` as a guardrail, and gas metering on
functions. Settlement is in-memory here; a real backend (GemVault / LarzChain)
would implement the same credit/debit/record interface.
"""

from larzscript.errors import (LarzNameError, LarzTypeError, MoneyError,
                               RequireError, OutOfGasError)

__all__ = ["Interpreter", "Money", "Wallet", "Transaction"]


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
    """A named balance you can credit and debit (debiting too much raises)."""

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


class Transaction(object):
    """A recorded money movement between two wallets."""

    __slots__ = ("src", "dst", "amount")

    def __init__(self, src, dst, amount):
        self.src = src
        self.dst = dst
        self.amount = amount

    def __repr__(self):
        return "Transaction(%s -> %s: %s)" % (self.src, self.dst, self.amount)


class _Function(object):
    __slots__ = ("decl", "closure")

    def __init__(self, decl, closure):
        self.decl = decl
        self.closure = closure


class _Builtin(object):
    __slots__ = ("name", "fn")

    def __init__(self, name, fn):
        self.name = name
        self.fn = fn


class _Return(Exception):
    def __init__(self, value):
        self.value = value


# --------------------------------------------------------------------------- #
#  Environment
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
#  Helpers
# --------------------------------------------------------------------------- #

def _is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _truthy(v):
    if v is None:
        return False
    if isinstance(v, bool):
        return v
    if isinstance(v, Money):
        return v.cents != 0
    if _is_num(v):
        return v != 0
    if isinstance(v, str):
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
    if isinstance(v, _Function):
        return "<fn %s>" % v.decl.name
    if isinstance(v, _Builtin):
        return "<builtin %s>" % v.name
    return str(v)


# --------------------------------------------------------------------------- #
#  Interpreter
# --------------------------------------------------------------------------- #

class Interpreter(object):
    """Runs a Larzscript program. After :meth:`run`, inspect ``ledger``,
    ``output`` and ``globals``."""

    def __init__(self, gas=None, output=None):
        self.globals = Environment()
        self.ledger = []
        self.gas = gas               # None = unlimited
        self.gas_used = 0
        self._out = output if output is not None else []
        self.globals.define("print", _Builtin("print", self._print))
        self.globals.define("money", _Builtin("money", self._money))

    # -- public --
    def run(self, program):
        for stmt in program.body:
            self.execute(stmt, self.globals)
        return self

    def get(self, name):
        return self.globals.get(name)

    @property
    def output(self):
        return "\n".join(self._out)

    # -- builtins --
    def _print(self, *args):
        self._out.append(" ".join(stringify(a) for a in args))
        return None

    def _money(self, dollars):
        if not _is_num(dollars):
            raise LarzTypeError("money() expects a number of dollars")
        return Money(dollars * 100)

    # -- dispatch --
    def execute(self, node, env):
        return getattr(self, "exec_" + type(node).__name__)(node, env)

    def evaluate(self, node, env):
        return getattr(self, "eval_" + type(node).__name__)(node, env)

    # -- statements --
    def exec_Let(self, node, env):
        env.define(node.name, self.evaluate(node.value, env))

    def exec_Assign(self, node, env):
        env.assign(node.name, self.evaluate(node.value, env))

    def exec_Price(self, node, env):
        value = self.evaluate(node.value, env)
        if not isinstance(value, Money):
            raise LarzTypeError("a price must be money, got %s" % stringify(value))
        env.define(node.name, value)

    def exec_WalletDecl(self, node, env):
        balance = self.evaluate(node.value, env) if node.value is not None else Money(0)
        if not isinstance(balance, Money):
            raise LarzTypeError("a wallet balance must be money")
        env.define(node.name, Wallet(node.name, balance))

    def exec_Pay(self, node, env):
        amount = self.evaluate(node.amount, env)
        if not isinstance(amount, Money):
            raise LarzTypeError("you can only pay money, got %s" % stringify(amount))
        src = env.get(node.src)
        dst = env.get(node.dst)
        if not isinstance(src, Wallet) or not isinstance(dst, Wallet):
            raise LarzTypeError("pay requires two wallets")
        src.debit(amount)            # raises MoneyError if insufficient
        dst.credit(amount)
        self.ledger.append(Transaction(node.src, node.dst, amount))

    def exec_Require(self, node, env):
        if not _truthy(self.evaluate(node.cond, env)):
            raise RequireError(node.message or "requirement not met")

    def exec_Fn(self, node, env):
        env.define(node.name, _Function(node, env))

    def exec_Return(self, node, env):
        raise _Return(self.evaluate(node.value, env) if node.value is not None else None)

    def exec_If(self, node, env):
        if _truthy(self.evaluate(node.cond, env)):
            self.execute(node.then, env)
        elif node.orelse is not None:
            self.execute(node.orelse, env)

    def exec_While(self, node, env):
        while _truthy(self.evaluate(node.cond, env)):
            self.execute(node.body, env)

    def exec_Block(self, node, env):
        child = Environment(env)
        for stmt in node.body:
            self.execute(stmt, child)

    def exec_ExprStmt(self, node, env):
        self.evaluate(node.expr, env)

    # -- expressions --
    def eval_Num(self, node, env):
        return node.value

    def eval_MoneyLit(self, node, env):
        return Money(node.cents)

    def eval_Str(self, node, env):
        return node.value

    def eval_Bool(self, node, env):
        return node.value

    def eval_Nil(self, node, env):
        return None

    def eval_Name(self, node, env):
        return env.get(node.id)

    def eval_Binary(self, node, env):
        if node.op == "and":
            left = self.evaluate(node.left, env)
            return self.evaluate(node.right, env) if _truthy(left) else left
        if node.op == "or":
            left = self.evaluate(node.left, env)
            return left if _truthy(left) else self.evaluate(node.right, env)
        return self._binop(node.op, self.evaluate(node.left, env),
                           self.evaluate(node.right, env))

    def eval_Unary(self, node, env):
        value = self.evaluate(node.operand, env)
        if node.op == "not":
            return not _truthy(value)
        if node.op == "-":
            if isinstance(value, Money):
                return Money(-value.cents)
            if _is_num(value):
                return -value
            raise LarzTypeError("cannot negate %s" % stringify(value))

    def eval_Call(self, node, env):
        callee = self.evaluate(node.callee, env)
        args = [self.evaluate(a, env) for a in node.args]
        return self._call(callee, args)

    def eval_Get(self, node, env):
        obj = self.evaluate(node.obj, env)
        if isinstance(obj, Wallet):
            if node.name == "balance":
                return obj.balance
            if node.name == "name":
                return obj.name
            raise LarzTypeError("a wallet has no property '%s'" % node.name)
        raise LarzTypeError("cannot read '%s' of %s" % (node.name, stringify(obj)))

    def eval_MethodCall(self, node, env):
        obj = self.evaluate(node.obj, env)
        args = [self.evaluate(a, env) for a in node.args]
        if isinstance(obj, Wallet):
            if node.name in ("credit", "debit"):
                if len(args) != 1 or not isinstance(args[0], Money):
                    raise LarzTypeError("wallet.%s expects one money argument" % node.name)
                getattr(obj, node.name)(args[0])
                return None
            raise LarzTypeError("a wallet has no method '%s'" % node.name)
        raise LarzTypeError("cannot call method on %s" % stringify(obj))

    # -- machinery --
    def _call(self, callee, args):
        if isinstance(callee, _Builtin):
            return callee.fn(*args)
        if isinstance(callee, _Function):
            decl = callee.decl
            if len(args) != len(decl.params):
                raise LarzTypeError("%s expects %d argument(s), got %d"
                                    % (decl.name, len(decl.params), len(args)))
            self._charge_gas(decl.gas, decl.name)
            call_env = Environment(callee.closure)
            for param, arg in zip(decl.params, args):
                call_env.define(param, arg)
            try:
                for stmt in decl.body.body:
                    self.execute(stmt, call_env)
            except _Return as ret:
                return ret.value
            return None
        raise LarzTypeError("%s is not callable" % stringify(callee))

    def _charge_gas(self, cost, name):
        if not cost:
            return
        self.gas_used += cost
        if self.gas is not None:
            if cost > self.gas:
                raise OutOfGasError("out of gas calling '%s' (needed %d, %d left)"
                                    % (name, cost, self.gas))
            self.gas -= cost

    def _binop(self, op, left, right):
        if op == "==":
            return left == right
        if op == "!=":
            return left != right

        both_money = isinstance(left, Money) and isinstance(right, Money)
        both_num = _is_num(left) and _is_num(right)
        both_str = isinstance(left, str) and isinstance(right, str)

        if op == "+":
            if both_money:
                return left + right
            if both_num:
                return left + right
            if both_str:
                return left + right
            raise LarzTypeError("cannot add %s and %s" % (stringify(left), stringify(right)))
        if op == "-":
            if both_money:
                return left - right
            if both_num:
                return left - right
            raise LarzTypeError("cannot subtract %s from %s" % (stringify(right), stringify(left)))
        if op == "*":
            if isinstance(left, Money) and _is_num(right):
                return Money(left.cents * right)
            if _is_num(left) and isinstance(right, Money):
                return Money(right.cents * left)
            if both_num:
                return left * right
            raise LarzTypeError("cannot multiply %s and %s" % (stringify(left), stringify(right)))
        if op == "/":
            if isinstance(left, Money) and _is_num(right):
                if right == 0:
                    raise MoneyError("cannot divide money by zero")
                return Money(left.cents / right)
            if both_num:
                if right == 0:
                    raise LarzRuntimeError_divzero()
                return left / right
            raise LarzTypeError("cannot divide %s by %s" % (stringify(left), stringify(right)))
        if op == "%":
            if both_num:
                if right == 0:
                    raise LarzRuntimeError_divzero()
                return left % right
            raise LarzTypeError("cannot take %s %% %s" % (stringify(left), stringify(right)))

        # ordering comparisons
        if op in ("<", "<=", ">", ">="):
            lv, rv = _order_key(left), _order_key(right)
            if lv is None or rv is None or lv[0] != rv[0]:
                raise LarzTypeError("cannot compare %s and %s" % (stringify(left), stringify(right)))
            a, b = lv[1], rv[1]
            if op == "<":
                return a < b
            if op == "<=":
                return a <= b
            if op == ">":
                return a > b
            return a >= b
        raise LarzTypeError("unknown operator %r" % op)  # pragma: no cover


def _order_key(v):
    """A ``(kind, key)`` for ordering - money and numbers won't cross-compare."""
    if isinstance(v, Money):
        return ("money", v.cents)
    if _is_num(v):
        return ("num", v)
    if isinstance(v, str):
        return ("str", v)
    return None


def LarzRuntimeError_divzero():
    from larzscript.errors import LarzRuntimeError
    return LarzRuntimeError("division by zero")
