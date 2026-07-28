#!/bin/sh
# Build the native larzscript and check it against the expected outputs.
set -e
cd "$(dirname "$0")"
CC="${CC:-cc}"
$CC -O2 -std=c11 -o /tmp/_larzscript_test larzscript.c
pass=0; fail=0
for lz in tests/*.lz; do
  exp="${lz%.lz}.expected"
  got="$(/tmp/_larzscript_test "$lz" 2>&1 || true)"
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
  /tmp/_larzscript_test fmt "$lz" > "$tmp" 2>/dev/null
  f1="$(cat "$tmp")"
  f2="$(/tmp/_larzscript_test fmt "$tmp" 2>/dev/null)"
  orig="$(/tmp/_larzscript_test "$lz" 2>&1 || true)"
  fout="$(/tmp/_larzscript_test "$tmp" 2>&1 || true)"
  rm -f "$tmp"
  if [ "$f1" = "$f2" ] && [ "$orig" = "$fout" ]; then fpass=$((fpass+1));
  else ffail=$((ffail+1)); echo "FMT FAIL $lz"; fi
done
echo "formatter: $fpass ok, $ffail failed"
[ "$ffail" = 0 ]
