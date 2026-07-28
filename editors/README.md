# Editor support for Larzscript

Syntax highlighting for `.lz` files, so Larzscript looks first-class in your
editor — keywords, money literals (`$3.50`), f-strings, functions, builtins, and
the money-native primitives all highlighted.

## VS Code

```bash
cp -r editors/vscode ~/.vscode/extensions/larzscript-1.0.0
```

Reload VS Code; `.lz` files are highlighted. See `vscode/README.md`.

## Vim / Neovim

```bash
mkdir -p ~/.vim/syntax ~/.vim/ftdetect
cp editors/vim/syntax/larzscript.vim   ~/.vim/syntax/
cp editors/vim/ftdetect/larzscript.vim ~/.vim/ftdetect/
```

(For Neovim, use `~/.config/nvim/` instead of `~/.vim/`.)
