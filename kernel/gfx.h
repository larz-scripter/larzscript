/* gfx.h - a real linear framebuffer (via Multiboot, boot.S), real RGB color.
 * Public interface used by kernel.c and the kernel-native `ui` Larzscript
 * module in native/larzscript.c. Replaces the original 320x200x256 VGA
 * Mode 13h backend - screen size/pitch/bpp are read from the Multiboot
 * info struct at boot, not assumed, and may differ from what boot.S asked
 * for (1024x768x32) if GRUB granted something else - use gfx_width()/
 * gfx_height(), never hardcode a resolution. */
#ifndef _LARZOS_GFX_H
#define _LARZOS_GFX_H
#include <stdint.h>

/* kernel_main (kernel.c) calls this once, immediately, with the Multiboot
 * info pointer it received from boot.S - stored for gfx_init() to actually
 * use later. Split from gfx_init() itself because gfx_init() is invoked
 * lazily, on first use, from inside the kernel-native `ui` Larzscript
 * module (native/larzscript.c's ensure_kernel_gfx()) - code with no other
 * way to reach the pointer kernel_main was handed at boot. */
void gfx_set_multiboot_info(uint64_t mb_info);

/* Parses the framebuffer fields out of the info pointer gfx_set_multiboot_
 * info() stored and sets up pixel-writing accordingly. Call once. */
void gfx_init(void);

int gfx_width(void);
int gfx_height(void);

/* colors are plain 24-bit RGB, 0x00RRGGBB - no palette anymore, any value
 * is valid. A handful of named ones for the UI chrome this project already
 * draws with, kept for readability at call sites: */
enum {
    GFX_BLACK      = 0x000000,
    GFX_WHITE      = 0xFFFFFF,
    GFX_DARK_GRAY  = 0x14161f,   /* matches larzos.com's #0b0f1a card bg, roughly */
    GFX_MID_GRAY   = 0x3c4048,
    GFX_ACCENT     = 0x2ec4a0,   /* teal/green accent */
    GFX_ACCENT_DIM = 0x1a6a54,
    GFX_RED        = 0xb01a1a,
};

void gfx_set_pixel(int x, int y, uint32_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_hline(int x, int y, int w, uint32_t color);
void gfx_vline(int x, int y, int h, uint32_t color);

/* draws `s` starting at (x,y), 8x8 cells per character, fg on bg.
 * ASCII only (bytes >=128 render as a placeholder glyph); ASCII 32-126
 * covered, uppercase and lowercase both render (see gfx.c for coverage
 * notes on the embedded font). */
void gfx_draw_text(int x, int y, const char *s, uint32_t fg, uint32_t bg);

/* ---- a minimal retained widget model, keyboard-driven (no mouse in v1) ----
 * The kernel has no pre-existing markup to select into (unlike the browser's
 * `ui` module, which queries a real DOM) - widgets are *created* by these
 * calls, keyed by a small stable id string. This file knows nothing about
 * Larzscript; the kernel-native `ui` Larzscript module (native/larzscript.c)
 * calls these and maps ids to stored closures itself - the same split as the
 * browser build, where EM_JS doesn't know about Larzscript Values either. */
#define GFX_MAX_WIDGETS 16
#define GFX_ID_LEN 32
#define GFX_TEXT_LEN 64

int  gfx_widget_label(const char *id, int x, int y, const char *text);
int  gfx_widget_button(const char *id, int x, int y, int w, int h, const char *text);
void gfx_widget_set_text(const char *id, const char *text);
void gfx_widget_redraw_all(void);

/* A scrolling text pane - the one widget kind that isn't set via
 * gfx_widget_set_text: its content is a running LOG, appended to a
 * character at a time by gfx_terminal_putc() (see kernel/libk.c's
 * serial_putc(), which calls it for every character *any* program prints -
 * this is what makes existing/future scripts' plain print() output show up
 * in the GUI with no code changes to them). Exactly one terminal widget can
 * be active at a time in v1 - the most recently created one. */
int  gfx_widget_terminal(const char *id, int x, int y, int w, int h);
void gfx_terminal_putc(char c);   /* no-op if no terminal widget exists yet */

/* index of the widget with this id, or -1 - lets a caller (the kernel-native
 * `ui` Larzscript module) key its own per-widget storage (click closures) by
 * the same small integer index gfx.c already uses internally. */
int gfx_widget_index(const char *id);

/* Blocks for the next keyboard event. Tab cycles focus among buttons
 * (forward-only wraparound - the existing scancode table has no distinct
 * shift+tab ASCII value, so that's an honest v1 limitation, not a bug).
 * Returns the id of a button just clicked (Enter while focused), or NULL if
 * the event was a focus change or an unhandled key - callers just loop. */
const char *gfx_widget_poll(void);

#endif
