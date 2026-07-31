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
/* A GENERIC app-launch mechanism: turns "add a desktop app" into "write a
 * .lz script that calls ui.window(...)" (native/larzscript.c) - no bespoke
 * C wrapper needed per app at all - the Terminal itself (larzsh.lz) is now
 * launched this same way (it used to be the one remaining bespoke
 * task_terminal() C wrapper; retired once larzsh.lz started creating its
 * own window via ui.window()/ui.window_size(), which is what let it join
 * the icon-launcher manifest below and become relaunchable after being
 * closed - see kernel/README.md). launch_app() stashes which
 * script to run in a per-SLOT array, indexed by the EXACT slot
 * next_task_slot() (kernel/libk.c) says task_create() is about to assign -
 * safe regardless of what else is running, unlike a single shared "pending
 * launch" variable a second launch could overwrite before the first new
 * task ever reads it (task_create() assigns slots synchronously and in
 * order, so populating the same index right before calling it is race-free). */
#define APP_MAX_SLOTS 8
static char g_app_script[APP_MAX_SLOTS][64];

static void task_generic_app(void){
    int tid = current_task_id();
    char *a[] = { "larzscript", g_app_script[tid], 0 };
    larz_main(2, a);
    for(;;) __asm__ volatile("hlt");
}

/* Launches `script` as a new app task - the script itself creates its own
 * window via ui.window(title,w,h). Returns the task slot used, or -1 if
 * every slot is already taken (no free app slot to launch into) - matches
 * a real desktop just not opening a new window rather than crashing when
 * you're out of room; the return value lets a caller (ui.launch(), the
 * `launch` shell command) tell the user why nothing happened instead of
 * silently doing nothing. */
int launch_app(const char *script){
    int slot = next_task_slot();
    if(slot < 0 || slot >= APP_MAX_SLOTS) return -1;
    int i=0; for(; script[i] && i<63; i++) g_app_script[slot][i]=script[i];
    g_app_script[slot][i]=0;
    task_create(task_generic_app);
    return slot;
}

/* Stage 4: a small fixed manifest of real, already-launchable scripts
 * turned into desktop icons (kernel/gfx.c draws/hit-tests them generically
 * by label alone). Clock/About/Files all create their own window via
 * ui.window() and are launched on demand; Terminal joined this same list
 * once larzsh.lz became self-contained too - this is what actually fixes
 * the "close Terminal -> no way to get it back" bug: closing it now behaves
 * exactly like closing any other app, and clicking its icon relaunches it. */
static const char *g_app_manifest_labels[] = { "Terminal", "Clock", "About", "Files" };
static const char *g_app_manifest_scripts[] = { "/larzsh.lz", "/clock.lz", "/about.lz", "/files.lz" };
#define APP_MANIFEST_COUNT 4

void desktop_icon_activate(int i){
    if(i>=0 && i<APP_MANIFEST_COUNT) launch_app(g_app_manifest_scripts[i]);
}
#else
void desktop_icon_activate(int i){ (void)i; }
/* ui.launch()/ui.close() (native/larzscript.c) call launch_app() unconditionally
 * on every boot target - a no-op stub here so non-desktop targets still link. */
int launch_app(const char *script){ (void)script; return -1; }
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
    /* the real windowed desktop: the terminal auto-starts (task 1, its own
     * window, launched the same generic way any other app is) plus a
     * taskbar and a row of desktop-icon launchers for everything else - all
     * launched on demand, none auto-started besides Terminal, since NTASK=3
     * only leaves ONE free app-task slot beside it (closing an app frees its
     * slot back up - see gfx_window_close()/task_exit()). task 0 stays idle
     * (redraw-on-demand is already event-driven, not polled). */
    ints_init();
    gfx_init();
    sched_init();
    vfs_init();                    /* launch_app()'s apps open .lz files - FS must be mounted first */
    gfx_desktop_icons_init(g_app_manifest_labels, APP_MANIFEST_COUNT);
    gfx_windows_redraw_all();
    launch_app("/larzsh.lz");
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
