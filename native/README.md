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

It has zero dependencies — copy it anywhere and run it. On Windows, grab
`larzscript-windows-x86_64.exe` (or run `install.ps1` from the repo root to
also add it to your PATH) — it's statically linked too, so it only touches
`kernel32.dll`/`msvcrt.dll`, present on every Windows install.

## Or build it yourself

```bash
cc -O2 -o larzscript native/larzscript.c        # or: make -C native
./larzscript program.lz
./larzscript -e "print(1 + 2)"                  # run a snippet
./larzscript --ledger program.lz                # also print the ledger + gas
./larzscript repl                               # interactive REPL
./larzscript fmt program.lz                     # canonical auto-formatter (idempotent)
sudo make -C native install                     # -> /usr/local/bin/larzscript
make -C native windows                          # cross-compile larzscript.exe (needs mingw-w64)
```

The **formatter** (`larzscript fmt`) reprints a program in canonical style
(consistent indentation and spacing). It's idempotent like `gofmt` — running it
twice changes nothing, and formatted code behaves identically.

One source file, ~2400 lines, libc only. Runs on Linux (aarch64, x86_64) and Windows (x86_64).

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

## Language (v1.2 — a general-purpose standalone language)

Full reference: **[LANGUAGE.md](LANGUAGE.md)**.

**Values:** numbers · strings · booleans · nil · **lists** · **dicts** ·
**functions** (incl. anonymous `fn(x){ ... }` **lambdas**) · **modules** ·
money (`$` = exact integer cents) · wallets.

**Operators:** `+ - * / % // **` · `== != < <= > >=` · `and or not` ·
**`in`** (list/dict/string membership) · `has` · **`cond ? a : b`** (ternary).

**Syntax:** `let` / assign / **compound assign** (`+= -= *= /= %=`) ·
`if`/`else`/`else if` · `while` · **`for x in`** lists/dicts/strings ·
**`break`** / **`continue`** · **`try` / `catch` / `throw`** · functions with
**default parameters** + recursion + closures + lambdas · **gas-metered
functions** · **`import "file.lz" as m`** (relative paths, cached) · list & dict
literals · indexing · **slicing** (`a[i:j]`, negatives) · **element assignment**
(`a[i] = x`, `d[k] = v`) · **comprehensions** (`[x*x for x in xs if c]`,
`{k: v for x in xs}`) · **f-strings** (`f"hi {name}, {1+2}"`) · `;` separates
statements · errors reported **with line numbers** and **catchable**.

**Methods:** list `push`/`pop`/`insert`/`sort`/`reverse`/`contains`/`index` ·
dict `keys`/`values`/`has`/`get`/`remove` · string
`upper`/`lower`/`strip`/`split`/`replace`/`contains`/`starts_with`/`ends_with`/`find`.

**Builtins:** `print len range str int float bool type abs min max sum sorted
reversed floor ceil round sqrt pow chr ord assert input keys values push money
map filter reduce join enumerate zip exit read_file write_file append_file
file_exists all any count unique hex bin oct gcd factorial sign clamp list dict`
· `args` (command-line args) · `"ab"*3` / `[0]*5` repetition.

**OS / scripting:** `env run capture cwd chdir listdir mkdir remove rename time
clock sleep` — enough to write real OS scripts.

## Packages (`larzpkg`)

Larzscript has a package manager — written in Larzscript itself — that installs
libraries from a registry into `~/.larzscript/lib`, where `import` finds them:

```bash
larzscript tools/larzpkg.lz install mathx     # git-clones the package
larzscript tools/larzpkg.lz list
larzscript tools/larzpkg.lz search math
```

```
import "mathx" as m
print(m.mean([1, 2, 3, 4]), m.fib(10))
```

Import resolution searches: relative to the file, then `$LARZSCRIPT_PATH`, then
`~/.larzscript/lib`, then `./lz_modules`. Packages don't have to live in the
official monorepo — `registry.txt` entries can point straight at anyone's
own git repo, versioned and owned by them. Publish your own:
`larzscript tools/larzpkg.lz publish <your-git-url>` — see
[`../packages/PUBLISHING.md`](../packages/PUBLISHING.md).

**Editor support:** syntax highlighting for VS Code and Vim — see
[`../editors/`](../editors/).

**Money-native:** `price` · `pay ... from ... to ...` · `require` ·
`paywall` / `subscribe` / `has`.

## Modules

```
# mathx.lz
fn square(x) { return x * x }
let PI = 3.14159

# main.lz
import "mathx.lz" as m
print(m.square(5), m.PI)      # 25 3.14159
```

## Browser (WebAssembly)

The same `larzscript.c` also compiles to WebAssembly and runs client-side in a
real browser tab - `wallet`/`pay`/`capability` plus a small `ui` module
(`ui.set_text`, `ui.on`, `ui.fetch`, ...) for wiring Larzscript straight to the
DOM, no JavaScript required for the logic. See **[WEB.md](WEB.md)** for the
build, the `ui.*` API, and a live example. Try it:
[larzos.com/larzscript/gui/](https://larzos.com/larzscript/gui/).

## Memory

A **precise mark-sweep garbage collector** reclaims container objects (lists,
dicts, environments, closures, wallets, paywalls), so long-running programs and
loops stay memory-bounded instead of growing without limit. It runs between
statements and protects in-flight temporaries with a temp-root stack. The GC is
verified under AddressSanitizer with collection forced on every statement.
(Strings are not yet collected.)

## Tests

```bash
sh native/run_tests.sh      # runs each test; also checks GC + formatter invariants
```

## License

MIT (c) larz-scripter
