# Build the LarzOS Stage 1 kernel: a 64-bit multiboot kernel that boots to long
# mode and runs the *unmodified* Larzscript interpreter on bare metal.
#
#   make            # build larzkernel.elf
#   make iso        # build larzos.iso (GRUB) for QEMU / VirtualBox / real hardware
#   make test       # boot the ISO headlessly, drive the REPL over serial
#   make run        # boot interactively (your terminal is the serial console)
#
# Requires: gcc, ld, qemu-system-x86_64; ISO also needs grub-mkrescue + xorriso.

CC     := gcc
LD     := ld
INTERP := ../native/larzscript.c            # the official Larzscript interpreter
GCCINC := $(shell $(CC) -print-file-name=include)

# Freestanding + our headers replace libc. SSE stays ON (doubles use xmm);
# only -mno-red-zone (kernel code) and no stack protector / PIE.
FREE   := -m64 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pie \
          -mno-red-zone -fno-asynchronous-unwind-tables -O2 -nostdlib \
          -nostdinc -isystem $(GCCINC) -Ilibc -I.
CFLAGS := $(FREE) -Wall
AFLAGS := -m64 -ffreestanding -fno-pie
LDFLAGS := -m elf_x86_64 -T linker.ld -nostdlib -no-pie -z max-page-size=0x1000

KERNEL := larzkernel.elf
OBJS   := boot.o setjmp.o kernel.o libk.o larzscript.o

QEMU   := qemu-system-x86_64

all: $(KERNEL)

boot.o: boot.S
	$(CC) $(AFLAGS) -c boot.S -o boot.o
setjmp.o: setjmp.S
	$(CC) $(AFLAGS) -c setjmp.S -o setjmp.o
kernel.o: kernel.c console.h
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o
libk.o: libk.c console.h
	$(CC) $(CFLAGS) -c libk.c -o libk.o
# the interpreter, unmodified; its main() is renamed so the kernel is the entry
larzscript.o: $(INTERP)
	$(CC) $(FREE) -w -Dmain=larz_main -c $(INTERP) -o larzscript.o

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $(KERNEL) $(OBJS)
	@echo "built $(KERNEL)"

iso: $(KERNEL)
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/larzkernel.elf
	printf 'set timeout=0\nset default=0\nmenuentry "LarzOS" {\n  multiboot /boot/larzkernel.elf\n  boot\n}\n' > iso/boot/grub/grub.cfg
	grub-mkrescue -o larzos.iso iso 2>/dev/null
	@echo "built larzos.iso"

# Boot the ISO headlessly and drive the Larzscript REPL over serial from
# demo.lzin. Input is throttled (a boot delay + a pause per line) because the
# kernel uses polled serial with no flow control, so a burst would overflow the
# 16-byte UART FIFO. A human typing never hits this.
test: iso
	( sleep 4; while IFS= read -r l; do printf '%s\n' "$$l"; sleep 0.5; done < demo.lzin ) | \
	  timeout 90 $(QEMU) -m 128 -cdrom larzos.iso -display none -serial stdio \
	    -monitor none -no-reboot \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04 ; \
	  echo "(qemu exited $$?)"

run: iso
	$(QEMU) -m 128 -cdrom larzos.iso -display none -serial stdio -monitor none -no-reboot \
	        -device isa-debug-exit,iobase=0xf4,iosize=0x04

clean:
	rm -f $(OBJS) $(KERNEL) larzos.iso
	rm -rf iso

.PHONY: all iso test run clean
