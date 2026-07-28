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

Build variants: default runs `/boot.lz` then halts (deterministic, for
`make test`); `EXTRA=-DLARZ_SHELL` = demo then the shell (shipped);
`EXTRA=-DLARZ_DEMO_REPL` = demo then a raw `larz>` REPL; `EXTRA=-DLARZ_REPL` =
REPL only.

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
RAM**). You'll see the demo, then a `larz>` prompt — **type Larzscript on the
keyboard**. End a session with `exit(0)`.

## Known limitations (Stage 1)

- Input is **polled** (both the keyboard and the 16-byte UART FIFO), so a *burst*
  of pasted/piped *serial* input can overflow and drop bytes. A human typing (on
  the keyboard or a serial terminal) is fine. Interrupt-driven input with a ring
  buffer is a future revision.
- The filesystem is **in-memory** (seeded from the baked initramfs), so files
  created at runtime **do not persist across a reboot**. Disk-backed storage is
  next. Process/exec/clock builtins remain stubbed, so `larzsh`'s external-command
  and `pkg` paths are no-ops (its builtins all work).
- 64-bit multiboot kernels can't use QEMU's 32-bit-only `-kernel`; they boot via
  the GRUB ISO — the same path VirtualBox / real hardware use.

## Next

A PS/2 keyboard + framebuffer driver (local, non-serial use), then a real
storage/filesystem layer so `import` and files work — at which point `init.lz`
and `larzsh.lz` from [`../os/`](../os/) run directly on this kernel.
