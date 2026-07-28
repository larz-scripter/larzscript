"""larzscript - a money-native programming language, in pure Python.

Money, wallets, payments (``pay ... from ... to ...``), guardrails (``require``)
and gas-metered functions are first-class parts of the language - not a library
bolted on. Tokenize -> parse -> tree-walking interpret, all readable and
zero-dependency. Settlement is in-memory here; the same credit/debit/record
interface is what a real backend (GemVault / LarzChain) would implement.

    from larzscript import run

    program = '''
        wallet customer = $10.00
        wallet shop
        price coffee = $3.50

        pay coffee from customer to shop
        print("shop balance:", shop.balance)      # $3.50
    '''
    result = run(program)
    result.get("shop").balance       # Money(350)  -> "$3.50"
    result.ledger                    # [Transaction(customer -> shop: $3.50)]
    print(result.output)             # shop balance: $3.50
"""

from larzscript.lexer import tokenize
from larzscript.parser import parse as _parse
from larzscript.interpreter import Interpreter, Money, Wallet, Transaction
from larzscript.runtime import Settlement, CallbackSettlement
from larzscript.errors import (LarzScriptError, LarzSyntaxError, LarzRuntimeError,
                               LarzNameError, LarzTypeError, MoneyError,
                               RequireError, OutOfGasError, SettlementError)

__version__ = "1.0.0"
__all__ = ["run", "compile_source", "parse", "tokenize", "Interpreter", "Money", "Wallet",
           "Transaction", "Settlement", "CallbackSettlement",
           "LarzScriptError", "LarzSyntaxError", "LarzRuntimeError",
           "LarzNameError", "LarzTypeError", "MoneyError", "RequireError",
           "OutOfGasError", "SettlementError"]


def parse(source):
    """Tokenize and parse Larzscript source into a Program AST."""
    return _parse(tokenize(source))


def compile_source(source):
    """Parse and compile Larzscript source to a bytecode Chunk (the VM backend)."""
    from larzscript.compiler import compile_program
    return compile_program(parse(source))


def run(source, gas=None, output=None, backend="tree", settlement=None):
    """Run Larzscript source and return the engine (inspect .ledger, .output,
    .get(name), .gas_used). ``gas`` sets a metering budget (None = unlimited).

    ``backend="tree"`` (default) walks the AST; ``backend="vm"`` compiles to
    bytecode and runs it on the stack VM. Both produce identical results.

    ``settlement`` plugs in a custom :class:`Settlement` backend so every
    ``pay``/``subscribe`` is authorized against, and recorded to, a real ledger
    (on-chain, fiat gateway, audit log). Defaults to in-memory settlement.
    """
    if backend == "vm":
        from larzscript.vm import VM
        from larzscript.compiler import compile_program
        engine = VM(gas=gas, output=output, settlement=settlement)
        engine.run(compile_program(parse(source)))
        return engine
    if backend != "tree":
        raise ValueError("backend must be 'tree' or 'vm', got %r" % backend)
    interp = Interpreter(gas=gas, output=output, settlement=settlement)
    interp.run(parse(source))
    return interp
