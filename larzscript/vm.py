# -*- coding: utf-8 -*-
"""The Larzscript bytecode VM - execute compiled chunks on a stack machine.

The "compiled" backend: runs the bytecode the compiler produced. It shares the
exact runtime (values, semantics, gas) with the tree-walking interpreter, so a
program produces identical output, ledger and gas whichever backend runs it.
"""

from larzscript import compiler as C
from larzscript.runtime import (Money, Wallet, Paywall, Transaction, Builtin,
                                Environment, is_num, truthy, stringify,
                                binop, unary, index, make_builtins)
from larzscript.errors import LarzTypeError, RequireError, OutOfGasError

__all__ = ["VM", "Closure"]


class Closure(object):
    """A compiled function bound to the environment it was defined in."""

    __slots__ = ("chunk", "env")

    def __init__(self, chunk, env):
        self.chunk = chunk
        self.env = env

    def __repr__(self):
        return "<fn %s>" % self.chunk.name


class VM(object):
    """Executes compiled Larzscript. After :meth:`run`, inspect ``ledger``,
    ``output``, ``gas_used`` and ``.get(name)``."""

    def __init__(self, gas=None, output=None):
        self.globals = Environment()
        self.ledger = []
        self.subscriptions = set()
        self.gas = gas
        self.gas_used = 0
        self._out = output if output is not None else []
        for name, fn in make_builtins(self).items():
            self.globals.define(name, Builtin(name, fn))

    # -- public --
    def run(self, chunk):
        self._exec(chunk, self.globals)
        return self

    def get(self, name):
        return self.globals.get(name)

    @property
    def output(self):
        return "\n".join(self._out)

    def _charge_gas(self, cost, name):
        if not cost:
            return
        self.gas_used += cost
        if self.gas is not None:
            if cost > self.gas:
                raise OutOfGasError("out of gas calling '%s' (needed %d, %d left)"
                                    % (name, cost, self.gas))
            self.gas -= cost

    # -- the execution loop for one chunk (one call frame) --
    def _exec(self, chunk, env):
        code = chunk.code
        stack = []
        push = stack.append
        pop = stack.pop
        ip = 0
        n = len(code)
        while ip < n:
            op, arg = code[ip]
            ip += 1

            if op == C.PUSH:
                push(arg)
            elif op == C.LOAD:
                push(env.get(arg))
            elif op == C.DEFINE:
                env.define(arg, pop())
            elif op == C.STORE:
                env.assign(arg, pop())
            elif op == C.POP:
                pop()
            elif op == C.BINARY:
                b = pop()
                a = pop()
                push(binop(arg, a, b))
            elif op == C.UNARY:
                push(unary(arg, pop()))
            elif op == C.HAS:
                plan = pop()
                wallet = pop()
                if not isinstance(wallet, Wallet) or not isinstance(plan, Paywall):
                    raise LarzTypeError("'has' needs a wallet and a paywall")
                push((wallet.name, plan.name) in self.subscriptions)
            elif op == C.JUMP:
                ip = arg
            elif op == C.JUMP_IF_FALSE:
                if not truthy(pop()):
                    ip = arg
            elif op == C.JUMP_IF_FALSE_KEEP:
                if not truthy(stack[-1]):
                    ip = arg
                else:
                    pop()
            elif op == C.JUMP_IF_TRUE_KEEP:
                if truthy(stack[-1]):
                    ip = arg
                else:
                    pop()
            elif op == C.ENTER_SCOPE:
                env = Environment(env)
            elif op == C.EXIT_SCOPE:
                env = env.parent
            elif op == C.MAKE_FN:
                push(Closure(arg, env))
            elif op == C.RETURN:
                return pop()
            elif op == C.CALL:
                argc = arg
                args = [pop() for _ in range(argc)]
                args.reverse()
                callee = pop()
                push(self._call(callee, args))
            elif op == C.BUILD_LIST:
                items = [pop() for _ in range(arg)]
                items.reverse()
                push(items)
            elif op == C.INDEX:
                i = pop()
                obj = pop()
                push(index(obj, i))
            elif op == C.GET_PROP:
                obj = pop()
                push(self._get_prop(obj, arg))
            elif op == C.CALL_METHOD:
                name, argc = arg
                margs = [pop() for _ in range(argc)]
                margs.reverse()
                obj = pop()
                push(self._call_method(obj, name, margs))
            elif op == C.DEF_WALLET:
                balance = pop()
                if not isinstance(balance, Money):
                    raise LarzTypeError("a wallet balance must be money")
                env.define(arg, Wallet(arg, balance))
            elif op == C.DEF_PRICE:
                value = pop()
                if not isinstance(value, Money):
                    raise LarzTypeError("a price must be money, got %s" % stringify(value))
                env.define(arg, value)
            elif op == C.PAY:
                self._do_pay(pop(), arg[0], arg[1], env)
            elif op == C.DEF_PAYWALL:
                price = pop()
                if not isinstance(price, Money):
                    raise LarzTypeError("a paywall price must be money")
                name, period, payee = arg
                env.define(name, Paywall(name, price, period, payee))
            elif op == C.SUBSCRIBE:
                self._do_subscribe(arg[0], arg[1], env)
            elif op == C.REQUIRE:
                if not truthy(pop()):
                    raise RequireError(arg or "requirement not met")
            else:                                    # pragma: no cover
                raise LarzTypeError("unknown opcode %r" % op)
        return None

    # -- helpers shared with the runtime semantics --
    def _call(self, callee, args):
        if isinstance(callee, Builtin):
            return callee.fn(*args)
        if isinstance(callee, Closure):
            chunk = callee.chunk
            if len(args) != len(chunk.params):
                raise LarzTypeError("%s expects %d argument(s), got %d"
                                    % (chunk.name, len(chunk.params), len(args)))
            self._charge_gas(chunk.gas, chunk.name)
            call_env = Environment(callee.env)
            for param, value in zip(chunk.params, args):
                call_env.define(param, value)
            result = self._exec(chunk, call_env)
            return result
        raise LarzTypeError("%s is not callable" % stringify(callee))

    def _get_prop(self, obj, name):
        if isinstance(obj, Wallet):
            if name == "balance":
                return obj.balance
            if name == "name":
                return obj.name
            raise LarzTypeError("a wallet has no property '%s'" % name)
        raise LarzTypeError("cannot read '%s' of %s" % (name, stringify(obj)))

    def _call_method(self, obj, name, args):
        if isinstance(obj, Wallet):
            if name in ("credit", "debit"):
                if len(args) != 1 or not isinstance(args[0], Money):
                    raise LarzTypeError("wallet.%s expects one money argument" % name)
                getattr(obj, name)(args[0])
                return None
            raise LarzTypeError("a wallet has no method '%s'" % name)
        raise LarzTypeError("cannot call method on %s" % stringify(obj))

    def _do_pay(self, amount, src_name, dst_name, env):
        if not isinstance(amount, Money):
            raise LarzTypeError("you can only pay money, got %s" % stringify(amount))
        src = env.get(src_name)
        dst = env.get(dst_name)
        if not isinstance(src, Wallet) or not isinstance(dst, Wallet):
            raise LarzTypeError("pay requires two wallets")
        src.debit(amount)
        dst.credit(amount)
        self.ledger.append(Transaction(src_name, dst_name, amount))

    def _do_subscribe(self, who, plan_name, env):
        wallet = env.get(who)
        plan = env.get(plan_name)
        if not isinstance(wallet, Wallet):
            raise LarzTypeError("can only subscribe a wallet")
        if not isinstance(plan, Paywall):
            raise LarzTypeError("can only subscribe to a paywall")
        payee = env.get(plan.payee)
        if not isinstance(payee, Wallet):
            raise LarzTypeError("paywall payee '%s' is not a wallet" % plan.payee)
        wallet.debit(plan.price)
        payee.credit(plan.price)
        self.ledger.append(Transaction(who, plan.payee, plan.price))
        self.subscriptions.add((wallet.name, plan.name))
