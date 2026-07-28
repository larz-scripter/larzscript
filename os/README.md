# LarzOS — an operating system written in Larzscript

LarzOS is an operating system whose userland — its init, shell, and utilities —
is written **entirely in Larzscript**, the money-native language in this repo.
It is money-native at the core: the OS meters compute in a built-in wallet, and
work that would overspend fails closed, exactly like `require` in the language.

This directory is **Stage 0**: the LarzOS userland running on top of Linux, so
you can use it today. The [roadmap](ROADMAP.md) is to lift the *same* Larzscript
userland onto our own kernel and, ultimately, boot it on real laptops and
servers.

## Boot it

```bash
larzscript os/init.lz          # "boot" LarzOS — init provisions the system and starts the shell
```

or start the shell directly:

```bash
larzscript os/larzsh.lz
```

You get an interactive prompt:

```
larzos:~/larzscript $100.00 $ sysinfo
LarzOS system information
-------------------------
kernel        Linux 4.14.186
machine       aarch64
larzscript    larzscript (native) 1.11.0
wallet        $100.00 compute credit
larzos:~/larzscript $99.99 $ help
```

## What's here

| File | Role | Analogous to |
|---|---|---|
| `init.lz` | first process: prepares the system, launches the shell | `/sbin/init` (PID 1) |
| `larzsh.lz` | the interactive shell | `bash` / `sh` |
| `bin/*.lz` | system utilities, each a Larzscript program | `/bin` coreutils |

`larzsh` builtins: `cd pwd ls cat echo mkdir rm touch write env whoami clear
history balance earn pkg about help exit`. Anything else is looked up in the bin
directory (`bin/<cmd>.lz`) and run with the interpreter, then falls back to an
external program.

## Money-native by design

Every LarzOS session carries a **compute wallet** (persisted at
`~/.larzos/wallet.cents`). Launching a program costs compute credit; when the
balance would go negative the launch is refused rather than run — the same
fail-closed guarantee the language gives `pay`/`require`. This is the seed of the
idea that makes LarzOS distinct: an OS where **compute is metered and paid for as
a first-class kernel service**, not bolted on.

## Where this is going

See **[ROADMAP.md](ROADMAP.md)**. In short: keep the userland exactly as it is
(pure Larzscript) and replace what it runs *on* — from Linux, to our own
freestanding kernel booted in an emulator, to real hardware.
