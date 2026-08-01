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

/* ---- GUI login screen ----
 *
 * A real, full-screen username/password gate before the desktop - not a
 * "window" (no title bar, no close-X, you can't skip login), drawn
 * directly with the same public gfx_* primitives every other diagnostic
 * block in this file already uses. Runs synchronously in kernel_main(),
 * BEFORE sched_init()/task_create() - console_getc() already works safely
 * in a blocking loop this early (the exact mechanism /dev/password's
 * masked-line-read already relies on, kernel/libk.c) so no task is needed
 * just to read keystrokes.
 *
 * Credential verification reuses the REAL auth.lz logic (via a tiny
 * internal script, /logincheck.lz) rather than duplicating the hash
 * algorithm in C - kernel.c writes the typed username/password to two
 * temp files, runs logincheck.lz synchronously with larz_main() (proven
 * safe here: vfs_init() has already run), then reads its verdict back
 * from a third file. logincheck.lz is carefully written to never call
 * exit() - on this kernel exit() halts the whole machine (see its own
 * comment), which would turn a login attempt into a hang instead of
 * returning control to the loop below. */
#define LOGIN_BUF 64
static int text_w(const char *s){ int n=0; while(s[n]) n++; return n*8; }

/* Draws one bordered field box + its current contents, centered text
 * omitted deliberately (real login fields are left-aligned, like every
 * other OS) - focused field gets an accent border, unfocused a dim one,
 * the same focus-color convention buttons/windows already use elsewhere
 * in this compositor. */
static void login_draw_field(int x,int y,int w,int h,int focused,const char *text){
    gfx_fill_rect(x,y,w,h,GFX_DARK_GRAY);
    uint32_t border = focused ? GFX_ACCENT : GFX_MID_GRAY;
    gfx_hline(x,y,w,border); gfx_hline(x,y+h-1,w,border);
    gfx_vline(x,y,h,border); gfx_vline(x+w-1,y,h,border);
    gfx_draw_text(x+8,y+(h-8)/2,text,GFX_WHITE,GFX_DARK_GRAY);
}

static void login_screen(void){
    gfx_set_taskbar_visible(0);
    /* No icons/windows exist yet at this point in the boot sequence (see
     * kernel_main() below - login_screen() runs BEFORE gfx_desktop_icons_
     * init()/launch_app()), so this redraw is naturally just the plain
     * gradient background - no extra gating needed to hide anything. */
    gfx_windows_redraw_all();

    int cw=360, ch=280;
    int cx=(gfx_width()-cw)/2, cy=(gfx_height()-ch)/2;
    int fx=cx+40, fw=cw-80, fh=26;
    int uy=cy+92, py=cy+156;

    /* Card chrome + static labels/hint - drawn ONCE; only the two fields
     * and the error line change per keystroke/attempt below. */
    gfx_fill_rect(cx,cy,cw,ch,GFX_DARK_GRAY);
    gfx_hline(cx,cy,cw,GFX_MID_GRAY); gfx_hline(cx,cy+ch-1,cw,GFX_MID_GRAY);
    gfx_vline(cx,cy,ch,GFX_MID_GRAY); gfx_vline(cx+cw-1,cy,ch,GFX_MID_GRAY);
    gfx_draw_text(cx+(cw-text_w("LARZOS"))/2, cy+24, "LARZOS", GFX_ACCENT, GFX_DARK_GRAY);
    gfx_draw_text(fx, uy-14, "USERNAME", GFX_MID_GRAY, GFX_DARK_GRAY);
    gfx_draw_text(fx, py-14, "PASSWORD", GFX_MID_GRAY, GFX_DARK_GRAY);
    const char *hint = "ENTER TO CONTINUE";
    gfx_draw_text(cx+(cw-text_w(hint))/2, cy+ch-26, hint, GFX_MID_GRAY, GFX_DARK_GRAY);
    int ey = cy+ch+16;   /* error line, just below the card */

    char ubuf[LOGIN_BUF]; int ulen=0;
    char pbuf[LOGIN_BUF]; int plen=0;
    char mask[LOGIN_BUF];
    int focus=0;                          /* 0=username, 1=password */
    ubuf[0]=0; pbuf[0]=0;

    for(;;){
        login_draw_field(fx,uy,fw,fh,focus==0,ubuf);
        for(int i=0;i<plen;i++) mask[i]='.';
        mask[plen]=0;
        login_draw_field(fx,py,fw,fh,focus==1,mask);

        char c = console_getc();
        if(c=='\t'){ focus = 1-focus; continue; }
        if(c==0x7F || c==0x08){
            if(focus==0 && ulen>0) ubuf[--ulen]=0;
            else if(focus==1 && plen>0) pbuf[--plen]=0;
            continue;
        }
        if(c=='\n' || c=='\r'){
            if(focus==0){ focus=1; continue; }
            /* focus==1: submit - verify via the real auth.lz logic. */
            FILE *f = fopen("/tmp/.login_u","w"); if(f){ fwrite(ubuf,1,(size_t)ulen,f); fclose(f); }
            f = fopen("/tmp/.login_p","w"); if(f){ fwrite(pbuf,1,(size_t)plen,f); fclose(f); }
            char *login_argv[] = { "larzscript", "/logincheck.lz", 0 };
            larz_main(2, login_argv);
            char result[8]={0};
            f = fopen("/tmp/.login_result","r");
            if(f){ fgets(result,sizeof result,f); fclose(f); }
            remove("/tmp/.login_u"); remove("/tmp/.login_p"); remove("/tmp/.login_result");
            if(result[0]=='O' && result[1]=='K') break;   /* success - fall out to kernel_main() */
            gfx_fill_rect(cx, ey-10, cw, 12, GFX_DARK_GRAY);   /* clear any previous error first */
            gfx_draw_text(cx+(cw-text_w("INVALID USERNAME OR PASSWORD"))/2, ey-10,
                           "INVALID USERNAME OR PASSWORD", GFX_RED, GFX_DARK_GRAY);
            plen=0; pbuf[0]=0; focus=1;
            continue;
        }
        if(c>=32 && c<127){
            if(focus==0 && ulen<LOGIN_BUF-1){ ubuf[ulen++]=c; ubuf[ulen]=0; }
            else if(focus==1 && plen<LOGIN_BUF-1){ pbuf[plen++]=c; pbuf[plen]=0; }
        }
    }
    gfx_set_taskbar_visible(1);
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
    /* the real windowed desktop: a full-screen login gate first (username/
     * password, verified against the real auth.lz accounts - see
     * login_screen() above), THEN the terminal auto-starts (task 1, its
     * own window, launched the same generic way any other app is) plus a
     * taskbar and a row of desktop-icon launchers for everything else -
     * all launched on demand, none auto-started besides Terminal, since
     * NTASK=3 only leaves ONE free app-task slot beside it (closing an
     * app frees its slot back up - see gfx_window_close()/task_exit()).
     * task 0 stays idle (redraw-on-demand is already event-driven, not
     * polled). */
    ints_init();
    gfx_init();
    sched_init();
    vfs_init();                    /* login_screen()/launch_app()'s apps open .lz files - FS must be mounted first */
    net_selftest();                /* detect the NIC + ping the gateway - this branch never fell through to
                                     * the general path's own net_selftest() call below, so any script reading
                                     * from the net-VFS (net.lz) always saw "no NIC" on this boot target even
                                     * with a real one attached; output only reaches the serial log post-
                                     * gfx_init(), invisible on screen, same as every other printf() here on */
    login_screen();                /* blocks here until real credentials verify - see its own comment */
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
