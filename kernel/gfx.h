/* gfx.h - VGA Mode 13h (320x200, 256 colors) graphics primitives.
 * Public interface used by kernel.c (diagnostic) and by the kernel-native
 * `ui` Larzscript module in native/larzscript.c. */
#ifndef _LARZOS_GFX_H
#define _LARZOS_GFX_H

#define GFX_W 320
#define GFX_H 200

/* switches the VGA card from text mode into linear 320x200x256 graphics mode
 * and sets a small fixed palette (see gfx.c for the color indices). Only
 * call this once - it destroys the text console. */
void gfx_init(void);

void gfx_set_pixel(int x, int y, unsigned char color);
void gfx_fill_rect(int x, int y, int w, int h, unsigned char color);
void gfx_hline(int x, int y, int w, unsigned char color);
void gfx_vline(int x, int y, int h, unsigned char color);

/* draws `s` starting at (x,y), 8x8 cells per character, fg on bg.
 * ASCII only (bytes >=128 render as a placeholder glyph); ASCII 32-126
 * covered, uppercase and lowercase both render (see gfx.c for coverage
 * notes on the embedded font). */
void gfx_draw_text(int x, int y, const char *s, unsigned char fg, unsigned char bg);

/* fixed palette indices set up by gfx_init() - use these, not raw numbers */
enum {
    GFX_BLACK = 0,
    GFX_WHITE = 1,
    GFX_DARK_GRAY = 2,
    GFX_MID_GRAY = 3,
    GFX_ACCENT = 4,     /* teal/green, matches the larzos.com accent */
    GFX_ACCENT_DIM = 5,
    GFX_RED = 6,
};

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

/* Blocks for the next keyboard event. Tab cycles focus among buttons
 * (forward-only wraparound - the existing scancode table has no distinct
 * shift+tab ASCII value, so that's an honest v1 limitation, not a bug).
 * Returns the id of a button just clicked (Enter while focused), or NULL if
 * the event was a focus change or an unhandled key - callers just loop. */
const char *gfx_widget_poll(void);

#endif
