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

/* The Multiboot1 info struct GRUB hands back (see boot.S's mb_info_ptr) -
 * field order/sizes exactly match the spec (multiboot.org), packed
 * defensively rather than relying on default alignment happening to agree
 * (it does here, but don't leave that to chance). Only the fields this
 * kernel actually reads are named; the rest are left as anonymous padding.
 * Bit 12 of `flags` says whether framebuffer_* below are valid at all. */
typedef struct __attribute__((packed)) {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr;
    uint32_t drives_length, drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info, vbe_mode_info;
    uint16_t vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width, framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6];
} MultibootInfo;
#define MB_FLAG_FRAMEBUFFER (1u << 12)

void kernel_main(uint64_t mb_info){
    console_init();
#ifdef LARZ_MBDIAG
    /* raw Multiboot-info diagnostic (never returns) - proves the info
     * pointer boot.S now preserves and passes through actually parses to
     * sane values, BEFORE any graphics-mode code is written against it.
     * See kernel/README.md. */
    MultibootInfo *mbi = (MultibootInfo*)mb_info;
    printf("\nmb_info ptr = 0x%llx\n", (unsigned long long)mb_info);
    printf("flags = 0x%x\n", mbi->flags);
    if(mbi->flags & MB_FLAG_FRAMEBUFFER){
        printf("framebuffer_addr   = 0x%llx\n", (unsigned long long)mbi->framebuffer_addr);
        printf("framebuffer_pitch  = %u\n", mbi->framebuffer_pitch);
        printf("framebuffer_width  = %u\n", mbi->framebuffer_width);
        printf("framebuffer_height = %u\n", mbi->framebuffer_height);
        printf("framebuffer_bpp    = %u\n", mbi->framebuffer_bpp);
        printf("framebuffer_type   = %u  (1 = RGB linear)\n", mbi->framebuffer_type);
    } else {
        printf("NO framebuffer info reported - flags bit 12 not set\n");
    }
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
    gfx_fill_rect(0, 0, GFX_W, GFX_H, GFX_DARK_GRAY);
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
