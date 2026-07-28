# -*- coding: utf-8 -*-
"""The Larzscript parser - tokens into an AST (a recursive-descent parser).

The grammar is small but includes the money-native statements ``price``,
``wallet``, ``pay ... from ... to ...`` and ``require``, plus ``fn ... gas N``
for metered functions.
"""

from larzscript.errors import LarzSyntaxError

__all__ = ["Parser", "parse"]


# --- AST node types (tiny classes; the interpreter dispatches on their name) --

def _node(name, *fields):
    def __init__(self, *args):
        for f, a in zip(fields, args):
            setattr(self, f, a)

    def __repr__(self):
        return "%s(%s)" % (name, ", ".join("%s=%r" % (f, getattr(self, f))
                                           for f in fields))
    return type(name, (object,), {"__init__": __init__, "__repr__": __repr__,
                                  "__slots__": fields, "_fields": fields})


Program = _node("Program", "body")
Let = _node("Let", "name", "value")
Assign = _node("Assign", "name", "value")
Price = _node("Price", "name", "value")
WalletDecl = _node("WalletDecl", "name", "value")
Pay = _node("Pay", "amount", "src", "dst")
Require = _node("Require", "cond", "message")
Fn = _node("Fn", "name", "params", "gas", "body")
Return = _node("Return", "value")
If = _node("If", "cond", "then", "orelse")
While = _node("While", "cond", "body")
Block = _node("Block", "body")
ExprStmt = _node("ExprStmt", "expr")

Num = _node("Num", "value")
MoneyLit = _node("MoneyLit", "cents")
Str = _node("Str", "value")
Bool = _node("Bool", "value")
Nil = _node("Nil")
Name = _node("Name", "id")
Binary = _node("Binary", "op", "left", "right")
Unary = _node("Unary", "op", "operand")
Call = _node("Call", "callee", "args")
Get = _node("Get", "obj", "name")
MethodCall = _node("MethodCall", "obj", "name", "args")

_EXPR_STARTERS = ("NUMBER", "MONEY", "STRING", "IDENT", "true", "false", "nil", "(")


class Parser(object):
    def __init__(self, tokens):
        self.t = tokens
        self.i = 0

    # -- token helpers --
    def peek(self, k=0):
        idx = self.i + k
        return self.t[idx] if idx < len(self.t) else self.t[-1]

    def advance(self):
        tok = self.t[self.i]
        self.i += 1
        return tok

    def check(self, type):
        return self.peek().type == type

    def check_op(self, *ops):
        p = self.peek()
        return p.type == "OP" and p.value in ops

    def match(self, *types):
        if self.peek().type in types:
            return self.advance()
        return None

    def expect(self, type, what):
        if self.check(type):
            return self.advance()
        p = self.peek()
        raise LarzSyntaxError("expected %s but got %r on line %d" % (what, p.value, p.line))

    def expect_op(self, op, what):
        if self.check_op(op):
            return self.advance()
        p = self.peek()
        raise LarzSyntaxError("expected %s but got %r on line %d" % (what, p.value, p.line))

    # -- entry --
    def parse(self):
        body = []
        while not self.check("EOF"):
            body.append(self.statement())
        return Program(body)

    # -- statements --
    def statement(self):
        t = self.peek().type
        if t == "let":
            return self._let()
        if t == "price":
            return self._price()
        if t == "wallet":
            return self._wallet()
        if t == "pay":
            return self._pay()
        if t == "require":
            return self._require()
        if t == "fn":
            return self._fn()
        if t == "return":
            return self._return()
        if t == "if":
            return self._if()
        if t == "while":
            return self._while()
        if t == "{":
            return self.block()
        # assignment: IDENT '=' ...
        if t == "IDENT" and self.peek(1).type == "OP" and self.peek(1).value == "=":
            name = self.advance().value
            self.advance()  # '='
            return Assign(name, self.expression())
        return ExprStmt(self.expression())

    def _let(self):
        self.advance()
        name = self.expect("IDENT", "a name after 'let'").value
        self.expect_op("=", "'=' in a let")
        return Let(name, self.expression())

    def _price(self):
        self.advance()
        name = self.expect("IDENT", "a name after 'price'").value
        self.expect_op("=", "'=' in a price")
        return Price(name, self.expression())

    def _wallet(self):
        self.advance()
        name = self.expect("IDENT", "a name after 'wallet'").value
        value = None
        if self.check_op("="):
            self.advance()
            value = self.expression()
        return WalletDecl(name, value)

    def _pay(self):
        self.advance()
        amount = self.expression()
        self.expect("from", "'from' in a pay")
        src = self.expect("IDENT", "a wallet after 'from'").value
        self.expect("to", "'to' in a pay")
        dst = self.expect("IDENT", "a wallet after 'to'").value
        return Pay(amount, src, dst)

    def _require(self):
        self.advance()
        cond = self.expression()
        message = None
        if self.match(","):
            message = self.expect("STRING", "a message string after ','").value
        return Require(cond, message)

    def _fn(self):
        self.advance()
        name = self.expect("IDENT", "a function name").value
        self.expect("(", "'(' after the function name")
        params = []
        if not self.check(")"):
            params.append(self.expect("IDENT", "a parameter name").value)
            while self.match(","):
                params.append(self.expect("IDENT", "a parameter name").value)
        self.expect(")", "')' after parameters")
        gas = None
        if self.check("gas"):
            self.advance()
            gas = self.expect("NUMBER", "a gas amount after 'gas'").value
            gas = int(gas)
        return Fn(name, params, gas, self.block())

    def _return(self):
        self.advance()
        if self.peek().type in _EXPR_STARTERS or self.check("not") or self.check_op("-"):
            return Return(self.expression())
        return Return(None)

    def _if(self):
        self.advance()
        cond = self.expression()
        then = self.block()
        orelse = None
        if self.match("else"):
            orelse = self._if() if self.check("if") else self.block()
        return If(cond, then, orelse)

    def _while(self):
        self.advance()
        cond = self.expression()
        return While(cond, self.block())

    def block(self):
        self.expect("{", "'{'")
        body = []
        while not self.check("}") and not self.check("EOF"):
            body.append(self.statement())
        self.expect("}", "'}'")
        return Block(body)

    # -- expressions (precedence climbing) --
    def expression(self):
        return self._logic_or()

    def _logic_or(self):
        node = self._logic_and()
        while self.check("or"):
            self.advance()
            node = Binary("or", node, self._logic_and())
        return node

    def _logic_and(self):
        node = self._equality()
        while self.check("and"):
            self.advance()
            node = Binary("and", node, self._equality())
        return node

    def _equality(self):
        node = self._comparison()
        while self.check_op("==", "!="):
            op = self.advance().value
            node = Binary(op, node, self._comparison())
        return node

    def _comparison(self):
        node = self._term()
        while self.check_op("<", "<=", ">", ">="):
            op = self.advance().value
            node = Binary(op, node, self._term())
        return node

    def _term(self):
        node = self._factor()
        while self.check_op("+", "-"):
            op = self.advance().value
            node = Binary(op, node, self._factor())
        return node

    def _factor(self):
        node = self._unary()
        while self.check_op("*", "/", "%"):
            op = self.advance().value
            node = Binary(op, node, self._unary())
        return node

    def _unary(self):
        if self.check("not"):
            self.advance()
            return Unary("not", self._unary())
        if self.check_op("-"):
            self.advance()
            return Unary("-", self._unary())
        return self._postfix()

    def _postfix(self):
        node = self._primary()
        while True:
            if self.check("."):
                self.advance()
                name = self.expect("IDENT", "a property name after '.'").value
                if self.check("("):
                    node = MethodCall(node, name, self._args())
                else:
                    node = Get(node, name)
            elif self.check("("):
                node = Call(node, self._args())
            else:
                break
        return node

    def _args(self):
        self.expect("(", "'('")
        args = []
        if not self.check(")"):
            args.append(self.expression())
            while self.match(","):
                args.append(self.expression())
        self.expect(")", "')'")
        return args

    def _primary(self):
        p = self.peek()
        if p.type == "NUMBER":
            self.advance()
            return Num(p.value)
        if p.type == "MONEY":
            self.advance()
            return MoneyLit(p.value)
        if p.type == "STRING":
            self.advance()
            return Str(p.value)
        if p.type == "true":
            self.advance()
            return Bool(True)
        if p.type == "false":
            self.advance()
            return Bool(False)
        if p.type == "nil":
            self.advance()
            return Nil()
        if p.type == "IDENT":
            self.advance()
            return Name(p.value)
        if p.type == "(":
            self.advance()
            node = self.expression()
            self.expect(")", "')'")
            return node
        raise LarzSyntaxError("unexpected %r on line %d" % (p.value, p.line))


def parse(tokens):
    """Parse a token list into a Program AST."""
    return Parser(tokens).parse()
