/* libk.c - the LarzOS freestanding runtime.
 *
 * A tiny libc/libm replacement so the *unmodified* Larzscript interpreter can
 * be compiled and run on bare metal: a heap allocator, string/memory/ctype
 * helpers, a streaming printf with %g, strtod, freestanding math, and a FILE
 * layer that routes stdin/stdout/stderr through the serial console. OS-facing
 * calls (files, dirs, env, exec, clocks) are stubbed - bare metal has no
 * filesystem yet, so those Larzscript builtins simply fail gracefully.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "console.h"
#include "libc/stdio.h"
#include "libc/stdlib.h"
#include "libc/string.h"
#include "libc/ctype.h"
#include "libc/time.h"
#include "libc/unistd.h"
#include "libc/dirent.h"
#include "libc/sys/stat.h"

typedef unsigned char u8;
typedef unsigned short u16;

/* ======================================================================
 * Console: I/O ports, 16550 serial (COM1), VGA text, QEMU exit
 * ==================================================================== */
static inline void outb(u16 port, u8 val){ __asm__ volatile("outb %0,%1"::"a"(val),"Nd"(port)); }
static inline u8   inb(u16 port){ u8 r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r; }

#define COM1 0x3F8
int serial_can_read(void){ return inb(COM1+5) & 0x01; }
static int serial_tx_ready(void){ return inb(COM1+5) & 0x20; }

static volatile u16 *const VGA = (u16*)0xB8000;
static int vga_row = 0, vga_col = 0;
#define VGA_ATTR (0x0F<<8)                 /* white on black */
static void vga_cursor(void){
    unsigned pos = vga_row*80 + vga_col;
    outb(0x3D4,14); outb(0x3D5,(u8)(pos>>8));
    outb(0x3D4,15); outb(0x3D5,(u8)pos);
}
static void vga_scroll(void){
    for(int i=0;i<24*80;i++) VGA[i]=VGA[i+80];
    for(int i=24*80;i<25*80;i++) VGA[i]=' '|VGA_ATTR;
}
static void vga_putc(char c){
    if(c=='\n'){ vga_col=0; if(++vga_row>=25){ vga_row=24; vga_scroll(); } }
    else if(c=='\r'){ vga_col=0; }
    else if(c=='\b'){ if(vga_col>0){ vga_col--; VGA[vga_row*80+vga_col]=' '|VGA_ATTR; } }
    else {
        VGA[vga_row*80+vga_col] = (u16)c | VGA_ATTR;
        if(++vga_col>=80){ vga_col=0; if(++vga_row>=25){ vga_row=24; vga_scroll(); } }
    }
    vga_cursor();
}
void serial_putc(char c){
    if(c=='\n'){ while(!serial_tx_ready()){} outb(COM1,'\r'); }
    while(!serial_tx_ready()){}
    outb(COM1,(u8)c);
    vga_putc(c);
}
char serial_getc(void){
    while(!serial_can_read()){}
    return (char)inb(COM1);
}

/* ---- PS/2 keyboard (polled, scancode set 1) - local input on real HW/VM ---- */
static const char kmap[0x40] = {
    0,   27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0,'\\','z','x','c','v',
    'b','n','m',',','.','/', 0, '*', 0, ' ', 0,0,0,0,0,0,
};
static const char kmap_s[0x40] = {
    0,   27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0, 'A','S',
    'D','F','G','H','J','K','L',':','"','~', 0, '|','Z','X','C','V',
    'B','N','M','<','>','?', 0, '*', 0, ' ', 0,0,0,0,0,0,
};
static int kbd_shift=0, kbd_caps=0;
int kbd_translate(unsigned char sc){       /* scancode -> ASCII, or -1 (used by IRQ1) */
    if(sc==0x2A||sc==0x36){ kbd_shift=1; return -1; }
    if(sc==0xAA||sc==0xB6){ kbd_shift=0; return -1; }
    if(sc==0x3A){ kbd_caps^=1; return -1; }
    if(sc & 0x80) return -1;               /* key release */
    if(sc >= 0x40) return -1;              /* keypad / F-keys: ignore */
    char base = kmap[sc];
    if(base>='a' && base<='z'){ int up = kbd_shift ^ kbd_caps; return up? base-32 : base; }
    char c = kbd_shift ? kmap_s[sc] : kmap[sc];
    return c? (unsigned char)c : -1;
}

/* interrupt-driven keyboard: IRQ1 pushes chars into this ring */
static volatile char kbring[256]; static volatile int kbhead=0, kbtail=0;
void kbring_push(char c){ int nx=(kbhead+1)&255; if(nx!=kbtail){ kbring[kbhead]=c; kbhead=nx; } }
static int kbring_pop(void){ if(kbtail==kbhead) return -1; char c=kbring[kbtail]; kbtail=(kbtail+1)&255; return (unsigned char)c; }
int g_irq_on=0;

/* Block for a char from the serial line OR the PS/2 keyboard.
 * The keyboard is read by POLLING the 8042 output buffer directly - a single
 * reader of port 0x60, so there's no double-read. IRQ1 is masked at the PIC
 * (see pic_remap) precisely so the interrupt handler can't also consume the
 * same scancode (which produced duplicated keys on IRQ-delivering hosts like
 * QEMU). Polling reads the scancode buffer whether or not the host delivers
 * the keyboard interrupt, which is exactly what VirtualBox / real hardware
 * needed. The timer IRQ0 still drives the scheduler; only IRQ1 is polled. */
char console_getc(void){
    for(;;){
        if(serial_can_read()) return (char)inb(COM1);
        int k=kbring_pop(); if(k>=0) return (char)k;       /* keys captured by the timer tick */
    }
}

static void clock_init(void);            /* RTC/TSC clock, defined below */
void console_init(void){
    outb(COM1+1,0x00); outb(COM1+3,0x80);
    outb(COM1+0,0x01); outb(COM1+1,0x00);   /* 115200 baud */
    outb(COM1+3,0x03); outb(COM1+2,0xC7); outb(COM1+4,0x0B);
    while(inb(0x64)&1) (void)inb(0x60);      /* drain any pending keyboard bytes */
    vga_cursor();
    clock_init();                            /* calibrate the TSC against the PIT */
}
void qemu_exit(unsigned char code){ outb(0xf4, code); }

/* ======================================================================
 * Interrupts: IDT + PIC remap + timer(IRQ0) + keyboard(IRQ1). See interrupt.S.
 * ==================================================================== */
struct iframe { uint64_t r15,r14,r13,r12,r11,r10,r9,r8,rbp,rdi,rsi,rdx,rcx,rbx,rax, int_no, err, rip, cs, rflags, rsp, ss; };
struct idtent { u16 lo, sel; u8 ist, flags; u16 mid; uint32_t hi, zero; } __attribute__((packed));
struct idtptr { u16 limit; uint64_t base; } __attribute__((packed));
static struct idtent g_idt[256];
extern void *isr_stub_table[];
static volatile unsigned long long g_ticks=0;
/* per-command gas: the kernel bills + kills the interpreter (task 0) by CPU ticks */
extern volatile int larz_gas_kill;
static volatile unsigned g_gas_budget=0, g_gas_used=0; static volatile int g_gas_on=0;
static void idt_set(int n, void *h){
    uint64_t a=(uint64_t)h;
    g_idt[n].lo=a&0xFFFF; g_idt[n].sel=0x08; g_idt[n].ist=0; g_idt[n].flags=0x8E;
    g_idt[n].mid=(a>>16)&0xFFFF; g_idt[n].hi=(a>>32)&0xFFFFFFFF; g_idt[n].zero=0;
}
/* ---- preemptive scheduler: round-robin tasks switched on the timer tick ---- */
#define NTASK 4
#define TSTK  65536
struct task { uint64_t rsp; int used; };
static struct task g_tasks[NTASK];
static int g_ntask=0, g_cur=0;
static unsigned char g_tstack[NTASK][TSTK] __attribute__((aligned(16)));
volatile unsigned long long g_ca=0, g_cb=0;               /* demo task counters */

static uint64_t schedule(uint64_t rsp){
    if(g_ntask<2) return rsp;
    g_tasks[g_cur].rsp = rsp;
    do { g_cur=(g_cur+1)%g_ntask; } while(!g_tasks[g_cur].used);
    return g_tasks[g_cur].rsp;
}
static void task_create(void (*fn)(void)){
    if(g_ntask>=NTASK) return;
    int i=g_ntask++;
    g_tasks[i].used=1;
    uint64_t top=((uint64_t)(g_tstack[i]+TSTK)) & ~15ULL;
    struct iframe *f=(struct iframe*)(top - sizeof(struct iframe));
    for(unsigned k=0;k<sizeof(struct iframe)/8;k++) ((uint64_t*)f)[k]=0;
    f->rip=(uint64_t)fn; f->cs=0x08; f->rflags=0x202; f->rsp=top-8; f->ss=0; f->int_no=32;
    g_tasks[i].rsp=(uint64_t)f;
}
static void task_a(void){ for(;;){ g_ca++; for(volatile int i=0;i<80000;i++){} } }
static void task_b(void){ for(;;){ g_cb++; for(volatile int i=0;i<200000;i++){} } }
void sched_init(void){                                    /* task 0 = the main/interpreter context */
    g_tasks[0].used=1; g_ntask=1; g_cur=0;
    task_create(task_a);
    task_create(task_b);
}

/* Drain the 8042 output buffer into the key ring. Called from BOTH the keyboard
 * IRQ (n==33, immediate - no lost keys on fast bursts) AND the timer tick
 * (n==32, a 100 Hz safety net in case the host doesn't deliver the keyboard
 * IRQ). It's dup-free: every read is guarded by the status bit and reading
 * clears the byte, and interrupt handlers don't nest, so whichever fires first
 * consumes the byte and the other sees an empty buffer. Mouse (aux) bytes are
 * skipped. This is robust across QEMU, VirtualBox and bare metal. */
static void kbd_drain(void){
    for(int g=0; g<32; g++){
        unsigned char st=inb(0x64);
        if(!(st&1)) break;                                /* output buffer empty */
        unsigned char sc=inb(0x60);                       /* read clears the byte */
        if(st&0x20) continue;                             /* bit5 = mouse (aux) byte -> ignore */
        int c=kbd_translate(sc); if(c>0) kbring_push((char)c);
    }
}

uint64_t interrupt_dispatch(uint64_t rsp){                /* called from isr_common; returns new rsp */
    struct iframe *f=(struct iframe*)rsp;
    unsigned n=(unsigned)f->int_no;
    if(n<32){                                             /* CPU exception: report + halt */
        printf("\n[CPU exception %u  err=%x  RIP=%lx]  halting.\n", n, (unsigned)f->err, (unsigned long)f->rip);
        for(;;) __asm__ volatile("cli; hlt");
    }
    if(n==32){                                            /* timer: tick, poll kbd (backup), gas, switch task */
        g_ticks++;
        kbd_drain();
        if(g_gas_on && g_cur==0){ g_gas_used++; if(g_gas_used > g_gas_budget){ larz_gas_kill=1; g_gas_on=0; } }
        rsp=schedule(rsp);
    }
    else if(n==33){ kbd_drain(); }                        /* keyboard IRQ: immediate capture */
    if(n>=40) outb(0xA0,0x20);                            /* EOI to slave */
    outb(0x20,0x20);                                      /* EOI to master */
    return rsp;
}
static void pic_remap(void){
    outb(0x20,0x11); outb(0xA0,0x11);
    outb(0x21,0x20); outb(0xA1,0x28);                     /* master->32, slave->40 */
    outb(0x21,0x04); outb(0xA1,0x02);
    outb(0x21,0x01); outb(0xA1,0x01);
    outb(0x21,0xFC); outb(0xA1,0xFF);                     /* unmask IRQ0 (timer) + IRQ1 (keyboard) */
}

/* We deliberately do NOT reprogram the 8042 controller. The firmware/BIOS has
 * already enabled the keyboard port, turned on IRQ1 in the config byte and put
 * the keyboard into scanning mode before handing off (GRUB reads the keyboard,
 * so it is provably live by the time we run). An aggressive re-init that
 * disables the ports and rewrites the config byte was found to leave the
 * keyboard dead on real VirtualBox - so we just leave the working setup alone,
 * unmask IRQ1, and let the IRQ handler drain scancodes. */
void ints_init(void){
    for(int i=0;i<48;i++) idt_set(i, isr_stub_table[i]);
    struct idtptr p; p.limit=sizeof(g_idt)-1; p.base=(uint64_t)g_idt;
    __asm__ volatile("lidt %0"::"m"(p));
    pic_remap();                                                           /* unmasks IRQ0 (timer) + IRQ1 (keyboard) */
    while(inb(0x64)&1) (void)inb(0x60);                                    /* drain any stale scancode so IRQ1 can re-fire */
    outb(0x43,0x36); outb(0x40, 11932&0xFF); outb(0x40, (11932>>8)&0xFF);   /* PIT ch0 ~100 Hz */
    g_irq_on=1;
    __asm__ volatile("sti");
}

static void con_puts(const char *s){ while(*s) serial_putc(*s++); }

/* ======================================================================
 * Memory + string + ctype
 * ==================================================================== */
void *memset(void *s, int c, size_t n){ u8 *p=s; while(n--) *p++=(u8)c; return s; }
void *memcpy(void *d, const void *s, size_t n){ u8 *dp=d; const u8 *sp=s; while(n--) *dp++=*sp++; return d; }
void *memmove(void *d, const void *s, size_t n){
    u8 *dp=d; const u8 *sp=s;
    if(dp<sp){ while(n--) *dp++=*sp++; }
    else { dp+=n; sp+=n; while(n--) *--dp=*--sp; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n){ const u8 *x=a,*y=b; while(n--){ if(*x!=*y) return *x-*y; x++;y++; } return 0; }

size_t strlen(const char *s){ const char *p=s; while(*p) p++; return (size_t)(p-s); }
int strcmp(const char *a, const char *b){ while(*a&&*a==*b){a++;b++;} return (u8)*a-(u8)*b; }
int strncmp(const char *a, const char *b, size_t n){ while(n&&*a&&*a==*b){a++;b++;n--;} return n?(u8)*a-(u8)*b:0; }
char *strcpy(char *d, const char *s){ char *r=d; while((*d++=*s++)); return r; }
char *strncpy(char *d, const char *s, size_t n){ char *r=d; while(n&&(*d++=*s++))n--; while(n--)*d++=0; return r; }
char *strcat(char *d, const char *s){ char *r=d; while(*d)d++; while((*d++=*s++)); return r; }
char *strchr(const char *s, int c){ for(;*s;s++) if(*s==(char)c) return (char*)s; return c?0:(char*)s; }
char *strrchr(const char *s, int c){ const char *r=0; for(;*s;s++) if(*s==(char)c) r=s; return (char*)(c?r:s); }
char *strstr(const char *h, const char *n){
    if(!*n) return (char*)h;
    for(; *h; h++){ const char *a=h,*b=n; while(*a&&*b&&*a==*b){a++;b++;} if(!*b) return (char*)h; }
    return 0;
}
char *strtok_r(char *s, const char *delim, char **save){
    char *tok; if(!s) s=*save; if(!s) return 0;
    while(*s && strchr(delim,*s)) s++;
    if(!*s){ *save=s; return 0; }
    tok=s;
    while(*s && !strchr(delim,*s)) s++;
    if(*s){ *s=0; *save=s+1; } else *save=s;
    return tok;
}

int isdigit(int c){ return c>='0'&&c<='9'; }
int isspace(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\v'||c=='\f'; }
int isalpha(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
int isalnum(int c){ return isalpha(c)||isdigit(c); }
int isupper(int c){ return c>='A'&&c<='Z'; }
int islower(int c){ return c>='a'&&c<='z'; }
int isprint(int c){ return c>=32 && c<127; }
int toupper(int c){ return islower(c)? c-'a'+'A' : c; }
int tolower(int c){ return isupper(c)? c-'A'+'a' : c; }

/* ======================================================================
 * Heap allocator (K&R style: free-list over a static arena)
 * ==================================================================== */
#define HEAP_BYTES (32u*1024*1024)      /* 32 MiB - give the VM >= 64 MiB RAM */
typedef long Align;
typedef union header { struct { union header *ptr; size_t size; } s; Align x; } Header;
static Header base;
static Header *freep = 0;
static char arena[HEAP_BYTES] __attribute__((aligned(16)));
static size_t arena_used = 0;

static Header *morecore(size_t nu){
    if(nu < 4096) nu = 4096;
    size_t bytes = nu * sizeof(Header);
    if(arena_used + bytes > HEAP_BYTES) return 0;
    Header *up = (Header*)(arena + arena_used);
    arena_used += bytes;
    up->s.size = nu;
    free((void*)(up+1));
    return freep;
}
void *malloc(size_t nbytes){
    if(nbytes==0) return 0;
    size_t nunits = (nbytes + sizeof(Header) - 1)/sizeof(Header) + 1;
    Header *p, *prevp = freep;
    if(prevp==0){ base.s.ptr = freep = prevp = &base; base.s.size = 0; }
    for(p = prevp->s.ptr; ; prevp = p, p = p->s.ptr){
        if(p->s.size >= nunits){
            if(p->s.size == nunits) prevp->s.ptr = p->s.ptr;
            else { p->s.size -= nunits; p += p->s.size; p->s.size = nunits; }
            freep = prevp;
            return (void*)(p+1);
        }
        if(p == freep) if((p = morecore(nunits)) == 0) return 0;
    }
}
void free(void *ap){
    if(!ap) return;
    Header *bp = (Header*)ap - 1, *p;
    for(p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
        if(p >= p->s.ptr && (bp > p || bp < p->s.ptr)) break;
    if(bp + bp->s.size == p->s.ptr){ bp->s.size += p->s.ptr->s.size; bp->s.ptr = p->s.ptr->s.ptr; }
    else bp->s.ptr = p->s.ptr;
    if(p + p->s.size == bp){ p->s.size += bp->s.size; p->s.ptr = bp->s.ptr; }
    else p->s.ptr = bp;
    freep = p;
}
void *calloc(size_t n, size_t sz){ size_t t=n*sz; void*p=malloc(t); if(p) memset(p,0,t); return p; }
void *realloc(void *ptr, size_t size){
    if(!ptr) return malloc(size);
    if(size==0){ free(ptr); return 0; }
    Header *h = (Header*)ptr - 1;
    size_t oldbytes = (h->s.size - 1) * sizeof(Header);
    void *n = malloc(size);
    if(!n) return 0;
    memcpy(n, ptr, oldbytes<size?oldbytes:size);
    free(ptr);
    return n;
}
char *strdup(const char *s){ size_t n=strlen(s)+1; char*p=malloc(n); if(p) memcpy(p,s,n); return p; }

/* ======================================================================
 * Math (freestanding; SSE enabled in boot.S)
 * ==================================================================== */
double fabs(double x){ return __builtin_fabs(x); }
double sqrt(double x){ double r; __asm__("sqrtsd %1,%0":"=x"(r):"x"(x)); return r; }
double floor(double x){ long long i=(long long)x; double d=(double)i; return d>x? d-1.0 : d; }
double ceil(double x){ long long i=(long long)x; double d=(double)i; return d<x? d+1.0 : d; }
double round(double x){ return x<0 ? ceil(x-0.5) : floor(x+0.5); }
static double frac_exp(double x, int *e){          /* x = m*2^e, m in [1,2) */
    union { double d; uint64_t u; } v = { x };
    int ex = (int)((v.u>>52) & 0x7FF) - 1023;
    v.u = (v.u & ~(0x7FFULL<<52)) | (1023ULL<<52);
    *e = ex; return v.d;
}
static double k_log(double x){
    if(x<=0) return -1e308;
    int e; double m = frac_exp(x,&e);
    double s = (m-1)/(m+1), s2 = s*s, t = s, sum = 0;
    for(int k=1;k<40;k+=2){ sum += t/k; t *= s2; }
    return 2*sum + e*0.6931471805599453;           /* ln2 */
}
static double k_exp(double x){
    int k = (int)(x*1.4426950408889634 + (x<0?-0.5:0.5));   /* round(x/ln2) */
    double r = x - k*0.6931471805599453;
    double term = 1, sum = 1;
    for(int i=1;i<20;i++){ term *= r/i; sum += term; }
    union { double d; uint64_t u; } p;             /* 2^k */
    p.u = (uint64_t)(1023+k) << 52;
    return sum * p.d;
}
double pow(double b, double e){
    if(e==0) return 1;
    if(b==0) return 0;
    long long ie=(long long)e;
    if((double)ie==e){                              /* exact integer exponent */
        double r=1, base=b; long long n=ie<0?-ie:ie;
        for(long long i=0;i<n;i++) r*=base;
        return ie<0? 1.0/r : r;
    }
    return k_exp(e * k_log(b));
}

/* ======================================================================
 * strtod / strtol / qsort
 * ==================================================================== */
double strtod(const char *s, char **end){
    const char *p=s; while(isspace(*p))p++;
    int sign=1; if(*p=='+')p++; else if(*p=='-'){sign=-1;p++;}
    double val=0; int any=0;
    while(isdigit(*p)){ val=val*10+(*p-'0'); p++; any=1; }
    if(*p=='.'){ p++; double f=0.1; while(isdigit(*p)){ val+=(*p-'0')*f; f*=0.1; p++; any=1; } }
    if(any && (*p=='e'||*p=='E')){
        const char *e0=p; p++; int es=1;
        if(*p=='+')p++; else if(*p=='-'){es=-1;p++;}
        int ed=0,edany=0; while(isdigit(*p)){ ed=ed*10+(*p-'0'); p++; edany=1; }
        if(edany){ double pw=1; for(int i=0;i<ed;i++)pw*=10; val = es<0? val/pw : val*pw; }
        else p=e0;
    }
    if(!any){ if(end)*end=(char*)s; return 0; }
    if(end)*end=(char*)p;
    return sign*val;
}
long strtol(const char *s, char **end, int base){
    const char *p=s; while(isspace(*p))p++;
    int sign=1; if(*p=='+')p++; else if(*p=='-'){sign=-1;p++;}
    if((base==16||base==0) && p[0]=='0' && (p[1]=='x'||p[1]=='X')){ p+=2; base=16; }
    if(base==0) base=10;
    long v=0; int any=0;
    for(;;){
        int c=*p, d;
        if(c>='0'&&c<='9') d=c-'0';
        else if(c>='a'&&c<='z') d=c-'a'+10;
        else if(c>='A'&&c<='Z') d=c-'A'+10;
        else break;
        if(d>=base) break;
        v=v*base+d; p++; any=1;
    }
    if(end)*end=(char*)(any?p:s);
    return sign*v;
}
int atoi(const char *s){ return (int)strtol(s,0,10); }

static void swapb(char *a, char *b, size_t sz){ while(sz--){ char t=*a; *a++=*b; *b++=t; } }
void qsort(void *base_, size_t n, size_t sz, int(*cmp)(const void*,const void*)){
    char *a=base_;
    for(size_t gap=n/2; gap>0; gap/=2)
        for(size_t i=gap;i<n;i++)
            for(long j=(long)i; j>=(long)gap && cmp(a+(j-gap)*sz, a+j*sz)>0; j-=gap)
                swapb(a+(j-gap)*sz, a+j*sz, sz);
}

/* ======================================================================
 * printf family - streams to a sink (serial or a buffer), so no truncation
 * ==================================================================== */
typedef struct { char *buf; size_t cap, len; int serial; } Sink;
static void sink_putc(Sink *s, char c){
    if(s->serial) serial_putc(c);
    else if(s->len+1 < s->cap) s->buf[s->len]=c;
    s->len++;
}
static void sink_puts(Sink *s, const char *p){ while(*p) sink_putc(s,*p++); }

static void fmt_uint(char *out, unsigned long long v, int base, int upper){
    char tmp[24]; int i=0; const char *dig = upper?"0123456789ABCDEF":"0123456789abcdef";
    if(v==0) tmp[i++]='0';
    while(v){ tmp[i++]=dig[v%base]; v/=base; }
    int j=0; while(i) out[j++]=tmp[--i]; out[j]=0;
}
/* %g into out: <= `sig` significant digits, trailing zeros stripped */
static void fmt_g(char *out, double x, int sig){
    int oi=0;
    if(x!=x){ strcpy(out,"nan"); return; }
    if(x<0){ out[oi++]='-'; x=-x; }
    if(x>1e308){ strcpy(out+oi,"inf"); return; }
    if(x==0){ out[oi++]='0'; out[oi]=0; return; }
    int e10 = (int)floor(k_log(x)/2.302585092994046);   /* floor(log10(x)) */
    /* d = round(x / 10^(e10 - (sig-1))) : the sig-digit integer */
    double scale = pow(10.0, e10-(sig-1));
    double dd = x/scale + 0.5;
    unsigned long long d = (unsigned long long)dd;
    char digs[32]; fmt_uint(digs, d, 10, 0);
    int nd = (int)strlen(digs);
    if(nd > sig){ e10 += (nd-sig); digs[sig]=0; nd=sig; }   /* rounding overflow (e.g. 9.99->10) */
    /* strip trailing zeros */
    while(nd>1 && digs[nd-1]=='0'){ digs[--nd]=0; }
    if(e10 < -4 || e10 >= sig){
        /* scientific: d.dddde+XX */
        out[oi++]=digs[0];
        if(nd>1){ out[oi++]='.'; for(int k=1;k<nd;k++) out[oi++]=digs[k]; }
        out[oi++]='e'; out[oi++]= e10<0?'-':'+';
        int ae = e10<0?-e10:e10; char eb[8]; fmt_uint(eb,ae,10,0);
        if(strlen(eb)<2) out[oi++]='0';
        for(char *q=eb;*q;q++) out[oi++]=*q;
        out[oi]=0;
    } else if(e10 >= 0){
        for(int k=0;k<=e10;k++) out[oi++] = k<nd?digs[k]:'0';
        if(nd > e10+1){ out[oi++]='.'; for(int k=e10+1;k<nd;k++) out[oi++]=digs[k]; }
        out[oi]=0;
    } else {
        out[oi++]='0'; out[oi++]='.';
        for(int k=0;k<-e10-1;k++) out[oi++]='0';
        for(int k=0;k<nd;k++) out[oi++]=digs[k];
        out[oi]=0;
    }
}

static void vformat(Sink *s, const char *fmt, va_list ap){
    char num[64];
    for(; *fmt; fmt++){
        if(*fmt!='%'){ sink_putc(s,*fmt); continue; }
        fmt++;
        int left=0, zero=0, plus=0, space=0;
        for(;;){
            if(*fmt=='-') left=1; else if(*fmt=='0') zero=1;
            else if(*fmt=='+') plus=1; else if(*fmt==' ') space=1;
            else break;
            fmt++;
        }
        int width=0; while(isdigit(*fmt)) width=width*10+(*fmt++-'0');
        int prec=-1; if(*fmt=='.'){ fmt++; prec=0; while(isdigit(*fmt)) prec=prec*10+(*fmt++-'0'); }
        int lng=0; while(*fmt=='l'){ lng++; fmt++; } if(*fmt=='z'){ lng=2; fmt++; }

        char c=*fmt; const char *str=0; char sign=0;
        switch(c){
            case 'd': case 'i': {
                long long v = lng>=2? va_arg(ap,long long) : lng==1? va_arg(ap,long) : va_arg(ap,int);
                unsigned long long u; if(v<0){ sign='-'; u=(unsigned long long)(-v);} else { u=(unsigned long long)v; if(plus)sign='+'; else if(space)sign=' '; }
                fmt_uint(num,u,10,0); str=num; break;
            }
            case 'u': { unsigned long long v = lng>=2? va_arg(ap,unsigned long long) : lng==1? va_arg(ap,unsigned long) : va_arg(ap,unsigned); fmt_uint(num,v,10,0); str=num; break; }
            case 'x': { unsigned long long v = lng>=2? va_arg(ap,unsigned long long) : lng==1? va_arg(ap,unsigned long) : va_arg(ap,unsigned); fmt_uint(num,v,16,0); str=num; break; }
            case 'X': { unsigned long long v = lng>=2? va_arg(ap,unsigned long long) : lng==1? va_arg(ap,unsigned long) : va_arg(ap,unsigned); fmt_uint(num,v,16,1); str=num; break; }
            case 'p': { unsigned long long v=(unsigned long long)(uintptr_t)va_arg(ap,void*); num[0]='0'; num[1]='x'; fmt_uint(num+2,v,16,0); str=num; break; }
            case 'c': { num[0]=(char)va_arg(ap,int); num[1]=0; str=num; break; }
            case 's': { str=va_arg(ap,const char*); if(!str) str="(null)"; break; }
            case 'g': case 'G': case 'f': case 'F': case 'e': {
                double v=va_arg(ap,double); fmt_g(num,v, prec>=0?(prec>0?prec:1):6); str=num; break;
            }
            case '%': num[0]='%'; num[1]=0; str=num; break;
            default: num[0]='%'; num[1]=c; num[2]=0; str=num; break;
        }
        int slen=0; { const char *q=str; while(*q++) slen++; }
        int total = slen + (sign?1:0);
        int pad = width>total? width-total : 0;
        if(!left) for(int i=0;i<pad;i++) sink_putc(s, zero&&!sign?'0':' ');
        /* note: with zero-pad the sign should precede the zeros; simple form is fine here */
        if(sign) sink_putc(s,sign);
        if(!left && zero && sign){ for(int i=0;i<pad;i++) sink_putc(s,'0'); }
        sink_puts(s,str);
        if(left) for(int i=0;i<pad;i++) sink_putc(s,' ');
    }
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap){
    Sink s = { buf, n, 0, 0 };
    vformat(&s, fmt, ap);
    if(n) buf[s.len < n ? s.len : n-1] = 0;
    return (int)s.len;
}
int snprintf(char *buf, size_t n, const char *fmt, ...){ va_list ap; va_start(ap,fmt); int r=vsnprintf(buf,n,fmt,ap); va_end(ap); return r; }
int printf(const char *fmt, ...){ Sink s={0,0,0,1}; va_list ap; va_start(ap,fmt); vformat(&s,fmt,ap); va_end(ap); return (int)s.len; }

/* ======================================================================
 * FILE layer - std streams go to serial; real files don't exist yet
 * ==================================================================== */
/* the baked-in initramfs (mkramfs.py / ramfs_gen.c) - seeds the writable VFS */
typedef struct { const char *path; const unsigned char *data; unsigned size; } RamFile;
extern const RamFile g_ramfs[];
extern const int g_ramfs_count;

/* ---- a writable in-memory filesystem, seeded from the initramfs ---- */
typedef struct VNode {
    char name[64];
    int is_dir;
    unsigned char *data; unsigned size, cap;      /* file contents */
    struct VNode *child, *next, *parent;          /* directory tree */
} VNode;
static VNode *g_root, *g_cwd, *g_home;
static int  ata_init(void);      /* disk driver + persistence, defined below */
static void fs_load(void);
void        fs_sync(void);

static VNode *vn_new(const char *name, int dir){
    VNode *n=malloc(sizeof(VNode)); if(!n) return 0;
    memset(n,0,sizeof(VNode)); strncpy(n->name,name,sizeof(n->name)-1); n->is_dir=dir; return n;
}
static VNode *vn_child(VNode *d, const char *name){
    if(!d||!d->is_dir) return 0;
    for(VNode *c=d->child;c;c=c->next) if(strcmp(c->name,name)==0) return c;
    return 0;
}
static void vn_add(VNode *d, VNode *c){ c->parent=d; c->next=d->child; d->child=c; }
static void vn_grow(VNode *n, unsigned need){
    if(n->cap>=need) return;
    unsigned nc=n->cap?n->cap:64; while(nc<need) nc*=2;
    n->data=realloc(n->data,nc); n->cap=nc;
}
static VNode *vn_resolve(const char *path){
    VNode *cur=(path[0]=='/')?g_root:g_cwd;
    char buf[512]; strncpy(buf,path,sizeof(buf)-1); buf[sizeof(buf)-1]=0;
    char *save=0;
    for(char *t=strtok_r(buf,"/",&save); t; t=strtok_r(0,"/",&save)){
        if(strcmp(t,".")==0) continue;
        if(strcmp(t,"..")==0){ if(cur->parent) cur=cur->parent; continue; }
        cur=vn_child(cur,t); if(!cur) return 0;
    }
    return cur;
}
static VNode *vn_parent(const char *path, char *leaf, int leafsz, int mkparents){
    VNode *cur=(path[0]=='/')?g_root:g_cwd;
    char buf[512]; strncpy(buf,path,sizeof(buf)-1); buf[sizeof(buf)-1]=0;
    char *save=0, *t=strtok_r(buf,"/",&save);
    if(!t) return 0;
    for(;;){
        char *nx=strtok_r(0,"/",&save);
        if(!nx){ strncpy(leaf,t,leafsz-1); leaf[leafsz-1]=0; return cur; }
        if(strcmp(t,".")==0){}
        else if(strcmp(t,"..")==0){ if(cur->parent) cur=cur->parent; }
        else { VNode *c=vn_child(cur,t); if(!c){ if(!mkparents) return 0; c=vn_new(t,1); if(!c) return 0; vn_add(cur,c); } if(!c->is_dir) return 0; cur=c; }
        t=nx;
    }
}
static VNode *vn_open_write(const char *path, int trunc){
    char leaf[64]; VNode *p=vn_parent(path,leaf,sizeof leaf,1);
    if(!p) return 0;
    VNode *f=vn_child(p,leaf);
    if(f){ if(f->is_dir) return 0; if(trunc){ f->size=0; if(f->data) f->data[0]=0; } return f; }
    f=vn_new(leaf,0); if(!f) return 0; vn_add(p,f); return f;
}
void vfs_init(void){
    g_root=vn_new("",1); g_cwd=g_root;
    g_home=vn_new("home",1); vn_add(g_root,g_home);
    vn_add(g_root, vn_new("tmp",1));
    for(int i=0;i<g_ramfs_count;i++){
        VNode *f=vn_open_write(g_ramfs[i].path,1);
        if(f && g_ramfs[i].size){ vn_grow(f,g_ramfs[i].size+1); memcpy(f->data,g_ramfs[i].data,g_ramfs[i].size); f->size=g_ramfs[i].size; f->data[f->size]=0; }
    }
    ata_init();      /* detect a disk */
    fs_load();       /* restore /home from disk (LarzFS), if present */
}

/* ======================================================================
 * ATA PIO disk (primary master) + LarzFS: persist /home across reboots.
 * Everything no-ops when no disk is attached, so the OS still runs RAM-only.
 * ==================================================================== */
#define ATA_IO 0x1F0
static int g_disk=0;
static void insw(u16 port, void *buf, int cnt){ __asm__ volatile("cld; rep insw":"+D"(buf),"+c"(cnt):"d"(port):"memory"); }
static void outsw(u16 port, const void *buf, int cnt){ __asm__ volatile("cld; rep outsw":"+S"(buf),"+c"(cnt):"d"(port)); }
static int ata_wait(unsigned char mask, unsigned char val){
    for(unsigned i=0;i<20000000u;i++) if((inb(ATA_IO+7)&mask)==val) return 1;
    return 0;
}
static int ata_init(void){
    outb(ATA_IO+6,0xA0);                       /* select master */
    for(int i=0;i<4;i++) inb(ATA_IO+7);        /* 400ns settle */
    outb(ATA_IO+2,0); outb(ATA_IO+3,0); outb(ATA_IO+4,0); outb(ATA_IO+5,0);
    outb(ATA_IO+7,0xEC);                        /* IDENTIFY */
    if(inb(ATA_IO+7)==0) return g_disk=0;       /* no drive present */
    if(!ata_wait(0x80,0)) return g_disk=0;
    if(inb(ATA_IO+4)||inb(ATA_IO+5)) return g_disk=0;   /* not ATA */
    if(!ata_wait(0x08,0x08)) return g_disk=0;
    unsigned short id[256]; insw(ATA_IO,id,256); (void)id;
    return g_disk=1;
}
static void ata_lba(unsigned lba){
    ata_wait(0x80,0);
    outb(ATA_IO+6, 0xE0 | ((lba>>24)&0x0F));
    outb(ATA_IO+2, 1);
    outb(ATA_IO+3, lba&0xFF); outb(ATA_IO+4, (lba>>8)&0xFF); outb(ATA_IO+5, (lba>>16)&0xFF);
}
static void ata_read(unsigned lba, void *buf){
    if(!g_disk) return;
    ata_lba(lba); outb(ATA_IO+7,0x20);
    if(!ata_wait(0x80,0)||!ata_wait(0x08,0x08)) return;
    insw(ATA_IO, buf, 256);
}
static void ata_write(unsigned lba, const void *buf){
    if(!g_disk) return;
    ata_lba(lba); outb(ATA_IO+7,0x30);
    if(!ata_wait(0x80,0)||!ata_wait(0x08,0x08)) return;
    outsw(ATA_IO, buf, 256);
    outb(ATA_IO+7,0xE7); ata_wait(0x80,0);      /* flush cache */
}

/* LarzFS: a flat serialization of the /home subtree (path + bytes records). */
typedef struct { unsigned char *p; unsigned len, cap; } Buf;
static void buf_put(Buf *b, const void *d, unsigned n){
    if(b->len+n > b->cap){ unsigned nc=b->cap?b->cap:512; while(nc<b->len+n) nc*=2; b->p=realloc(b->p,nc); b->cap=nc; }
    memcpy(b->p+b->len,d,n); b->len+=n;
}
static void buf_u16(Buf*b,unsigned v){ unsigned char t[2]={(unsigned char)v,(unsigned char)(v>>8)}; buf_put(b,t,2); }
static void buf_u32(Buf*b,unsigned v){ unsigned char t[4]={(unsigned char)v,(unsigned char)(v>>8),(unsigned char)(v>>16),(unsigned char)(v>>24)}; buf_put(b,t,4); }
static void ser_dir(VNode *d, const char *prefix, Buf *b, int *count){
    for(VNode *c=d->child; c; c=c->next){
        char path[512];
        if(prefix[0]) snprintf(path,sizeof path,"%s/%s",prefix,c->name);
        else          snprintf(path,sizeof path,"%s",c->name);
        if(c->is_dir) ser_dir(c, path, b, count);
        else { unsigned pl=(unsigned)strlen(path); buf_u16(b,pl); buf_put(b,path,pl); buf_u32(b,c->size); buf_put(b,c->data?c->data:(const unsigned char*)"",c->size); (*count)++; }
    }
}
void fs_sync(void){
    if(!g_disk || !g_home) return;
    Buf b={0,0,0}; int count=0;
    ser_dir(g_home, "", &b, &count);
    unsigned char sb[512]; memset(sb,0,512);
    memcpy(sb,"LZF1",4); unsigned len=b.len; memcpy(sb+4,&len,4); memcpy(sb+8,&count,4);
    ata_write(0,sb);
    unsigned secs=(b.len+511)/512;
    for(unsigned i=0;i<secs;i++){ unsigned char s[512]; memset(s,0,512); unsigned n=b.len-i*512; if(n>512)n=512; memcpy(s,b.p+i*512,n); ata_write(1+i,s); }
    if(b.p) free(b.p);
}
static void fs_load(void){
    if(!g_disk || !g_home) return;
    unsigned char sb[512]; ata_read(0,sb);
    if(memcmp(sb,"LZF1",4)!=0) return;
    unsigned len,count; memcpy(&len,sb+4,4); memcpy(&count,sb+8,4);
    if(len==0 || len>8u*1024*1024) return;
    unsigned secs=(len+511)/512;
    unsigned char *blob=malloc(secs*512); if(!blob) return;
    for(unsigned i=0;i<secs;i++) ata_read(1+i, blob+i*512);
    unsigned off=0;
    for(unsigned k=0;k<count && off+2<=len;k++){
        unsigned pl=blob[off]|(blob[off+1]<<8); off+=2;
        if(pl>=512 || off+pl>len) break;
        char path[512]; memcpy(path,blob+off,pl); path[pl]=0; off+=pl;
        if(off+4>len) break;
        unsigned dl; memcpy(&dl,blob+off,4); off+=4;
        if(off+dl>len) break;
        char full[600]; snprintf(full,sizeof full,"/home/%s",path);
        VNode *f=vn_open_write(full,1);
        if(f && dl){ vn_grow(f,dl+1); memcpy(f->data,blob+off,dl); f->size=dl; f->data[dl]=0; }
        off+=dl;
    }
    free(blob);
}

char *net_vfile(const char *path);                              /* from net.c */
void  net_vfile_write(const char *path, const char *data, int len);
struct _LZ_FILE { int kind; VNode *vn; unsigned pos; int writing, ephemeral; char netpath[48]; };  /* 0..2=std, 3=vfs */
static FILE _stdin={0,0,0,0}, _stdout={1,0,0,0}, _stderr={2,0,0,0};
FILE *stdin=&_stdin, *stdout=&_stdout, *stderr=&_stderr;
static int is_std(FILE *f){ return f==&_stdin||f==&_stdout||f==&_stderr; }

int putchar(int c){ serial_putc((char)c); return c; }
int puts(const char *s){ con_puts(s); serial_putc('\n'); return 0; }
int fputs(const char *s, FILE *f){
    if(is_std(f)){ con_puts(s); return 0; }
    if(f->writing && f->vn){ unsigned l=(unsigned)strlen(s); vn_grow(f->vn,f->vn->size+l+1); memcpy(f->vn->data+f->vn->size,s,l); f->vn->size+=l; f->vn->data[f->vn->size]=0; }
    return 0;
}
int fputc(int c, FILE *f){
    if(is_std(f)) serial_putc((char)c);
    else if(f->writing && f->vn){ vn_grow(f->vn,f->vn->size+2); f->vn->data[f->vn->size++]=(unsigned char)c; f->vn->data[f->vn->size]=0; }
    return c;
}
int fflush(FILE *f){ (void)f; return 0; }
int fprintf(FILE *f, const char *fmt, ...){ Sink s={0,0,0,is_std(f)}; va_list ap; va_start(ap,fmt); vformat(&s,fmt,ap); va_end(ap); return (int)s.len; }
size_t fwrite(const void *p, size_t sz, size_t nm, FILE *f){
    size_t t=sz*nm;
    if(is_std(f)){ const char *b=p; while(t--) serial_putc(*b++); return nm; }
    if(f->writing && f->vn){ vn_grow(f->vn,f->vn->size+(unsigned)t+1); memcpy(f->vn->data+f->vn->size,p,t); f->vn->size+=(unsigned)t; f->vn->data[f->vn->size]=0; return nm; }
    return 0;
}
size_t fread(void *p, size_t sz, size_t nm, FILE *f){
    if(is_std(f)||!f->vn||f->writing) return 0;
    size_t want=sz*nm, avail=f->vn->size - f->pos;
    if(want>avail) want=avail;
    memcpy(p, f->vn->data + f->pos, want);
    f->pos += (unsigned)want;
    return sz? want/sz : 0;
}
char *fgets(char *buf, int size, FILE *f){
    if(f==&_stdin){
        int n=0;
        for(;;){
            char c=console_getc();                       /* serial OR local keyboard */
            if(c=='\r'||c=='\n'){ serial_putc('\n'); buf[n++]='\n'; buf[n]=0; return buf; }
            if((c==0x7F||c==0x08)){ if(n>0){ n--; con_puts("\b \b"); } continue; }
            if(c>=32 && c<127 && n<size-2){ buf[n++]=c; serial_putc(c); }
        }
    }
    if(f->vn && !f->writing){
        if(f->pos>=f->vn->size) return 0;
        int n=0;
        while(f->pos<f->vn->size && n<size-1){ char c=(char)f->vn->data[f->pos++]; buf[n++]=c; if(c=='\n') break; }
        buf[n]=0; return buf;
    }
    return 0;
}
static unsigned vn_total(VNode *d){                   /* bytes used under a dir */
    unsigned t=0; for(VNode *c=d->child;c;c=c->next){ if(c->is_dir) t+=vn_total(c); else t+=c->size; } return t;
}
static FILE *ephemeral_file(char *content){           /* a read-only FILE over a malloc'd string */
    if(!content) return 0;
    VNode *e=vn_new("x",0); if(!e){ free(content); return 0; }
    e->data=(unsigned char*)content; e->size=(unsigned)strlen(content); e->cap=e->size+1;
    FILE *f=malloc(sizeof(FILE)); if(!f){ free(content); free(e); return 0; }
    f->kind=3; f->vn=e; f->pos=0; f->writing=0; f->ephemeral=1; f->netpath[0]=0;
    return f;
}
static char *proc_content(const char *path){
    char *c=malloc(256); if(!c) return 0;
    if(strcmp(path,"/proc/meminfo")==0)
        snprintf(c,256,"MemTotal: %u KB\nMemUsed:  %u KB\nMemFree:  %u KB\n",
            (unsigned)(HEAP_BYTES/1024),(unsigned)(arena_used/1024),(unsigned)((HEAP_BYTES-arena_used)/1024));
    else if(strcmp(path,"/proc/diskinfo")==0)
        snprintf(c,256,"filesystem: LarzFS\nmount:      /home (persistent)\nused:       %u bytes\n", g_home?vn_total(g_home):0);
    else if(strcmp(path,"/proc/uptime")==0)
        snprintf(c,256,"ticks: %llu\nseconds: %llu  (timer IRQ @ 100 Hz)\n", g_ticks, g_ticks/100);
    else if(strcmp(path,"/proc/tasks")==0)
        snprintf(c,256,"scheduler: preemptive, round-robin\ntask0: shell/interpreter\ntask1: %llu ticks (bg)\ntask2: %llu ticks (bg)\n", g_ca, g_cb);
    else if(strcmp(path,"/proc/gas")==0)
        snprintf(c,256,"gas_used: %u ticks\ngas_budget: %u\nmetering: %s\n", g_gas_used, g_gas_budget, g_gas_on?"on":"off");
    else snprintf(c,256,"no such /proc file\n");
    return c;
}
/* ---- current user + home-directory permissions ---- */
static char g_user[64]="root";                        /* set via /dev/user (login/su) */
static void canon(const char *path, char *out, int outsz){  /* -> absolute, .. and . resolved */
    char work[512];
    if(path && path[0]=='/'){ strncpy(work,path,sizeof work-1); work[sizeof work-1]=0; }
    else { char cw[256]; getcwd(cw,sizeof cw); snprintf(work,sizeof work,"%s/%s",cw,path?path:""); }
    char *segs[64]; int ns=0, save_i=0; (void)save_i; char *save=0;
    for(char *t=strtok_r(work,"/",&save); t; t=strtok_r(0,"/",&save)){
        if(strcmp(t,".")==0) continue;
        if(strcmp(t,"..")==0){ if(ns>0) ns--; continue; }
        if(ns<64) segs[ns++]=t;
    }
    int p=0; if(p<outsz-1) out[p++]='/';
    for(int i=0;i<ns;i++){ int l=(int)strlen(segs[i]); if(i>0 && p<outsz-1) out[p++]='/'; for(int k=0;k<l && p<outsz-1;k++) out[p++]=segs[i][k]; }
    out[p]=0;
}
static int perm_ok(const char *path){
    char abs[512]; canon(path, abs, sizeof abs);
    if(strncmp(abs,"/home/",6)!=0) return 1;              /* only guard /home */
    const char *seg=abs+6;
    if(seg[0]=='.') return 1;                             /* /home/.larzos = shared state */
    char name[64]; int i=0; while(seg[i] && seg[i]!='/' && i<63){ name[i]=seg[i]; i++; } name[i]=0;
    if(name[0]==0) return 1;
    if(strcmp(g_user,"root")==0) return 1;               /* root sees all */
    return strcmp(name,g_user)==0;                        /* only your own home */
}

FILE *fopen(const char *path, const char *mode){
    int reading = !mode || mode[0]=='r';
    if(!reading && (strcmp(path,"/dev/user")==0 || strcmp(path,"/dev/gas")==0)){  /* control writes */
        VNode *e=vn_new("d",0); if(!e) return 0;
        FILE *f=malloc(sizeof(FILE)); if(!f){ free(e); return 0; }
        f->kind=3; f->vn=e; f->pos=0; f->writing=1; f->ephemeral=1;
        strncpy(f->netpath,path,sizeof(f->netpath)-1); f->netpath[sizeof(f->netpath)-1]=0;
        return f;
    }
    if(reading && strncmp(path,"/proc/",6)==0) return ephemeral_file(proc_content(path));
    if(reading && strcmp(path,"/dev/password")==0){   /* masked console line read */
        VNode *e=vn_new("pw",0); if(!e) return 0;
        char *buf=malloc(256); if(!buf){ free(e); return 0; }
        int n=0;
        for(;;){
            char c=console_getc();
            if(c=='\n'||c=='\r'){ serial_putc('\n'); break; }
            if(c==0x7F||c==0x08){ if(n>0){ n--; con_puts("\b \b"); } continue; }
            if(c>=32 && c<127 && n<255){ buf[n++]=c; serial_putc('*'); }
        }
        buf[n]=0;
        e->data=(unsigned char*)buf; e->size=(unsigned)n; e->cap=(unsigned)n+1;
        FILE *f=malloc(sizeof(FILE)); if(!f){ free(buf); free(e); return 0; }
        f->kind=3; f->vn=e; f->pos=0; f->writing=0; f->ephemeral=1; f->netpath[0]=0;
        return f;
    }
    if(strncmp(path,"/net/",5)==0){                   /* virtual networking files */
        VNode *e=vn_new("net",0); if(!e) return 0;
        FILE *f=malloc(sizeof(FILE)); if(!f){ free(e); return 0; }
        f->kind=3; f->vn=e; f->pos=0; f->ephemeral=1; f->netpath[0]=0;
        if(reading){
            char *c=net_vfile(path); if(!c){ free(e); free(f); return 0; }
            e->data=(unsigned char*)c; e->size=(unsigned)strlen(c); e->cap=e->size+1;
            f->writing=0;
        } else {
            f->writing=1;                             /* buffer, flush to net on close */
            strncpy(f->netpath, path, sizeof(f->netpath)-1); f->netpath[sizeof(f->netpath)-1]=0;
        }
        return f;
    }
    if(!perm_ok(path)) return 0;                          /* home-directory privacy */
    VNode *vn; int writing=0; unsigned pos=0;
    if(mode && mode[0]=='w'){ vn=vn_open_write(path,1); writing=1; }
    else if(mode && mode[0]=='a'){ vn=vn_open_write(path,0); writing=1; }
    else { vn=vn_resolve(path); if(vn && vn->is_dir) return 0; }
    if(!vn) return 0;
    FILE *f=malloc(sizeof(FILE)); if(!f) return 0;
    f->kind=3; f->vn=vn; f->pos=pos; f->writing=writing; f->ephemeral=0; f->netpath[0]=0; return f;
}
int fclose(FILE *f){
    if(!f || is_std(f)) return 0;
    int w=f->writing, net=f->netpath[0]!=0, eph=f->ephemeral;
    if(net){
        if(strcmp(f->netpath,"/dev/user")==0){            /* set current user (trim newline) */
            int k=0; unsigned char *d=f->vn->data;
            while(k<63 && d && d[k] && d[k]!='\n' && d[k]!='\r'){ g_user[k]=(char)d[k]; k++; }
            g_user[k]=0;
        } else if(strcmp(f->netpath,"/dev/gas")==0){      /* start/stop CPU-gas metering */
            unsigned n=0; unsigned char *d=f->vn->data;
            for(int k=0; d && d[k]>='0' && d[k]<='9'; k++) n=n*10+(d[k]-'0');
            g_gas_used=0; g_gas_budget=n; g_gas_on=(n>0); larz_gas_kill=0;
        } else net_vfile_write(f->netpath, (char*)(f->vn->data?f->vn->data:(unsigned char*)""), (int)f->vn->size);
    }
    if(eph){ if(f->vn){ if(f->vn->data) free(f->vn->data); free(f->vn); } }
    free(f);
    if(w && !net && !eph) fs_sync();      /* persist only real /home writes */
    return 0;
}
FILE *popen(const char *c, const char *m){ (void)c;(void)m; return 0; }
int pclose(FILE *f){ (void)f; return -1; }

/* ======================================================================
 * OS stubs - no processes/filesystem/clock on bare metal yet
 * ==================================================================== */
void exit(int code){ con_puts("\n[larzscript exited]\n"); qemu_exit((unsigned char)code); for(;;) __asm__ volatile("hlt"); }
void abort(void){ exit(134); }
char *getenv(const char *n){
    if(strcmp(n,"HOME")==0)            return "/home";
    if(strcmp(n,"USER")==0)            return "larz";
    if(strcmp(n,"SHELL")==0)           return "/larzsh.lz";
    if(strcmp(n,"LARZSCRIPT_PATH")==0) return "/:/home/.larzpkg";  /* baked + installed packages */
    return 0;
}
int system(const char *c){ (void)c; return -1; }   /* no processes on bare metal */
char *realpath(const char *p, char *r){ (void)r; if(!vn_resolve(p)) return 0; char *o=malloc(strlen(p)+1); if(o) strcpy(o,p); return o; }
char *getcwd(char *b, size_t n){
    if(!b||!n) return 0;
    if(g_cwd==g_root){ strncpy(b,"/",n); b[n-1]=0; return b; }
    char tmp[512]; int ti=(int)sizeof(tmp); tmp[--ti]=0;
    for(VNode *c=g_cwd; c && c!=g_root; c=c->parent){
        int l=(int)strlen(c->name); ti-=l; if(ti<1) break;
        memcpy(tmp+ti,c->name,l); tmp[--ti]='/';
    }
    strncpy(b,tmp+ti,n); b[n-1]=0; return b;
}
int chdir(const char *p){ if(!perm_ok(p)) return -1; VNode *n=vn_resolve(p); if(!n||!n->is_dir) return -1; g_cwd=n; return 0; }
static int vn_unlink(const char *p){
    if(!perm_ok(p)) return -1;
    VNode *n=vn_resolve(p); if(!n||n==g_root||!n->parent) return -1;
    VNode *par=n->parent;
    if(par->child==n) par->child=n->next;
    else for(VNode *c=par->child;c;c=c->next) if(c->next==n){ c->next=n->next; break; }
    if(n->data) free(n->data);
    free(n);
    fs_sync();
    return 0;
}
int rmdir(const char *p){ return vn_unlink(p); }
int unlink(const char *p){ return vn_unlink(p); }
int remove(const char *p){ return vn_unlink(p); }
int access(const char *p, int m){ (void)m; return vn_resolve(p)?0:-1; }

/* ---- RTC wall clock (CMOS) + TSC monotonic clock (PIT-calibrated) ---- */
static int cmos_read(int reg){ outb(0x70,(u8)reg); return inb(0x71); }
static int cmos_updating(void){ outb(0x70,0x0A); return inb(0x71)&0x80; }
static int bcd2bin(int v){ return (v&0x0F)+((v>>4)*10); }
static int is_leap(int y){ return (y%4==0 && y%100!=0)||y%400==0; }
static long rtc_unix(void){
    while(cmos_updating()){}
    int s=cmos_read(0), mi=cmos_read(2), h=cmos_read(4);
    int d=cmos_read(7), mo=cmos_read(8), y=cmos_read(9);
    int b=cmos_read(0x0B);
    if(!(b&0x04)){ s=bcd2bin(s); mi=bcd2bin(mi); int pm=h&0x80; h=bcd2bin(h&0x7F); if(pm)h+=12; d=bcd2bin(d); mo=bcd2bin(mo); y=bcd2bin(y); }
    int year=2000+y;
    static const int md[12]={31,28,31,30,31,30,31,31,30,31,30,31};
    long days=0;
    for(int yy=1970; yy<year; yy++) days += is_leap(yy)?366:365;
    for(int mm=1; mm<mo; mm++){ days += md[mm-1]; if(mm==2 && is_leap(year)) days++; }
    days += d-1;
    return days*86400L + h*3600L + mi*60L + s;
}
static unsigned long long rdtsc(void){ unsigned lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); return ((unsigned long long)hi<<32)|lo; }
static unsigned long long g_tsc_hz=0, g_tsc_start=0;
static void clock_init(void){
    outb(0x61, (inb(0x61)&0xFD)|1);          /* PIT ch2 gate on, speaker off */
    outb(0x43, 0xB0);                         /* ch2, lobyte/hibyte, mode 0, binary */
    outb(0x42, 11932&0xFF); inb(0x60); outb(0x42,(11932>>8)&0xFF);  /* ~1/100 s count */
    unsigned char p=inb(0x61)&0xFE; outb(0x61,p); outb(0x61,p|1);   /* restart the count */
    unsigned long long t0=rdtsc();
    for(unsigned i=0;i<200000000u;i++){ if(inb(0x61)&0x20) break; }  /* wait for OUT high */
    unsigned long long t1=rdtsc();
    g_tsc_hz=(t1-t0)*100; g_tsc_start=rdtsc();
    if(g_tsc_hz==0) g_tsc_hz=1000000000ULL;
}
int usleep(unsigned us){
    if(!g_tsc_hz) return 0;
    unsigned long long target=rdtsc() + (unsigned long long)us*g_tsc_hz/1000000ULL;
    while(rdtsc()<target){}
    return 0;
}
unsigned sleep(unsigned s){ while(s--) usleep(1000000); return 0; }
int stat(const char *p, struct stat *b){
    if(!perm_ok(p)) return -1;
    if(strncmp(p,"/proc/",6)==0){ if(b){ b->st_mode=S_IFREG; b->st_size=0; } return 0; }  /* virtual */
    VNode *n=vn_resolve(p); if(!n) return -1;
    if(b){ b->st_mode = n->is_dir?S_IFDIR:S_IFREG; b->st_size=n->size; }
    return 0;
}
int mkdir(const char *p, mode_t m){
    (void)m; if(!perm_ok(p)) return -1;
    char leaf[64]; VNode *par=vn_parent(p,leaf,sizeof leaf,1);
    if(!par || vn_child(par,leaf)) return -1;
    VNode *d=vn_new(leaf,1); if(!d) return -1; vn_add(par,d); fs_sync(); return 0;
}
int rename(const char *o, const char *nw){
    VNode *n=vn_resolve(o); if(!n) return -1;
    char leaf[64]; VNode *np=vn_parent(nw,leaf,sizeof leaf,1); if(!np) return -1;
    VNode *par=n->parent;
    if(par){ if(par->child==n) par->child=n->next; else for(VNode*c=par->child;c;c=c->next) if(c->next==n){ c->next=n->next; break; } }
    strncpy(n->name,leaf,sizeof(n->name)-1); n->name[sizeof(n->name)-1]=0; vn_add(np,n); fs_sync(); return 0;
}
struct _LZ_DIR { VNode *cur; struct dirent ent; };
DIR *opendir(const char *n){ if(n && n[0] && !perm_ok(n)) return 0; VNode *d=(n&&n[0])?vn_resolve(n):g_cwd; if(!d||!d->is_dir) return 0; DIR *dp=malloc(sizeof(DIR)); if(dp) dp->cur=d->child; return dp; }
struct dirent *readdir(DIR *d){
    if(!d||!d->cur) return 0;
    strncpy(d->ent.d_name,d->cur->name,sizeof(d->ent.d_name)-1);
    d->ent.d_name[sizeof(d->ent.d_name)-1]=0;
    d->cur=d->cur->next; return &d->ent;
}
int closedir(DIR *d){ if(d) free(d); return 0; }
time_t time(time_t *t){ long u=rtc_unix(); if(t)*t=u; return u; }
int clock_gettime(int id, struct timespec *tp){
    (void)id; if(!tp) return 0;
    if(!g_tsc_hz){ tp->tv_sec=0; tp->tv_nsec=0; return 0; }
    unsigned long long dt=rdtsc()-g_tsc_start;
    tp->tv_sec=(long)(dt/g_tsc_hz);
    tp->tv_nsec=(long)((dt%g_tsc_hz)*1000000000ULL/g_tsc_hz);
    return 0;
}
