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

## Full language (v0.2 — parity with the Python reference)

numbers · money (`$` = exact integer cents) · strings · booleans · nil ·
**lists** (`[1,2,3]`, indexing, `push`/`len`) · `let`/assign · `if`/`else` ·
`while` · **`for x in xs`** · `range` · functions + recursion + closures ·
**gas-metered functions** · **wallets** · `price` · `pay ... from ... to ...` ·
**`paywall` / `subscribe` / `has`** · `require` · `print` / `money`. Output is
byte-identical to the Python reference.

## Tests

```bash
sh native/run_tests.sh      # builds and checks against expected outputs
```

## License

MIT (c) larz-scripter
