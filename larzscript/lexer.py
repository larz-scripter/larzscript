# -*- coding: utf-8 -*-
"""The Larzscript lexer - turn source text into a list of tokens.

Notable: money literals like ``$1.50`` and ``$0.02`` are lexed as a first-class
MONEY token (value = integer cents), which is what makes money native to the
language rather than a library bolted on.
"""

from larzscript.errors import LarzSyntaxError

__all__ = ["tokenize", "Token", "KEYWORDS"]

KEYWORDS = {
    "let", "fn", "return", "if", "else", "while",
    "and", "or", "not", "true", "false", "nil",
    "price", "wallet", "pay", "from", "to", "require", "gas",
}

_TWO_CHAR_OPS = ("==", "!=", "<=", ">=")
_ONE_CHAR_OPS = "+-*/%<>="
_PUNCT = "(){},."


class Token(object):
    __slots__ = ("type", "value", "line")

    def __init__(self, type, value, line):
        self.type = type
        self.value = value
        self.line = line

    def __repr__(self):
        return "Token(%s, %r, line %d)" % (self.type, self.value, self.line)


def _money_to_cents(text, line):
    if text.count(".") > 1 or text == "":
        raise LarzSyntaxError("bad money literal '$%s' on line %d" % (text, line))
    if "." in text:
        whole, frac = text.split(".")
        frac = (frac + "00")[:2]
        return int(whole or "0") * 100 + int(frac)
    return int(text) * 100


def tokenize(source):
    """Turn Larzscript source into a list of :class:`Token`, ending in EOF."""
    tokens = []
    i, n, line = 0, len(source), 1
    while i < n:
        c = source[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c in " \t\r":
            i += 1
            continue
        if c == "#":                                   # comment to end of line
            while i < n and source[i] != "\n":
                i += 1
            continue
        if c == "$":                                   # money literal
            j = i + 1
            while j < n and (source[j].isdigit() or source[j] == "."):
                j += 1
            tokens.append(Token("MONEY", _money_to_cents(source[i + 1:j], line), line))
            i = j
            continue
        if c.isdigit() or (c == "." and i + 1 < n and source[i + 1].isdigit()):
            j, dot = i, 0
            while j < n and (source[j].isdigit() or source[j] == "."):
                if source[j] == ".":
                    dot += 1
                j += 1
            text = source[i:j]
            if dot > 1:
                raise LarzSyntaxError("bad number '%s' on line %d" % (text, line))
            tokens.append(Token("NUMBER", float(text) if "." in text else int(text), line))
            i = j
            continue
        if c == '"':                                   # string
            j, buf = i + 1, []
            while j < n and source[j] != '"':
                ch = source[j]
                if ch == "\\" and j + 1 < n:
                    esc = source[j + 1]
                    buf.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(esc, esc))
                    j += 2
                    continue
                buf.append(ch)
                j += 1
            if j >= n:
                raise LarzSyntaxError("unterminated string on line %d" % line)
            tokens.append(Token("STRING", "".join(buf), line))
            i = j + 1
            continue
        if c.isalpha() or c == "_":                    # identifier / keyword
            j = i
            while j < n and (source[j].isalnum() or source[j] == "_"):
                j += 1
            word = source[i:j]
            tokens.append(Token(word if word in KEYWORDS else "IDENT", word, line))
            i = j
            continue
        if source[i:i + 2] in _TWO_CHAR_OPS:           # two-char operator
            tokens.append(Token("OP", source[i:i + 2], line))
            i += 2
            continue
        if c in _ONE_CHAR_OPS:                          # one-char operator
            tokens.append(Token("OP", c, line))
            i += 1
            continue
        if c in _PUNCT:                                 # punctuation (type IS the char)
            tokens.append(Token(c, c, line))
            i += 1
            continue
        raise LarzSyntaxError("unexpected character %r on line %d" % (c, line))
    tokens.append(Token("EOF", None, line))
    return tokens
