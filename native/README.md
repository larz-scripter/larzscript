# larzscript (native) — a standalone Larzscript in C

This is the **money-native language implemented in C**, compiled to a single
native executable that runs `.lz` files **with no dependency on Python, no pip,
and no runtime**. Just as "Python" is really CPython (an interpreter written in
C), this is Larzscript standing at the same ground level — a real standalone
language.

The `larzscript/` package (Python) is the readable **reference implementation**;
this C version is the **standalone toolchain**.

## Build

```bash
cc -O2 -o larzscript larzscript.c      # or: make
./larzscript program.lz
./larzscript --version
```

Zero third-party dependencies — libc only. One source file, ~700 lines. Builds
with any C compiler; the resulting binary links only libc (`ldd larzscript`).

## Install

```bash
make && sudo make install              # -> /usr/local/bin/larzscript
larzscript program.lz
```

## Example

```
wallet customer = $20.00
wallet platform
wallet creator
price premium = $9.00

fn buy(buyer) {
    require buyer.balance >= premium, "not enough funds"
    pay premium from buyer to platform
    pay premium * 0.8 from platform to creator   # revenue split, in the language
}

buy(customer)
print("creator earned:", creator.balance)        # $7.20
```

```bash
$ larzscript buy.lz
creator earned: $7.20
```

## What the native core (v0.1) covers

numbers · money (`$` = exact cents) · strings · booleans · nil · `let`/assign ·
`if`/`else` · `while` · functions + recursion + closures · **gas-metered
functions** · **wallets** · `price` · `pay ... from ... to ...` · `require` ·
`print` / `money`. Output is identical to the Python reference implementation.

Coming to the native build (already in the Python reference): lists, `for`
loops, and `paywall`/`subscribe`/`has`.

## License

MIT (c) larz-scripter
