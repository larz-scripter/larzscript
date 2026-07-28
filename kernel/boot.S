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
.set MB_FLAGS,    0x00000003
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM

.section .bss
.align 4096
pml4:  .skip 4096
pdpt:  .skip 4096
pd:    .skip 4096
.align 16
stack_bottom:
.skip 0x800000                         /* 8 MiB kernel stack - the interpreter recurses deeply */
stack_top:

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
     * (below) and the kernel heap, and their globals rely on being zero. */
    mov $__bss_start, %edi
    mov $__bss_end, %ecx
    sub %edi, %ecx
    shr $2, %ecx                 /* bytes -> dwords */
    xor %eax, %eax
    rep stosl

    /* --- page tables: PML4[0] -> PDPT[0] -> PD, identity map 1 GiB --- */
    movl $pdpt, %eax
    orl  $0x3, %eax               /* present + writable */
    movl %eax, pml4
    movl $0,   pml4+4

    movl $pd, %eax
    orl  $0x3, %eax
    movl %eax, pdpt
    movl $0,   pdpt+4

    /* fill PD: 512 entries x 2 MiB = 1 GiB, each present+writable+huge(PS) */
    movl $pd, %edi
    movl $0x83, %eax              /* phys 0 | present | write | PS */
    movl $512, %ecx
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

    call kernel_main
    cli
1:  hlt
    jmp 1b
.size _start, . - _start

/* mark the stack non-executable (silences a linker warning) */
.section .note.GNU-stack,"",@progbits
