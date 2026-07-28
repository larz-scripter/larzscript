# LarzOS kernel — Stage 1

A from-scratch **64-bit** kernel that boots on bare metal with **no Linux
underneath**. This is the small machine-facing seed the [roadmap](../os/ROADMAP.md)
calls unavoidable: it talks to hardware directly so that everything above it can
eventually be Larzscript. It already carries the LarzOS identity — a money-native
compute wallet, live from the very bottom of the stack.

## What it does today

- **Multiboot1 kernel**, loaded by GRUB at 1 MiB.
- **Long-mode bring-up by hand** (`boot.S`): from 32-bit protected mode it builds
  page tables (identity-maps the low 1 GiB with 2 MiB pages), enables PAE, sets
  `EFER.LME`, turns on paging, loads a 64-bit GDT, and far-jumps into 64-bit code.
- **Freestanding C kernel** (`kernel.c`, no libc): 16550 UART serial console
  (COM1), VGA text output, port I/O, a line editor, and a tiny shell.
- **Shell**: `help about echo balance earn spend clear halt`.
- **Money-native at the kernel level**: a compute wallet (`balance`/`earn`/
  `spend`) that **fails closed** when credit runs out — the seed of compute being
  a paid, first-class kernel service.
- **Clean poweroff** via QEMU's isa-debug-exit, so it's testable in CI.

## Build & run

Needs `gcc`, `ld`, `qemu-system-x86_64`; the ISO also needs `grub-mkrescue` +
`xorriso`. (x86_64 hosts build this natively — no 32-bit multilib.)

```bash
make          # -> larzkernel.elf (64-bit multiboot ELF)
make iso      # -> larzos.iso  (GRUB rescue image)
make test     # boot the ISO headlessly, feed commands over serial, print output
make run      # boot interactively; your terminal is the serial console
```

Boot on a real machine or a VM by writing `larzos.iso` to a USB stick, or in
**VirtualBox**: new VM → type "Other/Unknown 64-bit" → attach `larzos.iso` as the
optical disc → start. (Enable the serial port, or watch the VGA text output.)

## Why 64-bit multiboot needs an ISO to test

QEMU's `-kernel` shortcut only understands *32-bit* multiboot ELFs, so a 64-bit
multiboot kernel is booted through GRUB — the same path VirtualBox and real
hardware use anyway. `make test`/`make run` therefore boot `larzos.iso`.

## Files

| File | Role |
|---|---|
| `boot.S` | multiboot header + 32-bit→64-bit long-mode bring-up + entry |
| `kernel.c` | freestanding kernel: serial, VGA, shell, compute wallet |
| `linker.ld` | links the ELF64 at 1 MiB |
| `Makefile` | build / iso / test / run |

## Next on the roadmap

Grow the kernel toward hosting the Larzscript userland: a real memory allocator,
PS/2 keyboard + framebuffer for local (non-serial) use, a syscall layer, then the
freestanding port of the Larzscript interpreter so `init.lz` / `larzsh.lz` run on
this kernel with no Linux. See [../os/ROADMAP.md](../os/ROADMAP.md).
