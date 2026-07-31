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
CFLAGS := $(FREE) -Wall $(EXTRA)     # e.g. make EXTRA=-DLARZ_REPL for an interactive prompt
AFLAGS := -m64 -ffreestanding -fno-pie
LDFLAGS := -m elf_x86_64 -T linker.ld -nostdlib -no-pie -z max-page-size=0x1000

KERNEL := larzkernel.elf
OBJS   := boot.o setjmp.o interrupt.o kernel.o libk.o net.o gfx.o ramfs_gen.o larzscript.o
ROOTFS := $(wildcard rootfs/* rootfs/repo/*/*)

QEMU   := qemu-system-x86_64

all: $(KERNEL)

boot.o: boot.S
	$(CC) $(AFLAGS) -c boot.S -o boot.o
setjmp.o: setjmp.S
	$(CC) $(AFLAGS) -c setjmp.S -o setjmp.o
interrupt.o: interrupt.S
	$(CC) $(AFLAGS) -c interrupt.S -o interrupt.o
kernel.o: kernel.c console.h
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o
libk.o: libk.c console.h
	$(CC) $(CFLAGS) -c libk.c -o libk.o
net.o: net.c net.h
	$(CC) $(CFLAGS) -c net.c -o net.o
gfx.o: gfx.c gfx.h
	$(CC) $(CFLAGS) -c gfx.c -o gfx.o
# bake rootfs/ into an in-kernel RAM filesystem
ramfs_gen.c: mkramfs.py $(ROOTFS)
	python3 mkramfs.py rootfs > ramfs_gen.c
ramfs_gen.o: ramfs_gen.c
	$(CC) $(CFLAGS) -c ramfs_gen.c -o ramfs_gen.o
# the interpreter, unmodified; its main() is renamed so the kernel is the entry
larzscript.o: $(INTERP)
	$(CC) $(FREE) -w -Dmain=larz_main -c $(INTERP) -o larzscript.o

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $(KERNEL) $(OBJS)
	@echo "built $(KERNEL)"

iso: $(KERNEL)
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/larzkernel.elf
	printf 'set timeout=0\nset default=0\nmenuentry "LarzOS" {\n  insmod vbe\n  insmod video\n  set gfxpayload=1024x768x32\n  multiboot /boot/larzkernel.elf\n  boot\n}\n' > iso/boot/grub/grub.cfg
	grub-mkrescue -o larzos.iso iso 2>/dev/null
	@echo "built larzos.iso"

# Boot the ISO headlessly. The kernel runs /boot.lz from the RAM filesystem -
# deterministic, no serial input needed. (Build with CFLAGS+=-DLARZ_REPL for an
# interactive larz> prompt instead.)
test: iso
	timeout 40 $(QEMU) -m 128 -cdrom larzos.iso -display none -serial stdio \
	    -monitor none -no-reboot \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04 ; \
	  echo "(qemu exited $$?)"

run: iso
	$(QEMU) -m 128 -cdrom larzos.iso -display none -serial stdio -monitor none -no-reboot \
	        -device isa-debug-exit,iobase=0xf4,iosize=0x04

# Two-boot persistence demo: write a file to /home, reboot, read it back.
persist:
	$(MAKE) clean >/dev/null 2>&1; $(MAKE) EXTRA=-DLARZ_SHELL >/dev/null && $(MAKE) iso >/dev/null
	dd if=/dev/zero of=disk.img bs=1M count=16 2>/dev/null
	@echo "--- boot 1: write /home/note.txt, then exit ---"
	@( sleep 6; printf 'cd /home\nwrite note.txt hello-from-a-previous-boot\nexit\n'; sleep 2 ) | \
	  timeout 60 $(QEMU) -m 128 -cdrom larzos.iso -hda disk.img -display none -serial stdio \
	    -monitor none -no-reboot -device isa-debug-exit,iobase=0xf4,iosize=0x04 2>/dev/null | tail -2
	@echo "--- boot 2: same disk, read it back ---"
	@( sleep 6; printf 'cat /home/note.txt\nexit\n'; sleep 2 ) | \
	  timeout 60 $(QEMU) -m 128 -cdrom larzos.iso -hda disk.img -display none -serial stdio \
	    -monitor none -no-reboot -device isa-debug-exit,iobase=0xf4,iosize=0x04 2>/dev/null | grep -A1 "cat /home"

clean:
	rm -f $(OBJS) $(KERNEL) larzos.iso ramfs_gen.c disk.img
	rm -rf iso

.PHONY: all iso test run clean
