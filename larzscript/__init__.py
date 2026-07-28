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
from larzscript.errors import (LarzScriptError, LarzSyntaxError, LarzRuntimeError,
                               LarzNameError, LarzTypeError, MoneyError,
                               RequireError, OutOfGasError)

__version__ = "0.2.0"
__all__ = ["run", "parse", "tokenize", "Interpreter", "Money", "Wallet",
           "Transaction", "LarzScriptError", "LarzSyntaxError", "LarzRuntimeError",
           "LarzNameError", "LarzTypeError", "MoneyError", "RequireError",
           "OutOfGasError"]


def parse(source):
    """Tokenize and parse Larzscript source into a Program AST."""
    return _parse(tokenize(source))


def run(source, gas=None, output=None):
    """Run Larzscript source. Returns the Interpreter (inspect .ledger, .output,
    .get(name), .gas_used). ``gas`` sets a metering budget (None = unlimited)."""
    interp = Interpreter(gas=gas, output=output)
    interp.run(parse(source))
    return interp
