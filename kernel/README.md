# LarzOS kernel — Stage 1

A from-scratch **64-bit** kernel that boots on bare metal with **no Linux
underneath** and runs the **unmodified Larzscript interpreter**. It boots a
multi-file Larzscript program straight from a baked-in RAM filesystem — `import`,
file reads, and money-native code all working on the hardware.

```
  LarzOS boot program - running from the RAM filesystem.
  Welcome to LarzOS - the money-native operating system.

[math]  primes<30  = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29]     (from an imported module)
[money] a marketplace sale, settled in-language:
        buyer=$60.00  seller=$36.00  platform=$4.00
[lang]  factorials  = [1, 2, 6, 24, 120, 720, 5040]
```

The published `larzos.iso` runs that demo and then launches **`larzsh`, the
LarzOS shell — which is itself written in Larzscript** (from [`../os/`](../os/)).
It runs *unmodified* on the kernel against a **writable in-memory filesystem**:

```
larzos:/ $100.00 $ mkdir /tmp/proj
larzos:/ $100.00 $ cd /tmp/proj
larzos:/tmp/proj $100.00 $ write readme.txt hello
larzos:/tmp/proj $100.00 $ cat readme.txt
hello
larzos:/tmp/proj $100.00 $ ls
readme.txt
```

That's **self-hosting**: the OS's shell, utilities, and boot program are all
Larzscript, running on our own kernel with no Linux. You type on a **real
keyboard** (or serial); output goes to a scrolling VGA console *and* the serial
line.

### Packages — extend the OS like `apt`

LarzOS ships **`pkg`**, an on-device package manager. Packages are Larzscript
modules that define `fn run(args)`; `larzsh` runs a command by importing its
package in-process and calling `run` (no subprocess needed on bare metal).
Installs go under `/home`, so — thanks to the disk (below) — **they persist
across reboots**:

```
larzos:/ $ pkg list
    banner      print text in a decorative box
    todo        a persistent to-do list (survives reboots)
    figlet      echo text as large block letters
    strutil     string helpers - a library
larzos:/ $ pkg install todo
installed todo
larzos:/ $ todo add ship larzos
larzos:/ $ todo list
1. ship larzos
```

After a reboot (with a disk attached), `todo` is still installed and its list is
intact.

### A money-native App Store (unique to LarzOS)

The thing no other OS has: because Larzscript is money-native, so is LarzOS's
**App Store**. `pkg store` browses a **remote** repo over the network; apps have
**prices**, and `pkg get` **pays from the OS wallet** — and *fails closed* with
no funds, the same guarantee the language gives `pay`/`require`. Every purchase
is written to a persistent **`ledger`**, and installs survive reboots.

```
larzos:/ $ pkg store
LarzOS App Store  @  10.0.2.2:28000     (wallet: $99.98)
  cowsay      free     an ascii cow says your message
  pro         $1.00    Pro power tools (premium)
larzos:/ $ pkg get pro
paid $1.00 for pro  (wallet: $98.95)
larzos:/ $ ledger
- $1.00  app: pro
larzos:/ $ pkg get enterprise
insufficient funds: enterprise costs $99999.00, wallet has $98.90
```

The whole thing — the store client, the wallet/ledger (`bank`), the apps — is
Larzscript; the kernel just provides an outbound TCP/HTTP client through the
virtual file `/net/get/<host>/<path>`.

### Server edition — init + services

The shipped build boots like a Linux server: **`init`** brings the system up with
a `[ ok ]` boot log (filesystem, RTC clock, network + IP, wallet, enabled
services) and hands off to a login shell (`root@<host>:<cwd> ($balance)$`).

```
  LarzOS  -  the money-native operating system
  booting larz-prod ...
  [  ok  ] mounted filesystem  (/ ramfs, /home persistent)
  [  ok  ] system clock synced from RTC
  [  ok  ] network up  (ip: 10.0.2.15)
  [  ok  ] services enabled: web
root@larz-prod:/ ($99.93)$ service start web
```

A **`service`** manager (systemctl-style: `list`/`enable`/`disable`/`start`,
config persisted) runs registered services; the **`web`** service is the
Larzscript HTTP server, which logs each request with a timestamp to a file you
read with **`logs`**. `hostname`, `date`, `uptime`, `netstat`, `ping`, `ledger`,
and the `pkg` App Store round out the admin toolkit — all Larzscript, all
persisting across reboots.

**Multi-user login.** The boot sequence is `init → login → shell`. On first boot
`login` creates `root` (password `larz`); accounts are djb2-hashed in
`/home/.larzos/users` and passwords are masked at the prompt. `useradd`,
`passwd`, `su`, `users`, `whoami`, and `id` manage accounts, each user gets a
`/home/<user>` (the shell starts there, shown as `~`), and the prompt shows the
logged-in user. Accounts persist and wrong passwords are rejected. **Home
directories are private** — the kernel tracks the current user (set at
login/`su`) and enforces it in the VFS, so one user can't read or `cd` into
another's `/home/<user>`; `root` sees all.

**Unix layout, `/proc`, and boot targets.** `init` builds an FHS-ish layout
(`/etc`, `/var/log`, `/bin`, `/root`, `/home`) with `/etc/os-release` and
`/etc/motd`. Virtual `/proc/meminfo` and `/proc/diskinfo` back **`free`** and
**`df`**; `uname`, `ps`, `motd`, `reboot`, and `shutdown` round out the tools.
Like systemd, LarzOS has **boot targets**: `target multi-user` (login, the
default) or `target web` — which, on the next boot, **auto-starts the web service
headless** (no login), so LarzOS boots straight into serving.

### Clock & networking

A CMOS/RTC driver gives real wall-clock time and a PIT-calibrated TSC gives a
monotonic clock, so `time()`/`clock()`/`sleep()` work and `date`/`uptime` are
real. A polled **RTL8139** driver plus a tiny **ARP/IPv4/ICMP** stack let LarzOS
talk to the network — exposed to Larzscript through virtual `/net/` files:

```
larzos:/ $ netstat
link: up
mac: 52:54:00:12:34:56
ip: 10.0.2.15
larzos:/ $ ping 10.0.2.2
10.0.2.2: reply
```

Boot with a NIC in QEMU: `-netdev user,id=n0 -device rtl8139,netdev=n0`
(VirtualBox: set the adapter type to PCnet/PCI or, ideally, add an RTL8139).

### A web server, in Larzscript, over TCP

A minimal single-connection **TCP** server (in `net.c`) handles the handshake and
exposes itself to Larzscript through virtual files: `read_file("/net/http/accept")`
blocks for a request, `write_file("/net/http/reply", resp)` sends it. So the whole
HTTP server — [`rootfs/webserver.lz`](rootfs/webserver.lz) — is Larzscript, and its
content is money-native:

```bash
# boot the server build with a forwarded port:
qemu-system-x86_64 -cdrom larzos.iso -device rtl8139,netdev=n0 \
  -netdev user,id=n0,hostfwd=tcp::8080-:80 ...
# then from the host:
curl http://localhost:8080/
#  Hello from LarzOS ... Money is native to the language: price pro = $5.00 -> $5.00
```

Build it with `EXTRA=-DLARZ_SERVER` (boots straight into the server), or just run
`webserver` inside `larzsh`.

**A money-native web service** — the unique angle, applied to the web. Visitors
**register/log in over HTTP** (cookie sessions), each gets **their own wallet**
(a `$10` signup bonus), and premium articles are paywalled per-user: an unpaid
request gets a real **`HTTP/1.0 402 Payment Required`**; `GET /buy?item=…` charges
**that user's** wallet (**fails closed**), and ownership is per-account. Accounts,
balances, and purchases persist across reboots (sessions are in-memory).

```
$ curl -c jar -b jar  .../register?user=alice&pw=secret   → $10 signup bonus
$ curl -c jar -b jar -i .../read?item=deepdive            → HTTP/1.0 402 Payment Required
$ curl -c jar -b jar    .../buy?item=deepdive             → paid $2.00; balance $8.00
$ curl -c jar -b jar -i .../read?item=deepdive            → HTTP/1.0 200 OK
# a different account (bob) still gets 402 for that article — ownership is per-user
```

Build variants: default runs `/boot.lz` then halts (deterministic, for
`make test`); `EXTRA=-DLARZ_SHELL` = demo then the shell (shipped);
`EXTRA=-DLARZ_DEMO_REPL` = demo then a raw `larz>` REPL; `EXTRA=-DLARZ_REPL` =
REPL only.

### Two independently-booted instances, trading real money

The clearest demonstration of "money is a kernel primitive, not a bolted-on
feature": two **separate** `qemu-system-x86_64` processes, each running its
own instance of this kernel with its own wallet, negotiate a real purchase
over a real (virtual) network - with no shared memory between them at all.
[`rootfs/demo_server.lz`](rootfs/demo_server.lz) serves one paywalled
article (simpler than `webserver.lz` above - no login/cookies, since the
outbound HTTP client can't send custom headers yet);
[`rootfs/demo_client.lz`](rootfs/demo_client.lz) fetches it, gets told to
pay, pays from its **own local wallet**, and re-fetches to prove it's now
unlocked. Money never crosses the wire - only HTTP does, exactly like real
payment rails - each side settles independently in response to the protocol.

```bash
# terminal 1 (the server) - a real host port forwarded into this VM's :80
qemu-system-x86_64 -m 128 -cdrom larzos.iso \
  -netdev user,id=n0,hostfwd=tcp::28080-:80 -device rtl8139,netdev=n0 \
  -display none -serial stdio -monitor none -no-reboot
# at the larzsh prompt: demo_server

# terminal 2 (the client) - its own independent instance, own SLIRP network.
# 10.0.2.2 is each VM's own gateway-to-host alias, which is how it reaches
# the OTHER vm's hostfwd'd port above - a real router hop, done by the host.
qemu-system-x86_64 -m 128 -cdrom larzos.iso \
  -netdev user,id=n1 -device rtl8139,netdev=n1 \
  -display none -serial stdio -monitor none -no-reboot
# at the larzsh prompt: demo_client 10.0.2.2:28080
```

Server transcript:
```
root@larzos:~ ($100.00)$ demo_server
demo server up. earnings starts at $0.00
served 402 /article  (earnings=$0.00)
served 200 /buy  (earnings=$2.00)
served 200 /article  (earnings=$2.00)
```

Client transcript (a genuinely separate kernel instance, separate wallet):
```
root@larzos:~ ($100.00)$ demo_client 10.0.2.2:28080
client (buyer) wallet before: $10.00
--- first fetch ---
payment required: $2.00 - GET /buy to pay
payment required - paying $2.00 from the buyer's own wallet
--- buy response (from the server) ---
paid - seller balance is now $2.00
--- second fetch ---
premium content unlocked - thanks for paying
client (buyer) wallet after: $8.00
client (spent) wallet after: $2.00
```

⚠ Both sides call `write_file("/dev/gas", "0")` as their first action -
`larzsh` gives every command a 300-tick budget meant for short interactive
commands, but a server is meant to run indefinitely and an outbound fetch's
busy-poll can plausibly exceed it too; `/dev/gas` resets the counter
immediately, so this disables metering before either script's real work
starts. ⚠ `/net/get/` strips HTTP headers (status line included) before
handing the response to Larzscript, so a client can only branch on **body
text**, not a numeric status code - `demo_client.lz` checks for the string
"payment required", not "402".

### A GUI on bare metal

The same `ui` module vocabulary the [browser build](../native/WEB.md) uses
(`ui.set_text`, `ui.on`) - a different backend behind an identical
Larzscript-level API. [`gfx.c`](gfx.c) switches the VGA card into linear
320x200x256 graphics (Mode 13h - a pure runtime register sequence, no
boot-time/Multiboot changes) and implements a small retained widget model:
`ui.label`/`ui.button` create widgets (there's no pre-existing markup to
select into, unlike a real DOM), `Tab` cycles keyboard focus, `Enter` fires
the focused widget's click handler. `ui.terminal(id,x,y,w,h)` adds a
scrolling log pane - once created, plain `print()` anywhere (this demo,
`webserver.lz`, `larzsh`, anything) shows up in it live, with no GUI-aware
code needed in whatever's printing: the mechanism is one hook in
`libk.c`'s `serial_putc()`, the single sink every std-stream write already
funnels through. [`rootfs/gui.lz`](rootfs/gui.lz) is the money-native demo:
a wallet balance, a `pay`-driven buy button, a `capability`-gated action
(`pay ... requires NAME` + `try`/`catch`), and a terminal pane showing each
action's outcome live - the bare-metal mirror of
[larzos.com/larzscript/gui/](https://larzos.com/larzscript/gui/).

```bash
make EXTRA=-DLARZ_GUI iso
qemu-system-x86_64 -m 128 -cdrom larzos.iso -display none -serial null \
  -monitor tcp:127.0.0.1:4444,server,nowait -no-reboot
```

No real display needed to verify it - same headless discipline as the
two-machine demo above. Connect to the monitor (e.g. `exec 3<>/dev/tcp/127.0.0.1/4444`
from bash) and drive it for real:

```
(qemu) screendump frame1.ppm     # initial state: "buy" focused
(qemu) sendkey ret               # click buy - pays $2, updates the balance label
(qemu) screendump frame2.ppm
(qemu) sendkey tab
(qemu) sendkey tab
(qemu) sendkey ret               # click "use premium" BEFORE unlocking - blocked
(qemu) screendump frame3.ppm     # "blocked: CapabilityError"
```

Verified this way through a full sequence: initial render, a successful
purchase, an attempt correctly blocked by the ungranted capability, granting
it, then a successful capability-gated purchase - five captured frames, real
wallet arithmetic changing on screen each time, not just a static render.
The terminal pane was verified the same way, separately: real `print()`
calls (not a special GUI-only call) from the button handlers showing up
live after each click, including a wrapped multi-line `CapabilityError`
message.

⚠ Keyboard-only in this first pass - no mouse/PS-2-IRQ12 work yet, and Tab
only cycles forward (the existing scancode table has no distinct shift+tab
ASCII value). ⚠ The embedded 8x8 font covers digits, uppercase A-Z, and the
punctuation this project's own UI text uses; lowercase folds to its
uppercase glyph.

## How it works

- **Multiboot1 ELF64**, loaded by GRUB at 1 MiB.
- **`boot.S`** — from 32-bit protected mode: zeroes the BSS, builds page tables
  (identity-maps the low 1 GiB with 2 MiB pages), enables PAE + long mode +
  paging, loads a 64-bit GDT, turns on **SSE** (the interpreter uses `double`/
  xmm), sets up an 8 MiB stack, and calls `kernel_main`.
- **`libk.c`** — a freestanding libc/libm replacement so the interpreter needs
  **zero source changes**: a K&R heap allocator over a 32 MiB arena, string/
  memory/ctype helpers, a streaming `printf` with a `%g` float formatter,
  `strtod`, freestanding math (`sqrt`/`floor`/`ceil`/`round`/`pow`/`exp`/`log`),
  and a FILE layer. `stdin`/`stdout`/`stderr` route to the console; the file and
  directory calls (`fopen`/`fread`/`fwrite`/`stat`/`mkdir`/`unlink`/`rename`/
  `chdir`/`getcwd`/`opendir`/`readdir`) are backed by a **writable in-memory
  filesystem** — a real directory tree with create/write/delete and a working
  current directory. So `import`, `read_file`, `write_file`, `mkdir`, `cd`, `ls`
  all work (files live only for the session; disk persistence is next). Process/
  exec/clock calls remain stubbed.
- **`rootfs/` + `mkramfs.py`** — the files baked into the image (the *initramfs*
  that seeds the writable FS at boot). `mkramfs.py` turns `rootfs/` into
  `ramfs_gen.c` (a C table of `{path, bytes, size}`). Ships `/boot.lz`,
  `/mathlib.lz`, `/motd.txt`, and **`/larzsh.lz`** (the shell).
- **ATA disk + persistence** (in `libk.c`) — a polled ATA PIO driver (primary
  master) plus **LarzFS**, a small on-disk format that serializes the `/home`
  subtree. `/home` is restored from disk at boot and re-synced on every write/
  `mkdir`/`rm`/`rename`, so files there — including the compute wallet —
  **persist across reboots**. No disk attached ⇒ it cleanly runs RAM-only.
- **Interrupts** (`interrupt.S` + `libk.c`) — a full IDT, an 8259 PIC remap, a
  **PIT timer at 100 Hz** (IRQ0 → a tick counter at `/proc/uptime`), and an
  **interrupt-driven keyboard** (IRQ1 fills a ring buffer). CPU exceptions print
  `CPU exception N … RIP` and halt instead of a silent triple fault.
- **Preemptive multitasking** — a round-robin **scheduler** switches tasks on the
  timer tick with a full context switch through the interrupt frame. The
  interpreter runs as one task; `/proc/tasks` shows background tasks advancing
  concurrently while the shell stays interactive.
- **PS/2 keyboard + VGA console** (in `libk.c`) — a polled scancode-set-1
  keyboard driver (shift/caps) gives **local input on real hardware / a VM**, and
  a scrolling VGA text console with a hardware cursor gives local output. Input
  blocks on *either* the serial line or the keyboard, so the same kernel works
  headless (serial) and locally (keyboard) with no rebuild.
- **`setjmp.S`** — a real x86_64 `setjmp`/`longjmp` (the interpreter's try/catch
  save/restores `jmp_buf` with `memcpy`, so `__builtin_setjmp` won't do).
- **`kernel.c`** — brings up the console and calls the interpreter's `main()`
  (renamed to `larz_main` via `-Dmain=`) in **REPL** mode; the REPL reads and
  evaluates Larzscript over the serial line.

The **only** non-Larzscript code is this small machine-facing seed (~2 files of
C/asm). Everything the language does — arithmetic, strings, lists, dicts,
functions, recursion, f-strings, comprehensions, and the money-native
`wallet`/`pay`/`require`/`gas` primitives — runs unmodified on top of it.

## Build & run

Needs `gcc`, `ld`, `qemu-system-x86_64`; the ISO also needs `grub-mkrescue` +
`xorriso`. x86_64 hosts build natively (no 32-bit multilib).

```bash
make          # -> larzkernel.elf (64-bit multiboot ELF)
make iso      # -> larzos.iso     (GRUB rescue image)
make test     # boot the ISO headlessly and run demo.lzin over serial
make run      # boot interactively; your terminal is the serial console
```

Boot on a **real 64-bit laptop** (write `larzos.iso` to a USB stick) or in
**VirtualBox** (type "Other/Unknown 64-bit", attach the ISO, give it **≥ 128 MiB
RAM**). You'll see the demo, then the `larzsh` shell — **type on the keyboard**.

For **persistent storage**, attach a hard disk: in QEMU add `-hda disk.img`
(create one with `dd if=/dev/zero of=disk.img bs=1M count=16`); in VirtualBox add
an IDE hard disk. Anything you put under `/home` survives reboots. Run `make
persist` for a two-boot demo (writes a file, reboots, reads it back).

## Known limitations (Stage 1)

- Input is **polled** (both the keyboard and the 16-byte UART FIFO), so a *burst*
  of pasted/piped *serial* input can overflow and drop bytes. A human typing (on
  the keyboard or a serial terminal) is fine. Interrupt-driven input with a ring
  buffer is a future revision.
- Files under `/home` persist to disk (LarzFS); `/tmp` and system files are
  in-memory. With no disk attached everything is RAM-only. Process/exec/clock
  builtins remain stubbed, so `larzsh`'s external-command and `pkg` paths are
  no-ops (its builtins all work).
- 64-bit multiboot kernels can't use QEMU's 32-bit-only `-kernel`; they boot via
  the GRUB ISO — the same path VirtualBox / real hardware use.

## Contributing

Want to add a `pkg`-installable command or a GUI app? See
[`CONTRIBUTING.md`](CONTRIBUTING.md) - both are plain Larzscript, no C
needed. Launching a GUI app on demand from `larzsh` (rather than only via a
dedicated boot target) isn't built yet - see "Next" below.

## Next

~~A PS/2 keyboard + framebuffer driver (local, non-serial use)~~ - done (see
"A GUI on bare metal" above): VGA Mode 13h graphics + a keyboard-driven
widget model, with the same `ui.*` vocabulary the browser build uses.
Mouse/PS-2-IRQ12 support and a shift+tab / reverse-focus path are the
natural next steps there, not yet built. Also not yet built: launching a
GUI app on demand from `larzsh` (would need a "revert VGA to text mode"
capability so control can hand back to the shell) - today a GUI app is its
own dedicated boot target, per `CONTRIBUTING.md`.
