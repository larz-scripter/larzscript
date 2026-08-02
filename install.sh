#!/bin/sh
# Larzscript one-line installer:  curl -fsSL <raw>/install.sh | sh
# Downloads the latest native binary + the larzpkg package manager.
set -e
arch=$(uname -m)
case "$arch" in
  x86_64) bin=larzscript-linux-x86_64 ;;
  aarch64|arm64) bin=larzscript-linux-aarch64 ;;
  *) echo "larzscript: unsupported architecture '$arch' (build from source: cc -O2 -o larzscript native/larzscript.c)"; exit 1 ;;
esac
bindir="${LARZSCRIPT_BIN:-$HOME/.local/bin}"
mkdir -p "$bindir" "$HOME/.larzscript/lib"
echo "downloading larzscript ($bin) ..."
curl -fsSL "https://github.com/larz-scripter/larzscript/releases/latest/download/$bin" -o "$bindir/larzscript"
chmod +x "$bindir/larzscript"
curl -fsSL "https://raw.githubusercontent.com/larz-scripter/larzscript/main/tools/larzpkg.lz" -o "$HOME/.larzscript/larzpkg.lz"
echo ""
echo "installed: $bindir/larzscript"
echo "$("$bindir/larzscript" --version)"
case ":$PATH:" in *":$bindir:"*) ;; *) echo "add $bindir to your PATH, e.g.:  export PATH=\"$bindir:\$PATH\"" ;; esac
echo ""
echo "try it:"
echo "  larzscript repl"
echo "  larzscript pkg install mathx"
echo "  larzscript -e 'print(\"hello from larzscript\")'"
