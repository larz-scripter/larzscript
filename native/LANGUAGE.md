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

---

## 4. Control flow

```
if x > 0 { ... } else if x < 0 { ... } else { ... }

while cond { ...; break; continue }

for item in [1, 2, 3] { ... }      # lists, dicts (keys), and strings
for k in {"a": 1} { ... }

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

Paths resolve relative to the importing file. Modules are executed once and
cached; re-importing returns the same module.

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

`print len range str int float bool type abs min max sum sorted reversed
floor ceil round sqrt pow chr ord assert input keys values push money
map filter reduce join enumerate zip exit all any count unique hex bin oct
gcd factorial sign clamp list dict`

`range(n)`, `range(start, stop)`, `range(start, stop, step)`.
`list(x)` builds a list from a string/dict/list; `dict(pairs)` from `[[k, v], ...]`.

`"ab" * 3` and `[0] * 5` repeat strings and lists.

### Files, args, exit

```
write_file("out.txt", "hello")        # write (overwrite)
append_file("out.txt", " more")       # append
read_file("out.txt")                  # -> string
file_exists("out.txt")                # -> bool
args                                   # command-line args after the script (a list)
exit(0)                               # exit with a status code
```

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
