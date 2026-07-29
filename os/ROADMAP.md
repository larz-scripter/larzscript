# LarzOS roadmap — from a Larzscript userland to a bare-metal OS

The goal: **an operating system written in Larzscript that boots on real laptops
and servers.** This document is honest about how to get there, including the one
part that cannot itself start out as Larzscript.

## The one hard truth

An *interpreter* can never be the very bottom layer of a computer. Something has
to run the first CPU instruction after power-on and talk directly to hardware
(CPU control registers, the MMU, interrupt controllers, the keyboard, the disk).
Larzscript today is an interpreter — a C program. So the first hardware-facing
code must be written in something that compiles to native machine code.

Every "language X operating system" solves this the same way: the language gains
a **native-code compiler**, and a tiny seed of assembly/C brings the machine up
far enough to run code in that language. TempleOS did it with HolyC; Rust and Go
hobby kernels do it with their compilers. LarzOS follows the same path — and in
the meantime keeps the *entire userland* (init, shell, every utility) in
Larzscript, which is already true in Stage 0.

## Stages

### Stage 0 — Larzscript userland on Linux ✅ (this directory)
`init.lz` + `larzsh.lz` + `bin/*.lz`. The complete OS personality — everything
you interact with — written in Larzscript, running on the Linux kernel. Usable
today. This is the userland that every later stage will carry unchanged.

### Stage 1 — our own kernel, Larzscript as userland  🚧 (interpreter RUNS on bare metal)
A minimal freestanding kernel (a small seed of C/asm: boot, memory manager,
interrupts, keyboard + screen + serial drivers, a syscall layer) with the
Larzscript interpreter **ported to run with no libc and no Linux** — its own heap
on kernel-provided memory, its I/O through kernel syscalls instead of `fopen`/
`printf`. Then the *same* `init.lz` / `larzsh.lz` from Stage 0 run as the first
Larzscript process. "Boots into a Larzscript prompt."

**Done so far** (see [`kernel/`](../kernel/)): a real **64-bit** multiboot kernel
that GRUB loads at 1 MiB, brings the CPU up into **long mode by hand** (page
tables + PAE + paging + 64-bit GDT + SSE), and then **runs the unmodified
Larzscript interpreter on bare metal** as its shell — no Linux underneath. A
freestanding runtime (`libk.c`: allocator, string/printf/`%g`, `strtod`, math,
setjmp, a serial-backed FILE layer) satisfies the interpreter's libc needs with
**zero changes to the language source**; OS-facing builtins are stubbed until
there's a filesystem. Verified live: `wallet`/`pay`/`require`, f-strings,
comprehensions, recursion (`fib`), dicts, floats — all evaluating at a `larz>`
prompt. Builds `larzos.iso`, bootable in QEMU, **VirtualBox**, or off a USB stick
on a real 64-bit laptop (give it ≥ 128 MiB RAM).

- Build + test host: **srv66** (x86_64, root, `gcc`, `ld`, `grub-mkrescue`,
  `qemu-system-x86_64`, `xorriso`). Tested **headlessly** over QEMU's serial
  console. QEMU's `-kernel` is 32-bit-only, so 64-bit kernels boot via the GRUB
  ISO — the same path VirtualBox / real hardware use.
- Milestones ✅: boot → long mode → interpreter on bare metal → RAM filesystem →
  PS/2 keyboard + scrolling VGA (local I/O) → writable filesystem + `larzsh` =
  self-hosting → ATA disk + LarzFS = persistent storage → **an `apt`-like
  package manager (`pkg`).** Larzscript packages install onto the running OS and
  extend it (new commands + libraries), persisting across reboots — LarzOS grows
  by installing packages, the way a Linux server does.
- Also ✅: **a real clock** (RTC wall-time + PIT-calibrated TSC → `time()`/
  `clock()`/`sleep()`, `date`/`uptime`), and **networking** — an RTL8139 driver
  + ARP/IPv4/ICMP stack that pings the gateway, exposed to Larzscript via virtual
  `/net/` files (`netstat`, `ping`). LarzOS talks to the network on bare metal.
- Also ✅: **TCP + a web server written in Larzscript** — a minimal TCP server
  (`net.c`) handles the handshake and hands requests to `webserver.lz` via
  `/net/http/*` files; verified with `curl` from the host returning money-native
  pages over TCP. A real, if minimal, server on bare metal — no Linux.
- Also ✅: **outbound TCP + a money-native App Store** — `pkg store`/`pkg get`
  fetch from a remote repo over the network, apps have **prices**, buying **pays
  from the OS wallet and fails closed** without funds, purchases hit a persistent
  **`ledger`**, and installs survive reboots. This is the unique thing: money is
  a first-class OS primitive, so the app store is money-native. Verified over
  QEMU.
- Also ✅: **server edition** — an `init` with a `[ ok ]` boot log, a systemctl-
  style **`service`** manager, a persisted **`hostname`** in the login prompt, and
  a web server that **logs requests** (viewable with `logs`). Boots like a Linux
  server; all state persists across reboots.
- Also ✅: **multi-user login** — `init → login → shell`, hashed + masked
  passwords, `useradd`/`passwd`/`su`/`users`, session-aware prompt; accounts
  persist and wrong passwords are rejected.
- Also ✅: a **Unix filesystem layout** (`/etc`, `/var/log`, per-user
  `/home/<user>`), **`/proc`** (`free`/`df`), systemd-style **boot targets**
  (`multi-user` login or `web` = auto-start the web server headless), and admin
  tools (`uname`/`ps`/`motd`/`reboot`/`shutdown`). Boots straight into serving
  when `target web` is set.
- Also ✅: a **money-native web SERVICE** — HTTP accounts (register/login, cookie
  sessions), a **per-user wallet** with a signup bonus, and per-user HTTP 402
  paywalls that charge that user's balance (fails closed). Accounts, balances and
  purchases persist across reboots. A real monetized SaaS on bare metal.
- Also ✅: **file permissions** — home-directory privacy enforced by the kernel
  (each `/home/<user>` is private, `root` sees all), tracking the current user
  via `/dev/user` (set at login/`su`).
- Also ✅: **real interrupts** — an IDT, an 8259 PIC remap, a **100 Hz PIT timer**
  IRQ (`/proc/uptime`), an **interrupt-driven keyboard**, and CPU exception
  handlers (print + halt instead of a silent triple fault). The foundation a real
  OS runs on.
- Also ✅: **preemptive multitasking** — a round-robin, context-switching
  scheduler on the timer tick. The interpreter is one task; background tasks run
  concurrently (`/proc/tasks`). A genuine scheduler on bare metal.
- Next: run **multiple interpreter tasks** (e.g. the web server as a background
  task alongside the shell — needs the interpreter made reentrant or per-task
  state); a multi-connection web server; per-process gas accounting on the timer;
  full path resolution in the permission check. Userland stays 100% Larzscript.

### Stage 2 — Larzscript native compiler (`larzc`)
Give Larzscript a **native-code backend** (Larzscript → x86_64/aarch64, most
practically via emitting C and compiling with gcc first, then direct codegen).
Once Larzscript compiles to native code, we rewrite the Stage-1 C seed *in
Larzscript* piece by piece and shrink the non-Larzscript core toward zero — the
TempleOS-style endgame where the OS really is "all Larzscript."

### Stage 3 — real hardware
Boot on actual laptops and servers: a real bootloader path (GRUB multiboot for
BIOS, a UEFI stub for modern machines), plus the long tail of real drivers
(AHCI/NVMe storage, USB, framebuffer, network). This is the largest and longest
stage; QEMU parity comes first, hardware after.

## Money-native, throughout

The differentiator is not "another hobby OS" — it's that LarzOS treats **money
and compute-metering as kernel services**, because its language does. The
compute wallet in Stage 0 becomes, by Stage 1, a real per-process accounting the
kernel enforces (gas-metered scheduling, pay-per-execution, safe untrusted code)
— capabilities most kernels bolt on with great effort and Larzscript has in its
grammar from the start.

## Immediate next steps

1. Grow the Stage 0 userland: more `bin/*.lz` utilities, pipes/redirection in
   `larzsh`, a tiny package/app model via `larzpkg`.
2. Stand up the Stage 1 kernel skeleton on srv66 under QEMU (boot + serial
   "hello" + keyboard), in a new `kernel/` directory.
3. Begin the freestanding port of the interpreter (identify and replace each
   libc touch-point).
