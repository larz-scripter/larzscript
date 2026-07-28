# Larzscript for VS Code

Syntax highlighting and editor configuration for
[Larzscript](https://github.com/larz-scripter/larzscript) — the money-native,
general-purpose language — for `.lz` files.

Highlights keywords, money literals (`$3.50`), f-strings with interpolation,
strings, numbers, functions, builtins, operators, and the money-native
primitives (`pay`, `wallet`, `require`, `paywall`, `subscribe`, `has`).

## Install (from source)

Copy this folder into your VS Code extensions directory:

```bash
cp -r editors/vscode ~/.vscode/extensions/larzscript-1.0.0
```

Then reload VS Code. Any `.lz` file will be highlighted as Larzscript.
