/* kernel.c - the LarzOS kernel (Stage 1), freestanding C, no libc.
 *
 * This is the small machine-facing seed the roadmap calls unavoidable: it talks
 * to hardware directly (serial port, VGA text buffer, I/O ports) so that
 * everything above it can eventually be Larzscript. For now it brings the
 * machine up and runs a tiny serial shell that already carries the LarzOS
 * identity - including a kernel-level compute wallet, money-native from the
 * very bottom.
 *
 * Testable headlessly:
 *   qemu-system-i386 -kernel larzkernel.elf -display none -serial stdio \
 *                    -monitor none -no-reboot \
 *                    -device isa-debug-exit,iobase=0xf4,iosize=0x04
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* ---- port I/O ---- */
static inline void outb(u16 port, u8 val) { __asm__ volatile("outb %0,%1"::"a"(val),"Nd"(port)); }
static inline u8   inb(u16 port) { u8 r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r; }

/* ---- serial (COM1 @ 0x3F8), 115200 8N1 - our headless console ---- */
#define COM1 0x3F8
static void serial_init(void) {
    outb(COM1+1, 0x00);      /* disable interrupts */
    outb(COM1+3, 0x80);      /* enable DLAB (baud divisor) */
    outb(COM1+0, 0x01);      /* divisor 1 => 115200 baud (low) */
    outb(COM1+1, 0x00);      /* divisor high */
    outb(COM1+3, 0x03);      /* 8 bits, no parity, 1 stop */
    outb(COM1+2, 0xC7);      /* enable + clear FIFO, 14-byte threshold */
    outb(COM1+4, 0x0B);      /* IRQs on, RTS/DSR set */
}
static int  serial_can_read(void) { return inb(COM1+5) & 0x01; }
static int  serial_tx_ready(void) { return inb(COM1+5) & 0x20; }
static void serial_putc(char c) {
    while (!serial_tx_ready()) {}
    outb(COM1, (u8)c);
}
static char serial_getc(void) {
    while (!serial_can_read()) {}
    return (char)inb(COM1);
}

/* ---- VGA text buffer (so output also shows on a real screen) ---- */
static volatile u16 *const VGA = (u16*)0xB8000;
static int vga_row = 0, vga_col = 0;
static void vga_putc(char c) {
    if (c == '\n') { vga_col = 0; if (++vga_row >= 25) vga_row = 0; return; }
    if (c == '\r') { vga_col = 0; return; }
    VGA[vga_row*80 + vga_col] = (u16)c | (0x0F << 8);   /* white on black */
    if (++vga_col >= 80) { vga_col = 0; if (++vga_row >= 25) vga_row = 0; }
}

/* ---- combined output ---- */
static void putc(char c) { serial_putc(c); vga_putc(c); }
static void puts(const char *s) { while (*s) putc(*s++); }
static void putln(const char *s) { puts(s); putc('\n'); }
static void putu(u32 n) {           /* unsigned decimal */
    char buf[12]; int i = 0;
    if (n == 0) { putc('0'); return; }
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i) putc(buf[--i]);
}
/* print integer cents as $D.CC */
static void put_money(u32 cents) {
    putc('$'); putu(cents / 100); putc('.');
    u32 c = cents % 100;
    putc('0' + (c / 10)); putc('0' + (c % 10));
}

/* ---- tiny string helpers ---- */
static int streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* ---- read a line from serial, with echo + backspace ---- */
static void readline(char *buf, int max) {
    int n = 0;
    for (;;) {
        char c = serial_getc();
        if (c == '\r' || c == '\n') { putc('\n'); buf[n] = 0; return; }
        if ((c == 0x7F || c == 0x08) && n > 0) { n--; puts("\b \b"); continue; }
        if (c >= 32 && c < 127 && n < max-1) { buf[n++] = c; putc(c); }
    }
}

/* ---- QEMU clean exit (isa-debug-exit device) ---- */
static void qemu_exit(u8 code) { outb(0xf4, code); }

/* ---- the shell: money-native from the kernel up ---- */
static u32 g_wallet = 10000;         /* $100.00 of compute credit */

static void banner(void) {
    putln("");
    putln("  LarzOS  -  the money-native operating system");
    putln("  kernel (Stage 1) booted on bare metal - no Linux underneath.");
    putln("  type 'help' for commands.");
    putln("");
}

static void cmd_help(void) {
    putln("kernel shell commands:");
    putln("  help      this help");
    putln("  about     about LarzOS");
    putln("  echo ...  print the rest of the line");
    putln("  balance   show the kernel compute wallet");
    putln("  earn      credit $1.00 of compute");
    putln("  spend     spend $0.50 of compute (fails closed when empty)");
    putln("  clear     clear the screen");
    putln("  halt      power off (QEMU)");
}

void kernel_main(void) {
    serial_init();
    banner();
    putln("  reached 64-bit long mode; paging on, low 1 GiB identity-mapped.");
    putln("");

    char line[256];
    for (;;) {
        puts("larzos# ");
        readline(line, sizeof line);

        /* split off the first word */
        char *arg = line;
        while (*arg && *arg != ' ') arg++;
        char saved = *arg; if (*arg) *arg = 0;
        char *rest = saved ? arg + 1 : arg;

        if (line[0] == 0) continue;
        else if (streq(line, "help"))  cmd_help();
        else if (streq(line, "about")) {
            putln("LarzOS - an OS whose userland is written in Larzscript.");
            putln("This kernel is the machine-facing seed; the goal is to shrink");
            putln("it to nothing as Larzscript gains a native compiler.");
        }
        else if (streq(line, "echo"))  putln(rest);
        else if (streq(line, "balance")) { puts("compute wallet: "); put_money(g_wallet); putc('\n'); }
        else if (streq(line, "earn")) { g_wallet += 100; puts("credited $1.00; balance "); put_money(g_wallet); putc('\n'); }
        else if (streq(line, "spend")) {
            if (g_wallet < 50) putln("out of compute credit - refused (fails closed).");
            else { g_wallet -= 50; puts("spent $0.50; balance "); put_money(g_wallet); putc('\n'); }
        }
        else if (streq(line, "clear")) { for (int i=0;i<80*25;i++) VGA[i]=(u16)' '|(0x0F<<8); vga_row=vga_col=0; puts("\033[2J\033[H"); }
        else if (streq(line, "halt")) { putln("LarzOS halted."); qemu_exit(0); break; }
        else { puts("unknown command: "); putln(line); }
    }
    for (;;) __asm__ volatile("hlt");
}
