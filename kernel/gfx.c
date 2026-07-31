/* gfx.c - a real linear framebuffer (via Multiboot, see boot.S), real RGB.
 *
 * Replaces the original VGA Mode 13h backend (320x200x256, paletted) - a
 * real desktop with overlapping windows needs real screen space, and Mode
 * 13h was never going to get there. GRUB negotiates a video mode via the
 * Multiboot header's video-mode-request fields and hands back where it put
 * the framebuffer (address/pitch/width/height/bpp) in the Multiboot info
 * struct passed to kernel_main - this file just reads that back and writes
 * pixels; no VGA register programming, no BIOS calls, nothing hardware-
 * specific here at all (that's all in boot.S + GRUB now).
 *
 * Getting the *header* right was the hard, verified part (see boot.S and
 * kernel/README.md) - a wrong field layout there produced a real, screen-
 * dump-confirmed GRUB failure ("unsupported graphical mode type"), not a
 * hypothetical. This file trusts what kernel_main hands it, but still
 * reads the actual granted width/height/bpp rather than assuming the
 * 1024x768x32 boot.S asked for was what GRUB actually gave.
 */
#include "gfx.h"
#include "console.h"
#include "libc/string.h"
#include "libc/stdlib.h"

/* The Multiboot1 info struct GRUB hands back - field order/sizes exactly
 * match the spec (multiboot.org), packed defensively rather than relying
 * on default alignment happening to agree (it does here, but don't leave
 * that to chance). Only the fields actually read are named. */
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

static volatile uint8_t *g_fb = 0;
static int g_fb_w = 0, g_fb_h = 0, g_fb_pitch = 0, g_fb_bpp = 0;
static uint64_t g_mb_info = 0;

void gfx_set_multiboot_info(uint64_t mb_info){ g_mb_info = mb_info; }

void gfx_init(void){
    MultibootInfo *mbi = (MultibootInfo*)(uintptr_t)g_mb_info;
    if(mbi->flags & MB_FLAG_FRAMEBUFFER){
        g_fb = (volatile uint8_t*)(uintptr_t)mbi->framebuffer_addr;
        g_fb_w = (int)mbi->framebuffer_width;
        g_fb_h = (int)mbi->framebuffer_height;
        g_fb_pitch = (int)mbi->framebuffer_pitch;
        g_fb_bpp = mbi->framebuffer_bpp;
    }
    /* if GRUB somehow didn't report a framebuffer at all, g_fb stays NULL -
     * every drawing call below is a bounds/null-checked no-op, so the
     * kernel doesn't crash, it just can't draw (a diagnostic elsewhere
     * would need to report this; not expected in practice, verified
     * granted exactly what was requested - see kernel/README.md). */
    gfx_fill_rect(0, 0, gfx_width(), gfx_height(), GFX_DARK_GRAY);
}

int gfx_width(void){ return g_fb_w; }
int gfx_height(void){ return g_fb_h; }
/* Has gfx_init() actually run? The PS/2 mouse driver (kernel/libk.c) shares
 * the IRQ path with every other boot target, including plain serial-only
 * ones that never touch graphics at all - it checks this before calling
 * ANY gfx_* function, so a mouse move on a target with no framebuffer is a
 * harmless no-op instead of writing pixels through a null/zeroed g_fb. */
int gfx_ready(void){ return g_fb != 0; }

void gfx_set_pixel(int x, int y, uint32_t color){
    if(!g_fb || x<0 || y<0 || x>=g_fb_w || y>=g_fb_h) return;
    volatile uint8_t *p = g_fb + (long)y*g_fb_pitch + (long)x*(g_fb_bpp/8);
    if(g_fb_bpp==32){
        *(volatile uint32_t*)p = color;
    } else if(g_fb_bpp==24){
        p[0]=(uint8_t)(color & 0xFF); p[1]=(uint8_t)((color>>8)&0xFF); p[2]=(uint8_t)((color>>16)&0xFF);
    } else if(g_fb_bpp==16){
        /* 5-6-5 - lossy, but a reasonable fallback if that's ever what's granted */
        uint16_t v = (uint16_t)((((color>>19)&0x1F)<<11) | (((color>>10)&0x3F)<<5) | ((color>>3)&0x1F));
        *(volatile uint16_t*)p = v;
    }
    /* other depths: not expected from a `mode_type=0` linear request; skip
     * rather than guess at a layout that could write out of bounds. */
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color){
    int x0 = x<0?0:x, y0 = y<0?0:y;
    int x1 = x+w; if(x1>g_fb_w) x1=g_fb_w;
    int y1 = y+h; if(y1>g_fb_h) y1=g_fb_h;
    for(int yy=y0; yy<y1; yy++) for(int xx=x0; xx<x1; xx++) gfx_set_pixel(xx, yy, color);
}

void gfx_hline(int x, int y, int w, uint32_t color){ gfx_fill_rect(x,y,w,1,color); }
void gfx_vline(int x, int y, int h, uint32_t color){ gfx_fill_rect(x,y,1,h,color); }

/* ---- 8x8 bitmap font, 5x7 glyphs left-packed into bits 7..3 of each row
 * byte (bits 2..0 stay 0 - natural inter-character spacing). Digits +
 * uppercase A-Z + the punctuation this project's demo text actually uses.
 * Lowercase letters render as their uppercase glyph (folded in
 * font_lookup) - an honest v1 limitation, not a bug: caps-only bitmap
 * fonts are a normal starting point for bare-metal text rendering. */
typedef struct { char ch; unsigned char rows[8]; } Glyph;
static const Glyph FONT[] = {
    {'0', {0b01110000,0b10001000,0b10011000,0b10101000,0b11001000,0b10001000,0b01110000,0}},
    {'1', {0b00100000,0b01100000,0b00100000,0b00100000,0b00100000,0b00100000,0b01110000,0}},
    {'2', {0b01110000,0b10001000,0b00001000,0b00010000,0b00100000,0b01000000,0b11111000,0}},
    {'3', {0b11111000,0b00010000,0b00100000,0b00010000,0b00001000,0b10001000,0b01110000,0}},
    {'4', {0b00010000,0b00110000,0b01010000,0b10010000,0b11111000,0b00010000,0b00010000,0}},
    {'5', {0b11111000,0b10000000,0b11110000,0b00001000,0b00001000,0b10001000,0b01110000,0}},
    {'6', {0b00110000,0b01000000,0b10000000,0b11110000,0b10001000,0b10001000,0b01110000,0}},
    {'7', {0b11111000,0b00001000,0b00010000,0b00100000,0b01000000,0b01000000,0b01000000,0}},
    {'8', {0b01110000,0b10001000,0b10001000,0b01110000,0b10001000,0b10001000,0b01110000,0}},
    {'9', {0b01110000,0b10001000,0b10001000,0b01111000,0b00001000,0b00010000,0b01100000,0}},
    {'A', {0b01110000,0b10001000,0b10001000,0b11111000,0b10001000,0b10001000,0b10001000,0}},
    {'B', {0b11110000,0b10001000,0b10001000,0b11110000,0b10001000,0b10001000,0b11110000,0}},
    {'C', {0b01110000,0b10001000,0b10000000,0b10000000,0b10000000,0b10001000,0b01110000,0}},
    {'D', {0b11100000,0b10010000,0b10001000,0b10001000,0b10001000,0b10010000,0b11100000,0}},
    {'E', {0b11111000,0b10000000,0b10000000,0b11110000,0b10000000,0b10000000,0b11111000,0}},
    {'F', {0b11111000,0b10000000,0b10000000,0b11110000,0b10000000,0b10000000,0b10000000,0}},
    {'G', {0b01110000,0b10001000,0b10000000,0b10111000,0b10001000,0b10001000,0b01111000,0}},
    {'H', {0b10001000,0b10001000,0b10001000,0b11111000,0b10001000,0b10001000,0b10001000,0}},
    {'I', {0b01110000,0b00100000,0b00100000,0b00100000,0b00100000,0b00100000,0b01110000,0}},
    {'J', {0b00001000,0b00001000,0b00001000,0b00001000,0b00001000,0b10001000,0b01110000,0}},
    {'K', {0b10001000,0b10010000,0b10100000,0b11000000,0b10100000,0b10010000,0b10001000,0}},
    {'L', {0b10000000,0b10000000,0b10000000,0b10000000,0b10000000,0b10000000,0b11111000,0}},
    {'M', {0b10001000,0b11011000,0b10101000,0b10101000,0b10001000,0b10001000,0b10001000,0}},
    {'N', {0b10001000,0b11001000,0b10101000,0b10101000,0b10011000,0b10001000,0b10001000,0}},
    {'O', {0b01110000,0b10001000,0b10001000,0b10001000,0b10001000,0b10001000,0b01110000,0}},
    {'P', {0b11110000,0b10001000,0b10001000,0b11110000,0b10000000,0b10000000,0b10000000,0}},
    {'Q', {0b01110000,0b10001000,0b10001000,0b10001000,0b10101000,0b10010000,0b01101000,0}},
    {'R', {0b11110000,0b10001000,0b10001000,0b11110000,0b10100000,0b10010000,0b10001000,0}},
    {'S', {0b01111000,0b10000000,0b10000000,0b01110000,0b00001000,0b00001000,0b11110000,0}},
    {'T', {0b11111000,0b00100000,0b00100000,0b00100000,0b00100000,0b00100000,0b00100000,0}},
    {'U', {0b10001000,0b10001000,0b10001000,0b10001000,0b10001000,0b10001000,0b01110000,0}},
    {'V', {0b10001000,0b10001000,0b10001000,0b10001000,0b10001000,0b01010000,0b00100000,0}},
    {'W', {0b10001000,0b10001000,0b10001000,0b10101000,0b10101000,0b10101000,0b01010000,0}},
    {'X', {0b10001000,0b10001000,0b01010000,0b00100000,0b01010000,0b10001000,0b10001000,0}},
    {'Y', {0b10001000,0b10001000,0b01010000,0b00100000,0b00100000,0b00100000,0b00100000,0}},
    {'Z', {0b11111000,0b00001000,0b00010000,0b00100000,0b01000000,0b10000000,0b11111000,0}},
    {' ', {0,0,0,0,0,0,0,0}},
    {'!', {0b00100000,0b00100000,0b00100000,0b00100000,0b00100000,0,0b00100000,0}},
    {'.', {0,0,0,0,0,0b01100000,0b01100000,0}},
    {',', {0,0,0,0,0b01100000,0b01100000,0b01000000,0}},
    {':', {0,0b01100000,0b01100000,0,0b01100000,0b01100000,0,0}},
    {'-', {0,0,0,0b11111000,0,0,0,0}},
    {'?', {0b01110000,0b10001000,0b00001000,0b00010000,0b00100000,0,0b00100000,0}},
    {'$', {0b00100000,0b01111000,0b10100000,0b01110000,0b00101000,0b11110000,0b00100000,0}},
    {'(', {0b00010000,0b00100000,0b01000000,0b01000000,0b01000000,0b00100000,0b00010000,0}},
    {')', {0b01000000,0b00100000,0b00010000,0b00010000,0b00010000,0b00100000,0b01000000,0}},
    {'\'',{0b00100000,0b00100000,0b01000000,0,0,0,0,0}},
    {'+', {0,0b00100000,0b00100000,0b11111000,0b00100000,0b00100000,0,0}},
};
#define NFONT (int)(sizeof(FONT)/sizeof(FONT[0]))

static const unsigned char *font_lookup(char c){
    if(c>='a' && c<='z') c = (char)(c - 32);          /* fold lowercase -> uppercase glyph */
    for(int i=0;i<NFONT;i++) if(FONT[i].ch==c) return FONT[i].rows;
    return 0;                                          /* unsupported char: draw nothing */
}

static void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg){
    const unsigned char *rows = font_lookup(c);
    for(int ry=0; ry<8; ry++){
        unsigned char bits = rows ? rows[ry] : 0;
        for(int rx=0; rx<8; rx++){
            int on = (bits >> (7-rx)) & 1;
            gfx_set_pixel(x+rx, y+ry, on ? fg : bg);
        }
    }
}

void gfx_draw_text(int x, int y, const char *s, uint32_t fg, uint32_t bg){
    int cx = x;
    for(const char *p=s; *p; p++){
        if(*p=='\n'){ cx=x; y+=8; continue; }
        gfx_draw_char(cx, y, *p, fg, bg);
        cx += 8;
    }
}

/* ---- windows: a real compositor, back-to-front by z-order ---- */
typedef struct {
    char title[GFX_TITLE_LEN];
    int x, y, w, h;
} Window;
static Window g_windows[GFX_MAX_WINDOWS];
static int g_window_order[GFX_MAX_WINDOWS];   /* window indices, back(0)..front(n-1) */
static int g_nwindows = 0;
static int g_focused_window = -1;

static void draw_window_chrome(int idx){
    Window *win = &g_windows[idx];
    int focused = (idx == g_focused_window);
    uint32_t bar = focused ? GFX_ACCENT : GFX_ACCENT_DIM;
    gfx_fill_rect(win->x, win->y, win->w, GFX_TITLEBAR_H, bar);
    gfx_draw_text(win->x + 6, win->y + (GFX_TITLEBAR_H - 8) / 2, win->title, GFX_WHITE, bar);
    gfx_fill_rect(win->x, win->y + GFX_TITLEBAR_H, win->w, win->h - GFX_TITLEBAR_H, GFX_DARK_GRAY);
    uint32_t border = focused ? GFX_WHITE : GFX_MID_GRAY;
    gfx_hline(win->x, win->y, win->w, border);
    gfx_hline(win->x, win->y + win->h - 1, win->w, border);
    gfx_vline(win->x, win->y, win->h, border);
    gfx_vline(win->x + win->w - 1, win->y, win->h, border);
}

/* classic arrow cursor, 'X'=black outline, 'W'=white fill, '.'=see-through.
 * Drawn last, on top, by every gfx_windows_redraw_all() call - the same
 * "repaint everything" compositor approach as windows themselves (see the
 * comment above), just one more layer on top. */
#define CURSOR_W 12
#define CURSOR_H 19
static const char *CURSOR[CURSOR_H] = {
    "X...........", "XX..........", "XWX.........", "XWWX........",
    "XWWWX.......", "XWWWWX......", "XWWWWWX.....", "XWWWWWWX....",
    "XWWWWWWWX...", "XWWWWWWWWX..", "XWWWWWWWWWX.", "XWWWWWWWWWWX",
    "XWWWWWWXXXX.", "XWWWXWWX....", "XWWX.XWWX...", "XWX..XWWX...",
    "XX....XWWX..", "X......XWWX.", ".......XXX..",
};
static int g_cursor_x = 100, g_cursor_y = 100;
static int g_cursor_ready = 0;   /* only draw once a caller has actually moved it */

static void draw_cursor(void){
    if(!g_cursor_ready) return;
    for(int r=0; r<CURSOR_H; r++){
        int py = g_cursor_y + r;
        if(py < 0 || py >= gfx_height()) continue;
        for(int c=0; CURSOR[r][c]; c++){
            int px = g_cursor_x + c;
            if(px < 0 || px >= gfx_width()) continue;
            char ch = CURSOR[r][c];
            if(ch=='X') gfx_set_pixel(px, py, GFX_BLACK);
            else if(ch=='W') gfx_set_pixel(px, py, GFX_WHITE);
        }
    }
}

void gfx_windows_redraw_all(void){
    gfx_fill_rect(0, 0, gfx_width(), gfx_height(), GFX_BLACK);   /* desktop background */
    for(int i=0; i<g_nwindows; i++) draw_window_chrome(g_window_order[i]);
    draw_cursor();
}

void gfx_cursor_move(int x, int y){
    if(x < 0) x = 0;
    if(x >= gfx_width()) x = gfx_width()-1;
    if(y < 0) y = 0;
    if(y >= gfx_height()) y = gfx_height()-1;
    g_cursor_x = x; g_cursor_y = y; g_cursor_ready = 1;
    gfx_windows_redraw_all();   /* draws the cursor too, but widgets (below) may still cover it */
    gfx_widget_redraw_all();   /* widgets aren't window-clipped yet (see gfx.h) - without this,
                                * every mouse move would wipe them along with the black desktop
                                * repaint above; a no-op when no widgets exist. */
    draw_cursor();             /* redraw on top, LAST - a widget whose rect happens to sit under
                                * the cursor (e.g. a terminal filling most of its window) would
                                * otherwise paint back over it, a real bug caught by an actual
                                * screendump: the cursor was there after gfx_windows_redraw_all()
                                * and gone again after gfx_widget_redraw_all() drew on top of it. */
}
int gfx_cursor_x(void){ return g_cursor_x; }
int gfx_cursor_y(void){ return g_cursor_y; }

int gfx_window_hit_test(int x, int y){
    for(int i=g_nwindows-1; i>=0; i--){        /* front-to-back */
        Window *win = &g_windows[g_window_order[i]];
        if(x>=win->x && x<win->x+win->w && y>=win->y && y<win->y+win->h) return g_window_order[i];
    }
    return -1;
}

void gfx_window_focus(int idx){
    if(idx < 0 || idx >= g_nwindows) return;
    int pos = -1;
    for(int i=0; i<g_nwindows; i++) if(g_window_order[i]==idx){ pos=i; break; }
    if(pos < 0) return;
    for(int i=pos; i<g_nwindows-1; i++) g_window_order[i] = g_window_order[i+1];
    g_window_order[g_nwindows-1] = idx;
    g_focused_window = idx;
    gfx_windows_redraw_all();
}

void gfx_window_focus_next(void){
    if(g_nwindows == 0) return;
    if(g_focused_window < 0){ gfx_window_focus(g_window_order[g_nwindows-1]); return; }
    int pos = 0;
    for(int i=0; i<g_nwindows; i++) if(g_window_order[i]==g_focused_window){ pos=i; break; }
    gfx_window_focus(g_window_order[(pos+1) % g_nwindows]);
}

int gfx_window_focused(void){ return g_focused_window; }

int gfx_window_create(const char *title, int x, int y, int w, int h){
    if(g_nwindows >= GFX_MAX_WINDOWS) return -1;
    int idx = g_nwindows++;
    Window *win = &g_windows[idx];
    strncpy(win->title, title, GFX_TITLE_LEN-1); win->title[GFX_TITLE_LEN-1] = 0;
    win->x=x; win->y=y; win->w=w; win->h=h;
    g_window_order[idx] = idx;   /* appended - already at the "front" end of the order array */
    gfx_window_focus(idx);       /* new windows come to front and take focus, redraws everything */
    return idx;
}

void gfx_window_client_rect(int idx, int *x, int *y, int *w, int *h){
    if(idx < 0 || idx >= g_nwindows){ *x=*y=*w=*h=0; return; }
    Window *win = &g_windows[idx];
    *x = win->x + 1;
    *y = win->y + GFX_TITLEBAR_H;
    *w = win->w - 2;
    *h = win->h - GFX_TITLEBAR_H - 1;
}

/* ---- widgets ---- */
typedef enum { WIDGET_LABEL, WIDGET_BUTTON, WIDGET_TERMINAL } WidgetKind;
typedef struct {
    char id[GFX_ID_LEN];
    WidgetKind kind;
    int x, y, w, h;
    char text[GFX_TEXT_LEN];
} Widget;
static Widget g_widgets[GFX_MAX_WIDGETS];
static int g_nwidgets = 0;
static int g_focus = -1;               /* index into g_widgets of the focused button, or -1 */

/* Terminal state, parallel to g_widgets by index (only populated for
 * WIDGET_TERMINAL widgets) - a real scrolling line buffer, unlike the fixed
 * GFX_TEXT_LEN text every other widget kind uses. Statically zero-init'd,
 * so `lines==NULL` reliably means "not yet initialized" (see
 * gfx_widget_terminal()/draw_terminal()). */
typedef struct {
    char **lines;              /* rows malloc'd buffers, each cols+1 bytes */
    int rows, cols;
    int cur_row, cur_col;
} Terminal;
static Terminal g_terminals[GFX_MAX_WIDGETS];
static int g_active_terminal = -1;     /* index into g_widgets/g_terminals, or -1 - the ONE
                                         * widget gfx_terminal_putc() feeds; v1 supports just one */

static int find_widget(const char *id){
    for(int i=0;i<g_nwidgets;i++) if(strcmp(g_widgets[i].id, id)==0) return i;
    return -1;
}
int gfx_widget_index(const char *id){ return find_widget(id); }

static void draw_label(int idx){
    Widget *w=&g_widgets[idx];
    /* Clear to the edge of the screen, not just the NEW text's width - if
     * the new text is shorter than what was there before (e.g. "$10.00" ->
     * "$8.00"), sizing the clear to the new text leaves a stale trailing
     * character on screen. Caught by an actual screendump during testing:
     * "seller $2.000" / "...for $2.00K" - real leftover glyph fragments,
     * not a rendering glitch. Assumes no other widget sits further right
     * on the same row, true for every label this project draws so far. */
    gfx_fill_rect(w->x, w->y, gfx_width() - w->x, 8, GFX_DARK_GRAY);
    gfx_draw_text(w->x, w->y, w->text, GFX_WHITE, GFX_DARK_GRAY);
}
static void draw_button(int idx){
    Widget *w=&g_widgets[idx];
    int focused = (idx==g_focus);
    uint32_t fill = focused ? GFX_ACCENT : GFX_ACCENT_DIM;
    uint32_t border = focused ? GFX_WHITE : GFX_MID_GRAY;
    gfx_fill_rect(w->x, w->y, w->w, w->h, fill);
    gfx_hline(w->x, w->y, w->w, border);
    gfx_hline(w->x, w->y+w->h-1, w->w, border);
    gfx_vline(w->x, w->y, w->h, border);
    gfx_vline(w->x+w->w-1, w->y, w->h, border);
    int tw = (int)strlen(w->text)*8;
    int tx = w->x + (w->w-tw)/2; if(tx < w->x+2) tx = w->x+2;
    int ty = w->y + (w->h-8)/2;
    gfx_draw_text(tx, ty, w->text, GFX_WHITE, fill);
}
static void draw_terminal(int idx){
    Widget *w = &g_widgets[idx];
    Terminal *t = &g_terminals[idx];
    gfx_fill_rect(w->x, w->y, w->w, w->h, GFX_BLACK);
    gfx_hline(w->x, w->y, w->w, GFX_MID_GRAY);
    gfx_hline(w->x, w->y+w->h-1, w->w, GFX_MID_GRAY);
    gfx_vline(w->x, w->y, w->h, GFX_MID_GRAY);
    gfx_vline(w->x+w->w-1, w->y, w->h, GFX_MID_GRAY);
    if(!t->lines) return;   /* first redraw, triggered from inside register_widget - not initialized yet */
    for(int r=0; r<t->rows; r++) gfx_draw_text(w->x+2, w->y+2+r*8, t->lines[r], GFX_WHITE, GFX_BLACK);
}
static void redraw_widget(int idx){
    if(idx<0 || idx>=g_nwidgets) return;
    if(g_widgets[idx].kind==WIDGET_BUTTON) draw_button(idx);
    else if(g_widgets[idx].kind==WIDGET_TERMINAL) draw_terminal(idx);
    else draw_label(idx);
}
void gfx_widget_redraw_all(void){ for(int i=0;i<g_nwidgets;i++) redraw_widget(i); }

static int register_widget(const char *id, WidgetKind kind, int x, int y, int w, int h, const char *text){
    int idx = find_widget(id);
    if(idx<0){
        if(g_nwidgets>=GFX_MAX_WIDGETS) return -1;
        idx = g_nwidgets++;
    }
    Widget *wi = &g_widgets[idx];
    strncpy(wi->id, id, GFX_ID_LEN-1); wi->id[GFX_ID_LEN-1]=0;
    wi->kind=kind; wi->x=x; wi->y=y; wi->w=w; wi->h=h;
    strncpy(wi->text, text, GFX_TEXT_LEN-1); wi->text[GFX_TEXT_LEN-1]=0;
    if(kind==WIDGET_BUTTON && g_focus<0) g_focus=idx;   /* first button registered gets initial focus */
    redraw_widget(idx);
    return idx;
}
int gfx_widget_label(const char *id, int x, int y, const char *text){
    return register_widget(id, WIDGET_LABEL, x, y, 0, 8, text);
}
int gfx_widget_button(const char *id, int x, int y, int w, int h, const char *text){
    return register_widget(id, WIDGET_BUTTON, x, y, w, h, text);
}
void gfx_widget_set_text(const char *id, const char *text){
    int idx = find_widget(id); if(idx<0) return;
    strncpy(g_widgets[idx].text, text, GFX_TEXT_LEN-1); g_widgets[idx].text[GFX_TEXT_LEN-1]=0;
    redraw_widget(idx);
}

int gfx_widget_terminal(const char *id, int x, int y, int w, int h){
    int idx = register_widget(id, WIDGET_TERMINAL, x, y, w, h, "");
    if(idx < 0) return -1;
    int cols = (w - 4) / 8; if(cols < 1) cols = 1;
    int rows = (h - 4) / 8; if(rows < 1) rows = 1;
    Terminal *t = &g_terminals[idx];
    t->lines = (char**)malloc(sizeof(char*) * (size_t)rows);
    for(int i=0; i<rows; i++){ t->lines[i] = (char*)malloc((size_t)cols + 1); t->lines[i][0] = 0; }
    t->rows = rows; t->cols = cols; t->cur_row = 0; t->cur_col = 0;
    g_active_terminal = idx;
    redraw_widget(idx);   /* the redraw triggered inside register_widget ran before t->lines existed */
    return idx;
}

/* Appends one character to the active terminal widget - called from
 * kernel/libk.c's serial_putc() for EVERY character any program prints, so
 * plain print()/puts()/printf() output shows up in the GUI with no code
 * changes to the program producing it. A no-op if no terminal widget has
 * been created yet - every other boot target is completely unaffected. */
void gfx_terminal_putc(char c){
    if(g_active_terminal < 0) return;
    Terminal *t = &g_terminals[g_active_terminal];
    if(!t->lines) return;
    if(c == '\r') return;         /* serial_putc already treats \n as the one newline signal */
    if(c == '\n'){
        t->cur_row++; t->cur_col = 0;
        if(t->cur_row >= t->rows){
            for(int i=0; i<t->rows-1; i++){ char *tmp=t->lines[i]; t->lines[i]=t->lines[i+1]; t->lines[i+1]=tmp; }
            t->lines[t->rows-1][0] = 0;
            t->cur_row = t->rows - 1;
            redraw_widget(g_active_terminal);   /* every line moved - a full repaint, not an append */
            return;
        }
        t->lines[t->cur_row][0] = 0;
        return;
    }
    if(c < 32 || c > 126) return;  /* other control chars (e.g. a raw \b/\t) - skip, not meaningful here */
    if(t->cur_col >= t->cols){ gfx_terminal_putc('\n'); gfx_terminal_putc(c); return; }
    char *line = t->lines[t->cur_row];
    line[t->cur_col] = c;
    line[t->cur_col + 1] = 0;
    t->cur_col++;
    Widget *w = &g_widgets[g_active_terminal];
    char buf[2] = { c, 0 };
    gfx_draw_text(w->x + 2 + (t->cur_col - 1) * 8, w->y + 2 + t->cur_row * 8, buf, GFX_WHITE, GFX_BLACK);
}

static void focus_next(void){
    if(g_nwidgets==0) return;
    int old=g_focus, i=g_focus;
    for(int step=0; step<g_nwidgets; step++){
        i=(i+1)%g_nwidgets;
        if(g_widgets[i].kind==WIDGET_BUTTON){ g_focus=i; break; }
    }
    if(old>=0) redraw_widget(old);
    if(g_focus>=0) redraw_widget(g_focus);
}

const char *gfx_widget_poll(void){
    char c = console_getc();
    if(c=='\t'){ focus_next(); return 0; }
    if(c=='\n' || c=='\r'){
        if(g_focus>=0 && g_focus<g_nwidgets) return g_widgets[g_focus].id;
        return 0;
    }
    return 0;
}

int gfx_widget_click(int x, int y){
    for(int i=0; i<g_nwidgets; i++){
        Widget *w = &g_widgets[i];
        if(w->kind!=WIDGET_BUTTON) continue;
        if(x>=w->x && x<w->x+w->w && y>=w->y && y<w->y+w->h){
            int old=g_focus; g_focus=i;
            if(old>=0 && old!=i) redraw_widget(old);
            redraw_widget(i);
            return 1;
        }
    }
    return 0;
}
