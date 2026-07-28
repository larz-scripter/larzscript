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

Build with `CFLAGS+=-DLARZ_REPL` instead for an interactive `larz>` prompt over
serial (wallets, pay, f-strings, recursion, dicts, floats — all evaluate live).

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
  and a FILE layer. `stdin`/`stdout`/`stderr` route to the serial console;
  `fopen`/`fread`/`stat`/`opendir`/`readdir` are backed by a **read-only RAM
  filesystem** baked into the kernel (so `import`, `read_file`, and `listdir`
  work). Writes and OS/process/clock calls are stubbed — those builtins fail
  gracefully.
- **`rootfs/` + `mkramfs.py`** — the files baked into the image. `mkramfs.py`
  turns `rootfs/` into `ramfs_gen.c` (a C table of `{path, bytes, size}`); the
  kernel boots `/boot.lz`, which `import`s `/mathlib.lz` and reads `/motd.txt`.
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
RAM**). At the `larz>` prompt, type Larzscript. End a session with `exit(0)`.

## Known limitations (Stage 1)

- **Serial input has no flow control.** The kernel polls the 16-byte UART FIFO,
  so a *burst* of pasted/piped input can overflow it and drop bytes. A human
  typing is fine; `make test` throttles its scripted input for the same reason.
  A future revision adds interrupt-driven input with a ring buffer.
- The filesystem is a **read-only RAM image** baked into the kernel, so `import`,
  `read_file`, and `listdir` work but writes (`write_file`, `mkdir`) and
  process/env/clock builtins are stubbed. A writable + disk-backed FS is next.
- 64-bit multiboot kernels can't use QEMU's 32-bit-only `-kernel`; they boot via
  the GRUB ISO — the same path VirtualBox / real hardware use.

## Next

A PS/2 keyboard + framebuffer driver (local, non-serial use), then a real
storage/filesystem layer so `import` and files work — at which point `init.lz`
and `larzsh.lz` from [`../os/`](../os/) run directly on this kernel.
