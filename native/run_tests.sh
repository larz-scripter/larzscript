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
