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

#endif
