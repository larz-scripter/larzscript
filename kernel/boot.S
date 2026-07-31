/* boot.S - multiboot1 header + long-mode bring-up for the LarzOS kernel.
 *
 * GRUB / QEMU -kernel load this ELF64 at 1 MiB and enter at _start in 32-bit
 * protected mode with paging OFF. To reach 64-bit we must, by hand:
 *   1. build page tables that identity-map the low 1 GiB (2 MiB pages),
 *   2. enable PAE, load CR3, set EFER.LME, turn on paging,
 *   3. load a 64-bit GDT and far-jump into 64-bit code.
 * Then we set up the stack and call kernel_main (compiled -m64).
 */

.set MB_MAGIC,    0x1BADB002
.set MB_FLAGS,    0x00000007   /* bit0 align modules, bit1 mem map, bit2 video mode request (below) */
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM
/* header_addr/load_addr/load_end_addr/bss_end_addr/entry_addr - the
 * Multiboot1 header is a FIXED byte layout regardless of which flag bits
 * are set: these 5 fields always occupy offset 12-31, and mode_type/width/
 * height/depth always come at a fixed offset 32 right after them - flags
 * bit 16 (not set here) only says whether their VALUES should be trusted,
 * not whether they're physically present. Getting this wrong doesn't fail
 * cleanly: GRUB read 20 bytes of real .text instruction bytes as if they
 * were these fields and choked on a garbage "mode type" - a real, verified
 * failure (kernel/README.md), not a hypothetical one. Zero is correct here
 * since bit 16 isn't set, so GRUB ignores their values anyway. */
.long 0        /* header_addr (unused - bit 16 not set) */
.long 0        /* load_addr */
.long 0        /* load_end_addr */
.long 0        /* bss_end_addr */
.long 0        /* entry_addr */
/* Present because flags bit 2 IS set. GRUB honors this on a best-effort
 * basis and reports what it actually set in the info struct it hands
 * back - read that back in kernel_main, don't assume these exact numbers
 * were granted. */
.long 0        /* mode_type: linear graphics */
.long 1024     /* width */
.long 768      /* height */
.long 32       /* depth (bits per pixel) */

.section .bss
.align 4096
pml4:  .skip 4096
pdpt:  .skip 4096
/* 4 page directories = 4 GiB identity-mapped (2 MiB pages each), not just
 * the original 1 GiB. Needed because the framebuffer's physical address is
 * a PCI BAR that QEMU places near the 4 GiB boundary (observed at
 * 0xfd000000) - well above the low 1 GiB the kernel's own code/data/heap
 * live in. A REAL triple fault was hit here before this fix: accessing an
 * unmapped page faults, and with no page-fault handler installed this
 * early, that escalates silently to a double fault, then a triple fault
 * (CPU reset) - zero serial output at all, because it happens mid-write,
 * before there's anything to report it with. Confirmed via QEMU's
 * `-d int,cpu_reset`: CR2=0xfd000000 (the exact framebuffer address),
 * "check_exception old: 0xe new 0xd" (an unhandled #PF escalating to
 * #GP/#DF), then "Triple fault" - not a guess, a captured trace. */
pd:    .skip 4096 * 4
.align 16
stack_bottom:
.skip 0x800000                         /* 8 MiB kernel stack - the interpreter recurses deeply */
stack_top:
.align 4
mb_info_ptr: .long 0                   /* the Multiboot info pointer GRUB hands in %ebx at entry -
                                         * preserved here (see _start) before anything else can
                                         * clobber it, then passed to kernel_main */

.section .rodata
.align 8
gdt64:
    .quad 0                                              /* null descriptor */
    .set GDT_CODE, . - gdt64
    /* 64-bit code segment: present, descriptor-type, executable, long-mode */
    .quad (1<<43) | (1<<44) | (1<<47) | (1<<53)
gdt64_pointer:
    .word . - gdt64 - 1
    .quad gdt64

.section .text
.code32
.global _start
.type _start, @function
_start:
    mov $stack_top, %esp

    /* zero the BSS now, while paging is off - it holds both the page tables
     * (below) and the kernel heap, and their globals rely on being zero.
     * mb_info_ptr lives in .bss too - GRUB's Multiboot info pointer, handed
     * in %ebx at entry, MUST be saved only AFTER this zeroing, not before
     * (an earlier draft got this backwards: saving first, then having this
     * same rep stosl immediately zero it right back out). %ebx itself isn't
     * touched by rep stosl or anything else between here and long_mode_start
     * below, so just holding it in the register across all of this is fine. */
    mov $__bss_start, %edi
    mov $__bss_end, %ecx
    sub %edi, %ecx
    shr $2, %ecx                 /* bytes -> dwords */
    xor %eax, %eax
    rep stosl
    movl %ebx, mb_info_ptr

    /* --- page tables: PML4[0] -> PDPT[0..3] -> 4x PD, identity map 4 GiB --- */
    movl $pdpt, %eax
    orl  $0x3, %eax               /* present + writable */
    movl %eax, pml4
    movl $0,   pml4+4

    /* PDPT[0..3] each point at one of the 4 contiguous 4096-byte PD tables
     * inside `pd` (see the .bss layout above) - PDPT[i] -> pd + i*4096,
     * covering physical [i*1GiB, (i+1)*1GiB). */
    movl $pd, %ebx
    movl $0, %ecx                 /* loop index i = 0..3 */
2:
    movl %ebx, %eax
    orl  $0x3, %eax
    movl %eax, pdpt(,%ecx,8)
    movl $0,   pdpt+4(,%ecx,8)
    addl $4096, %ebx
    incl %ecx
    cmpl $4, %ecx
    jl   2b

    /* fill all 4 PD tables: 4x512 entries x 2 MiB = 4 GiB, each
     * present+writable+huge(PS), physical address counting up contiguously
     * across all 4 tables (pd[0] covers phys 0, pd[1] covers phys 1 GiB,
     * ... - matches the PDPT mapping above exactly). */
    movl $pd, %edi
    movl $0x83, %eax              /* phys 0 | present | write | PS */
    movl $2048, %ecx              /* 4 tables * 512 entries */
1:
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop 1b

    /* load CR3 */
    movl $pml4, %eax
    movl %eax, %cr3

    /* CR4.PAE = 1 */
    movl %cr4, %eax
    orl  $0x20, %eax
    movl %eax, %cr4

    /* EFER.LME = 1 (MSR 0xC0000080) */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $0x100, %eax
    wrmsr

    /* CR0.PG = 1 (PE already set by the loader) */
    movl %cr0, %eax
    orl  $0x80000000, %eax
    movl %eax, %cr0

    /* enter 64-bit mode */
    lgdt gdt64_pointer
    ljmp $GDT_CODE, $long_mode_start

.code64
.global long_mode_start
long_mode_start:
    xorq %rax, %rax              /* reload data segments with null selector */
    movw %ax, %ss
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    movq $stack_top, %rsp

    /* enable SSE - the Larzscript interpreter uses xmm for doubles */
    movq %cr0, %rax
    andq $0xFFFFFFFFFFFFFFFB, %rax   /* clear CR0.EM (bit 2) */
    orq  $0x2, %rax                  /* set   CR0.MP (bit 1) */
    movq %rax, %cr0
    movq %cr4, %rax
    orq  $0x600, %rax                /* set CR4.OSFXSR (9) + OSXMMEXCPT (10) */
    movq %rax, %cr4

    /* pass the saved Multiboot info pointer as kernel_main's first (rdi)
     * argument, System V AMD64 convention. movl zero-extends into rdi. */
    movl mb_info_ptr, %edi
    call kernel_main
    cli
1:  hlt
    jmp 1b
.size _start, . - _start

/* mark the stack non-executable (silences a linker warning) */
.section .note.GNU-stack,"",@progbits
