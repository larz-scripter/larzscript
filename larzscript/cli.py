# -*- coding: utf-8 -*-
"""The Larzscript command-line interface: run a .lz file, or start a REPL."""

import sys

from larzscript import run, parse
from larzscript.interpreter import Interpreter
from larzscript.errors import LarzScriptError

__all__ = ["main", "repl"]

USAGE = "usage: larzscript <file.lz>   |   larzscript repl"


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help"):
        print(USAGE)
        return 0
    if argv[0] in ("-v", "--version"):
        import larzscript
        print("larzscript " + larzscript.__version__)
        return 0
    if argv[0] == "repl":
        return repl()

    path = argv[0]
    try:
        with open(path, "r") as f:
            source = f.read()
    except IOError as e:
        sys.stderr.write("larzscript: cannot read %s: %s\n" % (path, e))
        return 1
    try:
        result = run(source)
    except LarzScriptError as e:
        sys.stderr.write("%s: %s\n" % (type(e).__name__, e))
        return 1
    if result.output:
        print(result.output)
    return 0


def repl():
    """A simple REPL - state (variables, wallets, subscriptions) persists across
    lines on a shared interpreter."""
    print("Larzscript REPL - type statements; 'exit' or Ctrl-D to quit.")
    interp = Interpreter()
    while True:
        try:
            line = _read("larz> ")
        except EOFError:
            print()
            break
        line = line.strip()
        if not line:
            continue
        if line in ("exit", "quit"):
            break
        before = len(interp._out)
        try:
            interp.run(parse(line))
        except LarzScriptError as e:
            print("%s: %s" % (type(e).__name__, e))
            continue
        for produced in interp._out[before:]:
            print(produced)
    return 0


def _read(prompt):
    try:
        return raw_input(prompt)   # noqa: F821  (Python 2)
    except NameError:
        return input(prompt)


if __name__ == "__main__":
    sys.exit(main())
