/* gfx.c - VGA Mode 13h (320x200, 256-color, linear, chain-4) graphics.
 *
 * Mode 13h is the classic "easiest real graphics mode" on PC hardware: after
 * the register sequence in gfx_init(), the framebuffer is one linear byte
 * per pixel at physical 0xA0000 (already inside the low-1GiB identity map
 * boot.S sets up - no page-table changes needed here). No BIOS/VESA calls,
 * no boot-time changes - this is a pure runtime mode switch via outb/inb,
 * the same style already used for the VGA text cursor in libk.c.
 *
 * Register values below are the standard, widely-documented Mode 13h
 * initialization sequence (Sequencer/CRTC/Graphics-Controller/Attribute
 * Controller) - the same one nearly every "VGA without BIOS" OS-dev
 * reference uses. Verify visually via QEMU's monitor `screendump` command
 * (see kernel/README.md) - this is hardware register programming, so the
 * real proof is a real captured frame, not just "it compiled."
 */
#include "gfx.h"
#include "console.h"
#include "libc/string.h"
#include "libc/stdlib.h"

typedef unsigned char u8;
typedef unsigned short u16;

static inline void outb(u16 port, u8 val){ __asm__ volatile("outb %0,%1"::"a"(val),"Nd"(port)); }
static inline u8   inb(u16 port){ u8 r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r; }

static volatile u8 *const FB = (u8*)0xA0000;

/* ---- mode switch ---- */
static void vga_write_seq(u8 idx, u8 val){ outb(0x3C4, idx); outb(0x3C5, val); }
static void vga_write_crtc(u8 idx, u8 val){ outb(0x3D4, idx); outb(0x3D5, val); }
static void vga_write_gc(u8 idx, u8 val){ outb(0x3CE, idx); outb(0x3CF, val); }
static void vga_write_attr(u8 idx, u8 val){
    (void)inb(0x3DA);          /* reset the index/data flip-flop */
    outb(0x3C0, idx);
    outb(0x3C0, val);
}

static const u8 SEQ[5]  = { 0x03, 0x01, 0x0F, 0x00, 0x0E };
static const u8 CRTC[25] = {
    0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0x41,0x00,0x00,
    0x00,0x00,0x00,0x00,0x9C,0x8E,0x8F,0x28,0x40,0x96,0xB9,0xA3,0xFF
};
static const u8 GC[9]   = { 0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0F,0xFF };
static const u8 ATTR[21] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x41,0x00,0x0F,0x00,0x00
};

/* fixed palette (index -> RGB, 6-bit-per-channel VGA DAC values 0-63) -
 * see the GFX_* enum in gfx.h. Set once at init; not meant to be dynamic. */
static const u8 PALETTE[7][3] = {
    {0,0,0},        /* GFX_BLACK */
    {63,63,63},     /* GFX_WHITE */
    {12,13,17},     /* GFX_DARK_GRAY  - matches larzos.com's #0b0f1a card bg, roughly */
    {30,32,38},     /* GFX_MID_GRAY */
    {20,58,48},     /* GFX_ACCENT     - teal/green accent */
    {12,35,29},     /* GFX_ACCENT_DIM */
    {50,10,10},     /* GFX_RED */
};

void gfx_init(void){
    outb(0x3C2, 0x63);                                   /* misc output */
    vga_write_seq(0x00, 0x03);                            /* sync reset off */
    for(int i=1;i<5;i++) vga_write_seq((u8)i, SEQ[i]);

    /* unlock CRTC registers 0-7 (bit 7 of index 0x11 protects them) */
    outb(0x3D4, 0x11); u8 cur = inb(0x3D5);
    outb(0x3D4, 0x11); outb(0x3D5, cur & 0x7F);
    for(int i=0;i<25;i++) vga_write_crtc((u8)i, CRTC[i]);

    for(int i=0;i<9;i++) vga_write_gc((u8)i, GC[i]);

    for(int i=0;i<21;i++) vga_write_attr((u8)i, ATTR[i]);
    outb(0x3C0, 0x20);                                    /* re-enable video output */

    /* palette: identity-mapped by ATTR[]'s first 16 entries, program the DAC */
    outb(0x3C8, 0);                                       /* start writing at DAC index 0 */
    for(int i=0;i<7;i++){ outb(0x3C9, PALETTE[i][0]); outb(0x3C9, PALETTE[i][1]); outb(0x3C9, PALETTE[i][2]); }

    gfx_fill_rect(0, 0, GFX_W, GFX_H, GFX_DARK_GRAY);
}

void gfx_set_pixel(int x, int y, unsigned char color){
    if(x<0 || y<0 || x>=GFX_W || y>=GFX_H) return;
    FB[y*GFX_W + x] = color;
}

void gfx_fill_rect(int x, int y, int w, int h, unsigned char color){
    int x0 = x<0?0:x, y0 = y<0?0:y;
    int x1 = x+w; if(x1>GFX_W) x1=GFX_W;
    int y1 = y+h; if(y1>GFX_H) y1=GFX_H;
    for(int yy=y0; yy<y1; yy++) for(int xx=x0; xx<x1; xx++) FB[yy*GFX_W+xx] = color;
}

void gfx_hline(int x, int y, int w, unsigned char color){ gfx_fill_rect(x,y,w,1,color); }
void gfx_vline(int x, int y, int h, unsigned char color){ gfx_fill_rect(x,y,1,h,color); }

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

static void gfx_draw_char(int x, int y, char c, unsigned char fg, unsigned char bg){
    const unsigned char *rows = font_lookup(c);
    for(int ry=0; ry<8; ry++){
        unsigned char bits = rows ? rows[ry] : 0;
        for(int rx=0; rx<8; rx++){
            int on = (bits >> (7-rx)) & 1;
            gfx_set_pixel(x+rx, y+ry, on ? fg : bg);
        }
    }
}

void gfx_draw_text(int x, int y, const char *s, unsigned char fg, unsigned char bg){
    int cx = x;
    for(const char *p=s; *p; p++){
        if(*p=='\n'){ cx=x; y+=8; continue; }
        gfx_draw_char(cx, y, *p, fg, bg);
        cx += 8;
    }
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
    gfx_fill_rect(w->x, w->y, GFX_W - w->x, 8, GFX_DARK_GRAY);
    gfx_draw_text(w->x, w->y, w->text, GFX_WHITE, GFX_DARK_GRAY);
}
static void draw_button(int idx){
    Widget *w=&g_widgets[idx];
    int focused = (idx==g_focus);
    unsigned char fill = focused ? GFX_ACCENT : GFX_ACCENT_DIM;
    unsigned char border = focused ? GFX_WHITE : GFX_MID_GRAY;
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
