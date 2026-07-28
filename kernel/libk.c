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
static void vga_putc(char c){
    if(c=='\n'){ vga_col=0; if(++vga_row>=25) vga_row=0; return; }
    if(c=='\r'){ vga_col=0; return; }
    VGA[vga_row*80+vga_col] = (u16)c | (0x0F<<8);
    if(++vga_col>=80){ vga_col=0; if(++vga_row>=25) vga_row=0; }
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
void console_init(void){
    outb(COM1+1,0x00); outb(COM1+3,0x80);
    outb(COM1+0,0x01); outb(COM1+1,0x00);   /* 115200 baud */
    outb(COM1+3,0x03); outb(COM1+2,0xC7); outb(COM1+4,0x0B);
}
void qemu_exit(unsigned char code){ outb(0xf4, code); }

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
struct _LZ_FILE { int kind; };      /* 0=stdin 1=stdout 2=stderr */
static FILE _stdin={0}, _stdout={1}, _stderr={2};
FILE *stdin=&_stdin, *stdout=&_stdout, *stderr=&_stderr;

static int is_std(FILE *f){ return f==&_stdin||f==&_stdout||f==&_stderr; }

int putchar(int c){ serial_putc((char)c); return c; }
int puts(const char *s){ con_puts(s); serial_putc('\n'); return 0; }
int fputs(const char *s, FILE *f){ if(is_std(f)) con_puts(s); return 0; }
int fputc(int c, FILE *f){ if(is_std(f)) serial_putc((char)c); return c; }
int fflush(FILE *f){ (void)f; return 0; }
int fprintf(FILE *f, const char *fmt, ...){
    Sink s = { 0,0,0, is_std(f) };
    va_list ap; va_start(ap,fmt); vformat(&s,fmt,ap); va_end(ap);
    return (int)s.len;
}
size_t fwrite(const void *p, size_t sz, size_t nm, FILE *f){
    if(is_std(f)){ const char *b=p; size_t t=sz*nm; while(t--) serial_putc(*b++); }
    return nm;
}
size_t fread(void *p, size_t sz, size_t nm, FILE *f){ (void)p;(void)sz;(void)nm;(void)f; return 0; }
/* line editor on stdin: echo + backspace; returns NULL only never (serial has no EOF) */
char *fgets(char *buf, int size, FILE *f){
    if(f!=&_stdin) return 0;
    int n=0;
    for(;;){
        char c=serial_getc();
        if(c=='\r'||c=='\n'){ serial_putc('\n'); buf[n++]='\n'; buf[n]=0; return buf; }
        if((c==0x7F||c==0x08)){ if(n>0){ n--; con_puts("\b \b"); } continue; }
        if(c>=32 && c<127 && n<size-2){ buf[n++]=c; serial_putc(c); }
    }
}
FILE *fopen(const char *path, const char *mode){ (void)path;(void)mode; return 0; }   /* no filesystem */
int fclose(FILE *f){ (void)f; return 0; }
FILE *popen(const char *c, const char *m){ (void)c;(void)m; return 0; }
int pclose(FILE *f){ (void)f; return -1; }

/* ======================================================================
 * OS stubs - no processes/filesystem/clock on bare metal yet
 * ==================================================================== */
void exit(int code){ con_puts("\n[larzscript exited]\n"); qemu_exit((unsigned char)code); for(;;) __asm__ volatile("hlt"); }
void abort(void){ exit(134); }
char *getenv(const char *n){ (void)n; return 0; }
int system(const char *c){ (void)c; return -1; }
char *realpath(const char *p, char *r){ (void)p;(void)r; return 0; }
char *getcwd(char *b, size_t n){ if(b&&n){ strncpy(b,"/",n); return b; } return 0; }
int chdir(const char *p){ (void)p; return -1; }
int rmdir(const char *p){ (void)p; return -1; }
int unlink(const char *p){ (void)p; return -1; }
int access(const char *p, int m){ (void)p;(void)m; return -1; }
int usleep(unsigned us){ for(volatile unsigned i=0;i<us*20u;i++){} return 0; }
unsigned sleep(unsigned s){ (void)s; return 0; }
int stat(const char *p, struct stat *b){ (void)p;(void)b; return -1; }
int mkdir(const char *p, mode_t m){ (void)p;(void)m; return -1; }
int rename(const char *o, const char *nw){ (void)o;(void)nw; return -1; }
int remove(const char *p){ (void)p; return -1; }
DIR *opendir(const char *n){ (void)n; return 0; }
struct dirent *readdir(DIR *d){ (void)d; return 0; }
int closedir(DIR *d){ (void)d; return 0; }
time_t time(time_t *t){ if(t)*t=0; return 0; }
int clock_gettime(int id, struct timespec *tp){ (void)id; if(tp){ tp->tv_sec=0; tp->tv_nsec=0; } return 0; }
