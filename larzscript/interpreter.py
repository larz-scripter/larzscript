# -*- coding: utf-8 -*-
"""The Larzscript tree-walking interpreter (the default backend).

Walks the AST and executes each node, using the shared :mod:`larzscript.runtime`
for values and semantics. Money, wallets, ``pay``, ``require``, subscriptions and
gas metering are handled here as node rules.
"""

from larzscript.runtime import (Money, Wallet, Paywall, Transaction, Builtin,
                                Environment, is_num, truthy, stringify,
                                binop, unary)
from larzscript.errors import (LarzTypeError, RequireError, OutOfGasError)

__all__ = ["Interpreter", "Money", "Wallet", "Transaction", "Paywall"]


class _Function(object):
    __slots__ = ("decl", "closure")

    def __init__(self, decl, closure):
        self.decl = decl
        self.closure = closure

    def __repr__(self):
        return "<fn %s>" % self.decl.name


class _Return(Exception):
    def __init__(self, value):
        self.value = value


class Interpreter(object):
    """Runs a Larzscript program by walking the tree. After :meth:`run`, inspect
    ``ledger``, ``output``, ``gas_used`` and ``.get(name)``."""

    def __init__(self, gas=None, output=None):
        self.globals = Environment()
        self.ledger = []
        self.subscriptions = set()
        self.gas = gas
        self.gas_used = 0
        self._out = output if output is not None else []
        self.globals.define("print", Builtin("print", self._print))
        self.globals.define("money", Builtin("money", self._money))
        self.globals.define("len", Builtin("len", self._len))

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
        if not is_num(dollars):
            raise LarzTypeError("money() expects a number of dollars")
        return Money(dollars * 100)

    def _len(self, value):
        if isinstance(value, str):
            return len(value)
        raise LarzTypeError("len() expects a string")

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
        src.debit(amount)
        dst.credit(amount)
        self.ledger.append(Transaction(node.src, node.dst, amount))

    def exec_Paywall(self, node, env):
        price = self.evaluate(node.amount, env)
        if not isinstance(price, Money):
            raise LarzTypeError("a paywall price must be money")
        env.define(node.name, Paywall(node.name, price, node.period, node.payee))

    def exec_Subscribe(self, node, env):
        wallet = env.get(node.who)
        plan = env.get(node.plan)
        if not isinstance(wallet, Wallet):
            raise LarzTypeError("can only subscribe a wallet")
        if not isinstance(plan, Paywall):
            raise LarzTypeError("can only subscribe to a paywall")
        payee = env.get(plan.payee)
        if not isinstance(payee, Wallet):
            raise LarzTypeError("paywall payee '%s' is not a wallet" % plan.payee)
        wallet.debit(plan.price)
        payee.credit(plan.price)
        self.ledger.append(Transaction(node.who, plan.payee, plan.price))
        self.subscriptions.add((wallet.name, plan.name))

    def exec_Require(self, node, env):
        if not truthy(self.evaluate(node.cond, env)):
            raise RequireError(node.message or "requirement not met")

    def exec_Fn(self, node, env):
        env.define(node.name, _Function(node, env))

    def exec_Return(self, node, env):
        raise _Return(self.evaluate(node.value, env) if node.value is not None else None)

    def exec_If(self, node, env):
        if truthy(self.evaluate(node.cond, env)):
            self.execute(node.then, env)
        elif node.orelse is not None:
            self.execute(node.orelse, env)

    def exec_While(self, node, env):
        while truthy(self.evaluate(node.cond, env)):
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
            return self.evaluate(node.right, env) if truthy(left) else left
        if node.op == "or":
            left = self.evaluate(node.left, env)
            return left if truthy(left) else self.evaluate(node.right, env)
        if node.op == "has":
            wallet = self.evaluate(node.left, env)
            plan = self.evaluate(node.right, env)
            if not isinstance(wallet, Wallet) or not isinstance(plan, Paywall):
                raise LarzTypeError("'has' needs a wallet and a paywall")
            return (wallet.name, plan.name) in self.subscriptions
        return binop(node.op, self.evaluate(node.left, env), self.evaluate(node.right, env))

    def eval_Unary(self, node, env):
        return unary(node.op, self.evaluate(node.operand, env))

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
        if isinstance(callee, Builtin):
            return callee.fn(*args)
        if isinstance(callee, _Function):
            decl = callee.decl
            if len(args) != len(decl.params):
                raise LarzTypeError("%s expects %d argument(s), got %d"
                                    % (decl.name, len(decl.params), len(args)))
            self.charge_gas(decl.gas, decl.name)
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

    def charge_gas(self, cost, name):
        if not cost:
            return
        self.gas_used += cost
        if self.gas is not None:
            if cost > self.gas:
                raise OutOfGasError("out of gas calling '%s' (needed %d, %d left)"
                                    % (name, cost, self.gas))
            self.gas -= cost
