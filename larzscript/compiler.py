# -*- coding: utf-8 -*-
"""The Larzscript compiler - turn the AST into flat bytecode for the VM.

This is the "compiled" path (vs the tree-walking interpreter): instead of walking
the tree every time, we compile it once into a linear list of instructions that a
simple stack machine executes. Functions compile to their own Chunk, referenced as
a constant. Names still resolve through an environment at run time (a dict-based
name model), which keeps closures and scoping simple while still being real
bytecode.

Instruction = (OP, arg). Jumps use absolute instruction indices.
"""

from larzscript.runtime import Money

__all__ = ["Chunk", "compile_program"]

# --- opcodes ---
PUSH = "PUSH"                 # arg = constant value
LOAD = "LOAD"                 # arg = name
DEFINE = "DEFINE"            # arg = name  (let / define new)
STORE = "STORE"             # arg = name  (assign existing)
POP = "POP"
BINARY = "BINARY"           # arg = op
UNARY = "UNARY"             # arg = op
HAS = "HAS"
JUMP = "JUMP"               # arg = target index
JUMP_IF_FALSE = "JUMP_IF_FALSE"          # pops, jumps if falsy
JUMP_IF_FALSE_KEEP = "JUMP_IF_FALSE_KEEP"  # for 'and': keep value+jump if falsy, else pop
JUMP_IF_TRUE_KEEP = "JUMP_IF_TRUE_KEEP"    # for 'or'
CALL = "CALL"               # arg = argc
RETURN = "RETURN"
MAKE_FN = "MAKE_FN"         # arg = Chunk  -> closure over current env
GET_PROP = "GET_PROP"       # arg = name
CALL_METHOD = "CALL_METHOD"  # arg = (name, argc)
BUILD_LIST = "BUILD_LIST"   # arg = count
INDEX = "INDEX"
ENTER_SCOPE = "ENTER_SCOPE"
EXIT_SCOPE = "EXIT_SCOPE"
# money-native statements
DEF_WALLET = "DEF_WALLET"   # arg = name   (pops balance money)
DEF_PRICE = "DEF_PRICE"     # arg = name   (pops money)
PAY = "PAY"                 # arg = (src, dst)  (pops amount)
REQUIRE = "REQUIRE"         # arg = message   (pops condition)
DEF_PAYWALL = "DEF_PAYWALL"  # arg = (name, period, payee)  (pops price)
SUBSCRIBE = "SUBSCRIBE"     # arg = (who, plan)


class Chunk(object):
    """A compiled unit of code (the module, or one function)."""

    def __init__(self, name="<module>", params=None, gas=None):
        self.name = name
        self.params = params or []
        self.gas = gas
        self.code = []            # list of [op, arg]

    def emit(self, op, arg=None):
        self.code.append([op, arg])
        return len(self.code) - 1

    def patch(self, index, arg):
        self.code[index][1] = arg

    def here(self):
        return len(self.code)


class _Compiler(object):
    def __init__(self, chunk):
        self.chunk = chunk

    # -- statements --
    def stmt(self, node):
        getattr(self, "s_" + type(node).__name__)(node)

    def s_Let(self, node):
        self.expr(node.value)
        self.chunk.emit(DEFINE, node.name)

    def s_Assign(self, node):
        self.expr(node.value)
        self.chunk.emit(STORE, node.name)

    def s_Price(self, node):
        self.expr(node.value)
        self.chunk.emit(DEF_PRICE, node.name)

    def s_WalletDecl(self, node):
        if node.value is not None:
            self.expr(node.value)
        else:
            self.chunk.emit(PUSH, Money(0))
        self.chunk.emit(DEF_WALLET, node.name)

    def s_Pay(self, node):
        self.expr(node.amount)
        self.chunk.emit(PAY, (node.src, node.dst))

    def s_Paywall(self, node):
        self.expr(node.amount)
        self.chunk.emit(DEF_PAYWALL, (node.name, node.period, node.payee))

    def s_Subscribe(self, node):
        self.chunk.emit(SUBSCRIBE, (node.who, node.plan))

    def s_Require(self, node):
        self.expr(node.cond)
        self.chunk.emit(REQUIRE, node.message)

    def s_Fn(self, node):
        sub = Chunk(node.name, list(node.params), node.gas)
        body = _Compiler(sub)
        for stmt in node.body.body:            # function body: no extra scope
            body.stmt(stmt)
        sub.emit(PUSH, None)                    # implicit 'return nil'
        sub.emit(RETURN)
        self.chunk.emit(MAKE_FN, sub)
        self.chunk.emit(DEFINE, node.name)

    def s_Return(self, node):
        if node.value is not None:
            self.expr(node.value)
        else:
            self.chunk.emit(PUSH, None)
        self.chunk.emit(RETURN)

    def s_If(self, node):
        self.expr(node.cond)
        jf = self.chunk.emit(JUMP_IF_FALSE)
        self._scoped_block(node.then)
        if node.orelse is not None:
            jend = self.chunk.emit(JUMP)
            self.chunk.patch(jf, self.chunk.here())
            if type(node.orelse).__name__ == "If":
                self.stmt(node.orelse)
            else:
                self._scoped_block(node.orelse)
            self.chunk.patch(jend, self.chunk.here())
        else:
            self.chunk.patch(jf, self.chunk.here())

    def s_While(self, node):
        start = self.chunk.here()
        self.expr(node.cond)
        jf = self.chunk.emit(JUMP_IF_FALSE)
        self._scoped_block(node.body)
        self.chunk.emit(JUMP, start)
        self.chunk.patch(jf, self.chunk.here())

    def s_Block(self, node):
        self._scoped_block(node)

    def _scoped_block(self, block):
        self.chunk.emit(ENTER_SCOPE)
        for stmt in block.body:
            self.stmt(stmt)
        self.chunk.emit(EXIT_SCOPE)

    def s_ExprStmt(self, node):
        self.expr(node.expr)
        self.chunk.emit(POP)

    # -- expressions --
    def expr(self, node):
        getattr(self, "e_" + type(node).__name__)(node)

    def e_Num(self, node):
        self.chunk.emit(PUSH, node.value)

    def e_MoneyLit(self, node):
        self.chunk.emit(PUSH, Money(node.cents))

    def e_Str(self, node):
        self.chunk.emit(PUSH, node.value)

    def e_Bool(self, node):
        self.chunk.emit(PUSH, node.value)

    def e_Nil(self, node):
        self.chunk.emit(PUSH, None)

    def e_Name(self, node):
        self.chunk.emit(LOAD, node.id)

    def e_Binary(self, node):
        if node.op == "and":
            self.expr(node.left)
            j = self.chunk.emit(JUMP_IF_FALSE_KEEP)
            self.expr(node.right)
            self.chunk.patch(j, self.chunk.here())
            return
        if node.op == "or":
            self.expr(node.left)
            j = self.chunk.emit(JUMP_IF_TRUE_KEEP)
            self.expr(node.right)
            self.chunk.patch(j, self.chunk.here())
            return
        if node.op == "has":
            self.expr(node.left)
            self.expr(node.right)
            self.chunk.emit(HAS)
            return
        self.expr(node.left)
        self.expr(node.right)
        self.chunk.emit(BINARY, node.op)

    def e_Unary(self, node):
        self.expr(node.operand)
        self.chunk.emit(UNARY, node.op)

    def e_Call(self, node):
        self.expr(node.callee)
        for arg in node.args:
            self.expr(arg)
        self.chunk.emit(CALL, len(node.args))

    def e_Get(self, node):
        self.expr(node.obj)
        self.chunk.emit(GET_PROP, node.name)

    def e_MethodCall(self, node):
        self.expr(node.obj)
        for arg in node.args:
            self.expr(arg)
        self.chunk.emit(CALL_METHOD, (node.name, len(node.args)))

    def e_Array(self, node):
        for element in node.elements:
            self.expr(element)
        self.chunk.emit(BUILD_LIST, len(node.elements))

    def e_Index(self, node):
        self.expr(node.obj)
        self.expr(node.index)
        self.chunk.emit(INDEX)


def compile_program(program):
    """Compile a Program AST into a module :class:`Chunk`."""
    chunk = Chunk()
    compiler = _Compiler(chunk)
    for stmt in program.body:
        compiler.stmt(stmt)
    return chunk
