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

### Stage 1 — our own kernel, Larzscript as userland  🚧 (skeleton BOOTS)
A minimal freestanding kernel (a small seed of C/asm: boot, memory manager,
interrupts, keyboard + screen + serial drivers, a syscall layer) with the
Larzscript interpreter **ported to run with no libc and no Linux** — its own heap
on kernel-provided memory, its I/O through kernel syscalls instead of `fopen`/
`printf`. Then the *same* `init.lz` / `larzsh.lz` from Stage 0 run as the first
Larzscript process. "Boots into a Larzscript prompt."

**Done so far** (see [`kernel/`](../kernel/)): a real **64-bit** multiboot kernel
that GRUB loads at 1 MiB and that brings the CPU up into **long mode by hand**
(page tables + PAE + paging + 64-bit GDT), then runs a serial shell — with the
money-native **compute wallet already at the kernel level** (`balance`/`earn`/
`spend`, fails closed). Builds a `larzos.iso` bootable in QEMU, **VirtualBox**, or
off a USB stick on a real 64-bit laptop.

- Build + test host: **srv66** (x86_64, root, `gcc`, `ld`, `grub-mkrescue`,
  `qemu-system-x86_64`, `xorriso`). Tested **headlessly** over QEMU's serial
  console. Note: QEMU's `-kernel` is 32-bit-only, so 64-bit kernels boot via the
  GRUB ISO — the same path VirtualBox / real hardware use.
- First milestone ✅: boot → long mode → serial shell.
- Next: a memory allocator, PS/2 keyboard + framebuffer, a syscall layer, then
  the interpreter port — replace each libc dependency (`malloc`, `printf`,
  `fgets`, `fopen`) with a freestanding runtime the kernel provides.

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
