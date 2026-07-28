# Build the LarzOS Stage 1 kernel (64-bit multiboot ELF, boots to long mode).
#
#   make            # build larzkernel.elf
#   make run        # boot it in QEMU (serial console on your terminal)
#   make test       # boot headlessly, feed scripted input, print output
#   make iso        # build a GRUB ISO for VirtualBox / real hardware (needs grub-mkrescue)
#
# Requires: gcc, ld, qemu-system-x86_64. ISO also needs grub-mkrescue + xorriso.

CC      := gcc
LD      := ld
# Freestanding, no libc, no SIMD/red-zone (no FPU state set up yet).
CFLAGS  := -m64 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pie \
           -mno-red-zone -mgeneral-regs-only \
           -Wall -Wextra -O2 -nostdlib
ASFLAGS := -m64 -ffreestanding -fno-pie
LDFLAGS := -m elf_x86_64 -T linker.ld -nostdlib -no-pie -z max-page-size=0x1000

KERNEL  := larzkernel.elf
OBJS    := boot.o kernel.o

QEMU     := qemu-system-x86_64

all: $(KERNEL)

boot.o: boot.S
	$(CC) $(ASFLAGS) -c boot.S -o boot.o

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $(KERNEL) $(OBJS)
	@echo "built $(KERNEL)"

# Interactive: your terminal becomes the serial console. Ctrl-A X to quit QEMU.
run: iso
	$(QEMU) -cdrom larzos.iso -display none -serial stdio -monitor none -no-reboot \
	        -device isa-debug-exit,iobase=0xf4,iosize=0x04

# Headless smoke test: boot the ISO, feed commands, capture output.
# QEMU's -kernel loader is 32-bit-only, so a 64-bit multiboot kernel is tested
# via the same GRUB ISO that VirtualBox/real hardware use. The leading newline
# absorbs the one byte the UART drops if input arrives before serial_init.
test: iso
	printf '\nabout\nbalance\nearn\nspend\nspend\nbalance\nhalt\n' | \
	  timeout 25 $(QEMU) -cdrom larzos.iso -display none -serial stdio \
	    -monitor none -no-reboot \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04 ; \
	  echo "(qemu exited $$? - isa-debug-exit reports 1 on clean halt)"

# A bootable ISO (GRUB) for VirtualBox / a USB stick / real hardware.
iso: $(KERNEL)
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/larzkernel.elf
	printf 'set timeout=0\nset default=0\nmenuentry "LarzOS" {\n  multiboot /boot/larzkernel.elf\n  boot\n}\n' > iso/boot/grub/grub.cfg
	grub-mkrescue -o larzos.iso iso
	@echo "built larzos.iso (boot in VirtualBox or: qemu-system-x86_64 -cdrom larzos.iso)"

clean:
	rm -f $(OBJS) $(KERNEL) larzos.iso
	rm -rf iso

.PHONY: all run test iso clean
