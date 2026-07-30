#!/bin/sh
# Build the native larzscript and check it against the expected outputs.
#
# Env overrides (used by CI to test cross-compiled binaries without rebuilding
# for the local platform):
#   BINARY=<path>        skip the local build, run this binary instead
#   RUN_PREFIX=<cmd>     prepend to every invocation (e.g. "wine",
#                        "qemu-aarch64-static")
#   LZ_CRLF_NORMALIZE=1  strip one trailing \r per line of actual output
#                        before comparing (Windows CRT text-mode translates
#                        \n -> \r\n on output; this is NOT a blanket \r strip,
#                        since tests/escapes.lz embeds a literal mid-string \r
#                        as real content that a blanket strip would corrupt)
set -e
cd "$(dirname "$0")"
CC="${CC:-cc}"
if [ -n "$BINARY" ]; then
  BIN="$BINARY"
else
  $CC -O2 -std=c11 -o /tmp/_larzscript_test larzscript.c
  BIN="/tmp/_larzscript_test"
fi
run() { $RUN_PREFIX "$BIN" "$@"; }
norm() { if [ "$LZ_CRLF_NORMALIZE" = "1" ]; then sed 's/\r$//'; else cat; fi; }

pass=0; fail=0
for lz in tests/*.lz; do
  exp="${lz%.lz}.expected"
  got="$(run "$lz" 2>&1 | norm || true)"
  if [ "$got" = "$(cat "$exp")" ]; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "FAIL $lz"; echo "--- expected ---"; cat "$exp"; echo "--- got ---"; echo "$got"
  fi
done
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]

# formatter invariants: fmt is idempotent, and formatted code runs identically
echo "--- formatter checks ---"
fpass=0; ffail=0
for lz in tests/*.lz; do
  dir=$(dirname "$lz"); tmp="$dir/.fmtcheck.lz"
  run fmt "$lz" 2>/dev/null | norm > "$tmp"
  f1="$(cat "$tmp")"
  f2="$(run fmt "$tmp" 2>/dev/null | norm)"
  orig="$(run "$lz" 2>&1 | norm || true)"
  fout="$(run "$tmp" 2>&1 | norm || true)"
  rm -f "$tmp"
  if [ "$f1" = "$f2" ] && [ "$orig" = "$fout" ]; then fpass=$((fpass+1));
  else ffail=$((ffail+1)); echo "FMT FAIL $lz"; fi
done
echo "formatter: $fpass ok, $ffail failed"
[ "$ffail" = 0 ]
