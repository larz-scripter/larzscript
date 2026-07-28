# -*- coding: utf-8 -*-
"""Enable `python -m larzscript <file.lz>` and `python -m larzscript repl`."""
import sys
from larzscript.cli import main

if __name__ == "__main__":
    sys.exit(main())
