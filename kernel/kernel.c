/* kernel.c - the LarzOS kernel entry (Stage 1).
 *
 * The machine is now in 64-bit long mode with SSE on and the BSS cleared
 * (see boot.S). We bring up the console and hand control to the *unmodified*
 * Larzscript interpreter, running its REPL with stdin/stdout wired to the
 * serial console. This is Larzscript running on bare metal - no Linux.
 */
#include "libc/stdio.h"
#include "console.h"
#include "net.h"

/* the interpreter's main(), renamed via -Dmain=larz_main at compile time */
int larz_main(int argc, char **argv);

void kernel_main(void){
    console_init();
    vfs_init();                    /* mount the writable filesystem */
    printf("\n");
    net_selftest();                /* detect the NIC + ping the gateway */
    printf("  LarzOS  -  the money-native operating system\n");
    printf("  kernel (Stage 1): 64-bit long mode, no Linux underneath.\n");
    printf("  The Larzscript interpreter is running on bare metal.\n");
    printf("\n");
#if defined(LARZ_REPL)
    /* interactive only */
    printf("  Try it at the larz> prompt, e.g.  wallet a = $20.00\n");
    printf("  End the session with:  exit(0)\n\n");
    char *repl_argv[] = { "larzscript", "repl", 0 };
    larz_main(2, repl_argv);
#elif defined(LARZ_DEMO_REPL)
    /* run the boot demo, then drop into an interactive prompt */
    printf("  Loading /boot.lz from the RAM filesystem...\n");
    char *boot_argv[] = { "larzscript", "/boot.lz", 0 };
    larz_main(2, boot_argv);
    printf("\n  [ demo complete - interactive Larzscript prompt; exit(0) to halt ]\n\n");
    char *repl2_argv[] = { "larzscript", "repl", 0 };
    larz_main(2, repl2_argv);
#elif defined(LARZ_INIT)
    /* Linux-server-style boot: init brings the system up, login authenticates,
     * then the user's shell runs. */
    { char *a[] = { "larzscript", "/init.lz", 0 };    larz_main(2, a); }
    { char *a[] = { "larzscript", "/login.lz", 0 };   larz_main(2, a); }
    { char *a[] = { "larzscript", "/larzsh.lz", 0 };  larz_main(2, a); }
#elif defined(LARZ_SERVER)
    /* boot straight into the Larzscript web server */
    printf("  starting the LarzOS web server (/webserver.lz) ...\n\n");
    char *srv_argv[] = { "larzscript", "/webserver.lz", 0 };
    larz_main(2, srv_argv);
#elif defined(LARZ_SHELL)
    /* run the boot demo, then launch the LarzOS shell (larzsh) - self-hosting */
    printf("  Loading /boot.lz from the RAM filesystem...\n");
    char *boot_argv[] = { "larzscript", "/boot.lz", 0 };
    larz_main(2, boot_argv);
    printf("\n  [ starting the LarzOS shell - 'help' for commands, 'exit' to halt ]\n");
    char *sh_argv[] = { "larzscript", "/larzsh.lz", 0 };
    larz_main(2, sh_argv);
#else
    /* deterministic: run the boot program and halt (used by `make test`) */
    printf("  Loading /boot.lz from the RAM filesystem...\n");
    char *boot_argv[] = { "larzscript", "/boot.lz", 0 };
    larz_main(2, boot_argv);
#endif

    printf("\n  LarzOS halted.\n");
    qemu_exit(0);
    for(;;) __asm__ volatile("hlt");
}
