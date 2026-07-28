# LarzOS kernel — Stage 1

A from-scratch **64-bit** kernel that boots on bare metal with **no Linux
underneath** and runs the **unmodified Larzscript interpreter** as its shell.
This is the milestone the roadmap was aiming at: Larzscript running directly on
the hardware, money-native and all.

```
larz> wallet a = $20.00
larz> price coffee = $3.50
larz> pay coffee from a to shop
larz> print(f"a={a.balance}  shop={shop.balance}")
a=$16.50  shop=$3.50
larz> print([x*x for x in range(6)])
[0, 1, 4, 9, 16, 25]
larz> print("fib(15) =", fib(15))
fib(15) = 610
```

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
  and a FILE layer that routes `stdin`/`stdout`/`stderr` to the serial console.
  OS-facing calls (files, dirs, env, exec, clocks) are stubbed — bare metal has
  no filesystem yet, so those Larzscript builtins fail gracefully.
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
- **No filesystem**, so `import`, `read_file`, `run`, `listdir`, `env`, and the
  clock builtins are stubbed. Core language + money-native primitives all work.
- 64-bit multiboot kernels can't use QEMU's 32-bit-only `-kernel`; they boot via
  the GRUB ISO — the same path VirtualBox / real hardware use.

## Next

A PS/2 keyboard + framebuffer driver (local, non-serial use), then a real
storage/filesystem layer so `import` and files work — at which point `init.lz`
and `larzsh.lz` from [`../os/`](../os/) run directly on this kernel.
