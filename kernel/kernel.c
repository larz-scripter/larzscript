/* kernel.c - the LarzOS kernel entry (Stage 1).
 *
 * The machine is now in 64-bit long mode with SSE on and the BSS cleared
 * (see boot.S). We bring up the console and hand control to the *unmodified*
 * Larzscript interpreter, running its REPL with stdin/stdout wired to the
 * serial console. This is Larzscript running on bare metal - no Linux.
 */
#include "libc/stdio.h"
#include "console.h"

/* the interpreter's main(), renamed via -Dmain=larz_main at compile time */
int larz_main(int argc, char **argv);

void kernel_main(void){
    console_init();
    printf("\n");
    printf("  LarzOS  -  the money-native operating system\n");
    printf("  kernel (Stage 1): 64-bit long mode, no Linux underneath.\n");
    printf("  The Larzscript interpreter is running on bare metal.\n");
    printf("\n");
#ifdef LARZ_REPL
    printf("  Try it at the larz> prompt, e.g.  wallet a = $20.00\n");
    printf("  End the session with:  exit(0)\n\n");
    char *argv[] = { "larzscript", "repl", 0 };
#else
    printf("  Loading /boot.lz from the RAM filesystem...\n");
    char *argv[] = { "larzscript", "/boot.lz", 0 };
#endif
    larz_main(2, argv);

    printf("\n  LarzOS halted.\n");
    qemu_exit(0);
    for(;;) __asm__ volatile("hlt");
}
