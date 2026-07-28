# larzscript (native) — a standalone Larzscript in C

The **money-native language implemented in C**, compiled to a single native
executable that runs `.lz` files with **no Python, no pip, no runtime** — the
release binaries are *statically linked*, so they depend on nothing at all (not
even libc). Just as "Python" is really CPython (an interpreter written in C),
this is Larzscript at the same ground level: a real, standalone language.

The `larzscript/` package (Python) is the readable **reference implementation**;
this C build is the **standalone toolchain**, and it produces byte-identical
output.

## Download a prebuilt binary (no compiler needed)

Grab the static binary for your platform from the
[latest release](https://github.com/larz-scripter/larzscript/releases), then:

```bash
chmod +x larzscript-linux-x86_64
./larzscript-linux-x86_64 program.lz
```

It has zero dependencies — copy it anywhere and run it.

## Or build it yourself

```bash
cc -O2 -o larzscript native/larzscript.c        # or: make -C native
./larzscript program.lz
./larzscript --ledger program.lz                # also print the ledger + gas
./larzscript repl                               # interactive REPL
sudo make -C native install                     # -> /usr/local/bin/larzscript
```

One source file, ~850 lines, libc only. Runs on aarch64 and x86_64.

## Example

```
wallet customer = $20.00
wallet platform
paywall pro = $9.00 / month to platform
subscribe customer to pro

fn premium(user) {
    require user has pro, "subscribe to Pro first"
    return "premium content"
}

let basket = [$3.50, $2.00]
for item in basket { pay item from customer to platform }
print(premium(customer))
print("customer left:", customer.balance)
```

## A general-purpose program

```
# fizzbuzz - a real, general-purpose language
for i in range(1, 16) {
    if i % 15 == 0 { print("fizzbuzz") }
    else if i % 3 == 0 { print("fizz") }
    else if i % 5 == 0 { print("buzz") }
    else { print(i) }
}

let counts = {}                          # dictionaries
for word in "the cat the dog the bird".split(" ") {
    counts[word] = counts.get(word, 0) + 1
}
print(counts)                            # {the: 3, cat: 1, dog: 1, bird: 1}

let nums = [5, 3, 8, 1]
nums.sort()
print("sorted", nums, "sum", sum(nums), "max", max(nums))
```

## Language (v1.1 — a general-purpose standalone language)

**Values:** numbers · strings · booleans · nil · **lists** · **dicts** ·
**functions** (incl. anonymous `fn(x){ ... }` **lambdas**) ·
money (`$` = exact integer cents) · wallets.

**Operators:** `+ - * / % // **` · `== != < <= > >=` · `and or not` ·
**`in`** (list/dict/string membership) · `has`.

**Syntax:** `let` / assign / **compound assign** (`+= -= *= /= %=`) ·
`if`/`else`/`else if` · `while` · **`for x in`** lists/dicts/strings ·
**`break`** / **`continue`** · **`try` / `catch` / `throw`** · functions +
recursion + closures + lambdas · **gas-metered functions** · list & dict
literals · indexing · **slicing** (`a[i:j]`, negatives) · **element assignment**
(`a[i] = x`, `d[k] = v`) · **f-strings** (`f"hi {name}, {1+2}"`) · errors
reported **with line numbers** and **catchable**.

**Methods:** list `push`/`pop`/`insert`/`sort`/`reverse`/`contains`/`index` ·
dict `keys`/`values`/`has`/`get`/`remove` · string
`upper`/`lower`/`strip`/`split`/`replace`/`contains`/`starts_with`/`ends_with`/`find`.

**Builtins:** `print len range str int float bool type abs min max sum sorted
reversed floor ceil round sqrt pow chr ord assert input keys values push money
map filter reduce join enumerate`.

**Money-native:** `price` · `pay ... from ... to ...` · `require` ·
`paywall` / `subscribe` / `has`.

## Tests

```bash
sh native/run_tests.sh      # builds and checks against expected outputs
```

## License

MIT (c) larz-scripter
