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
int gfx_ready(void);   /* has gfx_init() actually run? (a real framebuffer exists) */

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

/* ---- windows: a real compositor, back-to-front by z-order ---- *
 * A window is a title-barred rect on the desktop. Compositing is the
 * simplest correct approach for a handful of windows: repaint every window
 * in back-to-front order on any change - a front window's pixels overwrite
 * a back window's wherever they overlap, which alone is the right visual
 * result, no explicit clipping/damage-region math needed. Not yet wired to
 * the widget system above (that's the next step - each app/task will own
 * one window and draw its widgets inside it); this is the compositor core
 * on its own: create, z-order, focus/bring-to-front, redraw. */
#define GFX_MAX_WINDOWS 8
#define GFX_TITLEBAR_H 22
#define GFX_TITLE_LEN 32

/* Creates a window and brings it to front; returns its index (stable for
 * the window's lifetime - windows are never destroyed in v1) or -1 if
 * GFX_MAX_WINDOWS is already in use. */
int gfx_window_create(const char *title, int x, int y, int w, int h);

/* Moves a window to the front of the z-order (drawn last = on top) and
 * marks it focused (a highlighted title bar) - redraws immediately. */
void gfx_window_focus(int idx);

/* Cycles focus to the next window in z-order (wrapping), bringing it to
 * front - the placeholder keyboard-only equivalent of clicking a window
 * (mouse click-to-focus is a later step). */
void gfx_window_focus_next(void);

/* index of the currently focused window, or -1 if none exist yet */
int gfx_window_focused(void);

/* repaints the whole desktop: background, then every window back-to-front */
void gfx_windows_redraw_all(void);

/* the window's CLIENT area (inside the border and title bar) - where
 * window-owned content should actually draw, in screen coordinates. */
void gfx_window_client_rect(int idx, int *x, int *y, int *w, int *h);

/* the window's RAW rect (its own x,y,w,h - the whole frame, title bar
 * included) - distinct from gfx_window_client_rect's inset area. The mouse
 * driver (kernel/libk.c) reads this once when a drag starts, to seed where
 * the window currently is before applying subsequent mouse deltas. */
void gfx_window_rect(int idx, int *x, int *y, int *w, int *h);

/* how many windows exist right now - used by ui.window() (native/
 * larzscript.c) to pick a simple cascading default position for a newly
 * launched app's window, so several launched-on-demand apps don't all
 * land in an identical spot. */
int gfx_window_count(void);

/* index of the topmost (frontmost) window whose rect contains (x,y), or -1 -
 * searches z-order back-to-front, i.e. checks the FRONT window first, since
 * that's what a real click would actually hit if windows overlap there. */
int gfx_window_hit_test(int x, int y);

/* Same idea, but restricted to the window's title-bar strip specifically -
 * used to start a drag (kernel/libk.c's mouse driver): clicking a window's
 * BODY focuses it without moving it; only the title bar drags. */
int gfx_window_titlebar_hit_test(int x, int y);

/* Moves a window to a new top-left position and redraws - its widgets move
 * for free (they're stored relative to their owning window). Clamped so
 * the title bar always stays reachable (not fully off-screen or under the
 * taskbar). */
void gfx_window_move(int idx, int x, int y);

/* window index whose taskbar entry contains (x,y), or -1 if (x,y) isn't
 * over the taskbar strip at all (see gfx.c's draw_taskbar()) - checked
 * before gfx_window_hit_test() by the mouse driver (kernel/libk.c), since
 * the taskbar is the more specific target when it's what was clicked. */
int gfx_taskbar_hit_test(int x, int y);

/* ---- desktop icons: a small fixed strip of launchable-app icons drawn on
 * the bare desktop background (stage 4) - gfx.c only knows their LABELS,
 * for drawing/hit-testing; kernel.c owns which script each one launches
 * (desktop_icon_activate()), the same generic/domain-knowledge split the
 * taskbar above already uses. Registered once at boot. */
void gfx_desktop_icons_init(const char **labels, int count);

/* icon index at (x,y), or -1 if (x,y) isn't over any icon - checked by the
 * mouse driver only after both the taskbar and every window have already
 * missed, since a window dragged on top of an icon should win. */
int gfx_desktop_icon_hit_test(int x, int y);

/* ---- mouse: a cursor sprite drawn on top of everything, moved by the PS/2
 * mouse driver (kernel/libk.c, IRQ12). Position is authoritative here (not
 * in libk.c) because moving the cursor means repainting - the same
 * back-to-front redraw gfx_windows_redraw_all() already does, with the
 * sprite drawn last, on top, wherever it is; keeping "where's the cursor"
 * and "how do I redraw it" in one place avoids the two being able to drift
 * apart. */
void gfx_cursor_move(int x, int y);   /* clamps to the screen, repaints */
int  gfx_cursor_x(void);
int  gfx_cursor_y(void);

/* ---- a minimal retained widget model, keyboard- AND mouse-driven ----
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

/* Hit-tests (x,y) against every BUTTON widget's rect; on a hit, focuses it
 * (the same visible state Tab landing on it would produce) and returns 1 -
 * a real left-click's press-edge (kernel/libk.c's mouse driver) then injects
 * a synthetic '\n' into the keyboard ring so the existing gfx_widget_poll()
 * loop picks the click up exactly the way it already picks up a real Enter
 * key, with no separate mouse-event code path needed there. Returns 0 if no
 * button was hit. */
int gfx_widget_click(int x, int y);

#endif
