/* kernel.c - the LarzOS kernel entry (Stage 1).
 *
 * The machine is now in 64-bit long mode with SSE on and the BSS cleared
 * (see boot.S). We bring up the console and hand control to the *unmodified*
 * Larzscript interpreter, running its REPL with stdin/stdout wired to the
 * serial console. This is Larzscript running on bare metal - no Linux.
 */
#include <stdint.h>
#include "libc/stdio.h"
#include "console.h"
#include "net.h"
#include "gfx.h"

/* the interpreter's main(), renamed via -Dmain=larz_main at compile time */
int larz_main(int argc, char **argv);

#ifdef LARZ_TASKDIAG
/* Runs a full Larzscript interpreter instance inside its own SPAWNED task
 * (task_create, kernel/libk.c) rather than task 0's main context - task 0's
 * own stack is already a proven-sufficient 8 MiB (see boot.S); this is the
 * empirically real question task #31 has to answer: is a *spawned* task's
 * (much smaller, per-task) stack big enough for the same interpreter's real
 * recursion depth? See stress.lz and kernel/README.md. */
static void task_stress(void){
    char *a[] = { "larzscript", "/stress.lz", 0 };
    larz_main(2, a);
    printf("\n[stress] task 1 finished - see the \"stress result\" line above.\n");
    qemu_exit(0);
    for(;;) __asm__ volatile("hlt");
}
#endif
#ifdef LARZ_DESKTOP
/* The interactive terminal as a real task (task 1), owning its own window -
 * the first genuine "app" in the windowed-desktop model, not the whole
 * screen implicitly being one app the way LARZ_GUI's /gui.lz still is. */
static void task_terminal(void){
    int idx = gfx_window_create("Terminal", 40, 40, 640, 420);
    int cx, cy, cw, ch;
    gfx_window_client_rect(idx, &cx, &cy, &cw, &ch);
    gfx_widget_terminal("term1", cx, cy, cw, ch);
    char *a[] = { "larzscript", "/larzsh.lz", 0 };
    larz_main(2, a);
    for(;;) __asm__ volatile("hlt");
}

/* The second real app (task #33) - a live, independently-updating window
 * running concurrently alongside the terminal, the actual "looks like
 * Windows" proof: more than one app genuinely active at once, not a single-
 * app-at-a-time switcher. Positioned clear of the terminal window and the
 * taskbar strip along the bottom (gfx.c's draw_taskbar()). */
static void task_clock(void){
    int idx = gfx_window_create("Clock", 720, 40, 260, 140);
    int cx, cy, cw, ch;
    gfx_window_client_rect(idx, &cx, &cy, &cw, &ch);
    gfx_widget_label("clock_time", cx+4, cy+4, "uptime: 0s");
    char *a[] = { "larzscript", "/clock.lz", 0 };
    larz_main(2, a);
    for(;;) __asm__ volatile("hlt");
}
#endif

void kernel_main(uint64_t mb_info){
    console_init();
    gfx_set_multiboot_info(mb_info);   /* stash it - see gfx.h for why this is split from gfx_init() */
#ifdef LARZ_MBDIAG
    /* raw Multiboot-info diagnostic (never returns) - proves the info
     * pointer boot.S now preserves and passes through actually parses to
     * sane values via the real gfx_init()/gfx_width()/gfx_height() path
     * (not a separate copy of the parsing logic). See kernel/README.md. */
    gfx_init();
    printf("\nmb_info ptr = 0x%llx\n", (unsigned long long)mb_info);
    printf("gfx_width()  = %d\n", gfx_width());
    printf("gfx_height() = %d\n", gfx_height());
    for(;;) __asm__ volatile("hlt");
#endif
#ifdef LARZ_KBDDIAG
    kbd_diag();                    /* raw keyboard diagnostic (never returns) */
#endif
#ifdef LARZ_GFXDIAG
    /* raw graphics+widget diagnostic (never returns) - draws a test pattern
     * plus two widgets and drives Tab/Enter focus-cycling for `screendump`
     * verification, independent of the Larzscript ui module built on top of
     * this later. See kernel/README.md. Needs interrupts on (keyboard is
     * IRQ-driven) - unlike the plain gfx test, this one calls ints_init(). */
    ints_init();
    gfx_init();
    gfx_fill_rect(0, 0, gfx_width(), gfx_height(), GFX_DARK_GRAY);
    gfx_draw_text(10, 10, "LARZOS GUI TEST 0123", GFX_WHITE, GFX_DARK_GRAY);
    gfx_draw_text(10, 25, "$1.00 (buy) - a.b.c?", GFX_ACCENT, GFX_DARK_GRAY);
    gfx_widget_label("bal", 10, 50, "balance: $10.00");
    gfx_widget_button("buy", 10, 70, 100, 24, "buy - $2.00");
    gfx_widget_button("premium", 120, 70, 140, 24, "unlock premium");
    gfx_widget_terminal("term", 10, 100, 220, 60);
    /* real printf() calls, not direct gfx_terminal_putc() - proves the
     * ACTUAL mechanism (the serial_putc hook in libk.c), not just the
     * widget's own append/wrap/scroll logic in isolation. */
    printf("line one\n");
    printf("line two\n");
    printf("a much longer line that should wrap onto the next row for sure\n");
    printf("line four\n");
    printf("line five\n");
    printf("line six\n");
    printf("line seven - should have scrolled by now\n");
    int clicks = 0;
    for(;;){
        const char *clicked = gfx_widget_poll();
        if(clicked){
            clicks++;
            char buf[GFX_TEXT_LEN];
            snprintf(buf, sizeof buf, "clicked %d (%s)", clicks, clicked);
            gfx_widget_set_text("bal", buf);
        }
    }
#endif
#ifdef LARZ_WMDIAG
    /* raw window-manager diagnostic (never returns) - three overlapping
     * windows (deliberately positioned in a staircase so each partially
     * covers the last) proving z-order compositing is correct, then Tab
     * cycles focus/bring-to-front for real via the keyboard, verified by
     * screendump before/after - not just a single static frame. See
     * kernel/README.md. */
    ints_init();
    gfx_init();
    gfx_windows_redraw_all();          /* paints the (currently empty) desktop background */
    gfx_window_create("Window A", 60, 60, 300, 200);
    gfx_window_create("Window B", 160, 140, 300, 200);
    gfx_window_create("Window C", 260, 220, 300, 200);
    for(;;){
        char c = console_getc();
        if(c=='\t') gfx_window_focus_next();
    }
#endif
#ifdef LARZ_TASKDIAG
    /* stack-sizing stress diagnostic (never returns) - stress.lz recurses
     * 3000 deep inside a SPAWNED task (task 1), while task 0 idles; task 1
     * prints its own pass/fail (a verifiable closed-form sum, so silent
     * stack corruption producing a wrong-but-plausible number is caught
     * too, not just a crash) then halts the machine. Run once with the
     * ORIGINAL TSTK (64 KiB) to confirm this genuinely fails without the
     * fix, then again after raising TSTK to confirm it passes - a real
     * before/after, not an assumption. See kernel/README.md. */
    ints_init();
    sched_init();
    vfs_init();                    /* task_stress opens /stress.lz - needs the writable FS mounted first */
    task_create(task_stress);
    for(;;) __asm__ volatile("hlt");
#endif
#ifdef LARZ_DESKTOP
    /* the real windowed desktop: two independently-running real apps (the
     * terminal + the clock, each its own task/window) plus a taskbar to
     * switch between them - the actual "looks like Windows" proof, genuine
     * concurrent multitasking rather than one app at a time. task 0 stays
     * idle (redraw-on-demand is already event-driven, not polled). */
    ints_init();
    gfx_init();
    sched_init();
    vfs_init();                    /* task_terminal/task_clock open .lz files - FS must be mounted first */
    gfx_windows_redraw_all();
    task_create(task_terminal);
    task_create(task_clock);
    for(;;) __asm__ volatile("hlt");
#endif
    ints_init();                   /* IDT + PIC + timer/keyboard interrupts */
    sched_init();                  /* preemptive scheduler + background tasks */
    vfs_init();                    /* mount the writable filesystem */
    printf("\n");
    net_selftest();                /* detect the NIC + ping the gateway */
    printf("  input: PS/2 keyboard = IRQ1 + timer-poll; multitasking on  [build kbd12]\n");
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
#elif defined(LARZ_GUI)
    /* boot straight into the GUI - real VGA graphics + keyboard-driven
     * widgets, no serial/text console involved once /gui.lz calls ui.run().
     * See kernel/README.md for the money-native story and how to verify it
     * headlessly via QEMU's monitor screendump/sendkey. */
    printf("  starting the LarzOS GUI (/gui.lz) ...\n\n");
    char *gui_argv[] = { "larzscript", "/gui.lz", 0 };
    larz_main(2, gui_argv);
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
