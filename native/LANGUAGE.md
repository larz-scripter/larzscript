# The Larzscript Language

Larzscript is a small, general-purpose programming language with money as a
first-class citizen. This is the reference for the language as implemented by
the standalone native binary (`larzscript.c`) — the official implementation.

- **Run:** `larzscript program.lz` · `larzscript -e "code"` · `larzscript repl`
- **Comments:** `# to end of line`
- **Statements** are separated by newlines or `;` (both optional at block ends).

---

## 1. Values

| Type | Examples | Notes |
|------|----------|-------|
| number | `42`, `3.14`, `-7` | one numeric type (integers print without a point) |
| string | `"hi"`, `f"x={x}"` | escapes `\n \t \\ \"`; f-strings interpolate |
| bool | `true`, `false` | |
| nil | `nil` | absence of a value |
| list | `[1, 2, 3]` | ordered, mutable, mixed types |
| dict | `{"a": 1, "b": 2}` | keys may be strings or numbers |
| function | `fn(x){ return x }` | first-class; closures capture their scope |
| money | `$3.50` | **exact integer cents**, never a float |
| wallet | `wallet w = $10.00` | a named balance you can `pay` from/to |
| module | `import "m.lz" as m` | a namespace of another file's definitions |

Truthiness: `nil`, `false`, `0`, `$0.00`, `""`, `[]`, `{}` are falsy; all else truthy.

---

## Read it like English

Larzscript's money statements already read like sentences - `pay coffee
from customer to shop`, `subscribe customer to pro`. The rest of the
language leans the same way: every natural form below is real syntax
that desugars to the equivalent symbolic form (`larzscript fmt` preserves
whichever spelling you wrote), not a different feature - use either, mix
both, whatever reads best where you're writing it.

```
if age is 18 { ... }               # ==
if age is not 18 { ... }           # !=
if balance is at least $5 { ... }  # >=
if balance is at most $5 { ... }   # <=
if score is more than 0 { ... }    # >
if attempts is less than 3 { ... } # <

unless balance >= price {          # if not (...)
    throw "not enough funds"
}

for i from 1 to 10 { ... }         # counts up, 1..10 INCLUSIVE
for i from 10 to 1 { ... }         # auto-detects direction, counts down

say "hello, " + name               # print(...)
wait 2                             # sleep(2)
```

---

## 2. Variables

```
let x = 10          # declare
x = 20              # reassign (must already exist)
x += 5              # compound assign: += -= *= /= %=
```

---

## 3. Operators

Precedence, lowest to highest:

1. `? :` — ternary (`cond ? a : b`)
2. `or`
3. `and`
4. `==` `!=`
5. `has` `in`
6. `<` `<=` `>` `>=`
7. `+` `-`
8. `*` `/` `%` `//`
9. `**` (right-associative)
10. unary `-`, `not`
11. postfix: call `f(...)`, index `a[i]`, slice `a[i:j]`, member `m.x`

`+` concatenates strings and lists; `*` scales money by a number; `//` is floor
division; `**` is power. `in` tests membership in a list, dict (keys) or string
(substring). `has` tests a wallet subscription.

`==`/`!=`/`>=`/`<=`/`>`/`<` also read as `is`/`is not`/`is at least`/`is at
most`/`is more than`/`is less than` - see "Read it like English" above.
The natural phrases bind at the same precedence as their symbols
(`is at least`/`is at most`/`is more than`/`is less than` as tight as `>=`
etc.; bare `is`/`is not` as loose as `==`/`!=`).

---

## 4. Control flow

```
if x > 0 { ... } else if x < 0 { ... } else { ... }
unless x <= 0 { ... }              # if not (x <= 0) { ... } - see above

while cond { ...; break; continue }

for item in [1, 2, 3] { ... }      # lists, dicts (keys), and strings
for k in {"a": 1} { ... }
for i from 1 to 10 { ... }         # a natural counting loop - see above

try { risky() } catch e { print(e["type"], e["message"]) }
throw "something went wrong"       # or throw {"type": "MyError", "message": "..."}
```

---

## 5. Functions

```
fn add(a, b) { return a + b }               # named
fn greet(name, greeting="Hello") { ... }    # default parameters
let square = fn(x) { return x * x }         # anonymous (lambda)

fn scan() gas 500 { ... }                   # gas-metered: fails closed past budget
```

Functions are values: pass them around, return them, and use the higher-order
builtins `map`, `filter`, `reduce`.

---

## 6. Collections

```
let xs = [1, 2, 3]
xs[0] = 10           # element assignment
xs.push(4); xs.pop(); xs.sort(); xs.reverse()
xs.count(1); xs.extend([5, 6]); xs.clear()
xs[1:3]; xs[:2]; xs[-2:]              # slicing (lists and strings)

let d = {"a": 1}
d["b"] = 2           # insert / update
d.keys(); d.values(); d.has("a"); d.get("z", 0); d.remove("a")
```

### Comprehensions

```
[x * x for x in range(6)]                 # [0, 1, 4, 9, 16, 25]
[n for n in range(20) if n % 2 == 0]      # filter with `if`
[w.upper() for w in "a b c".split(" ")]   # any expression, incl. calls
{k: k * k for k in range(5) if k > 1}     # dict comprehension
```

A comprehension has one `for x in iterable` clause and an optional `if`.

---

## 7. Strings

f-strings interpolate expressions; `{{`/`}}` are literal braces.

```
print(f"hi {name}, {1 + 2} = {1+2}")
"HELLO".lower(); "  x  ".strip(); "a,b".split(","); "ab".replace("a","A")
"hello".contains("ell"); "hi".starts_with("h"); "hi".ends_with("i")
"hello world".capitalize(); "hello world".title(); "x".ljust(5); "x".rjust(5, ".")
```

---

## 8. Modules

```
# lib.lz
fn double(x) { return x * 2 }
let VERSION = "1.0"

# main.lz
import "lib.lz" as lib
print(lib.double(21), lib.VERSION)
```

Import resolution searches, in order: relative to the importing file, then each
directory in `$LARZSCRIPT_PATH`, then `~/.larzscript/lib`, then `./lz_modules`.
A bare name like `import "mathx"` also matches `mathx.lz`, `mathx/mathx.lz` or
`mathx/main.lz` — which is how packages installed with **larzpkg** are found.
Modules are executed once and cached.

### Packages (larzpkg)

`larzpkg` is the package manager (itself written in Larzscript,
`tools/larzpkg.lz`, installed by `install.sh` to `~/.larzscript/larzpkg.lz`).
`larzscript pkg ...` resolves that path for you from any directory - the
same as running `larzscript ~/.larzscript/larzpkg.lz ...` yourself:

```
larzscript pkg install mathx     # git-clones into ~/.larzscript/lib
larzscript pkg list
larzscript pkg search math
```

Then `import "mathx"` just works. Packages are git repos with a `main.lz` (or
`<name>.lz`) at their root, listed in a registry.

## OS / system builtins

For real scripting: `env(name[, default])` · `run(cmd)` (shell, returns the exit
code) · `capture(cmd)` (returns stdout) · `cwd()` · `chdir(path)` ·
`listdir(path)` · `mkdir(path)` · `remove(path)` · `rename(from, to)` ·
`time()` (unix seconds) · `clock()` (monotonic seconds) · `sleep(seconds)`.

### TCP sockets

Real listening sockets - `socket_listen(port)` (returns a handle),
`socket_accept(fd)` (blocks for the next client, returns its handle),
`socket_read(fd, max_bytes)`, `socket_write(fd, data)` (returns bytes
actually sent - may be less than asked, same as a real `send()`),
`socket_close(fd)`. A handle is a plain number, the OS fd/`SOCKET` value.
Errors (bind failure, a bad handle, ...) throw a catchable `SocketError`.

The outbound half: `socket_connect(host, port)` resolves `host` (hostname or
literal IP) and dials out, returning a connected handle - the counterpart
`socket_listen`/`socket_accept` never had, since those can only be dialed
*into*. And `socket_poll(fds, timeout_ms)` - a `select()` wrapper - takes a
list of handles and returns the subset that are readable within
`timeout_ms` (empty on timeout, not an error), so one single-threaded
program can service several live sockets without a blocking `socket_read()`
on an idle one starving a ready one.

These are deliberately low-level, the same way `read_file`/`write_file`
are - the `tcp` package builds the ergonomic layer (`tcp.serve(port,
handler)`, a real loop that answers one request at a time, forever):

```
import "tcp" as tcp
tcp.serve(8080, fn(request) {
  return "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"
})
```

Hosted only (Linux/macOS/Windows) - LarzOS's kernel build has its own real
networking already (`kernel/net.c`, a from-scratch driver-level stack, the
`net`/`fetch` packages), a completely different, unrelated code path; the
browser/wasm build has no raw TCP at all by sandbox design, so every one of
these throws a clear `SocketError` there instead of failing unpredictably.
One connection at a time by design too - this is a single-threaded
interpreter with no concurrency primitive, so a real production server
needs more than this; a real *server that works at all* is what this adds.

---

## 9. Money (the money-native core)

```
wallet customer = $20.00
wallet shop
price coffee = $3.50

pay coffee from customer to shop          # moves money, records a ledger entry
require customer.balance >= $5, "min $5"  # a guardrail the runtime enforces

paywall pro = $9.00 / month to shop
subscribe customer to pro                 # charges, records, grants access
require customer has pro, "subscribe first"
```

`$X.YZ` is exact integer cents. `customer.balance` is money; `wallet.credit(m)`
and `wallet.debit(m)` adjust it. Run with `--ledger` to print the money ledger.

---

## 10. Standard library (builtins)

`print len range range_to str int float bool type abs min max sum sorted reversed
floor ceil round sqrt pow chr ord assert input keys values push money
map filter reduce join enumerate zip exit all any count unique hex bin oct
gcd factorial sign clamp list dict`

`range(n)`, `range(start, stop)`, `range(start, stop, step)`. A range is a **lazy
sequence** — `for i in range(100000000)` iterates in O(1) space (no list is built),
and `range` supports `len`, indexing, `in`, and slicing. Use `list(range(...))`
to materialize one.
`range_to(from, to)` is `range`'s **inclusive**, direction-auto-detecting sibling -
what `for i from A to B { ... }` desugars to (see "Read it like English").
`list(x)` builds a list from a string/dict/list/range; `dict(pairs)` from `[[k, v], ...]`.

`"ab" * 3` and `[0] * 5` repeat strings and lists.

### Files, args, exit

```
write_file("out.txt", "hello")        # write (overwrite)
append_file("out.txt", " more")       # append
read_file("out.txt")                  # -> string
read_file_bytes("out.bin")            # -> list of ints 0-255
file_exists("out.txt")                # -> bool
args                                   # command-line args after the script (a list)
exit(0)                               # exit with a status code
```

Strings are C-style and NUL-terminated internally, so a `0x00` byte
inside one silently ends it early for `len`/`+`/`for..in`/writing.
`write_file`/`append_file` accept a list of ints 0-255 as binary-safe
content (raw `fwrite`, no truncation) instead of a string - use that,
with `read_file_bytes`, for any real binary format (zip, tar, packed
protocols) that has to survive a `0x00` byte intact.

---

## 11. Errors

Runtime errors carry a type and a message with the source line, and are
catchable with `try`/`catch` (the caught value is a `{type, message}` dict).
Types include `LarzNameError`, `LarzTypeError`, `LarzRuntimeError`,
`LarzValueError`, `LarzKeyError`, `MoneyError`, `RequireError`, `OutOfGasError`,
`AssertionError`, `ImportError`, and `Error` (from `throw`).

---

*The native implementation is a single C file with zero third-party
dependencies. See `README.md` to build or download it.*
