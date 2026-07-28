/* Link the LarzOS kernel (ELF64) at the multiboot load address of 1 MiB. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)          /* the multiboot header must come first */
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K) { *(.rodata) }
    .data   BLOCK(4K) : ALIGN(4K) { *(.data) }

    .bss    BLOCK(4K) : ALIGN(4K)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        . = ALIGN(8);
        __bss_end = .;
    }
}
