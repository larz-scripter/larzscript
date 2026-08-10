/* larzscript.c - a native, standalone implementation of Larzscript in C.
 *
 * This is the money-native language implemented in C, with NO dependency on
 * Python: it compiles to a single native `larzscript` executable that runs
 * .lz files directly. Just as "Python" is really CPython (an interpreter
 * written in C), this is Larzscript standing at the same ground level.
 *
 *   gcc -O2 -o larzscript larzscript.c
 *   ./larzscript program.lz
 *
 * v1.2 native - a real general-purpose language, standalone:
 *   values:   numbers, strings, booleans, nil, lists, dicts, money, wallets,
 *             functions (incl. anonymous fn(x){...} lambdas), modules
 *   binding:  let / assign / compound assign (+= -= *= /= %=)
 *   control:  if/else, while, for-in (lists, dicts, strings), break, continue,
 *             try/catch/throw, cond ? a : b (ternary)
 *   modules:  import "file.lz" as name  ->  name.member (cached, relative paths)
 *   operators: + - * / % // ** ; == != < <= > >= ; and or not ; in ; has
 *   funcs:    functions (default params) + recursion + closures + lambdas;
 *             gas-metered functions
 *   data:     list & dict literals, indexing, slicing a[i:j], element assign a[i]=x
 *   comprehensions: [x*x for x in xs if c] and {k: v for x in xs if c}
 *   strings:  f-strings  f"hi {name}, {1+2}"
 *   cli:      larzscript file.lz | -e "code" | repl | fmt file.lz
 *             ';' separates statements; `fmt` canonically formats a file
 *   methods:  list (push/pop/insert/sort/reverse/contains/index),
 *             dict (keys/values/has/get/remove), string (upper/lower/strip/
 *             split/replace/contains/starts_with/ends_with/find)
 *   stdlib:   print money len range str int float bool type abs min max sum
 *             sorted reversed floor ceil round sqrt pow chr ord assert input
 *             keys values push map filter reduce join enumerate zip exit
 *             all any count unique hex bin oct gcd factorial sign clamp list dict
 *             read_file write_file append_file file_exists; `args` = argv list
 *   range:    range(...) is a LAZY sequence (O(1) space) - iterate huge
 *             ranges without building a list; indexes, len, `in`, slicing work
 *   os:       env run capture cwd chdir listdir mkdir remove rename time clock sleep
 *   modules:  import <expr> (usually a string literal); searches relative,
 *             $LARZSCRIPT_PATH, ~/.larzscript/lib, ./lz_modules - so packages
 *             installed by `larzpkg` just import
 *   repl:     multi-line (keeps reading until brackets balance)
 *   strings/lists also support repetition: "ab"*3, [0]*5
 *   money:    $ = integer cents; price; pay .. from .. to ..; require;
 *             paywall / subscribe / has (money-native primitives)
 *   errors:   reported with line numbers; catchable with try/catch.
 * Memory: a precise mark-sweep garbage collector reclaims all heap objects -
 * lists, dicts, envs, closures, wallets, paywalls, AND strings - so
 * long-running programs stay bounded; it runs between statements and protects
 * in-flight temporaries with a temp-root stack. Verified under AddressSanitizer
 * with the GC forced on every statement. Zero third-party deps (libc only).
 */
#define LARZSCRIPT_VERSION "1.40.0"   /* single source of truth: --version, REPL banner, self-update */
#define _GNU_SOURCE   /* enable POSIX/GNU: popen, strtok_r, usleep, realpath, clock_gettime */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#ifdef _WIN32
/* MinGW's mkdir() takes no mode (no POSIX permission bits on Windows), and
 * doesn't declare realpath() (its closest analog is the CRT's _fullpath,
 * same "NULL dest = malloc a buffer" calling convention). */
#define mkdir(path,mode) mkdir(path)
#define realpath(path,resolved) _fullpath(resolved,path,4096)
#define WIN32_LEAN_AND_MEAN   /* GetModuleFileNameA only (larzscript update) - keep windows.h small */
#include <windows.h>          /* no identifier collisions here: file has no min/max/ERROR/IN/OUT */
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>       /* EM_JS/EM_ASM/EMSCRIPTEN_KEEPALIVE - the browser `ui` module + callback bridge */
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>      /* _NSGetExecutablePath - macOS's answer to Linux's /proc/self/exe (larzscript update) */
#endif
/* Hosted-only TCP sockets (socket_listen/accept/read/write/close, below).
 * The LarzOS kernel build (__STDC_HOSTED__==0) has its OWN real networking
 * (kernel/net.c, a from-scratch driver-level stack exposed via /net/ VFS
 * files as the `net`/`fetch` packages) - it has no <sys/socket.h> to
 * include (kernel/libc/ is a hand-written freestanding stub libc with no
 * socket.h in it), so this whole feature is compiled out there rather than
 * faking BSD socket semantics on top of a differently-shaped real stack.
 * Emscripten/wasm gets real headers (its libc has them) but every builtin
 * below still throws immediately - browsers have no raw TCP by sandbox
 * design, full stop, so pretending otherwise would just fail unpredictably
 * at runtime instead of with one clear error. */
#if !defined(__STDC_HOSTED__) || __STDC_HOSTED__
#ifndef __EMSCRIPTEN__
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET larz_sock_t;
#define LARZ_INVALID_SOCK INVALID_SOCKET
#define larz_sock_close closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>            /* getaddrinfo - socket_connect()'s DNS resolution */
#include <sys/select.h>       /* select()/fd_set - socket_poll() */
typedef int larz_sock_t;
#define LARZ_INVALID_SOCK (-1)
#define larz_sock_close close
#endif
#endif /* !__EMSCRIPTEN__ */
#endif /* hosted */
/* Real SSH via libssh - a real, audited C library, not a from-scratch
 * reimplementation (Larzscript's own double-only numeric type can't do the
 * big-integer/elliptic-curve math real SSH key exchange needs - see
 * crypto's README on X25519/Ed25519). Only compiled in when built with
 * -DLARZ_HAVE_LIBSSH -lssh - linux-x86_64, macos-x86_64, macos-arm64,
 * windows-x86_64, and linux-aarch64 CI jobs all do (see native.yml);
 * wasm never does (no raw TCP in that sandbox, same reasoning as
 * sockets there). ssh_* builtins exist everywhere but throw a clear
 * SshError "not available in this build" where libssh isn't linked. */
#ifdef LARZ_HAVE_LIBSSH
#include <libssh/libssh.h>
#include <libssh/server.h>   /* ssh_bind/ssh_message_* - the server-role API lives in a separate header from the client one */
/* Interactive shell/pty for the ssh server role - real fork()+forkpty(),
 * POSIX only (Linux x86_64/aarch64, macOS x86_64/arm64). Windows has no
 * fork(); a real equivalent needs ConPTY (CreatePseudoConsole), a wholly
 * different Win32 API - not implemented yet, so ssh_channel_shell()
 * throws a clear SshError there instead of silently pretending to work.
 * exec (ssh_run/the exec message type) is unaffected and already works
 * on all five targets - this only gates the interactive-shell path. */
#ifndef _WIN32
#define LARZ_HAVE_PTY 1
#if defined(__APPLE__)
#include <util.h>        /* openpty/forkpty on macOS/BSD */
#else
#include <pty.h>         /* openpty/forkpty on Linux (glibc) */
#endif
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#endif
#endif
#if defined(__STDC_HOSTED__) && !__STDC_HOSTED__
#include "gfx.h"              /* VGA Mode 13h graphics + widget model - the kernel-native `ui` module's backend */
#include "console.h"          /* task_exit()/launch_app() - needed by ui.close()/ui.launch() below */
#endif

/* ===================== small helpers ===================== */
static void *xmalloc(size_t n){ void *p = malloc(n); if(!p){ fprintf(stderr,"out of memory\n"); exit(2);} return p; }
static char *xstrndup(const char *s, size_t n){ char *p = xmalloc(n+1); memcpy(p,s,n); p[n]=0; return p; }
static char *xstrdup(const char *s){ return xstrndup(s, strlen(s)); }

/* ===================== garbage collector (mark-sweep) ===================== *
 * A precise mark-sweep GC over the container objects (lists, dicts, envs,
 * entries, closures, wallets, paywalls). Each such object begins with a GCObj
 * header and is threaded on a global list. Strings are NOT collected (they may
 * point into the permanent AST), so they leak - honest, and the containers are
 * what pile up in loops. GC runs only between statements (at exec entry), where
 * no half-built object is unrooted; the few builders that hold an object across
 * a user call (arrays, dicts, map/filter/reduce) protect it via temp roots. */
typedef struct GCObj { struct GCObj *gc_next; unsigned char gc_kind; unsigned char gc_marked; } GCObj;
enum { GC_LIST=1, GC_DICT, GC_ENV, GC_ENTRY, GC_CLOSURE, GC_WALLET, GC_PAYWALL, GC_STR, GC_RANGE };
typedef struct Str { GCObj gc; char data[]; } Str;   /* a GC-managed string; Value.str points at .data */
#define STR_HDR(p) ((GCObj*)((char*)(p) - offsetof(Str, data)))
static GCObj *g_gc_head=NULL;
static long g_gc_count=0, g_gc_threshold=200000;
static void gc_register(void *p, unsigned char kind){ GCObj *o=(GCObj*)p; o->gc_kind=kind; o->gc_marked=0; o->gc_next=g_gc_head; g_gc_head=o; g_gc_count++; }

/* ===================== values ===================== */
typedef enum { V_NIL, V_BOOL, V_NUM, V_MONEY, V_STR, V_WALLET, V_FUNC, V_BUILTIN, V_LIST, V_PAYWALL, V_DICT, V_MODULE, V_RANGE, V_CAPABILITY } VType;
struct Node; struct Env; struct Interp; struct List; struct Paywall; struct Dict; struct Range;
typedef struct Wallet { GCObj gc; char *name; long long cents; } Wallet;
typedef struct Closure { GCObj gc; struct Node *decl; struct Env *env; const char *name; } Closure;
typedef struct Value Value;
typedef Value (*BuiltinFn)(struct Interp*, Value*, int);
typedef struct Builtin { const char *name; BuiltinFn fn; } Builtin;

struct Value {
  VType t;
  int b;                 /* bool */
  double num;            /* number */
  long long cents;       /* money */
  char *str;             /* string */
  Wallet *wal;
  Closure *fn;
  Builtin *bi;
  struct List *list;
  struct Paywall *pw;
  struct Dict *dict;
  struct Env *mod;          /* module namespace (its globals) */
  char *modname;
  struct Range *rng;        /* lazy range sequence */
};

static Value V_nil(void){ Value v; v.t=V_NIL; return v; }
static Value V_bool(int b){ Value v; v.t=V_BOOL; v.b=b!=0; return v; }
/* a capability: a named, revocable grant `pay`/`subscribe requires` can check
 * before moving money - a plain scalar like V_BOOL (same .b storage), just a
 * distinct tag so grant/revoke/requires can reject "that's not a capability"
 * with a clear error instead of silently treating any boolean as one. */
static Value V_capability(int granted){ Value v; v.t=V_CAPABILITY; v.b=granted!=0; return v; }
static Value V_number(double d){ Value v; v.t=V_NUM; v.num=d; return v; }
static Value V_money(long long c){ Value v; v.t=V_MONEY; v.cents=c; return v; }
/* strings are GC-managed: copy into a Str object. V_string copies (source is
 * owned elsewhere - AST, wallet name, ...); V_take copies then frees a
 * runtime-allocated buffer; mkstr_n copies n bytes (for substrings). */
static Value mkstr_n(const char *s, size_t n){ Str *st=xmalloc(sizeof(Str)+n+1); gc_register(st, GC_STR); if(s && n) memcpy(st->data,s,n); st->data[n]=0; Value v; v.t=V_STR; v.str=st->data; return v; }
static Value V_string(const char *s){ return mkstr_n(s, s?strlen(s):0); }
static Value V_take(char *s){ Value v=mkstr_n(s, s?strlen(s):0); free(s); return v; }
static Value V_wallet(Wallet *w){ Value v; v.t=V_WALLET; v.wal=w; return v; }
static Value V_func(Closure *c){ Value v; v.t=V_FUNC; v.fn=c; return v; }
static Value V_builtin(Builtin *b){ Value v; v.t=V_BUILTIN; v.bi=b; return v; }

typedef struct List { GCObj gc; Value *items; int n, cap; } List;
typedef struct Paywall { GCObj gc; char *name; long long price; char *period; char *payee; } Paywall;
typedef struct Pair { Value key, val; } Pair;
typedef struct Dict { GCObj gc; Pair *items; int n, cap; } Dict;
static Value V_list(List *l){ Value v; v.t=V_LIST; v.list=l; return v; }
static Value V_paywall(Paywall *pw){ Value v; v.t=V_PAYWALL; v.pw=pw; return v; }
static Value V_dict(Dict *d){ Value v; v.t=V_DICT; v.dict=d; return v; }
static Value V_module(struct Env *e, char *name){ Value v; v.t=V_MODULE; v.mod=e; v.modname=name; return v; }
typedef struct Range { GCObj gc; long long start, stop, step; } Range;
static Value V_range(long long start, long long stop, long long step){ Range *r=xmalloc(sizeof(Range)); gc_register(r,GC_RANGE); r->start=start; r->stop=stop; r->step=step; Value v; v.t=V_RANGE; v.rng=r; return v; }
static long long range_len(Range *r){ if(r->step>0){ return r->stop>r->start ? (r->stop-r->start+r->step-1)/r->step : 0; } else { return r->start>r->stop ? (r->start-r->stop-r->step-1)/(-r->step) : 0; } }
static long long range_at(Range *r, long long i){ return r->start + i*r->step; }
static List *list_new(void){ List *l=xmalloc(sizeof(List)); l->items=NULL; l->n=0; l->cap=0; gc_register(l,GC_LIST); return l; }
static void list_push(List *l, Value v){ if(l->n==l->cap){ l->cap=l->cap?l->cap*2:8; l->items=realloc(l->items,l->cap*sizeof(Value)); } l->items[l->n++]=v; }
static Value range_to_list(Range *r){ List *l=list_new(); long long len=range_len(r); for(long long i=0;i<len;i++) list_push(l, V_number((double)range_at(r,i))); return V_list(l); }
static Value derange(Value v){ return v.t==V_RANGE ? range_to_list(v.rng) : v; }
static Dict *dict_new(void){ Dict *d=xmalloc(sizeof(Dict)); d->items=NULL; d->n=0; d->cap=0; gc_register(d,GC_DICT); return d; }

static long long money_round(double x){ return (long long)(x>=0 ? x+0.5 : x-0.5); }

static int is_num(Value v){ return v.t==V_NUM; }

/* value equality (used by ==, dict keys, list contains) - by value, not
 * identity: lists/dicts recurse into their elements instead of comparing
 * pointers, so `[1,2] == [1,2]` is true for two separately-built lists
 * (matches the doc comment above, which the V_LIST/V_DICT cases used to
 * violate by comparing container identity instead). */
static Value *dict_find(Dict *d, Value key);
static int values_equal(Value a, Value b){
  if(a.t!=b.t) return 0;
  switch(a.t){
    case V_NIL: return 1;
    case V_BOOL: return a.b==b.b;
    case V_CAPABILITY: return a.b==b.b;
    case V_NUM: return a.num==b.num;
    case V_MONEY: return a.cents==b.cents;
    case V_STR: return strcmp(a.str,b.str)==0;
    case V_WALLET: return a.wal==b.wal;
    case V_PAYWALL: return a.pw==b.pw;
    case V_LIST: {
      if(a.list==b.list) return 1;
      if(a.list->n!=b.list->n) return 0;
      for(int i=0;i<a.list->n;i++) if(!values_equal(a.list->items[i],b.list->items[i])) return 0;
      return 1;
    }
    case V_DICT: {
      if(a.dict==b.dict) return 1;
      if(a.dict->n!=b.dict->n) return 0;
      for(int i=0;i<a.dict->n;i++){
        Value *bv=dict_find(b.dict, a.dict->items[i].key);
        if(!bv || !values_equal(a.dict->items[i].val, *bv)) return 0;
      }
      return 1;
    }
    case V_MODULE: return a.mod==b.mod;
    case V_RANGE: return a.rng->start==b.rng->start && a.rng->stop==b.rng->stop && a.rng->step==b.rng->step;
    default: return 0;
  }
}
/* order comparison for sort/sorted; returns -1/0/1. Incomparable -> 0 (stable). */
static int value_compare(Value a, Value b){
  if(a.t==V_NUM && b.t==V_NUM) return a.num<b.num?-1:(a.num>b.num?1:0);
  if(a.t==V_MONEY && b.t==V_MONEY) return a.cents<b.cents?-1:(a.cents>b.cents?1:0);
  if(a.t==V_STR && b.t==V_STR){ int c=strcmp(a.str,b.str); return c<0?-1:(c>0?1:0); }
  return 0;
}
static int qsort_value_cmp(const void *x, const void *y){ return value_compare(*(const Value*)x, *(const Value*)y); }

/* dict get: returns pointer to the stored value, or NULL. Keys compared by value. */
static Value *dict_find(Dict *d, Value key){
  for(int i=0;i<d->n;i++) if(values_equal(d->items[i].key, key)) return &d->items[i].val;
  return NULL;
}
static void dict_set(Dict *d, Value key, Value val){
  Value *slot=dict_find(d,key);
  if(slot){ *slot=val; return; }
  if(d->n==d->cap){ d->cap=d->cap?d->cap*2:8; d->items=realloc(d->items,d->cap*sizeof(Pair)); }
  d->items[d->n].key=key; d->items[d->n].val=val; d->n++;
}
static int dict_del(Dict *d, Value key){
  for(int i=0;i<d->n;i++) if(values_equal(d->items[i].key,key)){ for(int j=i+1;j<d->n;j++) d->items[j-1]=d->items[j]; d->n--; return 1; }
  return 0;
}

static int truthy(Value v){
  switch(v.t){
    case V_NIL: return 0;
    case V_BOOL: return v.b;
    case V_CAPABILITY: return v.b;   /* truthy iff currently granted */
    case V_NUM: return v.num!=0;
    case V_MONEY: return v.cents!=0;
    case V_STR: return v.str[0]!=0;
    case V_LIST: return v.list->n!=0;
    case V_DICT: return v.dict->n!=0;
    case V_RANGE: return range_len(v.rng)!=0;
    default: return 1;
  }
}

/* print a number the way Larzscript does: integers without a decimal point */
static void print_number(double d){
  if(d==(long long)d) printf("%lld",(long long)d);
  else printf("%g",d);
}
static void print_value(Value v){
  switch(v.t){
    case V_NIL: printf("nil"); break;
    case V_BOOL: printf(v.b?"true":"false"); break;
    case V_CAPABILITY: printf(v.b?"<capability granted>":"<capability revoked>"); break;
    case V_NUM: print_number(v.num); break;
    case V_MONEY: {
      long long c = v.cents<0?-v.cents:v.cents;
      printf("%s$%lld.%02lld", v.cents<0?"-":"", c/100, c%100);
      break;
    }
    case V_STR: printf("%s", v.str); break;
    case V_WALLET: {
      long long c=v.wal->cents<0?-v.wal->cents:v.wal->cents;
      printf("<wallet %s: %s$%lld.%02lld>", v.wal->name, v.wal->cents<0?"-":"", c/100, c%100);
      break;
    }
    case V_FUNC: printf("<fn %s>", v.fn->name?v.fn->name:"?"); break;
    case V_BUILTIN: printf("<builtin %s>", v.bi->name); break;
    case V_LIST: {
      printf("[");
      for(int i=0;i<v.list->n;i++){ if(i) printf(", "); print_value(v.list->items[i]); }
      printf("]");
      break;
    }
    case V_PAYWALL: {
      long long c=v.pw->price<0?-v.pw->price:v.pw->price;
      printf("<paywall %s: $%lld.%02lld/%s>", v.pw->name, c/100, c%100, v.pw->period);
      break;
    }
    case V_MODULE: printf("<module %s>", v.modname?v.modname:"?"); break;
    case V_RANGE: { printf("["); long long ln=range_len(v.rng); for(long long i=0;i<ln;i++){ if(i) printf(", "); printf("%lld", range_at(v.rng,i)); } printf("]"); break; }
    case V_DICT: {
      printf("{");
      for(int i=0;i<v.dict->n;i++){
        if(i) printf(", ");
        print_value(v.dict->items[i].key); printf(": "); print_value(v.dict->items[i].val);
      }
      printf("}");
      break;
    }
  }
}

/* build a Value into a heap string (mirrors print_value; strings unquoted) */
typedef struct { char *s; int n, cap; } SB;
static void sb_putc(SB *b, char c){ if(b->n+1>=b->cap){ b->cap=b->cap?b->cap*2:32; b->s=realloc(b->s,b->cap);} b->s[b->n++]=c; }
static void sb_puts(SB *b, const char *s){ while(*s) sb_putc(b,*s++); }
static void sb_putf(SB *b, const char *fmt, ...){ char t[64]; va_list ap; va_start(ap,fmt); vsnprintf(t,sizeof t,fmt,ap); va_end(ap); sb_puts(b,t); }
static void val_to_sb(SB *b, Value v){
  switch(v.t){
    case V_NIL: sb_puts(b,"nil"); break;
    case V_BOOL: sb_puts(b, v.b?"true":"false"); break;
    case V_CAPABILITY: sb_puts(b, v.b?"<capability granted>":"<capability revoked>"); break;
    case V_NUM: if(v.num==(long long)v.num) sb_putf(b,"%lld",(long long)v.num); else sb_putf(b,"%g",v.num); break;
    case V_MONEY: { long long c=v.cents<0?-v.cents:v.cents; sb_putf(b,"%s$%lld.%02lld", v.cents<0?"-":"", c/100, c%100); break; }
    case V_STR: sb_puts(b, v.str); break;
    case V_WALLET: { long long c=v.wal->cents<0?-v.wal->cents:v.wal->cents; sb_putf(b,"<wallet %s: %s$%lld.%02lld>", v.wal->name, v.wal->cents<0?"-":"", c/100, c%100); break; }
    case V_FUNC: sb_putf(b,"<fn %s>", v.fn->name?v.fn->name:"?"); break;
    case V_BUILTIN: sb_putf(b,"<builtin %s>", v.bi->name); break;
    case V_LIST: { sb_putc(b,'['); for(int i=0;i<v.list->n;i++){ if(i) sb_puts(b,", "); val_to_sb(b,v.list->items[i]); } sb_putc(b,']'); break; }
    case V_DICT: { sb_putc(b,'{'); for(int i=0;i<v.dict->n;i++){ if(i) sb_puts(b,", "); val_to_sb(b,v.dict->items[i].key); sb_puts(b,": "); val_to_sb(b,v.dict->items[i].val); } sb_putc(b,'}'); break; }
    case V_PAYWALL: { long long c=v.pw->price<0?-v.pw->price:v.pw->price; sb_putf(b,"<paywall %s: $%lld.%02lld/%s>", v.pw->name, c/100, c%100, v.pw->period); break; }
    case V_MODULE: sb_putf(b,"<module %s>", v.modname?v.modname:"?"); break;
    case V_RANGE: { sb_putc(b,'['); long long ln=range_len(v.rng); for(long long i=0;i<ln;i++){ if(i) sb_puts(b,", "); sb_putf(b,"%lld", range_at(v.rng,i)); } sb_putc(b,']'); break; }
  }
}
static char *str_of(Value v){ SB b; b.s=NULL; b.n=0; b.cap=0; val_to_sb(&b,v); sb_putc(&b,0); return b.s; }

/* ===================== AST ===================== */
typedef enum {
  N_LET, N_ASSIGN, N_PRICE, N_WALLET, N_PAY, N_REQUIRE, N_FN, N_RETURN,
  N_IF, N_WHILE, N_BLOCK, N_EXPR, N_PAYWALL, N_SUBSCRIBE,
  N_NUM, N_MONEY, N_STR, N_BOOL, N_NIL, N_NAME, N_BIN, N_UN, N_CALL, N_GET, N_METHOD,
  N_ARRAY, N_INDEX, N_DICT, N_SETINDEX, N_BREAK, N_CONTINUE, N_FOR,
  N_SLICE, N_TRY, N_THROW, N_FSTR, N_IMPORT, N_TERNARY, N_LISTCOMP, N_DICTCOMP,
  N_CAPABILITY, N_GRANT, N_REVOKE, N_SPLIT
} NodeKind;

typedef struct Node {
  NodeKind kind;
  int line;                   /* source line, for error messages */
  double num; long long cents; char *str; int boolean;
  char *name;                 /* identifier / member / let-name / op holder */
  char *op;
  struct Node *a, *b, *c;
  struct Node **kids; int nkids;
  char **params; int nparams;
  struct Node **pdefs;        /* per-param default value expressions (or NULL) */
  char *cop;                  /* compound-assign op ("+", ...) for the formatter, or NULL */
  char *natural;               /* the natural-language spelling this node was parsed from
                                 * ("is", "is not", "is at least", "unless", "from_to", "say",
                                 * "wait"...) so the formatter can print it back instead of the
                                 * desugared symbolic form, or NULL for ordinary syntax */
  long long gas; int has_gas;
  char *src, *dst;            /* pay / subscribe */
  char *period;               /* paywall */
  int lam_id;                 /* larzc: 1-based id of a hoisted lambda/nested-fn (0 = none) */
} Node;

static int g_parse_line=0;   /* line of the most recently consumed token */
static Node *node(NodeKind k){ Node *n = xmalloc(sizeof(Node)); memset(n,0,sizeof(Node)); n->kind=k; n->line=g_parse_line; return n; }
static void push_kid(Node *b, Node *k){
  if(b->nkids % 8 == 0) b->kids = realloc(b->kids, (b->nkids+8)*sizeof(Node*));
  b->kids[b->nkids++]=k;
}

/* ===================== lexer ===================== */
typedef enum { T_EOF, T_NUM, T_MONEY, T_STR, T_FSTR, T_IDENT, T_KW, T_OP,
               T_LP, T_RP, T_LB, T_RB, T_COMMA, T_DOT, T_LBK, T_RBK, T_COLON, T_QUESTION } TokType;
typedef struct { TokType type; char *text; double num; long long cents; int line; } Token;

static const char *KEYWORDS[] = {
  "let","fn","return","if","else","while","and","or","not","true","false","nil",
  "price","wallet","pay","from","to","require","gas",
  "paywall","subscribe","has","for","in","break","continue",
  "try","catch","throw","import","as",
  "capability","grant","revoke","requires","split",
  /* natural-language sugar (2026-08): all desugar to existing AST nodes -
   * see equality()/statement()'s "unless"/"say"/"wait" branches and the
   * "for ... from ... to" case in the existing "for" branch. */
  "is","unless","at","least","most","more","less","than","say","wait", NULL
};
static int is_keyword(const char *s, int n){
  for(int i=0; KEYWORDS[i]; i++)
    if((int)strlen(KEYWORDS[i])==n && strncmp(KEYWORDS[i],s,n)==0) return 1;
  return 0;
}

typedef struct { Token *toks; int n, cap; } TokList;
static void tk_push(TokList *tl, Token t){
  if(tl->n==tl->cap){ tl->cap = tl->cap?tl->cap*2:64; tl->toks = realloc(tl->toks, tl->cap*sizeof(Token)); }
  tl->toks[tl->n++]=t;
}

static jmp_buf g_err;                 /* compile/lex/parse errors */
static char g_errmsg[256];
static void fail(const char *fmt, ...){
  va_list ap; va_start(ap,fmt); vsnprintf(g_errmsg,sizeof(g_errmsg),fmt,ap); va_end(ap);
  longjmp(g_err,1);
}

static long long money_cents(const char *s, int n){
  /* s: digits and one optional dot, length n */
  int dot=-1;
  for(int i=0;i<n;i++) if(s[i]=='.'){ if(dot>=0) fail("bad money literal"); dot=i; }
  if(dot<0){
    long long whole=0; for(int i=0;i<n;i++) whole=whole*10+(s[i]-'0'); return whole*100;
  } else {
    long long whole=0; for(int i=0;i<dot;i++) whole=whole*10+(s[i]-'0');
    int f0 = dot+1<n ? s[dot+1]-'0' : 0;
    int f1 = dot+2<n ? s[dot+2]-'0' : 0;
    return whole*100 + f0*10 + f1;
  }
}

static Token *lex(const char *src){
  TokList tl = {0}; int i=0, line=1;
  while(src[i]){
    char c=src[i];
    if(c=='\n'){ line++; i++; continue; }
    if(c==' '||c=='\t'||c=='\r'||c==';'){ i++; continue; }  /* ';' = optional separator */
    if(c=='#'){ while(src[i] && src[i]!='\n') i++; continue; }
    Token t; t.line=line; t.text=NULL;
    if(c=='$'){
      int j=i+1; while(isdigit((unsigned char)src[j])||src[j]=='.') j++;
      t.type=T_MONEY; t.cents=money_cents(src+i+1, j-(i+1)); i=j; tk_push(&tl,t); continue;
    }
    if(isdigit((unsigned char)c) || (c=='.'&&isdigit((unsigned char)src[i+1]))){
      int j=i; while(isdigit((unsigned char)src[j])||src[j]=='.') j++;
      char *num=xstrndup(src+i,j-i); t.type=T_NUM; t.num=strtod(num,NULL); i=j; tk_push(&tl,t); continue;
    }
    if(c=='"' || (c=='f' && src[i+1]=='"')){
      int isf = c=='f';
      int j = i + (isf?2:1); char buf[8192]; int b=0;
      while(src[j] && src[j]!='"'){
        if(src[j]=='\\' && src[j+1]){
          char e=src[j+1];
          buf[b++]= e=='n'?'\n': e=='t'?'\t': e=='r'?'\r': e=='0'?'\0': e;
          j+=2; continue;
        }
        buf[b++]=src[j++];
        if(b>=(int)sizeof(buf)-1) fail("string too long on line %d", line);
      }
      if(!src[j]) fail("unterminated string on line %d", line);
      buf[b]=0; t.type = isf?T_FSTR:T_STR; t.text=xstrdup(buf); i=j+1; tk_push(&tl,t); continue;
    }
    if(isalpha((unsigned char)c)||c=='_'){
      int j=i; while(isalnum((unsigned char)src[j])||src[j]=='_') j++;
      t.text=xstrndup(src+i,j-i);
      t.type = is_keyword(src+i,j-i) ? T_KW : T_IDENT;
      i=j; tk_push(&tl,t); continue;
    }
    /* ** (power) and // (floor division) - before compound-assign so *= /= still work */
    if((c=='*'&&src[i+1]=='*')||(c=='/'&&src[i+1]=='/')){
      t.type=T_OP; t.text=xstrndup(src+i,2); i+=2; tk_push(&tl,t); continue;
    }
    /* two-char operators (comparison + compound assignment) */
    if((c=='='&&src[i+1]=='=')||(c=='!'&&src[i+1]=='=')||(c=='<'&&src[i+1]=='=')||(c=='>'&&src[i+1]=='=')
       ||((c=='+'||c=='-'||c=='*'||c=='/'||c=='%')&&src[i+1]=='=')){
      t.type=T_OP; t.text=xstrndup(src+i,2); i+=2; tk_push(&tl,t); continue;
    }
    if(strchr("+-*/%<>=",c)){ t.type=T_OP; t.text=xstrndup(src+i,1); i++; tk_push(&tl,t); continue; }
    switch(c){
      case '(': t.type=T_LP; i++; break;
      case ')': t.type=T_RP; i++; break;
      case '{': t.type=T_LB; i++; break;
      case '}': t.type=T_RB; i++; break;
      case ',': t.type=T_COMMA; i++; break;
      case '.': t.type=T_DOT; i++; break;
      case '[': t.type=T_LBK; i++; break;
      case ']': t.type=T_RBK; i++; break;
      case ':': t.type=T_COLON; i++; break;
      case '?': t.type=T_QUESTION; i++; break;
      default: fail("unexpected character '%c' on line %d", c, line);
    }
    tk_push(&tl,t);
  }
  Token e; e.type=T_EOF; e.text=NULL; e.line=line; tk_push(&tl,e);
  return tl.toks;
}

/* ===================== parser ===================== */
typedef struct { Token *t; int i; int loopn; } Parser;
static Token *pk(Parser *p){ return &p->t[p->i]; }
static Token *padv(Parser *p){ Token *t=&p->t[p->i++]; g_parse_line=t->line; return t; }
static int is_kw(Token *t, const char *w){ return t->type==T_KW && strcmp(t->text,w)==0; }
static int is_op(Token *t, const char *o){ return t->type==T_OP && strcmp(t->text,o)==0; }
static void expect(Parser *p, TokType ty, const char *what){
  if(pk(p)->type!=ty) fail("expected %s on line %d", what, pk(p)->line);
  padv(p);
}
static void expect_kw(Parser *p, const char *w){
  if(!is_kw(pk(p),w)) fail("expected '%s' on line %d", w, pk(p)->line);
  padv(p);
}

static Node *statement(Parser *p);
static Node *expression(Parser *p);
static Node *parse_fn(Parser *p, int named);
static Node *fstring_node(const char *raw);

static Node *block(Parser *p){
  expect(p, T_LB, "'{'");
  Node *n = node(N_BLOCK); int cap=0;
  while(pk(p)->type!=T_RB && pk(p)->type!=T_EOF){
    if(n->nkids==cap){ cap=cap?cap*2:8; n->kids=realloc(n->kids,cap*sizeof(Node*)); }
    n->kids[n->nkids++]=statement(p);
  }
  expect(p, T_RB, "'}'");
  return n;
}

static void arglist(Parser *p, Node ***out, int *nout){
  expect(p, T_LP, "'('");
  Node **args=NULL; int n=0, cap=0;
  if(pk(p)->type!=T_RP){
    do {
      if(n==cap){ cap=cap?cap*2:4; args=realloc(args,cap*sizeof(Node*)); }
      args[n++]=expression(p);
    } while(pk(p)->type==T_COMMA && (padv(p),1));
  }
  expect(p, T_RP, "')'");
  *out=args; *nout=n;
}

static Node *primary(Parser *p){
  Token *t = pk(p);
  if(t->type==T_NUM){ padv(p); Node *n=node(N_NUM); n->num=t->num; return n; }
  if(t->type==T_MONEY){ padv(p); Node *n=node(N_MONEY); n->cents=t->cents; return n; }
  if(t->type==T_STR){ padv(p); Node *n=node(N_STR); n->str=t->text; return n; }
  if(t->type==T_FSTR){ padv(p); return fstring_node(t->text); }
  if(is_kw(t,"fn")){ return parse_fn(p, 0); }   /* anonymous function (lambda) */
  if(is_kw(t,"true")){ padv(p); Node *n=node(N_BOOL); n->boolean=1; return n; }
  if(is_kw(t,"false")){ padv(p); Node *n=node(N_BOOL); n->boolean=0; return n; }
  if(is_kw(t,"nil")){ padv(p); return node(N_NIL); }
  if(t->type==T_IDENT){ padv(p); Node *n=node(N_NAME); n->name=t->text; return n; }
  if(t->type==T_LP){ padv(p); Node *e=expression(p); expect(p,T_RP,"')'"); return e; }
  if(t->type==T_LBK){
    padv(p);
    if(pk(p)->type==T_RBK){ padv(p); return node(N_ARRAY); }
    Node *first=expression(p);
    if(is_kw(pk(p),"for")){          /* list comprehension: [expr for x in it (if c)?] */
      padv(p); Node *n=node(N_LISTCOMP); n->a=first; n->name=padv(p)->text;
      expect_kw(p,"in"); n->b=expression(p);
      if(is_kw(pk(p),"if")){ padv(p); n->c=expression(p); }
      expect(p, T_RBK, "']'"); return n;
    }
    Node *n=node(N_ARRAY); push_kid(n, first);
    while(pk(p)->type==T_COMMA){ padv(p); if(pk(p)->type==T_RBK) break; push_kid(n, expression(p)); }
    expect(p, T_RBK, "']'");
    return n;
  }
  if(t->type==T_LB){    /* dict literal or dict comprehension */
    padv(p);
    if(pk(p)->type==T_RB){ padv(p); return node(N_DICT); }
    Node *k=expression(p); expect(p, T_COLON, "':'"); Node *v=expression(p);
    if(is_kw(pk(p),"for")){          /* dict comprehension: {k: v for x in it (if c)?} */
      padv(p); Node *n=node(N_DICTCOMP); n->a=k; n->b=v; n->name=padv(p)->text;
      expect_kw(p,"in"); n->c=expression(p);
      if(is_kw(pk(p),"if")){ padv(p); push_kid(n, expression(p)); }
      expect(p, T_RB, "'}'"); return n;
    }
    Node *n=node(N_DICT); push_kid(n,k); push_kid(n,v);
    while(pk(p)->type==T_COMMA){ padv(p); if(pk(p)->type==T_RB) break; Node *kk=expression(p); expect(p,T_COLON,"':'"); Node *vv=expression(p); push_kid(n,kk); push_kid(n,vv); }
    expect(p, T_RB, "'}'");
    return n;
  }
  fail("unexpected token on line %d", t->line);
  return NULL;
}

static Node *postfix(Parser *p){
  Node *n = primary(p);
  for(;;){
    if(pk(p)->type==T_DOT){
      padv(p);
      if(pk(p)->type!=T_IDENT && pk(p)->type!=T_KW) fail("expected a property name on line %d", pk(p)->line);
      char *name = padv(p)->text;
      if(pk(p)->type==T_LP){
        Node *m=node(N_METHOD); m->a=n; m->name=name; arglist(p,&m->kids,&m->nkids); n=m;
      } else {
        Node *g=node(N_GET); g->a=n; g->name=name; n=g;
      }
    } else if(pk(p)->type==T_LP){
      Node *c=node(N_CALL); c->a=n; arglist(p,&c->kids,&c->nkids); n=c;
    } else if(pk(p)->type==T_LBK){
      padv(p);
      Node *start=NULL, *end=NULL; int slice=0;
      if(pk(p)->type!=T_COLON) start=expression(p);
      if(pk(p)->type==T_COLON){ slice=1; padv(p); if(pk(p)->type!=T_RBK) end=expression(p); }
      expect(p,T_RBK,"']'");
      if(slice){ Node *s=node(N_SLICE); s->a=n; s->b=start; s->c=end; n=s; }
      else { Node *ix=node(N_INDEX); ix->a=n; ix->b=start; n=ix; }
    } else break;
  }
  return n;
}

static Node *power(Parser *p){   /* right-associative ** */
  Node *n=postfix(p);
  if(is_op(pk(p),"**")){ padv(p); Node *b=node(N_BIN); b->op="**"; b->a=n; b->b=power(p); return b; }
  return n;
}
static Node *unary(Parser *p){
  if(is_kw(pk(p),"not")){ padv(p); Node *n=node(N_UN); n->op="not"; n->a=unary(p); return n; }
  if(is_op(pk(p),"-")){ padv(p); Node *n=node(N_UN); n->op="-"; n->a=unary(p); return n; }
  return power(p);
}
static Node *bin_lvl(Parser *p, Node*(*sub)(Parser*), const char **ops, int nops, int kw){
  Node *n = sub(p);
  for(;;){
    Token *t=pk(p); int m=-1;
    for(int i=0;i<nops;i++){
      if(kw ? is_kw(t,ops[i]) : is_op(t,ops[i])){ m=i; break; }
    }
    if(m<0) break;
    padv(p);
    Node *b=node(N_BIN); b->op=(char*)ops[m]; b->a=n; b->b=sub(p); n=b;
  }
  return n;
}
static Node *factor(Parser *p){ const char *o[]={"*","/","%","//"}; return bin_lvl(p,unary,o,4,0); }
static Node *term(Parser *p){ const char *o[]={"+","-"}; return bin_lvl(p,factor,o,2,0); }
static Node *comparison(Parser *p){ const char *o[]={"<","<=",">",">="}; return bin_lvl(p,term,o,4,0); }
static Node *membership(Parser *p){ const char *o[]={"has","in"}; return bin_lvl(p,comparison,o,2,1); }
/* Natural-language comparison phrases ("is"/"is not"/"is at least"/"is at
 * most"/"is more than"/"is less than") all desugar to an ordinary N_BIN
 * with a real "=="/"!="/">="/"<="/">"/"<" op - eval()/--emit-c need no new
 * cases, they already handle those op strings. Hand-written (not the
 * generic bin_lvl helper) because "is not"/"is at least"/etc need to look
 * two tokens ahead, and the tighter-binding phrases (at least/at most/more
 * than/less than) parse their right side at term() - the same level the
 * real >=/<=/>/< operators use - not membership() like bare is/is not, so
 * `a is at least b + c` binds exactly like `a >= b + c` already does. */
static Node *equality(Parser *p){
  Node *n = membership(p);
  for(;;){
    Token *t = pk(p);
    if(is_op(t,"==")||is_op(t,"!=")){
      const char *op=t->text; padv(p);
      Node *b=membership(p);
      Node *bn=node(N_BIN); bn->op=(char*)op; bn->a=n; bn->b=b; n=bn;
      continue;
    }
    if(is_kw(t,"is")){
      padv(p);
      const char *op="==", *nat="is"; int tight=0;
      if(is_kw(pk(p),"not")){ padv(p); op="!="; nat="is not"; }
      else if(is_kw(pk(p),"at")){
        padv(p);
        if(is_kw(pk(p),"least")){ padv(p); op=">="; nat="is at least"; tight=1; }
        else if(is_kw(pk(p),"most")){ padv(p); op="<="; nat="is at most"; tight=1; }
        else fail("expected 'least' or 'most' after 'is at' on line %d", pk(p)->line);
      } else if(is_kw(pk(p),"more")){ padv(p); expect_kw(p,"than"); op=">"; nat="is more than"; tight=1; }
      else if(is_kw(pk(p),"less")){ padv(p); expect_kw(p,"than"); op="<"; nat="is less than"; tight=1; }
      Node *b = tight ? term(p) : membership(p);
      Node *bn=node(N_BIN); bn->op=(char*)op; bn->natural=(char*)nat; bn->a=n; bn->b=b; n=bn;
      continue;
    }
    break;
  }
  return n;
}
static Node *logic_and(Parser *p){ const char *o[]={"and"}; return bin_lvl(p,equality,o,1,1); }
static Node *logic_or(Parser *p){ const char *o[]={"or"}; return bin_lvl(p,logic_and,o,1,1); }
static Node *ternary(Parser *p){
  Node *c=logic_or(p);
  if(pk(p)->type==T_QUESTION){ padv(p); Node *t=node(N_TERNARY); t->a=c; t->b=expression(p); expect(p,T_COLON,"':'"); t->c=expression(p); return t; }
  return c;
}
static Node *expression(Parser *p){ return ternary(p); }

static Node *parse_fn(Parser *p, int named){
  expect_kw(p,"fn");
  Node *n=node(N_FN);
  n->name = named ? padv(p)->text : NULL;
  expect(p,T_LP,"'('");
  int cap=0;
  if(pk(p)->type!=T_RP){
    do {
      if(n->nparams==cap){ cap=cap?cap*2:4; n->params=realloc(n->params,cap*sizeof(char*)); n->pdefs=realloc(n->pdefs,cap*sizeof(Node*)); }
      n->params[n->nparams]=padv(p)->text;
      Node *def=NULL;
      if(is_op(pk(p),"=")){ padv(p); def=expression(p); }
      n->pdefs[n->nparams]=def;
      n->nparams++;
    } while(pk(p)->type==T_COMMA && (padv(p),1));
  }
  expect(p,T_RP,"')'");
  if(is_kw(pk(p),"gas")){ padv(p); if(pk(p)->type!=T_NUM) fail("expected a gas amount on line %d",pk(p)->line); n->gas=(long long)padv(p)->num; n->has_gas=1; }
  n->b=block(p);
  return n;
}

/* build an f-string "f{...}" into a "+"-concatenation of strings and str(expr) */
static Node *mkstr_node(const char *s){ Node *n=node(N_STR); n->str=xstrdup(s); return n; }
static Node *mkadd(Node *a, Node *b){ if(!a) return b; Node *n=node(N_BIN); n->op="+"; n->a=a; n->b=b; return n; }
static Node *fstring_node(const char *raw){
  Node *result=NULL;
  SB lit; lit.s=NULL; lit.n=0; lit.cap=0;
  int i=0;
  while(raw[i]){
    char c=raw[i];
    if(c=='{'){
      if(raw[i+1]=='{'){ sb_putc(&lit,'{'); i+=2; continue; }
      /* flush any pending literal */
      sb_putc(&lit,0); result=mkadd(result, mkstr_node(lit.s?lit.s:"")); lit.n=0; if(lit.s) lit.s[0]=0;
      /* capture up to the matching '}' */
      int j=i+1, depth=1; SB ex; ex.s=NULL; ex.n=0; ex.cap=0;
      while(raw[j] && depth>0){
        if(raw[j]=='{') depth++;
        else if(raw[j]=='}'){ depth--; if(depth==0) break; }
        sb_putc(&ex, raw[j]); j++;
      }
      if(!raw[j]) fail("unclosed '{' in f-string");
      sb_putc(&ex,0);
      Token *toks=lex(ex.s?ex.s:"");
      Parser sp; sp.t=toks; sp.i=0; sp.loopn=0;
      Node *e=expression(&sp);
      Node *call=node(N_CALL); Node *nm=node(N_NAME); nm->name=xstrdup("str"); call->a=nm; push_kid(call, e);
      result=mkadd(result, call);
      i=j+1; continue;
    }
    if(c=='}' && raw[i+1]=='}'){ sb_putc(&lit,'}'); i+=2; continue; }
    sb_putc(&lit, c); i++;
  }
  sb_putc(&lit,0);
  if(lit.n>1 || result==NULL) result=mkadd(result, mkstr_node(lit.s?lit.s:""));
  Node *fs=node(N_FSTR); fs->str=(char*)raw; fs->a=result;   /* str=raw for the formatter, a=concat for eval */
  return fs;
}

static int starts_expr(Token *t){
  if(t->type==T_NUM||t->type==T_MONEY||t->type==T_STR||t->type==T_FSTR||t->type==T_IDENT||t->type==T_LP||t->type==T_LBK||t->type==T_LB) return 1;
  if(is_kw(t,"true")||is_kw(t,"false")||is_kw(t,"nil")||is_kw(t,"not")||is_kw(t,"fn")) return 1;
  if(is_op(t,"-")) return 1;
  return 0;
}

static Node *statement(Parser *p){
  Token *t = pk(p);
  if(is_kw(t,"let")){ padv(p); Node *n=node(N_LET); n->name=padv(p)->text; if(!is_op(pk(p),"=")) fail("expected '=' on line %d",pk(p)->line); padv(p); n->a=expression(p); return n; }
  if(is_kw(t,"price")){ padv(p); Node *n=node(N_PRICE); n->name=padv(p)->text; if(!is_op(pk(p),"=")) fail("expected '=' on line %d",pk(p)->line); padv(p); n->a=expression(p); return n; }
  if(is_kw(t,"wallet")){ padv(p); Node *n=node(N_WALLET); n->name=padv(p)->text; if(is_op(pk(p),"=")){ padv(p); n->a=expression(p); } return n; }
  if(is_kw(t,"capability")){ padv(p); Node *n=node(N_CAPABILITY); n->name=padv(p)->text; return n; }
  if(is_kw(t,"grant")){ padv(p); Node *n=node(N_GRANT); n->name=padv(p)->text; return n; }
  if(is_kw(t,"revoke")){ padv(p); Node *n=node(N_REVOKE); n->name=padv(p)->text; return n; }
  if(is_kw(t,"pay")){ padv(p); Node *n=node(N_PAY); n->a=expression(p); expect_kw(p,"from"); n->src=padv(p)->text; expect_kw(p,"to"); n->dst=padv(p)->text;
      if(is_kw(pk(p),"requires")){ padv(p); n->str=padv(p)->text; } return n; }
  if(is_kw(t,"split")){
      padv(p); Node *n=node(N_SPLIT); n->a=expression(p); expect_kw(p,"from"); n->src=padv(p)->text;
      expect(p,T_LB,"'{'");
      double total_pct=0;
      while(pk(p)->type!=T_RB && pk(p)->type!=T_EOF){
        if(pk(p)->type!=T_IDENT) fail("expected a wallet name in split on line %d",pk(p)->line);
        Node *leg=node(N_SPLIT);   /* a plain data holder (dst+num), never itself dispatched */
        leg->dst=padv(p)->text;
        if(pk(p)->type!=T_NUM) fail("expected a percentage in split on line %d",pk(p)->line);
        leg->num=padv(p)->num;
        if(!is_op(pk(p),"%")) fail("expected '%%' after the percentage on line %d",pk(p)->line);
        padv(p);
        total_pct += leg->num;
        push_kid(n,leg);
        if(pk(p)->type==T_COMMA) padv(p);
      }
      expect(p,T_RB,"'}'");
      { double diff=total_pct-100.0; if(diff<0)diff=-diff;
        if(diff>0.01) fail("split percentages must sum to exactly 100 (got %g) on line %d", total_pct, n->line); }
      return n;
  }
  if(is_kw(t,"require")){ padv(p); Node *n=node(N_REQUIRE); n->a=expression(p);
      if(pk(p)->type==T_COMMA){ padv(p); if(pk(p)->type!=T_STR) fail("expected a message string on line %d",pk(p)->line); n->str=padv(p)->text; } return n; }
  if(is_kw(t,"fn")){ return parse_fn(p, 1); }
  if(is_kw(t,"try")){ padv(p); Node *n=node(N_TRY); n->a=block(p); expect_kw(p,"catch"); n->name=padv(p)->text; n->b=block(p); return n; }
  if(is_kw(t,"throw")){ padv(p); Node *n=node(N_THROW); n->a=expression(p); return n; }
  if(is_kw(t,"import")){ padv(p); Node *n=node(N_IMPORT);
      n->a=expression(p);                 /* the path: usually a string literal, but any expression */
      if(is_kw(pk(p),"as")){ padv(p); n->name=padv(p)->text; }
      return n; }
  if(is_kw(t,"return")){ padv(p); Node *n=node(N_RETURN); if(starts_expr(pk(p))) n->a=expression(p); return n; }
  if(is_kw(t,"if")){ padv(p); Node *n=node(N_IF); n->a=expression(p); n->b=block(p);
      if(is_kw(pk(p),"else")){ padv(p); n->c = is_kw(pk(p),"if") ? statement(p) : block(p); } return n; }
  if(is_kw(t,"unless")){ padv(p);
      Node *cond=expression(p); Node *notn=node(N_UN); notn->op="not"; notn->a=cond;
      Node *n=node(N_IF); n->a=notn; n->natural="unless"; n->b=block(p);
      if(is_kw(pk(p),"else")){ padv(p); n->c = is_kw(pk(p),"if") ? statement(p) : block(p); } return n; }
  if(is_kw(t,"say")){ padv(p); Node *n=node(N_CALL); Node *nm=node(N_NAME); nm->name=xstrdup("print"); n->a=nm;
      push_kid(n, expression(p)); n->natural="say";
      Node *ex=node(N_EXPR); ex->a=n; return ex; }
  if(is_kw(t,"wait")){ padv(p); Node *n=node(N_CALL); Node *nm=node(N_NAME); nm->name=xstrdup("sleep"); n->a=nm;
      push_kid(n, expression(p)); n->natural="wait";
      Node *ex=node(N_EXPR); ex->a=n; return ex; }
  if(is_kw(t,"while")){ padv(p); Node *n=node(N_WHILE); n->a=expression(p); n->b=block(p); return n; }
  if(is_kw(t,"paywall")){ padv(p); Node *n=node(N_PAYWALL); n->name=padv(p)->text;
      if(!is_op(pk(p),"=")){ fail("expected '=' on line %d",pk(p)->line); }
      padv(p);
      n->a=unary(p);            /* the price (stops before the '/') */
      if(!is_op(pk(p),"/")){ fail("expected '/' before the period on line %d",pk(p)->line); }
      padv(p);
      n->period=padv(p)->text; expect_kw(p,"to"); n->dst=padv(p)->text; return n; }
  if(is_kw(t,"subscribe")){ padv(p); Node *n=node(N_SUBSCRIBE); n->src=padv(p)->text; expect_kw(p,"to"); n->dst=padv(p)->text;
      if(is_kw(pk(p),"requires")){ padv(p); n->str=padv(p)->text; } return n; }
  if(is_kw(t,"for")){
      padv(p); Node *n=node(N_FOR); n->name=padv(p)->text;
      if(is_kw(pk(p),"from")){
        /* "for i from A to B" - a natural counting loop, inclusive of B,
         * auto-detecting ascending/descending (can't decide direction at
         * parse time - A/B may be runtime expressions, not literals).
         * Desugars to `for i in range_to(A, B)` - see bi_range_to. */
        padv(p); Node *fromE=expression(p); expect_kw(p,"to"); Node *toE=expression(p);
        Node *call=node(N_CALL); Node *nm=node(N_NAME); nm->name=xstrdup("range_to"); call->a=nm;
        push_kid(call,fromE); push_kid(call,toE);
        n->a=call; n->natural="from_to";
      } else {
        expect_kw(p,"in"); n->a=expression(p);
      }
      n->b=block(p); return n;
  }
  if(is_kw(t,"break")){ padv(p); return node(N_BREAK); }
  if(is_kw(t,"continue")){ padv(p); return node(N_CONTINUE); }
  if(t->type==T_LB){ return block(p); }
  /* assignment (name or element) or a bare expression statement */
  {
    Node *lhs = expression(p);
    Token *o = pk(p);
    int isasg = o->type==T_OP && strcmp(o->text,"=")==0;
    int iscmp = o->type==T_OP && strlen(o->text)==2 && o->text[1]=='=' &&
                strchr("+-*/%", o->text[0]);
    if(isasg || iscmp){
      char baseop[2]; baseop[0]=o->text[0]; baseop[1]=0;
      padv(p);
      Node *rhs = expression(p);
      if(iscmp){ Node *bin=node(N_BIN); bin->op=xstrdup(baseop); bin->a=lhs; bin->b=rhs; rhs=bin; }
      Node *asg=NULL;
      if(lhs->kind==N_NAME){ asg=node(N_ASSIGN); asg->name=lhs->name; asg->a=rhs; }
      else if(lhs->kind==N_INDEX){ asg=node(N_SETINDEX); asg->a=lhs->a; asg->b=lhs->b; asg->c=rhs; }
      else fail("invalid assignment target on line %d", o->line);
      if(iscmp) asg->cop=xstrdup(baseop);
      return asg;
    }
    Node *n=node(N_EXPR); n->a=lhs; return n;
  }
}

static Node *parse_program(Token *toks){
  Parser p = { toks, 0 };
  Node *prog = node(N_BLOCK);
  int cap=0;
  while(pk(&p)->type!=T_EOF){
    if(prog->nkids==cap){ cap=cap?cap*2:16; prog->kids=realloc(prog->kids,cap*sizeof(Node*)); }
    prog->kids[prog->nkids++]=statement(&p);
  }
  return prog;
}

/* ===================== environment ===================== */
typedef struct Entry { GCObj gc; char *name; Value val; struct Entry *next; } Entry;
typedef struct Env { GCObj gc; Entry *head; struct Env *parent; } Env;
static Env *env_new(Env *parent){ Env *e=xmalloc(sizeof(Env)); e->head=NULL; e->parent=parent; gc_register(e,GC_ENV); return e; }
static Value *env_find(Env *e, const char *name){
  for(; e; e=e->parent) for(Entry *it=e->head; it; it=it->next) if(strcmp(it->name,name)==0) return &it->val;
  return NULL;
}
static void env_define(Env *e, const char *name, Value v){
  for(Entry *it=e->head; it; it=it->next) if(strcmp(it->name,name)==0){ it->val=v; return; }
  Entry *n=xmalloc(sizeof(Entry)); gc_register(n,GC_ENTRY); n->name=xstrdup(name); n->val=v; n->next=e->head; e->head=n;
}

/* ===================== interpreter ===================== */
typedef struct Txn { const char *src, *dst; long long cents; } Txn;
typedef struct Sub { const char *w, *p; } Sub;
typedef struct Interp {
  Env *globals;
  int has_gas; long long gas; long long gas_used;
  int returning; Value retval;
  int loopflow;                 /* 0 none, 1 break, 2 continue */
  int calldepth;                 /* nested Larzscript function calls in flight - see call_value() */
  int curline;                  /* line currently executing, for errors */
  char *basedir;                /* directory of the current file, for imports */
  struct ModRec { char *path; Value val; } *modcache; int nmod, modcap;
  Env **rootstack; int nroots, rootcap;   /* live scope envs (GC roots) */
  Value *temproots; int ntemp, tempcap;   /* objects held mid-build (GC roots) */
  Txn *ledger; int nled, ledcap;
  Sub *subs; int nsub, subcap;
  jmp_buf jb; char errmsg[256]; const char *errname;
} Interp;

/* -- GC roots + mark/sweep (defined here; needs Interp + the object structs) -- */
static void gc_root_push(Interp *ip, Env *e){ if(ip->nroots==ip->rootcap){ ip->rootcap=ip->rootcap?ip->rootcap*2:64; ip->rootstack=realloc(ip->rootstack,ip->rootcap*sizeof(Env*)); } ip->rootstack[ip->nroots++]=e; }
static void gc_root_pop(Interp *ip){ if(ip->nroots>0) ip->nroots--; }
static void gc_temp_push(Interp *ip, Value v){ if(ip->ntemp==ip->tempcap){ ip->tempcap=ip->tempcap?ip->tempcap*2:64; ip->temproots=realloc(ip->temproots,ip->tempcap*sizeof(Value)); } ip->temproots[ip->ntemp++]=v; }
static void gc_temp_pop(Interp *ip, int to){ ip->ntemp=to; }

#ifdef __EMSCRIPTEN__
/* Closures handed to ui.on()/ui.fetch() are invoked later, from a JS event
 * callback - by then the Larzscript call frame that registered them is long
 * gone, so they aren't reachable through the normal Env-chain/rootstack GC
 * roots. Without this they'd be collected out from under a still-registered
 * DOM listener the next time gc_collect() runs, and firing the event would
 * call into freed memory. Rooted here exactly like ip->modcache already is
 * (see gc_collect below). */
static Value *g_ui_callbacks=0; static int g_ui_ncb=0, g_ui_cbcap=0;
#endif

#if defined(__STDC_HOSTED__) && !__STDC_HOSTED__
/* Same rooting concern as the browser's g_ui_callbacks above, for the
 * kernel-native `ui` module's click handlers (see register_ui_module
 * further down). Fixed-size (bounded by GFX_MAX_WIDGETS, gfx.h) and
 * statically zero-initialized, so every slot starts as V_NIL (V_NIL==0). */
static Value g_kernel_ui_callbacks[GFX_MAX_WIDGETS];
#endif

static void gc_mark_env(Env *e);
static void gc_mark_value(Value v){
  switch(v.t){
    case V_STR: STR_HDR(v.str)->gc_marked=1; break;
    case V_LIST: { GCObj *o=(GCObj*)v.list; if(!o->gc_marked){ o->gc_marked=1; for(int i=0;i<v.list->n;i++) gc_mark_value(v.list->items[i]); } break; }
    case V_DICT: { GCObj *o=(GCObj*)v.dict; if(!o->gc_marked){ o->gc_marked=1; for(int i=0;i<v.dict->n;i++){ gc_mark_value(v.dict->items[i].key); gc_mark_value(v.dict->items[i].val); } } break; }
    case V_WALLET: ((GCObj*)v.wal)->gc_marked=1; break;
    case V_PAYWALL: ((GCObj*)v.pw)->gc_marked=1; break;
    case V_RANGE: ((GCObj*)v.rng)->gc_marked=1; break;
    case V_FUNC: { GCObj *o=(GCObj*)v.fn; if(!o->gc_marked){ o->gc_marked=1; gc_mark_env(v.fn->env); } break; }
    case V_MODULE: gc_mark_env(v.mod); break;
    default: break;
  }
}
static void gc_mark_env(Env *e){
  if(!e) return;
  GCObj *o=(GCObj*)e; if(o->gc_marked) return; o->gc_marked=1;
  for(Entry *it=e->head; it; it=it->next){ ((GCObj*)it)->gc_marked=1; gc_mark_value(it->val); }
  gc_mark_env(e->parent);
}
static void gc_collect(Interp *ip){
  for(GCObj *o=g_gc_head; o; o=o->gc_next) o->gc_marked=0;
  gc_mark_env(ip->globals);
  for(int i=0;i<ip->nroots;i++) gc_mark_env(ip->rootstack[i]);
  for(int i=0;i<ip->ntemp;i++) gc_mark_value(ip->temproots[i]);
  gc_mark_value(ip->retval);
  for(int i=0;i<ip->nmod;i++) gc_mark_value(ip->modcache[i].val);
#ifdef __EMSCRIPTEN__
  for(int i=0;i<g_ui_ncb;i++) gc_mark_value(g_ui_callbacks[i]);
#endif
#if defined(__STDC_HOSTED__) && !__STDC_HOSTED__
  for(int i=0;i<GFX_MAX_WIDGETS;i++) gc_mark_value(g_kernel_ui_callbacks[i]);
#endif
  GCObj **pp=&g_gc_head;
  while(*pp){
    GCObj *o=*pp;
    if(o->gc_marked){ pp=&o->gc_next; }
    else {
      *pp=o->gc_next;
      if(o->gc_kind==GC_LIST) free(((List*)o)->items);
      else if(o->gc_kind==GC_DICT) free(((Dict*)o)->items);
      free(o); g_gc_count--;
    }
  }
  g_gc_threshold = g_gc_count*2 + 200000;
}
static void maybe_gc(Interp *ip){ if(g_gc_count > g_gc_threshold) gc_collect(ip); }
volatile int larz_gas_kill = 0;   /* set by the host OS (LarzOS) when a command exceeds its gas budget */

static void define_builtins(Env *e);   /* forward: used by import */
#ifdef __EMSCRIPTEN__
static void register_ui_module(Env *g);   /* forward: used by define_builtins, defined after install_builtins */
#endif
#if defined(__STDC_HOSTED__) && !__STDC_HOSTED__
static void register_ui_module(Env *g);   /* forward: used by define_builtins, defined after install_builtins (kernel-native ui, VGA Mode 13h backend) */
#endif

static void runtime_error(Interp *ip, const char *name, const char *fmt, ...){
  ip->errname=name;
  char msg[224];
  va_list ap; va_start(ap,fmt); vsnprintf(msg,sizeof(msg),fmt,ap); va_end(ap);
  if(ip->curline>0) snprintf(ip->errmsg,sizeof(ip->errmsg),"%s (line %d)", msg, ip->curline);
  else snprintf(ip->errmsg,sizeof(ip->errmsg),"%s", msg);
  longjmp(ip->jb,1);
}
static void append_txn(Interp *ip, const char *s, const char *d, long long c){
  if(ip->nled==ip->ledcap){ ip->ledcap=ip->ledcap?ip->ledcap*2:16; ip->ledger=realloc(ip->ledger,ip->ledcap*sizeof(Txn)); }
  ip->ledger[ip->nled].src=s; ip->ledger[ip->nled].dst=d; ip->ledger[ip->nled].cents=c; ip->nled++;
}
static void add_sub(Interp *ip, const char *w, const char *p){
  if(ip->nsub==ip->subcap){ ip->subcap=ip->subcap?ip->subcap*2:8; ip->subs=realloc(ip->subs,ip->subcap*sizeof(Sub)); }
  ip->subs[ip->nsub].w=w; ip->subs[ip->nsub].p=p; ip->nsub++;
}
static int has_sub(Interp *ip, const char *w, const char *p){
  for(int i=0;i<ip->nsub;i++) if(strcmp(ip->subs[i].w,w)==0 && strcmp(ip->subs[i].p,p)==0) return 1;
  return 0;
}

static Value eval(Interp *ip, Node *n, Env *env);
static void exec(Interp *ip, Node *n, Env *env);
static const char *type_name(Value v);

static Value do_binop(Interp *ip, const char *op, Value a, Value b){
  int bm = a.t==V_MONEY && b.t==V_MONEY;
  int bn = is_num(a) && is_num(b);
  int bs = a.t==V_STR && b.t==V_STR;
  if(strcmp(op,"==")==0 || strcmp(op,"!=")==0){
    int eq=values_equal(a,b);
    return V_bool(strcmp(op,"==")==0 ? eq : !eq);
  }
  if(strcmp(op,"+")==0){
    if(bm) return V_money(a.cents+b.cents);
    if(bn) return V_number(a.num+b.num);
    if(bs){ size_t la=strlen(a.str), lb=strlen(b.str); char *s=xmalloc(la+lb+1); memcpy(s,a.str,la); memcpy(s+la,b.str,lb+1); return V_take(s); }
    if(a.t==V_LIST && b.t==V_LIST){ List *r=list_new(); for(int i=0;i<a.list->n;i++) list_push(r,a.list->items[i]); for(int i=0;i<b.list->n;i++) list_push(r,b.list->items[i]); return V_list(r); }
    runtime_error(ip,"LarzTypeError","cannot add those values");
  }
  if(strcmp(op,"-")==0){ if(bm) return V_money(a.cents-b.cents); if(bn) return V_number(a.num-b.num); runtime_error(ip,"LarzTypeError","cannot subtract those values"); }
  if(strcmp(op,"*")==0){
    if(a.t==V_MONEY && is_num(b)) return V_money(money_round((double)a.cents*b.num));
    if(is_num(a) && b.t==V_MONEY) return V_money(money_round((double)b.cents*a.num));
    if(bn) return V_number(a.num*b.num);
    /* string/list repetition: "ab" * 3, [0] * 4 (either order) */
    { Value s=a, k=b; if(is_num(a)&&(b.t==V_STR||b.t==V_LIST)){ s=b; k=a; }
      if(s.t==V_STR && is_num(k)){ long long m=(long long)k.num; if(m<0) m=0; size_t la=strlen(s.str); char *out=xmalloc(la*m+1); for(long long i=0;i<m;i++) memcpy(out+i*la, s.str, la); out[la*m]=0; return V_take(out); }
      if(s.t==V_LIST && is_num(k)){ long long m=(long long)k.num; if(m<0) m=0; List *r=list_new(); for(long long i=0;i<m;i++) for(int j=0;j<s.list->n;j++) list_push(r,s.list->items[j]); return V_list(r); }
    }
    runtime_error(ip,"LarzTypeError","cannot multiply those values");
  }
  if(strcmp(op,"/")==0){
    if(a.t==V_MONEY && is_num(b)){ if(b.num==0) runtime_error(ip,"MoneyError","cannot divide money by zero"); return V_money(money_round((double)a.cents/b.num)); }
    if(bn){ if(b.num==0) runtime_error(ip,"LarzRuntimeError","division by zero"); return V_number(a.num/b.num); }
    runtime_error(ip,"LarzTypeError","cannot divide those values");
  }
  if(strcmp(op,"%")==0){ if(bn){ if(b.num==0) runtime_error(ip,"LarzRuntimeError","division by zero"); return V_number((double)((long long)a.num % (long long)b.num)); } runtime_error(ip,"LarzTypeError","cannot take modulo"); }
  if(strcmp(op,"//")==0){ if(bn){ if(b.num==0) runtime_error(ip,"LarzRuntimeError","division by zero"); double q=a.num/b.num; long long f=(long long)q; if(q<0 && (double)f!=q) f--; return V_number((double)f); } runtime_error(ip,"LarzTypeError","cannot floor-divide those values"); }
  if(strcmp(op,"**")==0){ if(bn){ double e=b.num; if(e!=(long long)e) runtime_error(ip,"LarzValueError","** exponent must be a whole number"); long long ex=(long long)e; int neg=ex<0; if(neg)ex=-ex; double r=1,base=a.num; for(long long i=0;i<ex;i++) r*=base; if(neg){ if(a.num==0) runtime_error(ip,"LarzRuntimeError","0 to a negative power"); r=1/r; } return V_number(r); } runtime_error(ip,"LarzTypeError","cannot raise those values to a power"); }
  /* ordering */
  {
    double x,y; int kind; /* 0 money,1 num,2 str */
    if(bm){ x=a.cents; y=b.cents; kind=0; }
    else if(bn){ x=a.num; y=b.num; kind=1; }
    else if(bs){ int c=strcmp(a.str,b.str);
      if(strcmp(op,"<")==0) return V_bool(c<0);
      if(strcmp(op,"<=")==0) return V_bool(c<=0);
      if(strcmp(op,">")==0) return V_bool(c>0);
      return V_bool(c>=0);
    } else { runtime_error(ip,"LarzTypeError","cannot compare those values"); return V_nil(); }
    (void)kind;
    if(strcmp(op,"<")==0) return V_bool(x<y);
    if(strcmp(op,"<=")==0) return V_bool(x<=y);
    if(strcmp(op,">")==0) return V_bool(x>y);
    return V_bool(x>=y);
  }
}

static Value call_value(Interp *ip, Value callee, Value *args, int nargs);

/* ---- import resolution: relative first, then the package search path ---- *
 * Search dirs: $LARZSCRIPT_PATH (colon-separated), then ~/.larzscript/lib, then
 * ./lz_modules. A bare name like "coolmath" also matches coolmath.lz and the
 * package layout coolmath/coolmath.lz or coolmath/main.lz. */
/* $HOME is never set on plain Windows (cmd.exe/PowerShell/WMI-spawned
 * processes all lack it by default - only USERPROFILE exists there) -
 * found via a real live test: `import` of ANY package silently failed
 * to even reach a clean ImportError on a real Windows machine with no
 * $HOME set, because every "~/.larzscript/lib" search in this file used
 * bare getenv("HOME") with no fallback, unconditionally skipping that
 * whole search directory rather than falling back to where `pkg
 * install` actually put packages on Windows. Wine (used for Windows CI)
 * masked this entirely - it inherits $HOME from the Linux host
 * underneath it, so CI never ran with $HOME genuinely unset the way a
 * real Windows install always is. Same fallback order the ssh/netbridge
 * packages' own Larzscript code already uses (env("HOME",
 * env("USERPROFILE", "."))), just at the C level too. */
static const char *lz_home_dir(void){
  const char *h=getenv("HOME");
  if(h && *h) return h;
  h=getenv("USERPROFILE");
  if(h && *h) return h;
  return NULL;
}
static int _lz_isfile(const char *p){ struct stat st; return stat(p,&st)==0 && S_ISREG(st.st_mode); }
static int _lz_try(const char *cand, char *out, size_t outsz){
  if(!_lz_isfile(cand)) return 0;
  char *rp=realpath(cand,NULL); if(rp){ snprintf(out,outsz,"%s",rp); free(rp); } else snprintf(out,outsz,"%s",cand);
  return 1;
}
static int _lz_indir(const char *dir, const char *path, int has_ext, char *out, size_t outsz){
  char cand[4096];
  snprintf(cand,sizeof cand,"%s/%s", dir, path); if(_lz_try(cand,out,outsz)) return 1;
  if(!has_ext){
    snprintf(cand,sizeof cand,"%s/%s.lz", dir, path); if(_lz_try(cand,out,outsz)) return 1;
    const char *base=strrchr(path,'/'); base=base?base+1:path;
    snprintf(cand,sizeof cand,"%s/%s/%s.lz", dir, path, base); if(_lz_try(cand,out,outsz)) return 1;
    snprintf(cand,sizeof cand,"%s/%s/main.lz", dir, path); if(_lz_try(cand,out,outsz)) return 1;
  }
  return 0;
}
static int resolve_import(const char *path, const char *basedir, char *out, size_t outsz){
  int has_ext = strstr(path,".lz")!=NULL;
  if(path[0]=='/'){ if(_lz_try(path,out,outsz)) return 1; }
  else { if(_lz_indir(basedir?basedir:".", path, has_ext, out, outsz)) return 1; }
  const char *lp=getenv("LARZSCRIPT_PATH");
  if(lp && *lp){ char buf[8192]; snprintf(buf,sizeof buf,"%s",lp); char *save=NULL; for(char *d=strtok_r(buf,":",&save); d; d=strtok_r(NULL,":",&save)) if(_lz_indir(d,path,has_ext,out,outsz)) return 1; }
  const char *home=lz_home_dir();
  if(home){ char dir[4096]; snprintf(dir,sizeof dir,"%s/.larzscript/lib",home); if(_lz_indir(dir,path,has_ext,out,outsz)) return 1; }
  if(_lz_indir("lz_modules",path,has_ext,out,outsz)) return 1;
  return 0;
}

static Value method_call(Interp *ip, Value obj, const char *name, Value *args, int nargs){
      if(obj.t==V_MODULE){
        Value *fn=env_find(obj.mod, name);
        if(!fn) runtime_error(ip,"LarzNameError","module '%s' has no member '%s'", obj.modname?obj.modname:"?", name);
        return call_value(ip, *fn, args, nargs);
      }
      if(obj.t==V_WALLET){
        if(strcmp(name,"credit")==0 || strcmp(name,"debit")==0){
          if(nargs!=1 || args[0].t!=V_MONEY) runtime_error(ip,"LarzTypeError","wallet.%s expects one money argument", name);
          if(strcmp(name,"credit")==0) obj.wal->cents += args[0].cents;
          else { if(args[0].cents>obj.wal->cents) runtime_error(ip,"MoneyError","wallet '%s' has insufficient funds", obj.wal->name); obj.wal->cents -= args[0].cents; }
          return V_nil();
        }
        runtime_error(ip,"LarzTypeError","a wallet has no method '%s'", name);
      }
      if(obj.t==V_LIST){
        List *l=obj.list; const char *m=name; int na=nargs;
        if(strcmp(m,"push")==0||strcmp(m,"append")==0){ if(na!=1) runtime_error(ip,"LarzTypeError","%s expects one argument",m); list_push(l,args[0]); return V_nil(); }
        if(strcmp(m,"pop")==0){ if(l->n==0) runtime_error(ip,"LarzRuntimeError","pop from empty list"); long long i = na>=1 ? (long long)args[0].num : l->n-1; if(i<0) i+=l->n; if(i<0||i>=l->n) runtime_error(ip,"LarzRuntimeError","pop index out of range"); Value r=l->items[i]; for(int j=i+1;j<l->n;j++) l->items[j-1]=l->items[j]; l->n--; return r; }
        if(strcmp(m,"insert")==0){ if(na!=2||!is_num(args[0])) runtime_error(ip,"LarzTypeError","insert expects an index and a value"); long long i=(long long)args[0].num; if(i<0) i+=l->n; if(i<0) i=0; if(i>l->n) i=l->n; list_push(l,V_nil()); for(int j=l->n-1;j>i;j--) l->items[j]=l->items[j-1]; l->items[i]=args[1]; return V_nil(); }
        if(strcmp(m,"contains")==0){ if(na!=1) runtime_error(ip,"LarzTypeError","contains expects one argument"); for(int i=0;i<l->n;i++) if(values_equal(l->items[i],args[0])) return V_bool(1); return V_bool(0); }
        if(strcmp(m,"index")==0){ if(na!=1) runtime_error(ip,"LarzTypeError","index expects one argument"); for(int i=0;i<l->n;i++) if(values_equal(l->items[i],args[0])) return V_number(i); return V_number(-1); }
        if(strcmp(m,"sort")==0){ qsort(l->items,l->n,sizeof(Value),qsort_value_cmp); return V_nil(); }
        if(strcmp(m,"reverse")==0){ for(int i=0,j=l->n-1;i<j;i++,j--){ Value t=l->items[i]; l->items[i]=l->items[j]; l->items[j]=t; } return V_nil(); }
        if(strcmp(m,"count")==0){ if(na!=1) runtime_error(ip,"LarzTypeError","count expects one argument"); long long c=0; for(int i=0;i<l->n;i++) if(values_equal(l->items[i],args[0])) c++; return V_number((double)c); }
        if(strcmp(m,"extend")==0){ if(na!=1||args[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","extend expects a list"); for(int i=0;i<args[0].list->n;i++) list_push(l,args[0].list->items[i]); return V_nil(); }
        if(strcmp(m,"clear")==0){ l->n=0; return V_nil(); }
        runtime_error(ip,"LarzTypeError","a list has no method '%s'", m);
      }
      if(obj.t==V_DICT){
        Dict *d=obj.dict; const char *m=name; int na=nargs;
        if(strcmp(m,"keys")==0){ List *r=list_new(); for(int i=0;i<d->n;i++) list_push(r,d->items[i].key); return V_list(r); }
        if(strcmp(m,"values")==0){ List *r=list_new(); for(int i=0;i<d->n;i++) list_push(r,d->items[i].val); return V_list(r); }
        if(strcmp(m,"has")==0){ if(na!=1) runtime_error(ip,"LarzTypeError","has expects one argument"); return V_bool(dict_find(d,args[0])!=NULL); }
        if(strcmp(m,"get")==0){ if(na<1) runtime_error(ip,"LarzTypeError","get expects a key"); Value *s=dict_find(d,args[0]); if(s) return *s; return na>=2?args[1]:V_nil(); }
        if(strcmp(m,"remove")==0){ if(na!=1) runtime_error(ip,"LarzTypeError","remove expects one argument"); return V_bool(dict_del(d,args[0])); }
        runtime_error(ip,"LarzTypeError","a dict has no method '%s'", m);
      }
      if(obj.t==V_STR){
        const char *s=obj.str; const char *m=name; int na=nargs;
        if(strcmp(m,"upper")==0||strcmp(m,"lower")==0){ int up=m[0]=='u'; char *r=xstrdup(s); for(char *p=r;*p;p++) *p= up?toupper((unsigned char)*p):tolower((unsigned char)*p); return V_take(r); }
        if(strcmp(m,"strip")==0){ int a=0,b=(int)strlen(s); while(a<b&&isspace((unsigned char)s[a])) a++; while(b>a&&isspace((unsigned char)s[b-1])) b--; return mkstr_n(s+a, b-a); }
        if(strcmp(m,"contains")==0||strcmp(m,"find")==0){
          if((na!=1&&na!=2)||args[0].t!=V_STR||(na==2&&!is_num(args[1]))) runtime_error(ip,"LarzTypeError","%s expects a string and an optional start index",m);
          int slen=(int)strlen(s);
          int start = na==2 ? (int)args[1].num : 0;
          if(start<0) start+=slen;
          if(start<0) start=0;
          if(start>slen) start=slen;
          const char *f=strstr(s+start,args[0].str);
          if(m[0]=='c') return V_bool(f!=NULL);
          return V_number(f?(double)(f-s):-1);
        }
        if(strcmp(m,"starts_with")==0){ if(na!=1||args[0].t!=V_STR) runtime_error(ip,"LarzTypeError","starts_with expects a string"); size_t ln=strlen(args[0].str); return V_bool(strncmp(s,args[0].str,ln)==0); }
        if(strcmp(m,"ends_with")==0){ if(na!=1||args[0].t!=V_STR) runtime_error(ip,"LarzTypeError","ends_with expects a string"); size_t ls=strlen(s), le=strlen(args[0].str); return V_bool(ls>=le && strcmp(s+ls-le,args[0].str)==0); }
        if(strcmp(m,"replace")==0){ if(na!=2||args[0].t!=V_STR||args[1].t!=V_STR) runtime_error(ip,"LarzTypeError","replace expects two strings"); const char *from=args[0].str,*to=args[1].str; size_t lf=strlen(from); if(lf==0) return V_string(s); SB b; b.s=NULL;b.n=0;b.cap=0; const char *p=s; while(*p){ if(strncmp(p,from,lf)==0){ sb_puts(&b,to); p+=lf; } else sb_putc(&b,*p++); } sb_putc(&b,0); return V_take(b.s?b.s:xstrdup("")); }
        if(strcmp(m,"split")==0){ List *r=list_new(); if(na==0||args[0].t!=V_STR||args[0].str[0]==0){ for(const char *p=s;*p;p++){ char *c=xstrndup(p,1); list_push(r,V_take(c)); } return V_list(r); } const char *sep=args[0].str; size_t ls=strlen(sep); const char *p=s,*q; while((q=strstr(p,sep))){ list_push(r,mkstr_n(p,q-p)); p=q+ls; } list_push(r,V_string(p)); return V_list(r); }
        if(strcmp(m,"capitalize")==0){ char *r=xstrdup(s); if(r[0]){ r[0]=toupper((unsigned char)r[0]); for(char *p=r+1;*p;p++) *p=tolower((unsigned char)*p); } return V_take(r); }
        if(strcmp(m,"title")==0){ char *r=xstrdup(s); int start=1; for(char *p=r;*p;p++){ if(isspace((unsigned char)*p)){ start=1; } else { *p = start?toupper((unsigned char)*p):tolower((unsigned char)*p); start=0; } } return V_take(r); }
        if(strcmp(m,"ljust")==0||strcmp(m,"rjust")==0){ if(na<1||!is_num(args[0])) runtime_error(ip,"LarzTypeError","%s expects a width",m); int w=(int)args[0].num; char fill=(na>=2&&args[1].t==V_STR&&args[1].str[0])?args[1].str[0]:' '; int ls=(int)strlen(s); int pad=w>ls?w-ls:0; SB b; b.s=NULL;b.n=0;b.cap=0; if(m[0]=='r'){ for(int i=0;i<pad;i++) sb_putc(&b,fill); sb_puts(&b,s); } else { sb_puts(&b,s); for(int i=0;i<pad;i++) sb_putc(&b,fill); } sb_putc(&b,0); return V_take(b.s?b.s:xstrdup("")); }
        runtime_error(ip,"LarzTypeError","a string has no method '%s'", m);
      }
      runtime_error(ip,"LarzTypeError","cannot call a method on that value");
      return V_nil();
}

static Value eval(Interp *ip, Node *n, Env *env){
  if(n->line) ip->curline=n->line;
  switch(n->kind){
    case N_NUM: return V_number(n->num);
    case N_MONEY: return V_money(n->cents);
    case N_STR: return V_string(n->str);
    case N_BOOL: return V_bool(n->boolean);
    case N_NIL: return V_nil();
    case N_NAME: { Value *v=env_find(env,n->name); if(!v) runtime_error(ip,"LarzNameError","'%s' is not defined", n->name); return *v; }
    case N_UN: {
      Value v=eval(ip,n->a,env);
      if(strcmp(n->op,"not")==0) return V_bool(!truthy(v));
      if(v.t==V_MONEY) return V_money(-v.cents);
      if(is_num(v)) return V_number(-v.num);
      runtime_error(ip,"LarzTypeError","cannot negate that value");
    }
    case N_BIN: {
      if(strcmp(n->op,"and")==0){ Value l=eval(ip,n->a,env); return truthy(l)?eval(ip,n->b,env):l; }
      if(strcmp(n->op,"or")==0){ Value l=eval(ip,n->a,env); return truthy(l)?l:eval(ip,n->b,env); }
      if(strcmp(n->op,"has")==0){
        Value w=eval(ip,n->a,env), pw=eval(ip,n->b,env);
        if(w.t!=V_WALLET || pw.t!=V_PAYWALL) runtime_error(ip,"LarzTypeError","'has' needs a wallet and a paywall");
        return V_bool(has_sub(ip, w.wal->name, pw.pw->name));
      }
      if(strcmp(n->op,"in")==0){
        Value a=eval(ip,n->a,env), b=eval(ip,n->b,env);
        if(b.t==V_LIST){ for(int i=0;i<b.list->n;i++) if(values_equal(b.list->items[i],a)) return V_bool(1); return V_bool(0); }
        if(b.t==V_DICT) return V_bool(dict_find(b.dict,a)!=NULL);
        if(b.t==V_STR && a.t==V_STR) return V_bool(strstr(b.str,a.str)!=NULL);
        if(b.t==V_RANGE){ if(!is_num(a)) return V_bool(0); long long x=(long long)a.num; if((double)x!=a.num) return V_bool(0); Range *rr=b.rng; if(rr->step>0){ if(x<rr->start||x>=rr->stop) return V_bool(0); } else { if(x>rr->start||x<=rr->stop) return V_bool(0); } return V_bool((x-rr->start)%rr->step==0); }
        runtime_error(ip,"LarzTypeError","'in' needs a list, dict, string or range on the right");
      }
      { int tr=ip->ntemp; Value l=eval(ip,n->a,env); gc_temp_push(ip,l);
        Value r=eval(ip,n->b,env); gc_temp_pop(ip,tr);
        return do_binop(ip,n->op,l,r); }
    }
    case N_ARRAY: {
      List *l=list_new(); int tr=ip->ntemp; gc_temp_push(ip, V_list(l));  /* protect while building */
      for(int i=0;i<n->nkids;i++) list_push(l, eval(ip,n->kids[i],env));
      gc_temp_pop(ip, tr);
      return V_list(l);
    }
    case N_DICT: {
      Dict *d=dict_new(); int tr=ip->ntemp; gc_temp_push(ip, V_dict(d));
      for(int i=0;i+1<n->nkids;i+=2){
        Value k=eval(ip,n->kids[i],env), v=eval(ip,n->kids[i+1],env);
        dict_set(d, k, v);
      }
      gc_temp_pop(ip, tr);
      return V_dict(d);
    }
    case N_FN: { Closure *c=xmalloc(sizeof(Closure)); gc_register(c,GC_CLOSURE); c->decl=n; c->env=env; c->name=n->name; return V_func(c); }
    case N_FSTR: return eval(ip, n->a, env);
    case N_LISTCOMP: {
      List *r=list_new(); int tr=ip->ntemp; gc_temp_push(ip,V_list(r));
      Value it=eval(ip,n->b,env); gc_temp_push(ip,it);
      long long len; Value *arr=NULL; Dict *dd=NULL; const char *sp=NULL; Range *rg=NULL;
      if(it.t==V_LIST){ len=it.list->n; arr=it.list->items; }
      else if(it.t==V_DICT){ len=it.dict->n; dd=it.dict; }
      else if(it.t==V_STR){ len=(long long)strlen(it.str); sp=it.str; }
      else if(it.t==V_RANGE){ len=range_len(it.rng); rg=it.rng; }
      else { gc_temp_pop(ip,tr); runtime_error(ip,"LarzTypeError","cannot iterate that value"); return V_nil(); }
      for(long long i=0;i<len;i++){
        Value item = rg ? V_number((double)range_at(rg,i)) : arr?arr[i] : dd?dd->items[i].key : mkstr_n(sp+i,1);
        Env *child=env_new(env); env_define(child,n->name,item); gc_root_push(ip,child);
        if(!n->c || truthy(eval(ip,n->c,child))) list_push(r, eval(ip,n->a,child));
        gc_root_pop(ip);
      }
      gc_temp_pop(ip,tr);
      return V_list(r);
    }
    case N_DICTCOMP: {
      Dict *r=dict_new(); int tr=ip->ntemp; gc_temp_push(ip,V_dict(r));
      Value it=eval(ip,n->c,env); gc_temp_push(ip,it);
      Node *cond = n->nkids>0 ? n->kids[0] : NULL;
      long long len; Value *arr=NULL; Dict *dd=NULL; const char *sp=NULL; Range *rg=NULL;
      if(it.t==V_LIST){ len=it.list->n; arr=it.list->items; }
      else if(it.t==V_DICT){ len=it.dict->n; dd=it.dict; }
      else if(it.t==V_STR){ len=(long long)strlen(it.str); sp=it.str; }
      else if(it.t==V_RANGE){ len=range_len(it.rng); rg=it.rng; }
      else { gc_temp_pop(ip,tr); runtime_error(ip,"LarzTypeError","cannot iterate that value"); return V_nil(); }
      for(long long i=0;i<len;i++){
        Value item = rg ? V_number((double)range_at(rg,i)) : arr?arr[i] : dd?dd->items[i].key : mkstr_n(sp+i,1);
        Env *child=env_new(env); env_define(child,n->name,item); gc_root_push(ip,child);
        if(!cond || truthy(eval(ip,cond,child))){ Value k=eval(ip,n->a,child); Value v=eval(ip,n->b,child); dict_set(r,k,v); }
        gc_root_pop(ip);
      }
      gc_temp_pop(ip,tr);
      return V_dict(r);
    }
    case N_TERNARY: return truthy(eval(ip,n->a,env)) ? eval(ip,n->b,env) : eval(ip,n->c,env);
    case N_SLICE: {
      int tr=ip->ntemp;
      Value obj=eval(ip,n->a,env); if(obj.t==V_RANGE) obj=range_to_list(obj.rng); gc_temp_push(ip,obj);
      int len;
      if(obj.t==V_LIST) len=obj.list->n;
      else if(obj.t==V_STR) len=(int)strlen(obj.str);
      else { gc_temp_pop(ip,tr); runtime_error(ip,"LarzTypeError","cannot slice that value"); return V_nil(); }
      long long start=0, end=len;
      if(n->b){ Value s=eval(ip,n->b,env); if(!is_num(s)) runtime_error(ip,"LarzTypeError","slice bounds must be numbers"); start=(long long)s.num; }
      if(n->c){ Value e=eval(ip,n->c,env); if(!is_num(e)) runtime_error(ip,"LarzTypeError","slice bounds must be numbers"); end=(long long)e.num; }
      gc_temp_pop(ip,tr);
      if(start<0) start+=len;
      if(end<0) end+=len;
      if(start<0) start=0;
      if(start>len) start=len;
      if(end<0) end=0;
      if(end>len) end=len;
      if(end<start) end=start;
      if(obj.t==V_LIST){ List *r=list_new(); for(long long i=start;i<end;i++) list_push(r,obj.list->items[i]); return V_list(r); }
      return mkstr_n(obj.str+start, end-start);
    }
    case N_INDEX: {
      int tr=ip->ntemp;
      Value obj=eval(ip,n->a,env); gc_temp_push(ip,obj);
      Value iv=eval(ip,n->b,env); gc_temp_pop(ip,tr);
      if(obj.t==V_DICT){
        Value *slot=dict_find(obj.dict, iv);
        if(!slot) runtime_error(ip,"LarzKeyError","key not found");
        return *slot;
      }
      if(!is_num(iv) || iv.num!=(long long)iv.num) runtime_error(ip,"LarzTypeError","index must be a whole number");
      long long idx=(long long)iv.num;
      if(obj.t==V_LIST){ if(idx<0) idx+=obj.list->n; if(idx<0||idx>=obj.list->n) runtime_error(ip,"LarzRuntimeError","index %lld out of range (length %d)", (long long)iv.num, obj.list->n); return obj.list->items[idx]; }
      if(obj.t==V_STR){ int len=(int)strlen(obj.str); if(idx<0) idx+=len; if(idx<0||idx>=len) runtime_error(ip,"LarzRuntimeError","index out of range"); char *s=xmalloc(2); s[0]=obj.str[idx]; s[1]=0; return V_take(s); }
      if(obj.t==V_RANGE){ long long ln=range_len(obj.rng); if(idx<0) idx+=ln; if(idx<0||idx>=ln) runtime_error(ip,"LarzRuntimeError","index %lld out of range (length %lld)", (long long)iv.num, ln); return V_number((double)range_at(obj.rng, idx)); }
      runtime_error(ip,"LarzTypeError","cannot index that value");
    }
    case N_CALL: {
      if(n->nkids>64) runtime_error(ip,"LarzTypeError","too many arguments");
      int tr=ip->ntemp;
      Value callee=eval(ip,n->a,env); gc_temp_push(ip,callee);
      Value args[64];
      for(int i=0;i<n->nkids;i++){ args[i]=eval(ip,n->kids[i],env); gc_temp_push(ip,args[i]); }
      Value r=call_value(ip, callee, args, n->nkids);
      gc_temp_pop(ip,tr);
      return r;
    }
    case N_GET: {
      Value obj=eval(ip,n->a,env);
      if(obj.t==V_WALLET){
        if(strcmp(n->name,"balance")==0) return V_money(obj.wal->cents);
        if(strcmp(n->name,"name")==0) return V_string(obj.wal->name);
        runtime_error(ip,"LarzTypeError","a wallet has no property '%s'", n->name);
      }
      if(obj.t==V_MODULE){
        Value *v=env_find(obj.mod, n->name);
        if(!v) runtime_error(ip,"LarzNameError","module '%s' has no member '%s'", obj.modname?obj.modname:"?", n->name);
        return *v;
      }
      runtime_error(ip,"LarzTypeError","cannot read '%s'", n->name);
    }
    case N_METHOD: {
      if(n->nkids>64) runtime_error(ip,"LarzTypeError","too many arguments");
      int tr=ip->ntemp;
      Value obj=eval(ip,n->a,env); gc_temp_push(ip,obj);
      Value args[64];
      for(int i=0;i<n->nkids;i++){ args[i]=eval(ip,n->kids[i],env); gc_temp_push(ip,args[i]); }
      Value r=method_call(ip, obj, n->name, args, n->nkids);
      gc_temp_pop(ip,tr);
      return r;
    }
    default: runtime_error(ip,"LarzRuntimeError","cannot evaluate that node"); return V_nil();
  }
}

/* Every nested Larzscript call is a real native C recursion (eval() ->
 * call_value() -> exec() -> eval() ...), and this interpreter's own stack
 * frames are large (exec()'s switch alone reserves several KB for its
 * biggest case) - empirically measured on the kernel build at ~50 KiB of
 * *native* stack per nested Larzscript call (see kernel/README.md), not a
 * few hundred bytes as a naive estimate would suggest. On a hosted OS a
 * runaway recursion just SEGVs that one process; on the freestanding
 * kernel, task stacks sit contiguously in one identity-mapped, unguarded
 * region (kernel/libk.c) with no guard pages, so overflowing one task's
 * stack silently corrupts a NEIGHBORING task's memory instead of faulting
 * cleanly - a real, reproduced failure mode, not a hypothetical one. Fail
 * safe on every platform with a graceful, catchable error well before any
 * stack actually runs out, the same way CPython raises RecursionError
 * rather than letting the process SIGSEGV. 150 is far beyond any legitimate
 * LarzOS script's recursion depth (boot.lz's factorial demo is 7 deep). */
#define MAX_CALL_DEPTH 150
static Value call_value(Interp *ip, Value callee, Value *args, int nargs){
  if(callee.t==V_BUILTIN) return callee.bi->fn(ip,args,nargs);
  if(callee.t==V_FUNC){
    Node *decl=callee.fn->decl;
    const char *fname = decl->name ? decl->name : "function";
    if(ip->calldepth>=MAX_CALL_DEPTH) runtime_error(ip,"LarzRecursionError","maximum call depth (%d) exceeded calling '%s' - check for infinite/runaway recursion", MAX_CALL_DEPTH, fname);
    if(nargs>decl->nparams) runtime_error(ip,"LarzTypeError","%s expects at most %d argument(s), got %d", fname, decl->nparams, nargs);
    if(decl->has_gas && decl->gas){
      ip->gas_used += decl->gas;
      if(ip->has_gas){ if(decl->gas>ip->gas) runtime_error(ip,"OutOfGasError","out of gas calling '%s'", fname); ip->gas -= decl->gas; }
    }
    Env *call_env=env_new(callee.fn->env);
    for(int i=0;i<decl->nparams;i++){
      if(i<nargs) env_define(call_env, decl->params[i], args[i]);
      else if(decl->pdefs && decl->pdefs[i]) env_define(call_env, decl->params[i], eval(ip, decl->pdefs[i], call_env));
      else runtime_error(ip,"LarzTypeError","%s missing argument '%s'", fname, decl->params[i]);
    }
    ip->returning=0;
    ip->calldepth++;
    gc_root_push(ip, call_env);
    for(int i=0;i<decl->b->nkids;i++){ exec(ip, decl->b->kids[i], call_env); if(ip->returning) break; }
    gc_root_pop(ip);
    ip->calldepth--;
    Value r = ip->returning ? ip->retval : V_nil();
    ip->returning=0;
    return r;
  }
  runtime_error(ip,"LarzTypeError","that value is not callable");
  return V_nil();
}

/* `pay ... requires NAME` / `subscribe ... requires NAME`: capname is NULL
 * when no requires clause was written (the common case - no check at all).
 * When present, money must not move unless NAME is a granted capability -
 * checked before anything else in N_PAY/N_SUBSCRIBE, so a declined capability
 * never leaves a partial transfer behind. */
static void check_capability(Interp *ip, Env *env, const char *capname){
  if(!capname) return;
  Value *cv=env_find(env,capname);
  if(!cv||cv->t!=V_CAPABILITY) runtime_error(ip,"LarzTypeError","'%s' is not a capability", capname);
  if(!cv->b) runtime_error(ip,"CapabilityError","capability '%s' is not granted", capname);
}

static void exec(Interp *ip, Node *n, Env *env){
  maybe_gc(ip);                 /* safe point: between statements, nothing half-built is unrooted */
  if(larz_gas_kill){ larz_gas_kill=0; runtime_error(ip,"GasError","command exceeded its compute gas budget"); }
  if(n->line) ip->curline=n->line;
  switch(n->kind){
    case N_LET: env_define(env, n->name, eval(ip,n->a,env)); return;
    case N_ASSIGN: { Value *slot=env_find(env,n->name); if(!slot) runtime_error(ip,"LarzNameError","cannot assign to undefined '%s' (use 'let')", n->name); *slot=eval(ip,n->a,env); return; }
    case N_PRICE: { Value v=eval(ip,n->a,env); if(v.t!=V_MONEY) runtime_error(ip,"LarzTypeError","a price must be money"); env_define(env,n->name,v); return; }
    case N_WALLET: {
      long long c=0;
      if(n->a){ Value v=eval(ip,n->a,env); if(v.t!=V_MONEY) runtime_error(ip,"LarzTypeError","a wallet balance must be money"); c=v.cents; }
      Wallet *w=xmalloc(sizeof(Wallet)); gc_register(w,GC_WALLET); w->name=xstrdup(n->name); w->cents=c;
      env_define(env,n->name, V_wallet(w)); return;
    }
    case N_CAPABILITY: { env_define(env,n->name, V_capability(0)); return; }   /* starts revoked */
    case N_GRANT: case N_REVOKE: {
      Value *cv=env_find(env,n->name);
      if(!cv||cv->t!=V_CAPABILITY) runtime_error(ip,"LarzTypeError","'%s' is not a capability", n->name);
      cv->b = (n->kind==N_GRANT);
      return;
    }
    case N_PAY: {
      check_capability(ip,env,n->str);
      Value amt=eval(ip,n->a,env);
      if(amt.t!=V_MONEY) runtime_error(ip,"LarzTypeError","you can only pay money");
      Value *sv=env_find(env,n->src), *dv=env_find(env,n->dst);
      if(!sv||!dv) runtime_error(ip,"LarzNameError","pay needs two wallets");
      if(sv->t!=V_WALLET||dv->t!=V_WALLET) runtime_error(ip,"LarzTypeError","pay requires two wallets");
      if(amt.cents>sv->wal->cents) runtime_error(ip,"MoneyError","wallet '%s' has insufficient funds", sv->wal->name);
      sv->wal->cents -= amt.cents; dv->wal->cents += amt.cents;
      append_txn(ip, n->src, n->dst, amt.cents);
      return;
    }
    case N_SPLIT: {
      Value amt=eval(ip,n->a,env);
      if(amt.t!=V_MONEY) runtime_error(ip,"LarzTypeError","you can only split money");
      Value *sv=env_find(env,n->src);
      if(!sv||sv->t!=V_WALLET) runtime_error(ip,"LarzTypeError","split requires a source wallet");
      if(amt.cents>sv->wal->cents) runtime_error(ip,"MoneyError","wallet '%s' has insufficient funds", sv->wal->name);
      long long remaining=amt.cents;
      for(int i=0;i<n->nkids;i++){
        Node *leg=n->kids[i];
        Value *dv=env_find(env,leg->dst);
        if(!dv||dv->t!=V_WALLET) runtime_error(ip,"LarzTypeError","split recipient '%s' is not a wallet", leg->dst);
        /* the last recipient gets the exact remainder, so the split always
         * sums to the original amount with no cent lost or duplicated to
         * rounding - the standard split-rounding convention. */
        long long cut = (i==n->nkids-1) ? remaining : money_round((double)amt.cents*leg->num/100.0);
        remaining -= cut;
        sv->wal->cents -= cut; dv->wal->cents += cut;
        append_txn(ip, n->src, leg->dst, cut);
      }
      return;
    }
    case N_PAYWALL: {
      Value v=eval(ip,n->a,env);
      if(v.t!=V_MONEY) runtime_error(ip,"LarzTypeError","a paywall price must be money");
      Paywall *pw=xmalloc(sizeof(Paywall)); gc_register(pw,GC_PAYWALL);
      pw->name=xstrdup(n->name); pw->price=v.cents; pw->period=xstrdup(n->period); pw->payee=xstrdup(n->dst);
      env_define(env, n->name, V_paywall(pw));
      return;
    }
    case N_SUBSCRIBE: {
      check_capability(ip,env,n->str);
      Value *wv=env_find(env,n->src), *pv=env_find(env,n->dst);
      if(!wv||wv->t!=V_WALLET) runtime_error(ip,"LarzTypeError","can only subscribe a wallet");
      if(!pv||pv->t!=V_PAYWALL) runtime_error(ip,"LarzTypeError","can only subscribe to a paywall");
      Paywall *pw=pv->pw;
      Value *payee=env_find(env, pw->payee);
      if(!payee||payee->t!=V_WALLET) runtime_error(ip,"LarzTypeError","paywall payee '%s' is not a wallet", pw->payee);
      if(pw->price>wv->wal->cents) runtime_error(ip,"MoneyError","wallet '%s' has insufficient funds", wv->wal->name);
      wv->wal->cents -= pw->price; payee->wal->cents += pw->price;
      append_txn(ip, n->src, pw->payee, pw->price);
      add_sub(ip, wv->wal->name, pw->name);
      return;
    }
    case N_REQUIRE: { if(!truthy(eval(ip,n->a,env))) runtime_error(ip,"RequireError","%s", n->str?n->str:"requirement not met"); return; }
    case N_FN: { Closure *c=xmalloc(sizeof(Closure)); gc_register(c,GC_CLOSURE); c->decl=n; c->env=env; c->name=n->name; env_define(env,n->name,V_func(c)); return; }
    case N_RETURN: { ip->retval = n->a ? eval(ip,n->a,env) : V_nil(); ip->returning=1; return; }
    case N_IF: {
      if(truthy(eval(ip,n->a,env))) exec(ip,n->b,env);
      else if(n->c) exec(ip,n->c,env);
      return;
    }
    case N_WHILE: {
      while(truthy(eval(ip,n->a,env))){
        exec(ip,n->b,env);
        if(ip->returning) break;
        if(ip->loopflow==1){ ip->loopflow=0; break; }
        if(ip->loopflow==2){ ip->loopflow=0; continue; }
      }
      return;
    }
    case N_FOR: {
      Value it=eval(ip,n->a,env);
      int fortr=ip->ntemp; gc_temp_push(ip,it);      /* protect the iterator for the whole loop */
      long long len; Value *arr=NULL; Dict *d=NULL; const char *sp=NULL; Range *rg=NULL;
      if(it.t==V_LIST){ len=it.list->n; arr=it.list->items; }
      else if(it.t==V_DICT){ len=it.dict->n; d=it.dict; }
      else if(it.t==V_STR){ len=(long long)strlen(it.str); sp=it.str; }
      else if(it.t==V_RANGE){ len=range_len(it.rng); rg=it.rng; }
      else { gc_temp_pop(ip,fortr); runtime_error(ip,"LarzTypeError","cannot iterate that value"); return; }
      for(long long i=0;i<len;i++){
        Value item = rg ? V_number((double)range_at(rg,i)) : arr ? arr[i] : d ? d->items[i].key : mkstr_n(sp+i,1);
        Env *child=env_new(env); env_define(child,n->name,item);
        gc_root_push(ip,child);
        exec(ip,n->b,child);
        gc_root_pop(ip);
        if(ip->returning) break;
        if(ip->loopflow==1){ ip->loopflow=0; break; }
        if(ip->loopflow==2){ ip->loopflow=0; continue; }
      }
      gc_temp_pop(ip,fortr);
      return;
    }
    case N_SETINDEX: {
      int tr=ip->ntemp;
      Value obj=eval(ip,n->a,env); gc_temp_push(ip,obj);
      Value key=eval(ip,n->b,env); gc_temp_push(ip,key);
      Value val=eval(ip,n->c,env);
      gc_temp_pop(ip,tr);
      if(obj.t==V_LIST){
        if(!is_num(key)||key.num!=(long long)key.num) runtime_error(ip,"LarzTypeError","list index must be a whole number");
        long long i=(long long)key.num; if(i<0) i+=obj.list->n;
        if(i<0||i>=obj.list->n) runtime_error(ip,"LarzRuntimeError","index out of range");
        obj.list->items[i]=val; return;
      }
      if(obj.t==V_DICT){ dict_set(obj.dict,key,val); return; }
      runtime_error(ip,"LarzTypeError","cannot assign to an index of that value");
    }
    case N_BREAK: ip->loopflow=1; return;
    case N_CONTINUE: ip->loopflow=2; return;
    case N_TRY: {
      jmp_buf saved; memcpy(saved, ip->jb, sizeof(jmp_buf));
      int nr=ip->nroots, nt=ip->ntemp, ncd=ip->calldepth;  /* restore GC roots + call depth if the try unwinds */
      if(setjmp(ip->jb)==0){
        exec(ip, n->a, env);
        memcpy(ip->jb, saved, sizeof(jmp_buf));       /* normal exit: restore outer */
      } else {
        memcpy(ip->jb, saved, sizeof(jmp_buf));       /* error: restore outer first */
        ip->nroots=nr; ip->ntemp=nt; ip->calldepth=ncd;
        ip->returning=0; ip->loopflow=0;
        Dict *d=dict_new();
        dict_set(d, V_string("type"),    V_string(ip->errname?ip->errname:"Error"));
        dict_set(d, V_string("message"), V_string(ip->errmsg));
        Env *child=env_new(env); env_define(child, n->name, V_dict(d));
        gc_root_push(ip,child);
        exec(ip, n->b, child);
        gc_root_pop(ip);
      }
      return;
    }
    case N_THROW: {
      Value v=eval(ip,n->a,env);
      const char *ty="Error"; char *msg;
      if(v.t==V_STR){ msg=v.str; }
      else if(v.t==V_DICT){
        Value kt; memset(&kt,0,sizeof kt); kt.t=V_STR; kt.str=(char*)"type";
        Value km; memset(&km,0,sizeof km); km.t=V_STR; km.str=(char*)"message";
        Value *t2=dict_find(v.dict,kt); Value *m2=dict_find(v.dict,km);
        if(t2&&t2->t==V_STR) ty=t2->str;
        msg = (m2&&m2->t==V_STR)? m2->str : str_of(v);
      } else { msg=str_of(v); }
      runtime_error(ip, ty, "%s", msg);
      return;
    }
    case N_IMPORT: {
      Value pv=eval(ip,n->a,env);
      if(pv.t!=V_STR) runtime_error(ip,"ImportError","import path must be a string, got %s", type_name(pv));
      char path[4096]; snprintf(path,sizeof path,"%s",pv.str);
      char resolved[4096];
      if(!resolve_import(path, ip->basedir, resolved, sizeof resolved))
        runtime_error(ip,"ImportError","cannot find module '%s' (searched relative, $LARZSCRIPT_PATH, ~/.larzscript/lib, ./lz_modules)", path);
      const char *alias=n->name; char abuf[4096];
      if(!alias){ const char *base=strrchr(path,'/'); base=base?base+1:path; snprintf(abuf,sizeof abuf,"%s",base); char *dot=strrchr(abuf,'.'); if(dot)*dot=0; alias=abuf; }
      for(int i=0;i<ip->nmod;i++) if(strcmp(ip->modcache[i].path,resolved)==0){ env_define(env, alias, ip->modcache[i].val); return; }
      FILE *f=fopen(resolved,"rb"); if(!f) runtime_error(ip,"ImportError","cannot import '%s'", path);
      size_t cap=1<<16,len=0; char *src=xmalloc(cap); size_t r;
      while((r=fread(src+len,1,cap-len,f))>0){ len+=r; if(len==cap){ cap*=2; src=realloc(src,cap); } }
      src[len]=0; fclose(f);
      Token *toks=lex(src); Node *prog=parse_program(toks);
      Env *modenv=env_new(NULL); define_builtins(modenv);
      char *saved_base=ip->basedir;
      char moddir[4096]; snprintf(moddir,sizeof moddir,"%s",resolved);
      { char *sl=strrchr(moddir,'/'); if(sl)*sl=0; else snprintf(moddir,sizeof moddir,"."); }
      ip->basedir=xstrdup(moddir);
      int saved_ret=ip->returning; ip->returning=0;
      gc_root_push(ip,modenv);
      for(int i=0;i<prog->nkids;i++){ exec(ip,prog->kids[i],modenv); if(ip->returning) break; }
      gc_root_pop(ip);
      ip->returning=saved_ret; ip->basedir=saved_base;
      Value mod=V_module(modenv, xstrdup(alias));
      if(ip->nmod==ip->modcap){ ip->modcap=ip->modcap?ip->modcap*2:8; ip->modcache=realloc(ip->modcache,ip->modcap*sizeof(*ip->modcache)); }
      ip->modcache[ip->nmod].path=xstrdup(resolved); ip->modcache[ip->nmod].val=mod; ip->nmod++;
      env_define(env, alias, mod);
      return;
    }
    case N_BLOCK: { Env *child=env_new(env); gc_root_push(ip,child); for(int i=0;i<n->nkids;i++){ exec(ip,n->kids[i],child); if(ip->returning||ip->loopflow) break; } gc_root_pop(ip); return; }
    case N_EXPR: eval(ip,n->a,env); return;
    default: eval(ip,n,env); return;
  }
}

/* ===================== builtins ===================== */
static Value bi_print(Interp *ip, Value *args, int n){
  (void)ip;
  for(int i=0;i<n;i++){ if(i) printf(" "); print_value(args[i]); }
  printf("\n");
  return V_nil();
}
static Value bi_money(Interp *ip, Value *args, int n){
  if(n!=1 || !is_num(args[0])) runtime_error(ip,"LarzTypeError","money() expects one number");
  return V_money(money_round(args[0].num*100));
}
static Value bi_len(Interp *ip, Value *args, int n){
  if(n!=1) runtime_error(ip,"LarzTypeError","len() expects one argument");
  if(args[0].t==V_STR) return V_number((double)strlen(args[0].str));
  if(args[0].t==V_LIST) return V_number((double)args[0].list->n);
  if(args[0].t==V_DICT) return V_number((double)args[0].dict->n);
  if(args[0].t==V_RANGE) return V_number((double)range_len(args[0].rng));
  runtime_error(ip,"LarzTypeError","len() expects a string, list, dict or range");
  return V_nil();
}
static Value bi_push(Interp *ip, Value *args, int n){
  if(n!=2 || args[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","push() expects a list and an item");
  list_push(args[0].list, args[1]);
  return V_nil();
}
static Value bi_range(Interp *ip, Value *args, int n){
  /* range(n) or range(start, stop) or range(start, stop, step) */
  if(n<1||n>3) runtime_error(ip,"LarzTypeError","range() expects 1 to 3 numbers");
  for(int i=0;i<n;i++) if(!is_num(args[i])) runtime_error(ip,"LarzTypeError","range() expects numbers");
  long long start=0, stop, step=1;
  if(n==1){ stop=(long long)args[0].num; }
  else { start=(long long)args[0].num; stop=(long long)args[1].num; if(n==3) step=(long long)args[2].num; }
  if(step==0) runtime_error(ip,"LarzRuntimeError","range() step cannot be zero");
  return V_range(start, stop, step);
}
/* range_to(from, to): what `for i from A to B { }` desugars to - INCLUSIVE
 * of `to` (unlike range()'s exclusive stop - more natural for "from 1 to
 * 10"), auto-detecting ascending vs. descending from from/to themselves.
 * No step parameter - for custom stepping use range() directly. */
static Value bi_range_to(Interp *ip, Value *args, int n){
  if(n!=2) runtime_error(ip,"LarzTypeError","range_to() expects (from, to)");
  if(!is_num(args[0])||!is_num(args[1])) runtime_error(ip,"LarzTypeError","range_to() expects numbers");
  long long from=(long long)args[0].num, to=(long long)args[1].num;
  return from<=to ? V_range(from, to+1, 1) : V_range(from, to-1, -1);
}
static const char *type_name(Value v){
  switch(v.t){ case V_NIL:return "nil"; case V_BOOL:return "bool"; case V_NUM:return "number";
    case V_MONEY:return "money"; case V_STR:return "string"; case V_WALLET:return "wallet";
    case V_FUNC:return "function"; case V_BUILTIN:return "function"; case V_LIST:return "list";
    case V_DICT:return "dict"; case V_PAYWALL:return "paywall"; case V_MODULE:return "module"; case V_RANGE:return "range"; case V_CAPABILITY:return "capability"; default:return "value"; }
}
static Value bi_str(Interp *ip, Value *a, int n){ if(n!=1) runtime_error(ip,"LarzTypeError","str() expects one argument"); return V_take(str_of(a[0])); }
static Value bi_type(Interp *ip, Value *a, int n){ if(n!=1) runtime_error(ip,"LarzTypeError","type() expects one argument"); return V_string(type_name(a[0])); }
static Value bi_int(Interp *ip, Value *a, int n){
  if(n!=1) runtime_error(ip,"LarzTypeError","int() expects one argument");
  if(a[0].t==V_NUM) return V_number((double)(long long)a[0].num);
  if(a[0].t==V_BOOL) return V_number(a[0].b?1:0);
  if(a[0].t==V_STR){ char *e; double d=strtod(a[0].str,&e); if(e==a[0].str) runtime_error(ip,"LarzValueError","int(): not a number: '%s'", a[0].str); return V_number((double)(long long)d); }
  runtime_error(ip,"LarzTypeError","int() expects a number, bool or string"); return V_nil();
}
static Value bi_float(Interp *ip, Value *a, int n){
  if(n!=1) runtime_error(ip,"LarzTypeError","float() expects one argument");
  if(a[0].t==V_NUM) return a[0];
  if(a[0].t==V_BOOL) return V_number(a[0].b?1:0);
  if(a[0].t==V_STR){ char *e; double d=strtod(a[0].str,&e); if(e==a[0].str) runtime_error(ip,"LarzValueError","float(): not a number: '%s'", a[0].str); return V_number(d); }
  runtime_error(ip,"LarzTypeError","float() expects a number, bool or string"); return V_nil();
}
static Value bi_bool(Interp *ip, Value *a, int n){ if(n!=1) runtime_error(ip,"LarzTypeError","bool() expects one argument"); return V_bool(truthy(a[0])); }
static Value bi_abs(Interp *ip, Value *a, int n){
  if(n!=1) runtime_error(ip,"LarzTypeError","abs() expects one argument");
  if(a[0].t==V_NUM) return V_number(a[0].num<0?-a[0].num:a[0].num);
  if(a[0].t==V_MONEY) return V_money(a[0].cents<0?-a[0].cents:a[0].cents);
  runtime_error(ip,"LarzTypeError","abs() expects a number or money"); return V_nil();
}
static Value _minmax(Interp *ip, Value *a, int n, int want_max){ if(n==1) a[0]=derange(a[0]);
  Value *items; int count;
  if(n==1 && a[0].t==V_LIST){ items=a[0].list->items; count=a[0].list->n; }
  else { items=a; count=n; }
  if(count==0) runtime_error(ip,"LarzValueError","%s() of empty sequence", want_max?"max":"min");
  Value best=items[0];
  for(int i=1;i<count;i++){ int c=value_compare(items[i],best); if(want_max?c>0:c<0) best=items[i]; }
  return best;
}
static Value bi_min(Interp *ip, Value *a, int n){ return _minmax(ip,a,n,0); }
static Value bi_max(Interp *ip, Value *a, int n){ return _minmax(ip,a,n,1); }
static Value bi_sum(Interp *ip, Value *a, int n){ if(n>=1) a[0]=derange(a[0]);
  if(n!=1 || a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","sum() expects a list");
  List *l=a[0].list; if(l->n==0) return V_number(0);
  if(l->items[0].t==V_MONEY){ long long c=0; for(int i=0;i<l->n;i++){ if(l->items[i].t!=V_MONEY) runtime_error(ip,"LarzTypeError","sum(): mixed types"); c+=l->items[i].cents; } return V_money(c); }
  double s=0; for(int i=0;i<l->n;i++){ if(!is_num(l->items[i])) runtime_error(ip,"LarzTypeError","sum(): expects numbers or money"); s+=l->items[i].num; } return V_number(s);
}
static Value bi_sorted(Interp *ip, Value *a, int n){ if(n>=1) a[0]=derange(a[0]);
  if(n!=1 || a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","sorted() expects a list");
  List *r=list_new(); for(int i=0;i<a[0].list->n;i++) list_push(r, a[0].list->items[i]);
  qsort(r->items, r->n, sizeof(Value), qsort_value_cmp);
  return V_list(r);
}
static Value bi_reversed(Interp *ip, Value *a, int n){ if(n>=1) a[0]=derange(a[0]);
  if(n!=1 || a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","reversed() expects a list");
  List *r=list_new(); for(int i=a[0].list->n-1;i>=0;i--) list_push(r, a[0].list->items[i]);
  return V_list(r);
}
static Value bi_floor(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","floor() expects a number"); double x=a[0].num; long long d=(long long)x; if(x<0 && (double)d!=x) d--; return V_number((double)d); }
static Value bi_ceil(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ceil() expects a number"); double x=a[0].num; long long d=(long long)x; if(x>0 && (double)d!=x) d++; return V_number((double)d); }
static Value bi_round(Interp *ip, Value *a, int n){
  if(n<1||n>2||!is_num(a[0])) runtime_error(ip,"LarzTypeError","round() expects a number and optional digit count");
  double x=a[0].num;
  if(n==2){
    if(!is_num(a[1])) runtime_error(ip,"LarzTypeError","round(): digits must be a number");
    int d=(int)a[1].num, ad=d<0?-d:d; double m=1; for(int i=0;i<ad;i++) m*=10; if(d<0) m=1/m;
    double y=x*m; y = y>=0? (double)(long long)(y+0.5) : (double)(long long)(y-0.5);
    return V_number(y/m);
  }
  return V_number((double)(long long)(x>=0?x+0.5:x-0.5));
}
static Value bi_sqrt(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","sqrt() expects a number"); double x=a[0].num; if(x<0) runtime_error(ip,"LarzValueError","sqrt() of a negative number"); if(x==0) return V_number(0); double g=x>1?x:1; for(int i=0;i<60;i++) g=0.5*(g+x/g); return V_number(g); }
static Value bi_pow(Interp *ip, Value *a, int n){ if(n!=2||!is_num(a[0])||!is_num(a[1])) runtime_error(ip,"LarzTypeError","pow() expects two numbers"); double b=a[0].num, e=a[1].num; if(e!=(long long)e) runtime_error(ip,"LarzValueError","pow(): exponent must be a whole number"); long long ex=(long long)e; double r=1, base=b; int neg=ex<0; if(neg) ex=-ex; for(long long i=0;i<ex;i++) r*=base; if(neg){ if(b==0) runtime_error(ip,"LarzRuntimeError","0 to a negative power"); r=1/r; } return V_number(r); }
/* ---- dsp buffer accelerators -----------------------------------------
 * The `dsp` stack package (packages/dsp) documents and calls a
 * `_native_*` counterpart for each of its whole-buffer operations
 * (biquad_process_buffer, compressor_process_buffer, limit,
 * peak_dbfs/rms_dbfs, normalize_to) - the package's own comments describe
 * exactly this contract (same filter/compressor dict field names,
 * mutated in place; same LUT indexing formula). Those native functions
 * never actually existed in this interpreter, so every one of those
 * package functions has always thrown LarzNameError at runtime - a real,
 * silent bug in the published package (found while investigating why a
 * multi-minute Larzscript audio render was too slow to run per-sample
 * through the interpreter; see larzscript-beatstudio's PLAN.md). This
 * implements the exact already-documented contract, not a new one -
 * fixes that bug for every user of `dsp`, and gives buffer-at-a-time
 * biquad/compressor/limiter math a real C inner loop instead of one
 * interpreted function call per sample, which is what a multi-minute
 * render actually needs to finish in a reasonable time. */
static double dget(Dict *d, const char *key){ Value *v=dict_find(d,V_string(key)); return v?v->num:0.0; }
static void dset(Dict *d, const char *key, double val){ dict_set(d,V_string(key),V_number(val)); }
static void need_num_list(Interp *ip, Value v, const char *who){ if(v.t!=V_LIST) runtime_error(ip,"LarzTypeError","%s expects a list of numbers",who); for(int i=0;i<v.list->n;i++) if(!is_num(v.list->items[i])) runtime_error(ip,"LarzTypeError","%s expects a list of numbers",who); }

static Value bi_native_biquad_process_buffer(Interp *ip, Value *a, int n){
  if(n!=2||a[0].t!=V_DICT) runtime_error(ip,"LarzTypeError","_native_biquad_process_buffer() expects a filter dict and a buffer");
  need_num_list(ip,a[1],"_native_biquad_process_buffer()");
  Dict *f=a[0].dict; List *buf=a[1].list;
  double b0=dget(f,"b0"), b1=dget(f,"b1"), b2=dget(f,"b2"), a1=dget(f,"a1"), a2=dget(f,"a2");
  double x1=dget(f,"x1"), x2=dget(f,"x2"), y1=dget(f,"y1"), y2=dget(f,"y2");
  List *out=list_new();
  for(int i=0;i<buf->n;i++){
    double x=buf->items[i].num;
    double y=b0*x+b1*x1+b2*x2-a1*y1-a2*y2;
    x2=x1; x1=x; y2=y1; y1=y;
    list_push(out,V_number(y));
  }
  dset(f,"x1",x1); dset(f,"x2",x2); dset(f,"y1",y1); dset(f,"y2",y2);
  return V_list(out);
}

static Value bi_native_compressor_process_buffer(Interp *ip, Value *a, int n){
  if(n!=4||a[0].t!=V_DICT||a[2].t!=V_NUM||a[3].t!=V_NUM) runtime_error(ip,"LarzTypeError","_native_compressor_process_buffer() expects a compressor dict, a buffer, lut_size, lut_max_linear");
  need_num_list(ip,a[1],"_native_compressor_process_buffer()");
  Dict *c=a[0].dict; List *buf=a[1].list;
  int lut_size=(int)a[2].num; double lut_max=a[3].num;
  Value *lutv=dict_find(c,V_string("lut"));
  if(!lutv||lutv->t!=V_LIST) runtime_error(ip,"LarzTypeError","compressor dict has no 'lut' list");
  List *lut=lutv->list;
  double attack=dget(c,"attack"), release=dget(c,"release"), env=dget(c,"env");
  List *out=list_new();
  for(int i=0;i<buf->n;i++){
    double x=buf->items[i].num;
    double level=x<0?-x:x;
    double coeff=level>env?attack:release;
    env=env*coeff+level*(1-coeff);
    int idx=(int)((env/lut_max)*lut_size);
    if(idx<0) idx=0; if(idx>=lut_size) idx=lut_size-1;
    double gain=(idx<lut->n)?lut->items[idx].num:1.0;
    list_push(out,V_number(x*gain));
  }
  dset(c,"env",env);
  return V_list(out);
}

static Value bi_native_limit_buffer(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[1])) runtime_error(ip,"LarzTypeError","_native_limit_buffer() expects a buffer and a ceiling");
  need_num_list(ip,a[0],"_native_limit_buffer()");
  List *buf=a[0].list; double ceiling=a[1].num;
  List *out=list_new();
  for(int i=0;i<buf->n;i++){
    double x=buf->items[i].num;
    if(x>ceiling) x=ceiling; else if(x<-ceiling) x=-ceiling;
    list_push(out,V_number(x));
  }
  return V_list(out);
}

static Value bi_native_peak_abs(Interp *ip, Value *a, int n){
  if(n!=1) runtime_error(ip,"LarzTypeError","_native_peak_abs() expects a buffer");
  need_num_list(ip,a[0],"_native_peak_abs()");
  List *buf=a[0].list; double peak=0.0;
  for(int i=0;i<buf->n;i++){ double x=buf->items[i].num; if(x<0) x=-x; if(x>peak) peak=x; }
  return V_number(peak);
}

static Value bi_native_sum_sq(Interp *ip, Value *a, int n){
  if(n!=1) runtime_error(ip,"LarzTypeError","_native_sum_sq() expects a buffer");
  need_num_list(ip,a[0],"_native_sum_sq()");
  List *buf=a[0].list; double sum=0.0;
  for(int i=0;i<buf->n;i++){ double x=buf->items[i].num; sum+=x*x; }
  return V_number(sum);
}

/* Mixer accumulate: dst_l[i] += src[i]*lg; dst_r[i] += src[i]*rg, for a
 * whole chunk in one C loop. The remaining interpreted per-sample cost
 * once EQ/compressor/limiter were natively accelerated (see
 * _native_master_block above) - mix_chunk()'s own gain/pan summation
 * loop, measured taking a 5-minute preview/master render from seconds to
 * well over a minute on its own. Mutates dst_l/dst_r in place, matching
 * every other in-place native buffer op in this file. */
static Value bi_native_mix_add(Interp *ip, Value *a, int n){
  if(n!=5) runtime_error(ip,"LarzTypeError","_native_mix_add() expects dst_l, dst_r, src, lg, rg");
  need_num_list(ip,a[0],"_native_mix_add()");
  need_num_list(ip,a[1],"_native_mix_add()");
  need_num_list(ip,a[2],"_native_mix_add()");
  if(!is_num(a[3])||!is_num(a[4])) runtime_error(ip,"LarzTypeError","_native_mix_add(): lg/rg must be numbers");
  List *dl=a[0].list, *dr=a[1].list, *s=a[2].list;
  if(dl->n!=s->n||dr->n!=s->n) runtime_error(ip,"LarzValueError","_native_mix_add(): dst_l/dst_r/src must be the same length");
  double lg=a[3].num, rg=a[4].num;
  for(int i=0;i<s->n;i++){
    double v=s->items[i].num;
    dl->items[i]=V_number(dl->items[i].num + v*lg);
    dr->items[i]=V_number(dr->items[i].num + v*rg);
  }
  return V_nil();
}

/* Linear gain ramp across a whole buffer - out[i] = buf[i] * lerp(start_gain,
 * end_gain, i/(n-1)) - the fade-in/fade-out primitive for the recording
 * workspace's per-clip editing (larzscript-beatstudio PLAN2.md Phase B). */
static Value bi_native_fade_buffer(Interp *ip, Value *a, int n){
  if(n!=3||!is_num(a[1])||!is_num(a[2])) runtime_error(ip,"LarzTypeError","_native_fade_buffer() expects a buffer, start_gain, end_gain");
  need_num_list(ip,a[0],"_native_fade_buffer()");
  List *buf=a[0].list;
  double g0=a[1].num, g1=a[2].num;
  int len=buf->n;
  List *out=list_new();
  for(int i=0;i<len;i++){
    double frac = len>1 ? (double)i/(double)(len-1) : 0.0;
    double g = g0 + (g1-g0)*frac;
    list_push(out,V_number(buf->items[i].num*g));
  }
  return V_list(out);
}

/* Index of the first sample whose |value| crosses threshold, or len(buf)
 * if none do - auto-trim-leading-silence's detector. */
static Value bi_native_find_first_above(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[1])) runtime_error(ip,"LarzTypeError","_native_find_first_above() expects a buffer and a threshold");
  need_num_list(ip,a[0],"_native_find_first_above()");
  List *buf=a[0].list; double th=a[1].num;
  for(int i=0;i<buf->n;i++){
    double x=buf->items[i].num; if(x<0) x=-x;
    if(x>=th) return V_number((double)i);
  }
  return V_number((double)buf->n);
}

/* Sidechain gain: the envelope/LUT gain is driven by `detector`'s level,
 * then applied to `signal` - out[i] = signal[i] * gain(detector[i]).
 * detector and signal must be the same length but are otherwise
 * independent buffers (a bandpass-filtered copy driving gain reduction
 * applied to the dry signal for a de-esser; one track's level driving
 * gain reduction applied to another track for sidechain ducking - see
 * dsp's sidechain_process_buffer()). Same lut/attack/release/env
 * envelope-follower as _native_compressor_process_buffer, just decoupled
 * from which buffer it reads level from vs. which it gains. */
static Value bi_native_sidechain_process_buffer(Interp *ip, Value *a, int n){
  if(n!=5||a[0].t!=V_DICT||!is_num(a[3])||!is_num(a[4])) runtime_error(ip,"LarzTypeError","_native_sidechain_process_buffer() expects a compressor dict, detector buffer, signal buffer, lut_size, lut_max_linear");
  need_num_list(ip,a[1],"_native_sidechain_process_buffer()");
  need_num_list(ip,a[2],"_native_sidechain_process_buffer()");
  Dict *c=a[0].dict; List *detector=a[1].list, *signal=a[2].list;
  if(detector->n!=signal->n) runtime_error(ip,"LarzValueError","_native_sidechain_process_buffer(): detector and signal must be the same length");
  int lut_size=(int)a[3].num; double lut_max=a[4].num;
  Value *lutv=dict_find(c,V_string("lut"));
  if(!lutv||lutv->t!=V_LIST) runtime_error(ip,"LarzTypeError","compressor dict has no 'lut' list");
  List *lut=lutv->list;
  double attack=dget(c,"attack"), release=dget(c,"release"), env=dget(c,"env");
  List *out=list_new();
  for(int i=0;i<detector->n;i++){
    double dx=detector->items[i].num;
    double level=dx<0?-dx:dx;
    double coeff=level>env?attack:release;
    env=env*coeff+level*(1-coeff);
    int idx=(int)((env/lut_max)*lut_size);
    if(idx<0) idx=0; if(idx>=lut_size) idx=lut_size-1;
    double gain=lut->items[idx].num;
    list_push(out,V_number(signal->items[i].num*gain));
  }
  dset(c,"env",env);
  return V_list(out);
}

static Value bi_native_scale_buffer(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[1])) runtime_error(ip,"LarzTypeError","_native_scale_buffer() expects a buffer and a gain");
  need_num_list(ip,a[0],"_native_scale_buffer()");
  List *buf=a[0].list; double g=a[1].num;
  List *out=list_new();
  for(int i=0;i<buf->n;i++) list_push(out,V_number(buf->items[i].num*g));
  return V_list(out);
}

/* larzscript-beatstudio PLAN3.md Phase F (mix/master polish): three small
 * per-chunk buffer ops, native for the same reason every other op on this
 * page is - a whole multi-minute render running these per-sample in
 * interpreted Larzscript would cost real seconds-to-minutes (see
 * _native_master_block's own comment for the measured 50x this class of
 * fusion buys). None of the three need any new interpreter machinery -
 * same is_num/need_num_list/list_new/dget/dset toolkit every builtin
 * above already uses.
 *
 * native_exp/native_tanh: this file links no libm anywhere (bi_sqrt/
 * bi_pow above are hand-rolled too, Newton's method and a whole-number-
 * exponent loop respectively) - saturation needs tanh, so it gets the
 * same treatment rather than becoming the first libm dependency: a
 * small range-reduced exp() series (the same shape as the `dsp` package's
 * own _exp() at the Larzscript level, just here in C for this one native
 * use) feeding the standard tanh(x) = 1 - 2/(exp(2x)+1) identity. */
static double native_exp(double y){
  long long whole=(long long)y;
  if(y<0 && (double)whole!=y) whole -= 1;
  double frac=y-(double)whole;
  double base=1.0;
  long long k = whole<0 ? -whole : whole;
  for(long long i=0;i<k;i++) base = whole<0 ? base/2.718281828459045 : base*2.718281828459045;
  double term=1.0, total=1.0;
  for(int i=1;i<30;i++){ term = term*frac/i; total += term; }
  return base*total;
}
static double native_tanh(double x){
  if(x>20.0) return 1.0;
  if(x<-20.0) return -1.0;
  double e2x = native_exp(2.0*x);
  return (e2x-1.0)/(e2x+1.0);
}

/* Harmonic saturation: out[i] = tanh(drive*x)/tanh(drive) - the standard
 * soft-clip/waveshaper for analog-style warmth. Normalizing by tanh(drive)
 * keeps a full-scale (+/-1.0) input mapping back to roughly full scale
 * out, so `drive` controls how much of the curve's bend gets used, not
 * the buffer's overall level. */
static Value bi_native_saturate_buffer(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[1])) runtime_error(ip,"LarzTypeError","_native_saturate_buffer() expects a buffer and a drive amount");
  need_num_list(ip,a[0],"_native_saturate_buffer()");
  List *buf=a[0].list; double drive=a[1].num;
  if(drive<=0.0) runtime_error(ip,"LarzValueError","_native_saturate_buffer(): drive must be > 0");
  double norm=native_tanh(drive);
  List *out=list_new();
  for(int i=0;i<buf->n;i++) list_push(out, V_number(native_tanh(drive*buf->items[i].num)/norm));
  return V_list(out);
}

/* Stereo widening: mid-side processing - m=(l+r)/2, s=(l-r)/2*width,
 * recombine as (m+s, m-s). width=1.0 is unchanged; >1 widens the stereo
 * image, <1 narrows it (0 = fully mono). Returns {l, r} as a dict rather
 * than mutating in place, matching how mix_chunk() on the Larzscript side
 * already produces a fresh {l, r} pair each chunk. */
static Value bi_native_stereo_widen(Interp *ip, Value *a, int n){
  if(n!=3||!is_num(a[2])) runtime_error(ip,"LarzTypeError","_native_stereo_widen() expects l, r, width");
  need_num_list(ip,a[0],"_native_stereo_widen()");
  need_num_list(ip,a[1],"_native_stereo_widen()");
  List *l=a[0].list, *r=a[1].list;
  if(l->n!=r->n) runtime_error(ip,"LarzValueError","_native_stereo_widen(): l and r must be the same length");
  double width=a[2].num;
  List *ol=list_new(), *orr=list_new();
  for(int i=0;i<l->n;i++){
    double lv=l->items[i].num, rv=r->items[i].num;
    double m=(lv+rv)*0.5, s=(lv-rv)*0.5*width;
    list_push(ol, V_number(m+s));
    list_push(orr, V_number(m-s));
  }
  Dict *d=dict_new();
  dict_set(d, V_string("l"), V_list(ol));
  dict_set(d, V_string("r"), V_list(orr));
  return V_dict(d);
}

/* Feedback delay line ("echo"): a real circular buffer carried in the
 * caller's state dict (state["line"], a fixed-size list pre-filled with
 * zeros; state["pos"]/state["feedback"]/state["mix"] are plain scalars),
 * the exact same persistent-state-dict shape every filter/compressor
 * above already uses (x1/x2/y1/y2, lut/attack/release/env) - so it
 * carries correctly across chunk boundaries the same way they do.
 * out[i] = x[i] + delayed*mix; line[pos] = x[i] + delayed*feedback -
 * a single tap, real feedback echo (not a placeholder/no-op if feedback
 * is 0 - that's just a one-tap slapback delay, still correct). */
static Value bi_native_delay_process_buffer(Interp *ip, Value *a, int n){
  if(n!=2||a[0].t!=V_DICT) runtime_error(ip,"LarzTypeError","_native_delay_process_buffer() expects a delay-line dict and a buffer");
  need_num_list(ip,a[1],"_native_delay_process_buffer()");
  Dict *st=a[0].dict; List *buf=a[1].list;
  Value *linev=dict_find(st,V_string("line"));
  if(!linev||linev->t!=V_LIST) runtime_error(ip,"LarzTypeError","delay-line dict has no 'line' list - build it with dsp.delay_new()");
  List *line=linev->list;
  int size=line->n;
  int pos=(int)dget(st,"pos");
  if(size>0){ pos = pos % size; if(pos<0) pos += size; }
  double feedback=dget(st,"feedback"), mix=dget(st,"mix");
  List *out=list_new();
  for(int i=0;i<buf->n;i++){
    double x=buf->items[i].num;
    double delayed = size>0 ? line->items[pos].num : 0.0;
    list_push(out, V_number(x + delayed*mix));
    if(size>0){ line->items[pos]=V_number(x + delayed*feedback); pos=(pos+1)%size; }
  }
  dset(st,"pos",(double)pos);
  return V_list(out);
}

/* larzscript-beatstudio PLAN4.md Phase J (real pitch correction /
 * "autotune") - two native builtins, no FFT anywhere (this interpreter's
 * dsp package has none - see this project's own repeated disclosure of
 * that gap): a time-domain autocorrelation pitch detector, and a
 * time-domain PSOLA-style resynthesis that shifts pitch WITHOUT changing
 * duration. Both are the standard techniques real (non-FFT) pitch
 * correction tools actually use, not a simplification invented for this
 * project. native_sqrt mirrors bi_sqrt's own Newton's-method loop (this
 * file links no libm anywhere - bi_sqrt/bi_pow are hand-rolled for the
 * same reason, see native_exp/native_tanh above) rather than becoming a
 * second place a libm dependency could sneak in. */
static double native_sqrt(double x){
  if(x<=0.0) return 0.0;
  double g = x>1.0 ? x : 1.0;
  for(int i=0;i<60;i++) g = 0.5*(g + x/g);
  return g;
}

/* Per-frame normalized autocorrelation pitch detector. For each frame
 * (frame_size samples, hop_size apart), finds the lag in
 * [sample_rate/max_freq, sample_rate/min_freq] with the highest
 * normalized cross-correlation between x[pos..pos+m) and
 * x[pos+lag..pos+lag+m) (m = frame_size-lag, the standard shrinking-
 * window formulation - avoids needing samples past the frame). A voiced
 * gate (correlation strength above a real, empirically reasonable
 * threshold - 0.35, not tuned per-input) marks frames where that
 * detected period is trustworthy vs. silence/noise/unvoiced consonants,
 * where chasing a "pitch" would be chasing noise. Returns a list of
 * {pos, period, voiced, corr} dicts, one per frame. */
static Value bi_native_detect_pitch_track(Interp *ip, Value *a, int n){
  if(n!=6) runtime_error(ip,"LarzTypeError","_native_detect_pitch_track() expects buf, sample_rate, min_freq, max_freq, frame_size, hop_size");
  need_num_list(ip,a[0],"_native_detect_pitch_track()");
  List *buf=a[0].list;
  double sample_rate=a[1].num, min_freq=a[2].num, max_freq=a[3].num;
  int frame_size=(int)a[4].num, hop_size=(int)a[5].num;
  if(frame_size<8||hop_size<1) runtime_error(ip,"LarzValueError","_native_detect_pitch_track(): frame_size/hop_size too small");
  int min_lag=(int)(sample_rate/max_freq), max_lag=(int)(sample_rate/min_freq);
  if(min_lag<1) min_lag=1;
  if(max_lag>=frame_size) max_lag=frame_size-1;
  List *out=list_new();
  int nlags=max_lag-min_lag+1;
  double *corr=xmalloc(sizeof(double)*(size_t)(nlags>0?nlags:1));
  int pos=0;
  while(pos+frame_size<=buf->n){
    double best_r=-1.0; int best_lag=min_lag;
    for(int lag=min_lag; lag<=max_lag; lag++){
      int m=frame_size-lag;
      double r=0.0;
      if(m>0){
        double num=0.0,e1=0.0,e2=0.0;
        for(int i=0;i<m;i++){
          double x1=buf->items[pos+i].num, x2=buf->items[pos+lag+i].num;
          num+=x1*x2; e1+=x1*x1; e2+=x2*x2;
        }
        double denom=native_sqrt(e1*e2);
        r = denom>1e-9 ? num/denom : 0.0;
      }
      corr[lag-min_lag]=r;
      if(r>best_r){ best_r=r; best_lag=lag; }
    }
    /* Prefer the SHORTEST *genuine local peak* within 8% of the global
     * max, instead of the raw global-max lag - the standard fix for a
     * well-documented autocorrelation pitch-detector failure mode
     * (octave-down/subharmonic errors: a signal's 2nd or 3rd harmonic
     * period often correlates almost as strongly as the true
     * fundamental). A pitch-shifted PSOLA test signal genuinely hit
     * this - a clean 220Hz tone reported back ~82Hz (near a 3rd
     * subharmonic) after a shift.
     *
     * "Local peak" (corr[lag] >= corr[lag-1] AND corr[lag] >=
     * corr[lag+1]) matters, not just "any lag above the threshold" - a
     * first version of this fix used the latter and broke a real,
     * different case: a PURE sine's autocorrelation is a single broad
     * lobe (no genuine secondary peak at all, since a bare sinusoid has
     * no harmonics to alias against), and its smooth shoulder climbs
     * through the 92%-of-peak threshold well before reaching the true
     * peak - "any point above threshold" grabbed a spurious point
     * partway up that shoulder (measured: a 220Hz sine came back as
     * 234.57Hz). Requiring a genuine local maximum correctly skips the
     * monotonic shoulder of the SAME peak while still catching a real,
     * separate secondary peak at a harmonically-related shorter lag. */
    for(int lag=min_lag+1; lag<best_lag; lag++){
      double c=corr[lag-min_lag];
      if(c<best_r*0.92) continue;
      double prev=corr[lag-1-min_lag], next=corr[lag+1-min_lag];
      if(c>=prev && c>=next){ best_lag=lag; best_r=c; break; }
    }
    Dict *d=dict_new();
    dict_set(d,V_string("pos"),V_number((double)pos));
    dict_set(d,V_string("period"),V_number((double)best_lag));
    dict_set(d,V_string("voiced"),V_bool(best_r>0.35));
    dict_set(d,V_string("corr"),V_number(best_r));
    list_push(out,V_dict(d));
    pos+=hop_size;
  }
  free(corr);
  return V_list(out);
}

/* PSOLA-style pitch shift, duration-preserving by construction.
 *
 * THREE earlier versions of this function were wrong, each caught by
 * actually re-detecting pitch on the OUTPUT rather than trusting the
 * construction:
 * (1) placing grains at the coarse analysis-hop spacing left large
 *     silent gaps wherever period < hop_size (true for any real vocal
 *     pitch), so most of the signal fell back to unmodified passthrough.
 * (2) fixing that by walking pitch-synchronously but changing pitch via
 *     RESAMPLING each grain's content while keeping the grain landing
 *     position (= read position) unchanged - still didn't work, because
 *     summing a signal with itself at the SAME position is provably
 *     just that signal again (acc[idx]/wsum[idx] reduces to exactly
 *     buf[idx] when every contributing grain reads from - and writes
 *     to - the identical idx). Re-detected pitch was bit-for-bit the
 *     original both times, not approximately close.
 *
 * The actually-correct mechanism (real TD-PSOLA) needs READ position
 * (where a grain's content comes FROM in the analysis signal) and WRITE
 * position (where it lands in the synthesized output) to be genuinely
 * DIFFERENT - that decoupling is the entire mechanism. Two independent
 * walks: `t_analysis` advances by the local `period` each step (reading
 * through the source material at its own natural rate); `t_synth`
 * advances by `period/ratio` each step (denser for pitch up, sparser for
 * pitch down) and is bounded to end at N, which is what fixes output
 * duration to the input's regardless of ratio. Because the two walks
 * advance at different rates, `t_analysis` runs past the end of the
 * buffer before `t_synth` does whenever ratio>1 (pitch up needs MORE
 * total grains than there are natural periods in the source - clamped
 * to keep reusing the last available grain, a standard, reasonable
 * PSOLA fallback, not a bug) and stops short of the end whenever
 * ratio<1 (pitch down naturally uses fewer, skipping some source
 * material near the tail). */
static Value bi_native_psola_shift(Interp *ip, Value *a, int n){
  if(n!=4) runtime_error(ip,"LarzTypeError","_native_psola_shift() expects buf, frame_periods, frame_ratios, hop_size");
  need_num_list(ip,a[0],"_native_psola_shift()");
  need_num_list(ip,a[1],"_native_psola_shift()");
  need_num_list(ip,a[2],"_native_psola_shift()");
  if(!is_num(a[3])) runtime_error(ip,"LarzTypeError","_native_psola_shift(): hop_size must be a number");
  List *buf=a[0].list, *fperiods=a[1].list, *fratios=a[2].list;
  if(fperiods->n!=fratios->n) runtime_error(ip,"LarzValueError","_native_psola_shift(): frame_periods/frame_ratios must be the same length");
  int hop_size=(int)a[3].num;
  if(hop_size<1) runtime_error(ip,"LarzValueError","_native_psola_shift(): hop_size must be >= 1");
  int N=buf->n;
  int nframes=fperiods->n;
  double *acc=xmalloc(sizeof(double)*(size_t)N);
  double *wsum=xmalloc(sizeof(double)*(size_t)N);
  for(int i=0;i<N;i++){ acc[i]=0.0; wsum[i]=0.0; }
  double t_synth=0.0, t_analysis=0.0;
  int guard=0, max_guard=N*4+64; /* real safety net, not expected to trigger: both advances are clamped >0.5 samples below, so this only guards a future edit that could reintroduce a zero/negative-advance loop */
  while(t_synth<(double)N && guard<max_guard){
    guard++;
    int a_center=(int)(t_analysis+0.5);
    if(a_center>=N) a_center=N-1; /* reuse the last available grain rather than stop early - see header comment */
    if(a_center<0) a_center=0;
    int fi = nframes>0 ? a_center/hop_size : 0;
    if(fi>=nframes) fi=nframes-1;
    if(fi<0) fi=0;
    int period = nframes>0 ? (int)fperiods->items[fi].num : 0;
    double ratio = nframes>0 ? fratios->items[fi].num : 1.0;
    if(period<2) period=2;
    if(ratio<=0.0) ratio=1.0;
    int glen=2*period;
    int a_start=a_center-period;
    int s_center=(int)(t_synth+0.5);
    int s_start=s_center-period;
    for(int i=0;i<glen;i++){
      int ridx=a_start+i, widx=s_start+i;
      if(ridx<0||ridx>=N||widx<0||widx>=N) continue;
      double s=buf->items[ridx].num;
      double tri = glen>1 ? 2.0*i/(glen-1) - 1.0 : 0.0;
      double w = 1.0 - (tri<0.0 ? -tri : tri);
      acc[widx]+=s*w;
      wsum[widx]+=w;
    }
    double synth_advance = period/ratio;
    if(synth_advance<0.5) synth_advance=0.5;
    double analysis_advance = (double)period;
    if(analysis_advance<0.5) analysis_advance=0.5;
    t_synth += synth_advance;
    t_analysis += analysis_advance;
  }
  List *result=list_new();
  for(int i=0;i<N;i++){
    double v = wsum[i]>1e-6 ? acc[i]/wsum[i] : buf->items[i].num;
    list_push(result, V_number(v));
  }
  free(acc); free(wsum);
  return V_list(result);
}

/* Fused linked-stereo master chain block: EQ (3 biquads/channel, already
 * native above) -> linked compressor (one gain per frame from
 * max(|L|,|R|), applied to both channels so the stereo image doesn't
 * wobble) -> brickwall limiter, all in one C pass over one chunk.
 * Mutates l_buf/r_buf IN PLACE (same "no extra full-length copies"
 * discipline master_chain's own comment already establishes) and updates
 * every filter's + the compressor's state dicts so the next chunk picks
 * up exactly where this one left off - this is what makes chunked
 * streaming and this fused fast path compose correctly together. */
static Value bi_native_master_block(Interp *ip, Value *a, int n){
  /* args: low_l, mid_l, high_l, low_r, mid_r, high_r, comp, l_buf, r_buf, ceiling_linear */
  if(n!=10) runtime_error(ip,"LarzTypeError","_native_master_block() expects low_l, mid_l, high_l, low_r, mid_r, high_r, comp, l_buf, r_buf, ceiling_linear");
  for(int i=0;i<7;i++) if(a[i].t!=V_DICT) runtime_error(ip,"LarzTypeError","_native_master_block(): filter/compressor arguments must be dicts");
  need_num_list(ip,a[7],"_native_master_block()");
  need_num_list(ip,a[8],"_native_master_block()");
  if(!is_num(a[9])) runtime_error(ip,"LarzTypeError","_native_master_block(): ceiling must be a number");
  List *l=a[7].list, *r=a[8].list;
  if(l->n!=r->n) runtime_error(ip,"LarzValueError","_native_master_block(): l_buf and r_buf must be the same length");
  double ceiling=a[9].num;
  Dict *fl[3]={a[0].dict,a[1].dict,a[2].dict}, *fr[3]={a[3].dict,a[4].dict,a[5].dict};
  double b0[3],b1[3],b2[3],fa1[3],fa2[3],x1l[3],x2l[3],y1l[3],y2l[3],x1r[3],x2r[3],y1r[3],y2r[3];
  for(int k=0;k<3;k++){
    b0[k]=dget(fl[k],"b0"); b1[k]=dget(fl[k],"b1"); b2[k]=dget(fl[k],"b2"); fa1[k]=dget(fl[k],"a1"); fa2[k]=dget(fl[k],"a2");
    x1l[k]=dget(fl[k],"x1"); x2l[k]=dget(fl[k],"x2"); y1l[k]=dget(fl[k],"y1"); y2l[k]=dget(fl[k],"y2");
    x1r[k]=dget(fr[k],"x1"); x2r[k]=dget(fr[k],"x2"); y1r[k]=dget(fr[k],"y1"); y2r[k]=dget(fr[k],"y2");
  }
  Dict *c=a[6].dict;
  Value *lutv=dict_find(c,V_string("lut"));
  if(!lutv||lutv->t!=V_LIST) runtime_error(ip,"LarzTypeError","compressor dict has no 'lut' list");
  List *lut=lutv->list;
  double attack=dget(c,"attack"), release=dget(c,"release"), env=dget(c,"env");
  int lut_size=lut->n; double lut_max=4.0;   /* must match dsp's _LUT_MAX_LINEAR */
  for(int i=0;i<l->n;i++){
    double xl=l->items[i].num, xr=r->items[i].num;
    for(int k=0;k<3;k++){
      double yl=b0[k]*xl+b1[k]*x1l[k]+b2[k]*x2l[k]-fa1[k]*y1l[k]-fa2[k]*y2l[k];
      x2l[k]=x1l[k]; x1l[k]=xl; y2l[k]=y1l[k]; y1l[k]=yl; xl=yl;
      double yr=b0[k]*xr+b1[k]*x1r[k]+b2[k]*x2r[k]-fa1[k]*y1r[k]-fa2[k]*y2r[k];
      x2r[k]=x1r[k]; x1r[k]=xr; y2r[k]=y1r[k]; y1r[k]=yr; xr=yr;
    }
    double al=xl<0?-xl:xl, ar=xr<0?-xr:xr;
    double level=al>ar?al:ar;
    double coeff=level>env?attack:release;
    env=env*coeff+level*(1-coeff);
    int idx=(int)((env/lut_max)*lut_size);
    if(idx<0) idx=0; if(idx>=lut_size) idx=lut_size-1;
    double gain=lut->items[idx].num;
    xl*=gain; xr*=gain;
    if(xl>ceiling) xl=ceiling; else if(xl<-ceiling) xl=-ceiling;
    if(xr>ceiling) xr=ceiling; else if(xr<-ceiling) xr=-ceiling;
    l->items[i]=V_number(xl); r->items[i]=V_number(xr);
  }
  dset(c,"env",env);
  for(int k=0;k<3;k++){
    dset(fl[k],"x1",x1l[k]); dset(fl[k],"x2",x2l[k]); dset(fl[k],"y1",y1l[k]); dset(fl[k],"y2",y2l[k]);
    dset(fr[k],"x1",x1r[k]); dset(fr[k],"x2",x2r[k]); dset(fr[k],"y1",y1r[k]); dset(fr[k],"y2",y2r[k]);
  }
  return V_nil();
}

static Value bi_chr(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","chr() expects a number"); char *s=xmalloc(2); s[0]=(char)(int)a[0].num; s[1]=0; return V_take(s); }
static Value bi_ord(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_STR||a[0].str[0]==0) runtime_error(ip,"LarzTypeError","ord() expects a non-empty string"); return V_number((unsigned char)a[0].str[0]); }
static Value bi_assert(Interp *ip, Value *a, int n){ if(n<1) runtime_error(ip,"LarzTypeError","assert() expects a condition"); if(!truthy(a[0])) runtime_error(ip,"AssertionError","%s", (n>=2&&a[1].t==V_STR)?a[1].str:"assertion failed"); return V_nil(); }
static Value bi_input(Interp *ip, Value *a, int n){
  (void)ip; if(n>=1 && a[0].t==V_STR){ printf("%s", a[0].str); fflush(stdout); }
  char buf[8192]; if(!fgets(buf,sizeof buf,stdin)) return V_nil();
  int len=(int)strlen(buf); while(len>0 && (buf[len-1]=='\n'||buf[len-1]=='\r')) buf[--len]=0;
  return mkstr_n(buf,len);
}
static Value bi_keys(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_DICT) runtime_error(ip,"LarzTypeError","keys() expects a dict"); List *r=list_new(); for(int i=0;i<a[0].dict->n;i++) list_push(r,a[0].dict->items[i].key); return V_list(r); }
static Value bi_values(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_DICT) runtime_error(ip,"LarzTypeError","values() expects a dict"); List *r=list_new(); for(int i=0;i<a[0].dict->n;i++) list_push(r,a[0].dict->items[i].val); return V_list(r); }
static Value bi_map(Interp *ip, Value *a, int n){ if(n>=2) a[1]=derange(a[1]); if(n!=2||a[1].t!=V_LIST) runtime_error(ip,"LarzTypeError","map() expects a function and a list"); List *r=list_new(); int tr=ip->ntemp; gc_temp_push(ip,V_list(r)); for(int i=0;i<a[1].list->n;i++){ Value arg=a[1].list->items[i]; list_push(r, call_value(ip,a[0],&arg,1)); } gc_temp_pop(ip,tr); return V_list(r); }
static Value bi_filter(Interp *ip, Value *a, int n){ if(n>=2) a[1]=derange(a[1]); if(n!=2||a[1].t!=V_LIST) runtime_error(ip,"LarzTypeError","filter() expects a function and a list"); List *r=list_new(); int tr=ip->ntemp; gc_temp_push(ip,V_list(r)); for(int i=0;i<a[1].list->n;i++){ Value arg=a[1].list->items[i]; if(truthy(call_value(ip,a[0],&arg,1))) list_push(r,arg); } gc_temp_pop(ip,tr); return V_list(r); }
static Value bi_reduce(Interp *ip, Value *a, int n){ if(n>=2) a[1]=derange(a[1]); if(n<2||a[1].t!=V_LIST) runtime_error(ip,"LarzTypeError","reduce() expects a function, a list and an optional initial value"); List *l=a[1].list; int i=0; Value acc; if(n>=3) acc=a[2]; else { if(l->n==0) runtime_error(ip,"LarzValueError","reduce() of empty list with no initial value"); acc=l->items[0]; i=1; } int tr=ip->ntemp; gc_temp_push(ip,acc); for(; i<l->n; i++){ Value args[2]; args[0]=acc; args[1]=l->items[i]; acc=call_value(ip,a[0],args,2); ip->temproots[tr]=acc; } gc_temp_pop(ip,tr); return acc; }
static Value bi_join(Interp *ip, Value *a, int n){ if(n>=1) a[0]=derange(a[0]); if(n<1||a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","join() expects a list and an optional separator"); const char *sep=(n>=2&&a[1].t==V_STR)?a[1].str:""; SB b; b.s=NULL;b.n=0;b.cap=0; for(int i=0;i<a[0].list->n;i++){ if(i) sb_puts(&b,sep); char *s=str_of(a[0].list->items[i]); sb_puts(&b,s); } sb_putc(&b,0); return V_take(b.s?b.s:xstrdup("")); }
static Value bi_enumerate(Interp *ip, Value *a, int n){ if(n>=1) a[0]=derange(a[0]); if(n!=1||a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","enumerate() expects a list"); List *r=list_new(); for(int i=0;i<a[0].list->n;i++){ List *pair=list_new(); list_push(pair,V_number(i)); list_push(pair,a[0].list->items[i]); list_push(r,V_list(pair)); } return V_list(r); }
static Value bi_zip(Interp *ip, Value *a, int n){ if(n>=2){ a[0]=derange(a[0]); a[1]=derange(a[1]); } if(n!=2||a[0].t!=V_LIST||a[1].t!=V_LIST) runtime_error(ip,"LarzTypeError","zip() expects two lists"); int m=a[0].list->n<a[1].list->n?a[0].list->n:a[1].list->n; List *r=list_new(); for(int i=0;i<m;i++){ List *pair=list_new(); list_push(pair,a[0].list->items[i]); list_push(pair,a[1].list->items[i]); list_push(r,V_list(pair)); } return V_list(r); }
static Value bi_read_file(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","read_file() expects a path string"); FILE *f=fopen(a[0].str,"rb"); if(!f) runtime_error(ip,"IOError","cannot read file '%s'", a[0].str); size_t cap=1<<16,len=0; char *b=xmalloc(cap); size_t r; while((r=fread(b+len,1,cap-len,f))>0){ len+=r; if(len==cap){ cap*=2; b=realloc(b,cap); } } b[len]=0; fclose(f); return V_take(b); }
/* Binary-safe counterpart to read_file(): returns a list of ints 0-255
 * instead of a (NUL-truncating) string - the read-side match for
 * write_file()'s byte-list support, so a real binary file (zip, tar,
 * anything with 0x00 bytes) can round-trip through this language at all. */
static Value bi_read_file_bytes(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","read_file_bytes() expects a path string"); FILE *f=fopen(a[0].str,"rb"); if(!f) runtime_error(ip,"IOError","cannot read file '%s'", a[0].str); size_t cap=1<<16,len=0; unsigned char *b=xmalloc(cap); size_t r; while((r=fread(b+len,1,cap-len,f))>0){ len+=r; if(len==cap){ cap*=2; b=realloc(b,cap); } } fclose(f); List *out=list_new(); for(size_t i=0;i<len;i++) list_push(out,V_number((double)b[i])); free(b); return V_list(out); }
/* Writes a V_LIST of ints 0-255 as raw bytes via fwrite (binary-safe,
 * including 0x00) instead of the fputs() string path, which stops dead
 * at the first NUL - real binary formats (zip, tar, any packed byte
 * protocol) routinely contain 0x00 and would otherwise silently
 * truncate on write. Returns 1 if `v` was a byte list and was written,
 * 0 if the caller should fall back to the string path. */
static int write_bytes_if_list(Value v, FILE *f){
  if(v.t!=V_LIST) return 0;
  int n=v.list->n;
  unsigned char *buf=xmalloc(n>0?n:1);
  for(int i=0;i<n;i++){
    Value it=v.list->items[i];
    if(!is_num(it)) { free(buf); return 0; }
    long b=(long)it.num;
    buf[i]=(unsigned char)(b & 0xff);
  }
  if(n>0) fwrite(buf,1,n,f);
  free(buf);
  return 1;
}
static Value bi_write_file(Interp *ip, Value *a, int n){ if(n!=2||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","write_file() expects a path and content"); FILE *f=fopen(a[0].str,"wb"); if(!f) runtime_error(ip,"IOError","cannot write file '%s'", a[0].str); if(!write_bytes_if_list(a[1],f)){ char *s=str_of(a[1]); fputs(s,f); } fclose(f); return V_nil(); }
static Value bi_append_file(Interp *ip, Value *a, int n){ if(n!=2||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","append_file() expects a path and content"); FILE *f=fopen(a[0].str,"ab"); if(!f) runtime_error(ip,"IOError","cannot append to file '%s'", a[0].str); if(!write_bytes_if_list(a[1],f)){ char *s=str_of(a[1]); fputs(s,f); } fclose(f); return V_nil(); }
static Value bi_file_exists(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","file_exists() expects a path string"); struct stat st; return V_bool(stat(a[0].str,&st)==0); }

/* ---- ranged/patch file I/O + PCM16 codec ------------------------------
 * The `wav` stack package documents (and calls) a real streaming API -
 * open_write/write_chunk/close_write, open_read/read_chunk/eof - built on
 * file_size(), read_file_bytes_range(), patch_file_bytes() and a native
 * PCM16 encode/decode pair. None of those five existed in this
 * interpreter (found via the same "does the deployed interpreter
 * actually have what the package claims" check that turned up dsp's
 * missing _native_* buffer functions - see larzscript-beatstudio's
 * PLAN.md) - `wav.open_write()` and friends have always thrown
 * LarzNameError at runtime. This implements exactly that
 * already-documented contract: a real byte-range read, a real in-place
 * byte-range patch (only ever used here to fix up a WAV header's two
 * 4-byte size fields after streaming - never resizes the file), and a
 * fast PCM16<->float codec so a per-sample interpreted loop isn't the
 * cost of every chunk read/write on a multi-minute file. */
static Value bi_file_size(Interp *ip, Value *a, int n){
  if(n!=1||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","file_size() expects a path string");
  struct stat st;
  if(stat(a[0].str,&st)!=0) runtime_error(ip,"IOError","cannot stat file '%s'", a[0].str);
  return V_number((double)st.st_size);
}

static Value bi_read_file_bytes_range(Interp *ip, Value *a, int n){
  if(n!=3||a[0].t!=V_STR||!is_num(a[1])||!is_num(a[2])) runtime_error(ip,"LarzTypeError","read_file_bytes_range() expects a path, offset, and length");
  long long offset=(long long)a[1].num, want=(long long)a[2].num;
  if(offset<0||want<0) runtime_error(ip,"LarzValueError","read_file_bytes_range(): offset and length must be non-negative");
  FILE *f=fopen(a[0].str,"rb");
  if(!f) runtime_error(ip,"IOError","cannot read file '%s'", a[0].str);
  if(fseek(f,offset,SEEK_SET)!=0){ fclose(f); runtime_error(ip,"IOError","cannot seek in file '%s'", a[0].str); }
  unsigned char *buf=xmalloc(want>0?want:1);
  size_t got=want>0?fread(buf,1,(size_t)want,f):0;
  fclose(f);
  List *out=list_new();
  for(size_t i=0;i<got;i++) list_push(out,V_number((double)buf[i]));
  free(buf);
  return V_list(out);
}

static Value bi_patch_file_bytes(Interp *ip, Value *a, int n){
  if(n!=3||a[0].t!=V_STR||!is_num(a[1])||a[2].t!=V_LIST) runtime_error(ip,"LarzTypeError","patch_file_bytes() expects a path, offset, and a byte list");
  long long offset=(long long)a[1].num;
  if(offset<0) runtime_error(ip,"LarzValueError","patch_file_bytes(): offset must be non-negative");
  FILE *f=fopen(a[0].str,"r+b");
  if(!f) runtime_error(ip,"IOError","cannot open file '%s' for patching", a[0].str);
  if(fseek(f,offset,SEEK_SET)!=0){ fclose(f); runtime_error(ip,"IOError","cannot seek in file '%s'", a[0].str); }
  int nb=a[2].list->n;
  unsigned char *buf=xmalloc(nb>0?nb:1);
  for(int i=0;i<nb;i++){
    Value it=a[2].list->items[i];
    if(!is_num(it)){ free(buf); fclose(f); runtime_error(ip,"LarzTypeError","patch_file_bytes(): byte list must contain only numbers"); }
    buf[i]=(unsigned char)((long)it.num & 0xff);
  }
  if(nb>0) fwrite(buf,1,nb,f);
  free(buf);
  fclose(f);
  return V_nil();
}

static Value bi_native_pcm16_encode(Interp *ip, Value *a, int n){
  if(n!=1) runtime_error(ip,"LarzTypeError","_native_pcm16_encode() expects a list of samples");
  need_num_list(ip,a[0],"_native_pcm16_encode()");
  List *s=a[0].list;
  List *out=list_new();
  for(int i=0;i<s->n;i++){
    double v=s->items[i].num;
    if(v>1.0) v=1.0; else if(v<-1.0) v=-1.0;
    long nn=(long)(v*32767.0);
    if(nn<0) nn=65536+nn;
    list_push(out,V_number((double)(nn%256)));
    list_push(out,V_number((double)((nn/256)%256)));
  }
  return V_list(out);
}

static Value bi_native_pcm16_decode(Interp *ip, Value *a, int n){
  if(n!=1) runtime_error(ip,"LarzTypeError","_native_pcm16_decode() expects a byte list");
  need_num_list(ip,a[0],"_native_pcm16_decode()");
  List *b=a[0].list;
  List *out=list_new();
  int i=0;
  while(i+1<b->n){
    long lo=(long)b->items[i].num & 0xff, hi=(long)b->items[i+1].num & 0xff;
    long nn=lo+hi*256;
    if(nn>=32768) nn-=65536;
    list_push(out,V_number(nn/32768.0));
    i+=2;
  }
  return V_list(out);
}
static Value bi_exit(Interp *ip, Value *a, int n){ (void)ip; int code = (n>=1&&is_num(a[0]))?(int)a[0].num:0; exit(code); }
static Value bi_all(Interp *ip, Value *a, int n){ if(n>=1) a[0]=derange(a[0]); if(n!=1||a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","all() expects a list"); for(int i=0;i<a[0].list->n;i++) if(!truthy(a[0].list->items[i])) return V_bool(0); return V_bool(1); }
static Value bi_any(Interp *ip, Value *a, int n){ if(n>=1) a[0]=derange(a[0]); if(n!=1||a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","any() expects a list"); for(int i=0;i<a[0].list->n;i++) if(truthy(a[0].list->items[i])) return V_bool(1); return V_bool(0); }
static Value bi_count(Interp *ip, Value *a, int n){ if(n>=1) a[0]=derange(a[0]); if(n!=2) runtime_error(ip,"LarzTypeError","count() expects a list/string and a value"); long long c=0; if(a[0].t==V_LIST){ for(int i=0;i<a[0].list->n;i++) if(values_equal(a[0].list->items[i],a[1])) c++; } else if(a[0].t==V_STR&&a[1].t==V_STR&&a[1].str[0]){ const char *p=a[0].str,*q; size_t l=strlen(a[1].str); while((q=strstr(p,a[1].str))){ c++; p=q+l; } } else runtime_error(ip,"LarzTypeError","count() expects a list, or two strings"); return V_number((double)c); }
static Value bi_unique(Interp *ip, Value *a, int n){ if(n>=1) a[0]=derange(a[0]); if(n!=1||a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","unique() expects a list"); List *r=list_new(); for(int i=0;i<a[0].list->n;i++){ int seen=0; for(int j=0;j<r->n;j++) if(values_equal(r->items[j],a[0].list->items[i])){ seen=1; break; } if(!seen) list_push(r,a[0].list->items[i]); } return V_list(r); }
/* ---- minimal regex engine: literals, ., ^, $, *, +, ?, [...], [^...],
   \d \D \w \W \s \S, \-escaped literals. No capture groups, no alternation,
   no lookaround - a deliberately small backtracking subset (classic
   Kernighan-style matcher, extended for +/?/classes), not a full PCRE-class
   engine. Exponential worst case on pathological patterns like any
   classic backtracking regex - fine for the short, fixed patterns this
   language is meant for (validation/cleanup), not for untrusted input. */
static int re_atom_len(const char *p){
  if(!*p) return 0;
  if(*p=='\\' && p[1]) return 2;
  if(*p=='['){
    const char *q=p+1;
    if(*q=='^') q++;
    if(*q==']') q++;                       /* a leading ] is a literal member, not the terminator */
    while(*q && *q!=']'){
      if(*q=='\\' && q[1]) q+=2;           /* \] or \\ inside a class doesn't end it early */
      else q++;
    }
    return (int)(q-p) + (*q==']'?1:0);
  }
  return 1;
}
static int re_shorthand(char cls, char c){
  switch(cls){
    case 'd': return isdigit((unsigned char)c);
    case 'D': return !isdigit((unsigned char)c);
    case 'w': return isalnum((unsigned char)c)||c=='_';
    case 'W': return !(isalnum((unsigned char)c)||c=='_');
    case 's': return isspace((unsigned char)c);
    case 'S': return !isspace((unsigned char)c);
  }
  return 0;
}
static int re_class_has(const char *p, int plen, char c){
  const char *q=p+1, *end=p+plen-1; /* p='[' .. end=']' */
  int neg=0; if(q<end && *q=='^'){ neg=1; q++; }
  int found=0;
  while(q<end){
    char lo;
    if(*q=='\\' && q+1<end){ lo=q[1]; q+=2; } else { lo=*q; q++; }
    if(q<end && *q=='-' && q+1<end){
      q++;                                          /* consume '-' */
      char hi;
      if(*q=='\\' && q+1<end){ hi=q[1]; q+=2; } else { hi=*q; q++; }
      if((unsigned char)c>=(unsigned char)lo && (unsigned char)c<=(unsigned char)hi) found=1;
    } else if(lo==c) found=1;
  }
  return neg?!found:found;
}
static int re_atom_matches(const char *p, int alen, char c){
  if(alen==0) return 0;
  if(*p=='\\'){ char e=p[1]; if(strchr("dDwWsS",e)) return re_shorthand(e,c); return e==c; }
  if(*p=='[') return re_class_has(p, alen, c);
  if(*p=='.') return 1;
  return *p==c;
}
static const char *re_match_seq(const char *re, const char *text);
static const char *re_match_star(const char *atom, int alen, char quant, const char *re_rest, const char *text){
  int minrep = (quant=='+') ? 1 : 0;
  int maxrun = 0;
  while(text[maxrun] && re_atom_matches(atom, alen, text[maxrun])) maxrun++;
  for(int k=maxrun;k>=minrep;k--){
    const char *r = re_match_seq(re_rest, text+k);
    if(r) return r;
  }
  return NULL;
}
static const char *re_match_seq(const char *re, const char *text){
  if(*re==0) return text;
  if(*re=='$' && re[1]==0) return (*text==0) ? text : NULL;
  int alen = re_atom_len(re);
  char quant = re[alen];
  if(quant=='*' || quant=='+' || quant=='?'){
    if(quant=='?'){
      if(*text && re_atom_matches(re, alen, *text)){
        const char *r = re_match_seq(re+alen+1, text+1);
        if(r) return r;
      }
      return re_match_seq(re+alen+1, text);
    }
    return re_match_star(re, alen, quant, re+alen+1, text);
  }
  if(*text==0) return NULL;
  if(!re_atom_matches(re, alen, *text)) return NULL;
  return re_match_seq(re+alen, text+1);
}
static int re_search(const char *pattern, const char *text, int from, int *out_start, int *out_end){
  const char *re = pattern;
  int anchored = (*re=='^');
  if(anchored) re++;
  int tlen = (int)strlen(text);
  if(from<0) from=0;
  for(int i=from; i<=tlen; i++){
    const char *end = re_match_seq(re, text+i);
    if(end){ *out_start=i; *out_end=(int)(end-text); return 1; }
    if(anchored) break;
  }
  return 0;
}
static Value bi_regex_match(Interp *ip, Value *a, int n){
  if(n!=2||a[0].t!=V_STR||a[1].t!=V_STR) runtime_error(ip,"LarzTypeError","regex_match() expects (pattern, text)");
  int s,e; return V_bool(re_search(a[0].str,a[1].str,0,&s,&e));
}
static Value bi_regex_find(Interp *ip, Value *a, int n){
  if(n<2||n>3||a[0].t!=V_STR||a[1].t!=V_STR||(n==3&&!is_num(a[2]))) runtime_error(ip,"LarzTypeError","regex_find() expects (pattern, text, start=0)");
  int from = n==3 ? (int)a[2].num : 0;
  int s,e;
  if(re_search(a[0].str,a[1].str,from,&s,&e)){ List *r=list_new(); list_push(r,V_number(s)); list_push(r,V_number(e)); return V_list(r); }
  return V_nil();
}
static Value bi_regex_replace(Interp *ip, Value *a, int n){
  if(n!=3||a[0].t!=V_STR||a[1].t!=V_STR||a[2].t!=V_STR) runtime_error(ip,"LarzTypeError","regex_replace() expects (pattern, text, replacement)");
  const char *text=a[1].str; SB b; b.s=NULL;b.n=0;b.cap=0;
  int pos=0, tlen=(int)strlen(text), s,e;
  while(pos<=tlen && re_search(a[0].str,text,pos,&s,&e)){
    for(int i=pos;i<s;i++) sb_putc(&b,text[i]);
    sb_puts(&b,a[2].str);
    if(e==s){ if(s<tlen) sb_putc(&b,text[s]); pos=s+1; } else pos=e;
  }
  for(int i=pos;i<tlen;i++) sb_putc(&b,text[i]);
  sb_putc(&b,0);
  return V_take(b.s?b.s:xstrdup(""));
}
static Value bi_regex_split(Interp *ip, Value *a, int n){
  if(n!=2||a[0].t!=V_STR||a[1].t!=V_STR) runtime_error(ip,"LarzTypeError","regex_split() expects (pattern, text)");
  const char *text=a[1].str; List *r=list_new();
  int pos=0, tlen=(int)strlen(text), s,e, last=0;
  while(pos<=tlen && re_search(a[0].str,text,pos,&s,&e)){
    if(e==s){ pos=s+1; continue; }
    list_push(r, mkstr_n(text+last, s-last));
    last=e; pos=e;
  }
  list_push(r, V_string(text+last));
  return V_list(r);
}

/* ---- date/datetime: pure integer epoch->calendar, no libc <time.h>
   gmtime/strftime dependency (those aren't guaranteed to exist in the
   freestanding kernel build that shares this file) - Howard Hinnant's
   civil_from_days algorithm, proleptic Gregorian, correct for any epoch
   including negative (pre-1970). UTC only. */
static void civil_from_days(long long z, int *y, int *m, int *d){
  z += 719468;
  long long era = (z>=0?z:z-146096) / 146097;
  unsigned doe = (unsigned)(z - era*146097);
  unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
  long long yr = (long long)yoe + era*400;
  unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
  unsigned mp = (5*doy + 2)/153;
  unsigned dd = doy - (153*mp+2)/5 + 1;
  unsigned mm = mp + (mp<10?3:(unsigned)-9);
  *y = (int)(yr + (mm<=2?1:0));
  *m = (int)mm; *d=(int)dd;
}
static void epoch_parts(double ts, int *y, int *mo, int *d, int *hh, int *mi, int *ss){
  long long total=(long long)ts;
  long long days = total>=0 ? total/86400 : (total-86399)/86400;
  long long rem = total - days*86400;
  *hh=(int)(rem/3600); *mi=(int)((rem%3600)/60); *ss=(int)(rem%60);
  civil_from_days(days,y,mo,d);
}
static Value bi_date(Interp *ip, Value *a, int n){
  if(n>1 || (n==1&&!is_num(a[0]))) runtime_error(ip,"LarzTypeError","date() expects an optional unix timestamp");
  double ts = n==1 ? a[0].num : (double)time(NULL);
  int y,mo,d,hh,mi,ss; epoch_parts(ts,&y,&mo,&d,&hh,&mi,&ss);
  char buf[16]; snprintf(buf,sizeof buf,"%04d-%02d-%02d",y,mo,d);
  return V_string(buf);
}
static Value bi_datetime(Interp *ip, Value *a, int n){
  if(n>1 || (n==1&&!is_num(a[0]))) runtime_error(ip,"LarzTypeError","datetime() expects an optional unix timestamp");
  double ts = n==1 ? a[0].num : (double)time(NULL);
  int y,mo,d,hh,mi,ss; epoch_parts(ts,&y,&mo,&d,&hh,&mi,&ss);
  char buf[24]; snprintf(buf,sizeof buf,"%04d-%02d-%02dT%02d:%02d:%02dZ",y,mo,d,hh,mi,ss);
  return V_string(buf);
}
static Value _base_str(long long v, int base, const char *prefix){ char buf[80]; int neg=v<0; unsigned long long u=neg?(unsigned long long)(-v):(unsigned long long)v; int k=0; if(u==0) buf[k++]='0'; while(u){ int d=u%base; buf[k++]= d<10 ? '0'+d : 'a'+(d-10); u/=base; } SB b; b.s=NULL;b.n=0;b.cap=0; if(neg) sb_putc(&b,'-'); sb_puts(&b,prefix); for(int i=k-1;i>=0;i--) sb_putc(&b,buf[i]); sb_putc(&b,0); return V_take(b.s?b.s:xstrdup("")); }
static Value bi_hex(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","hex() expects a number"); return _base_str((long long)a[0].num,16,"0x"); }
static Value bi_bin(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","bin() expects a number"); return _base_str((long long)a[0].num,2,"0b"); }
static Value bi_oct(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","oct() expects a number"); return _base_str((long long)a[0].num,8,"0o"); }
static Value bi_gcd(Interp *ip, Value *a, int n){ if(n!=2||!is_num(a[0])||!is_num(a[1])) runtime_error(ip,"LarzTypeError","gcd() expects two numbers"); long long x=(long long)a[0].num, y=(long long)a[1].num; if(x<0)x=-x; if(y<0)y=-y; while(y){ long long t=x%y; x=y; y=t; } return V_number((double)x); }
static Value bi_factorial(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","factorial() expects a number"); long long k=(long long)a[0].num; if(k<0) runtime_error(ip,"LarzValueError","factorial() of a negative number"); double r=1; for(long long i=2;i<=k;i++) r*=i; return V_number(r); }
static Value bi_sign(Interp *ip, Value *a, int n){ if(n!=1) runtime_error(ip,"LarzTypeError","sign() expects one argument"); if(a[0].t==V_MONEY) return V_number(a[0].cents<0?-1:(a[0].cents>0?1:0)); if(is_num(a[0])) return V_number(a[0].num<0?-1:(a[0].num>0?1:0)); runtime_error(ip,"LarzTypeError","sign() expects a number or money"); return V_nil(); }
static Value bi_clamp(Interp *ip, Value *a, int n){ if(n!=3) runtime_error(ip,"LarzTypeError","clamp() expects a value, low and high"); if(value_compare(a[0],a[1])<0) return a[1]; if(value_compare(a[0],a[2])>0) return a[2]; return a[0]; }
static Value bi_list(Interp *ip, Value *a, int n){ if(n>=1) a[0]=derange(a[0]); if(n!=1) runtime_error(ip,"LarzTypeError","list() expects one argument"); List *r=list_new(); if(a[0].t==V_LIST){ for(int i=0;i<a[0].list->n;i++) list_push(r,a[0].list->items[i]); } else if(a[0].t==V_DICT){ for(int i=0;i<a[0].dict->n;i++) list_push(r,a[0].dict->items[i].key); } else if(a[0].t==V_STR){ for(const char *p=a[0].str;*p;p++) list_push(r,mkstr_n(p,1)); } else runtime_error(ip,"LarzTypeError","list() expects a list, dict or string"); return V_list(r); }
static Value bi_dict(Interp *ip, Value *a, int n){ Dict *d=dict_new(); if(n==0) return V_dict(d); if(n!=1||a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","dict() expects a list of [key, value] pairs"); for(int i=0;i<a[0].list->n;i++){ Value pr=a[0].list->items[i]; if(pr.t!=V_LIST||pr.list->n!=2) runtime_error(ip,"LarzTypeError","dict() pairs must be [key, value] lists"); dict_set(d, pr.list->items[0], pr.list->items[1]); } return V_dict(d); }
/* ---- OS / system builtins (general-purpose scripting) ---- */
static Value bi_env(Interp *ip, Value *a, int n){ if(n<1||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","env() expects a name"); char *v=getenv(a[0].str); if(v) return V_string(v); return n>=2?a[1]:V_nil(); }
static Value bi_run(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","run() expects a command string"); int rc=system(a[0].str); return V_number((double)(rc==-1?-1:(rc>>8)&0xff)); }
static Value bi_capture(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","capture() expects a command string"); FILE *f=popen(a[0].str,"r"); if(!f) runtime_error(ip,"IOError","could not run command"); size_t cap=1<<16,len=0; char *b=xmalloc(cap); size_t r; while((r=fread(b+len,1,cap-len,f))>0){ len+=r; if(len==cap){ cap*=2; b=realloc(b,cap);} } b[len]=0; pclose(f); return V_take(b); }
static Value bi_cwd(Interp *ip, Value *a, int n){ (void)a;(void)n; char buf[4096]; if(!getcwd(buf,sizeof buf)) runtime_error(ip,"IOError","cannot get cwd"); return V_string(buf); }
static Value bi_chdir(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","chdir() expects a path"); return V_bool(chdir(a[0].str)==0); }
static Value bi_listdir(Interp *ip, Value *a, int n){ const char *path=(n>=1&&a[0].t==V_STR)?a[0].str:"."; DIR *d=opendir(path); if(!d) runtime_error(ip,"IOError","cannot list directory '%s'", path); List *r=list_new(); struct dirent *e; while((e=readdir(d))){ if(strcmp(e->d_name,".")==0||strcmp(e->d_name,"..")==0) continue; list_push(r,V_string(e->d_name)); } closedir(d); return V_list(r); }
static Value bi_mkdir(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","mkdir() expects a path"); return V_bool(mkdir(a[0].str,0755)==0); }
static Value bi_remove(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","remove() expects a path"); return V_bool(remove(a[0].str)==0); }
static Value bi_rename(Interp *ip, Value *a, int n){ if(n!=2||a[0].t!=V_STR||a[1].t!=V_STR) runtime_error(ip,"LarzTypeError","rename() expects two paths"); return V_bool(rename(a[0].str,a[1].str)==0); }
static Value bi_time(Interp *ip, Value *a, int n){ (void)ip;(void)a;(void)n; return V_number((double)time(NULL)); }
static Value bi_clock(Interp *ip, Value *a, int n){ (void)ip;(void)a;(void)n; struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return V_number((double)ts.tv_sec + ts.tv_nsec/1e9); }
static Value bi_sleep(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","sleep() expects a number of seconds"); double s=a[0].num; if(s>0) usleep((useconds_t)(s*1e6)); return V_nil(); }

/* ---- TCP sockets (hosted only; see the header-guard block near the top
 * of this file for why the kernel/emscripten builds don't get real
 * syscalls here). Deliberately small and low-level, the same shape
 * run()/capture() already expose raw OS primitives at - a `tcp` package
 * built on these is what most scripts should actually import, the same
 * way nobody calls read_file/write_file directly once `fs` exists. A
 * socket handle is just a plain number (the OS fd/SOCKET), like every
 * other OS-ish value already flowing through this interpreter's one
 * numeric (double) type. */
#if !defined(__STDC_HOSTED__) || __STDC_HOSTED__
static Value bi_socket_listen(Interp *ip, Value *a, int n){
#ifdef __EMSCRIPTEN__
  (void)a; (void)n;
  runtime_error(ip,"SocketError","sockets are not available in the browser/wasm build - use a native binary");
  return V_nil();
#else
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","socket_listen() expects a port number");
  int port=(int)a[0].num;
#ifdef _WIN32
  static int wsa_started=0;
  if(!wsa_started){ WSADATA wsa; WSAStartup(MAKEWORD(2,2),&wsa); wsa_started=1; }
#endif
  larz_sock_t fd=socket(AF_INET,SOCK_STREAM,0);
  if(fd==LARZ_INVALID_SOCK) runtime_error(ip,"SocketError","could not create a socket");
  int yes=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,(const char*)&yes,sizeof(yes));
  struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
  addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons((unsigned short)port);
  if(bind(fd,(struct sockaddr*)&addr,sizeof(addr))!=0){ larz_sock_close(fd); runtime_error(ip,"SocketError","could not bind to port %d",port); }
  if(listen(fd,16)!=0){ larz_sock_close(fd); runtime_error(ip,"SocketError","could not listen on port %d",port); }
  return V_number((double)fd);
#endif
}
static Value bi_socket_accept(Interp *ip, Value *a, int n){
#ifdef __EMSCRIPTEN__
  (void)a; (void)n;
  runtime_error(ip,"SocketError","sockets are not available in the browser/wasm build - use a native binary");
  return V_nil();
#else
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","socket_accept() expects a listening socket handle");
  larz_sock_t listen_fd=(larz_sock_t)a[0].num;
  larz_sock_t client_fd=accept(listen_fd,NULL,NULL);
  if(client_fd==LARZ_INVALID_SOCK) runtime_error(ip,"SocketError","accept() failed");
  return V_number((double)client_fd);
#endif
}
static Value bi_socket_read(Interp *ip, Value *a, int n){
#ifdef __EMSCRIPTEN__
  (void)a; (void)n;
  runtime_error(ip,"SocketError","sockets are not available in the browser/wasm build - use a native binary");
  return V_nil();
#else
  if(n!=2||!is_num(a[0])||!is_num(a[1])) runtime_error(ip,"LarzTypeError","socket_read() expects a socket handle and a max byte count");
  larz_sock_t fd=(larz_sock_t)a[0].num;
  long maxlen=(long)a[1].num;
  if(maxlen<0) maxlen=0;
  char *buf=xmalloc((size_t)maxlen>0?(size_t)maxlen:1);
#ifdef _WIN32
  int r=recv(fd,buf,(int)maxlen,0);
#else
  long r=(long)recv(fd,buf,(size_t)maxlen,0);
#endif
  if(r<0){ free(buf); runtime_error(ip,"SocketError","read failed"); }
  /* mkstr_n (not V_take) - copies exactly r bytes, so a recv() that
   * happens to contain an embedded NUL doesn't get silently truncated
   * by a strlen() call before the caller ever sees it. (len()/other
   * string builtins DO still use strlen() throughout this language, so
   * that's the point embedded-NUL binary data stops being byte-exact -
   * a whole-language limitation this doesn't introduce, just doesn't
   * make needlessly worse on the way in.) */
  Value v=mkstr_n(buf,(size_t)r);
  free(buf);
  return v;
#endif
}
static Value bi_socket_write(Interp *ip, Value *a, int n){
#ifdef __EMSCRIPTEN__
  (void)a; (void)n;
  runtime_error(ip,"SocketError","sockets are not available in the browser/wasm build - use a native binary");
  return V_nil();
#else
  if(n!=2||!is_num(a[0])||a[1].t!=V_STR) runtime_error(ip,"LarzTypeError","socket_write() expects a socket handle and a string");
  larz_sock_t fd=(larz_sock_t)a[0].num;
  size_t len=strlen(a[1].str);
#ifdef _WIN32
  int sent=send(fd,a[1].str,(int)len,0);
#else
  long sent=(long)send(fd,a[1].str,len,0);
#endif
  if(sent<0) runtime_error(ip,"SocketError","write failed");
  return V_number((double)sent);
#endif
}

/* Byte-list variants - see ssh_channel_read_bytes()/write_bytes()'s
 * comment for why: a Larzscript Str has no stored length at all (just a
 * NUL-terminated char[]), so socket_read()/write() are limited the same
 * way for payloads with real embedded 0x00 bytes. A byte list (real
 * stored count, same representation read_file_bytes() already uses)
 * doesn't have that limitation. */
static Value bi_socket_read_bytes(Interp *ip, Value *a, int n){
#ifdef __EMSCRIPTEN__
  (void)a; (void)n;
  runtime_error(ip,"SocketError","sockets are not available in the browser/wasm build - use a native binary");
  return V_nil();
#else
  if(n!=2||!is_num(a[0])||!is_num(a[1])) runtime_error(ip,"LarzTypeError","socket_read_bytes() expects a socket handle and a max byte count");
  larz_sock_t fd=(larz_sock_t)a[0].num;
  long maxlen=(long)a[1].num;
  if(maxlen<0) maxlen=0;
  unsigned char *buf=xmalloc((size_t)maxlen>0?(size_t)maxlen:1);
#ifdef _WIN32
  int r=recv(fd,(char*)buf,(int)maxlen,0);
#else
  long r=(long)recv(fd,(char*)buf,(size_t)maxlen,0);
#endif
  if(r<0){ free(buf); runtime_error(ip,"SocketError","read failed"); }
  List *out=list_new();
  for(long i=0;i<r;i++) list_push(out,V_number((double)buf[i]));
  free(buf);
  return V_list(out);
#endif
}

static Value bi_socket_write_bytes(Interp *ip, Value *a, int n){
#ifdef __EMSCRIPTEN__
  (void)a; (void)n;
  runtime_error(ip,"SocketError","sockets are not available in the browser/wasm build - use a native binary");
  return V_nil();
#else
  if(n!=2||!is_num(a[0])||a[1].t!=V_LIST) runtime_error(ip,"LarzTypeError","socket_write_bytes() expects a socket handle and a byte list");
  larz_sock_t fd=(larz_sock_t)a[0].num;
  int nb=a[1].list->n;
  unsigned char *buf=xmalloc((size_t)nb>0?(size_t)nb:1);
  for(int i=0;i<nb;i++){
    Value it=a[1].list->items[i];
    if(!is_num(it)){ free(buf); runtime_error(ip,"LarzTypeError","socket_write_bytes(): byte list must contain only numbers"); }
    buf[i]=(unsigned char)((long)it.num & 0xff);
  }
#ifdef _WIN32
  int sent=send(fd,(const char*)buf,(int)nb,0);
#else
  long sent=(long)send(fd,(const char*)buf,(size_t)nb,0);
#endif
  free(buf);
  if(sent<0) runtime_error(ip,"SocketError","write failed");
  return V_number((double)sent);
#endif
}
static Value bi_socket_close(Interp *ip, Value *a, int n){
#ifdef __EMSCRIPTEN__
  (void)a; (void)n;
  runtime_error(ip,"SocketError","sockets are not available in the browser/wasm build - use a native binary");
  return V_nil();
#else
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","socket_close() expects a socket handle");
  larz_sock_t fd=(larz_sock_t)a[0].num;
  larz_sock_close(fd);
  return V_nil();
#endif
}
/* Outbound TCP dial - the counterpart socket_listen()/socket_accept() never
 * had: nothing before this builtin let a Larzscript program connect OUT to
 * an address, only accept incoming connections. Resolves `host` (hostname
 * or literal IP) via getaddrinfo and connects to the first address that
 * works. */
static Value bi_socket_connect(Interp *ip, Value *a, int n){
#ifdef __EMSCRIPTEN__
  (void)a; (void)n;
  runtime_error(ip,"SocketError","sockets are not available in the browser/wasm build - use a native binary");
  return V_nil();
#else
  if(n!=2||a[0].t!=V_STR||!is_num(a[1])) runtime_error(ip,"LarzTypeError","socket_connect() expects a host string and a port number");
  const char *host=a[0].str;
  char portbuf[16]; snprintf(portbuf,sizeof(portbuf),"%d",(int)a[1].num);
#ifdef _WIN32
  static int wsa_started=0;
  if(!wsa_started){ WSADATA wsa; WSAStartup(MAKEWORD(2,2),&wsa); wsa_started=1; }
#endif
  struct addrinfo hints; memset(&hints,0,sizeof(hints));
  hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
  struct addrinfo *res=NULL;
  int gai=getaddrinfo(host,portbuf,&hints,&res);
  if(gai!=0||!res) runtime_error(ip,"SocketError","could not resolve host '%s'",host);
  larz_sock_t fd=LARZ_INVALID_SOCK;
  for(struct addrinfo *rp=res; rp; rp=rp->ai_next){
    fd=socket(rp->ai_family,rp->ai_socktype,rp->ai_protocol);
    if(fd==LARZ_INVALID_SOCK) continue;
    if(connect(fd,rp->ai_addr,(int)rp->ai_addrlen)==0) break;
    larz_sock_close(fd); fd=LARZ_INVALID_SOCK;
  }
  freeaddrinfo(res);
  if(fd==LARZ_INVALID_SOCK) runtime_error(ip,"SocketError","could not connect to %s:%s",host,portbuf);
  return V_number((double)fd);
#endif
}
/* select()-based readiness check over a list of socket handles - lets one
 * single-threaded Larzscript process service multiple live sockets (e.g. a
 * control connection and a forwarded data connection) without a blocking
 * socket_read() on one starving the other. Returns the subset of `fds` that
 * are readable within `timeout_ms` (an empty list on timeout, not an
 * error - timing out with nothing ready is the normal case in a poll loop). */
static Value bi_socket_poll(Interp *ip, Value *a, int n){
#ifdef __EMSCRIPTEN__
  (void)a; (void)n;
  runtime_error(ip,"SocketError","sockets are not available in the browser/wasm build - use a native binary");
  return V_nil();
#else
  if(n!=2||a[0].t!=V_LIST||!is_num(a[1])) runtime_error(ip,"LarzTypeError","socket_poll() expects a list of socket handles and a timeout in milliseconds");
  List *fds=a[0].list;
  for(int i=0;i<fds->n;i++) if(!is_num(fds->items[i])) runtime_error(ip,"LarzTypeError","socket_poll() expects a list of socket handles");
  int timeout_ms=(int)a[1].num;
  if(timeout_ms<0) timeout_ms=0;
  fd_set readfds; FD_ZERO(&readfds);
  larz_sock_t maxfd=0;
  for(int i=0;i<fds->n;i++){
    larz_sock_t fd=(larz_sock_t)fds->items[i].num;
    FD_SET(fd,&readfds);
    if(fd>maxfd) maxfd=fd;
  }
  struct timeval tv; tv.tv_sec=timeout_ms/1000; tv.tv_usec=(timeout_ms%1000)*1000;
  int r=select((int)maxfd+1,&readfds,NULL,NULL,&tv);
  if(r<0) runtime_error(ip,"SocketError","poll failed");
  List *out=list_new();
  if(r>0){
    for(int i=0;i<fds->n;i++){
      larz_sock_t fd=(larz_sock_t)fds->items[i].num;
      if(FD_ISSET(fd,&readfds)) list_push(out,fds->items[i]);
    }
  }
  return V_list(out);
#endif
}
#endif /* hosted */

/* ===================== real SSH via libssh ===================== *
 * Client only for now (Phase 1 of bringing real SSH interop up
 * platform-by-platform - see the plan this was built against). A session
 * handle is the ssh_session pointer stored as a plain number, same
 * convention as every OS handle elsewhere in this file. Not compiled with
 * real libssh calls unless built with -DLARZ_HAVE_LIBSSH -lssh; every
 * other build keeps these names defined but throwing a clear SshError, so
 * a script fails the same understandable way everywhere rather than with
 * an "unknown identifier" on platforms libssh isn't linked on yet - the
 * browser/wasm build included: LARZ_HAVE_LIBSSH is never defined there
 * either, so it gets the same throwing stub as any other not-yet-wired
 * platform, not a compile error (an earlier version of this section
 * wrapped the stubs in an extra #ifndef __EMSCRIPTEN__, which meant the
 * Builtin registration below - unconditionally compiled for every hosted
 * target, emscripten included - referenced functions that didn't exist
 * there. Caught by CI, fixed here: the stub must be defined everywhere
 * hosted, only the REAL implementation is platform-gated). */
#if !defined(__STDC_HOSTED__) || __STDC_HOSTED__

#ifndef LARZ_HAVE_LIBSSH
static Value bi_ssh_open(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_auth_password(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_auth_key(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_run(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_close(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_is_connected(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_listen_forward(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_accept_forward(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_channel_poll(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_channel_read(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_channel_write(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_channel_read_bytes(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_channel_write_bytes(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_channel_eof(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_channel_free(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_bind_open(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_bind_accept_session(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_bind_free(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_server_next_message(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_msg_type(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_msg_auth_user(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_msg_auth_password(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_msg_auth_accept(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_msg_deny(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_msg_channel_accept(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_msg_exec_command(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_msg_request_accept(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_channel_send_exit_status(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_msg_channel(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_channel_close(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_msg_pty_width(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_msg_pty_height(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_channel_shell(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_check_host(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_trust_host(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
static Value bi_ssh_bridge_forward(Interp *ip, Value *a, int n){ (void)a; (void)n; runtime_error(ip,"SshError","real SSH is not available in this build (libssh not linked on this platform yet)"); return V_nil(); }
#else

/* Optional 3rd argument: connect timeout in seconds (default 15 if
 * omitted). Without this, a silently-unreachable host (firewalled, not
 * actively refusing) could hang ssh_connect() for libssh's own much
 * longer internal default - found wanting during a real live test
 * against a relay that occasionally stopped responding: no way to fail
 * fast and retry instead of hanging the whole script indefinitely. */
static Value bi_ssh_open(Interp *ip, Value *a, int n){
  if((n!=2&&n!=3)||a[0].t!=V_STR||!is_num(a[1])||(n==3&&!is_num(a[2]))) runtime_error(ip,"LarzTypeError","ssh_open() expects a host string, a port number, and an optional connect timeout in seconds");
#ifdef _WIN32
  /* libssh's ssh_connect() opens a real socket under the hood - on
   * Windows that needs WSAStartup() called first, same as the raw
   * socket_listen()/socket_connect() builtins already do. Without it,
   * ssh_connect() doesn't fail cleanly - it crashes (STATUS_ACCESS_
   * VIOLATION), reproducibly, ONLY on a real Windows machine (Wine's
   * winsock emulation tolerates the missing WSAStartup and silently
   * works, which is exactly why this shipped unnoticed - CI's "test
   * under wine" step never caught it). Found live on a real Windows box
   * via ssh_open() as the very FIRST network call in the process (the
   * ssh package is meant to need nothing else) - any prior call to one
   * of the raw socket builtins would have masked this by initializing
   * Winsock as a side effect first. */
  static int wsa_started=0;
  if(!wsa_started){ WSADATA wsa; WSAStartup(MAKEWORD(2,2),&wsa); wsa_started=1; }
#endif
  ssh_session sess=ssh_new();
  if(!sess) runtime_error(ip,"SshError","could not allocate an ssh session");
  ssh_options_set(sess,SSH_OPTIONS_HOST,a[0].str);
  unsigned int port=(unsigned int)a[1].num;
  ssh_options_set(sess,SSH_OPTIONS_PORT,&port);
  long timeout_s=(n==3 && a[2].num>0)?(long)a[2].num:15;
  ssh_options_set(sess,SSH_OPTIONS_TIMEOUT,&timeout_s);
  if(ssh_connect(sess)!=SSH_OK){
    char msg[256]; snprintf(msg,sizeof msg,"connect to %s:%u failed: %s",a[0].str,port,ssh_get_error(sess));
    ssh_free(sess);
    runtime_error(ip,"SshError","%s",msg);
  }
  /* Not verified against known_hosts here - that's ssh_check_host()/
   * ssh_trust_host() below, a separate step so callers can inspect the
   * server's identity before sending any auth data (real MITM detection,
   * not silently skipped - see the ssh package README for the current
   * "not yet implemented" note this closes out). */
  return V_number((double)(intptr_t)sess);
}

/* Checks the connected server's host key against a known_hosts file
 * using libssh's own known_hosts implementation (not hand-rolled) -
 * ssh_session_is_known_server() reads/parses the file itself. Returns a
 * plain string so Larzscript code never touches libssh's enum:
 * "ok" (key matches a known entry), "changed" (key differs from a
 * known entry - the real MITM-detection case, always fail closed on
 * this one), "not_found" (no known_hosts file yet), "unknown" (host not
 * in the file yet - normal on first connect), "other" (a known host
 * but this key TYPE isn't recorded for it), "error". Must be called
 * after ssh_open() (key exchange already happened by then) and before
 * any ssh_auth_*() call. */
static Value bi_ssh_check_host(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[0])||a[1].t!=V_STR) runtime_error(ip,"LarzTypeError","ssh_check_host() expects a session and a known_hosts path");
  ssh_session sess=(ssh_session)(intptr_t)a[0].num;
  if(ssh_options_set(sess,SSH_OPTIONS_KNOWNHOSTS,a[1].str)!=SSH_OK){
    runtime_error(ip,"SshError","could not set known_hosts path: %s",ssh_get_error(sess));
  }
  enum ssh_known_hosts_e state=ssh_session_is_known_server(sess);
  switch(state){
    case SSH_KNOWN_HOSTS_OK:      return V_string("ok");
    case SSH_KNOWN_HOSTS_CHANGED: return V_string("changed");
    case SSH_KNOWN_HOSTS_NOT_FOUND: return V_string("not_found");
    case SSH_KNOWN_HOSTS_UNKNOWN: return V_string("unknown");
    case SSH_KNOWN_HOSTS_OTHER:   return V_string("other");
    default:                      return V_string("error");
  }
}

/* Records the server's CURRENT host key into known_hosts (creating the
 * file/directory if needed) - the caller decides when this is safe to
 * call (e.g. only on "unknown"/"not_found", after showing the user a
 * fingerprint to confirm, or as an explicit trust-on-first-use policy -
 * never automatically on "changed", which would defeat the whole point). */
static Value bi_ssh_trust_host(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[0])||a[1].t!=V_STR) runtime_error(ip,"LarzTypeError","ssh_trust_host() expects a session and a known_hosts path");
  ssh_session sess=(ssh_session)(intptr_t)a[0].num;
  if(ssh_options_set(sess,SSH_OPTIONS_KNOWNHOSTS,a[1].str)!=SSH_OK){
    runtime_error(ip,"SshError","could not set known_hosts path: %s",ssh_get_error(sess));
  }
  if(ssh_session_update_known_hosts(sess)!=SSH_OK){
    runtime_error(ip,"SshError","could not update known_hosts: %s",ssh_get_error(sess));
  }
  return V_nil();
}

static Value bi_ssh_auth_password(Interp *ip, Value *a, int n){
  if(n!=3||!is_num(a[0])||a[1].t!=V_STR||a[2].t!=V_STR) runtime_error(ip,"LarzTypeError","ssh_auth_password() expects a session, username, and password");
  ssh_session sess=(ssh_session)(intptr_t)a[0].num;
  ssh_options_set(sess,SSH_OPTIONS_USER,a[1].str);
  int rc=ssh_userauth_password(sess,NULL,a[2].str);
  if(rc!=SSH_AUTH_SUCCESS) runtime_error(ip,"SshError","authentication failed: %s",ssh_get_error(sess));
  return V_nil();
}

static Value bi_ssh_auth_key(Interp *ip, Value *a, int n){
  if(n!=3||!is_num(a[0])||a[1].t!=V_STR||a[2].t!=V_STR) runtime_error(ip,"LarzTypeError","ssh_auth_key() expects a session, username, and private key path");
  ssh_session sess=(ssh_session)(intptr_t)a[0].num;
  ssh_options_set(sess,SSH_OPTIONS_USER,a[1].str);
  ssh_key key=NULL;
  if(ssh_pki_import_privkey_file(a[2].str,NULL,NULL,NULL,&key)!=SSH_OK){
    runtime_error(ip,"SshError","could not load private key %s",a[2].str);
  }
  int rc=ssh_userauth_publickey(sess,NULL,key);
  ssh_key_free(key);
  if(rc!=SSH_AUTH_SUCCESS) runtime_error(ip,"SshError","authentication failed: %s",ssh_get_error(sess));
  return V_nil();
}

static Value bi_ssh_run(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[0])||a[1].t!=V_STR) runtime_error(ip,"LarzTypeError","ssh_run() expects a session and a command string");
  ssh_session sess=(ssh_session)(intptr_t)a[0].num;
  ssh_channel ch=ssh_channel_new(sess);
  if(!ch) runtime_error(ip,"SshError","could not open a channel: %s",ssh_get_error(sess));
  if(ssh_channel_open_session(ch)!=SSH_OK){
    char msg[256]; snprintf(msg,sizeof msg,"could not open session channel: %s",ssh_get_error(sess));
    ssh_channel_free(ch);
    runtime_error(ip,"SshError","%s",msg);
  }
  if(ssh_channel_request_exec(ch,a[1].str)!=SSH_OK){
    char msg[256]; snprintf(msg,sizeof msg,"exec request failed: %s",ssh_get_error(sess));
    ssh_channel_close(ch); ssh_channel_free(ch);
    runtime_error(ip,"SshError","%s",msg);
  }
  SB outb; outb.s=NULL; outb.n=0; outb.cap=0;
  SB errb; errb.s=NULL; errb.n=0; errb.cap=0;
  char buf[4096];
  int nr;
  while((nr=ssh_channel_read(ch,buf,sizeof(buf),0))>0) for(int i=0;i<nr;i++) sb_putc(&outb,buf[i]);
  while((nr=ssh_channel_read(ch,buf,sizeof(buf),1))>0) for(int i=0;i<nr;i++) sb_putc(&errb,buf[i]);
  int exit_status=ssh_channel_get_exit_status(ch);
  ssh_channel_send_eof(ch);
  ssh_channel_close(ch);
  ssh_channel_free(ch);
  /* mkstr_n (not V_take) - copies exactly outb.n/errb.n bytes, same
   * binary-safety reasoning as socket_read: V_take's strlen() would
   * silently truncate real command output at an embedded NUL byte. */
  Value out_v=mkstr_n(outb.s,outb.n);
  Value err_v=mkstr_n(errb.s,errb.n);
  free(outb.s); free(errb.s);
  Dict *d=dict_new();
  dict_set(d,V_string("stdout"),out_v);
  dict_set(d,V_string("stderr"),err_v);
  dict_set(d,V_string("exit_status"),V_number((double)exit_status));
  return V_dict(d);
}

static Value bi_ssh_close(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_close() expects a session");
  ssh_session sess=(ssh_session)(intptr_t)a[0].num;
  ssh_disconnect(sess);
  ssh_free(sess);
  return V_nil();
}

/* Whether the transport is still up. Needed because ssh_accept_forward()
 * returns nil both when nothing has arrived yet AND when the underlying
 * session has died (see that function's own comment - "not distinguished
 * from a real error here... a real but minor simplification for this
 * first pass") - without this, forward_remote_port()/forward_remote_socks()'s
 * accept loop would spin on a dead connection forever, silently, with no
 * way for a caller's own reconnect-with-backoff logic to ever notice the
 * drop and get a turn to run. */
static Value bi_ssh_is_connected(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_is_connected() expects a session");
  ssh_session sess=(ssh_session)(intptr_t)a[0].num;
  return V_bool(ssh_is_connected(sess)!=0);
}

/* Remote port forwarding (the `ssh -R` equivalent) - low-level channel
 * primitives here, the accept/bridge loop itself lives in Larzscript
 * (packages/ssh's forward_remote_port), the same "C exposes primitives,
 * Larzscript does the orchestration loop" split as tcp.serve() over
 * socket_listen/accept/read/write. libssh channels aren't POSIX file
 * descriptors, so socket_poll() can't multiplex them - ssh_channel_poll
 * below is libssh's own non-blocking-with-timeout readiness check,
 * played the same role socket_poll plays for real sockets. */

/* ssh_channel_listen_forward() sends the tcpip-forward global request and
 * blocks waiting for the server's reply - with NO libssh-level timeout of
 * its own (SSH_OPTIONS_TIMEOUT, set in ssh_open() above, only governs the
 * initial ssh_connect()). Found hanging INDEFINITELY on a real Windows
 * machine even with a fully connected, authenticated session already
 * established to the relay (confirmed via a live packet-level check: the
 * TCP connection sat ESTABLISHED for days while this call never returned)
 * - reproducible on every fresh attempt, not a rare fluke, against a relay
 * account/server independently confirmed to reply correctly to the exact
 * same request from a stock OpenSSH client. Root cause not fully isolated
 * (would need a decrypted packet capture to compare byte-for-byte against
 * OpenSSH's own request), but the fix doesn't need to wait on that: same
 * precedent as ssh_open()'s own connect-timeout above - a socket-level
 * receive timeout means a hang becomes a clean, fast SshError instead of
 * wedging the whole process forever, which is exactly what
 * netbridge.supervise()'s own retry/backoff loop is already built to
 * handle. Timeout is reset to "block forever" afterward (success or
 * failure) - it must NOT stay applied to every later blocking call on
 * this same session/channel (ssh_channel_read_bytes etc. rely on their
 * own explicit poll-then-read pattern, not an implicit socket timeout). */
static Value bi_ssh_listen_forward(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[0])||!is_num(a[1])) runtime_error(ip,"LarzTypeError","ssh_listen_forward() expects a session and a remote port number");
  ssh_session sess=(ssh_session)(intptr_t)a[0].num;
  int port=(int)a[1].num;
  socket_t fd=ssh_get_fd(sess);
#ifdef _WIN32
  DWORD tmo_on=20000, tmo_off=0;
  if(fd!=SSH_INVALID_SOCKET) setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,(const char*)&tmo_on,sizeof(tmo_on));
#else
  struct timeval tmo_on={20,0}, tmo_off={0,0};
  if(fd!=SSH_INVALID_SOCKET) setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tmo_on,sizeof(tmo_on));
#endif
  int rc=ssh_channel_listen_forward(sess,NULL,port,NULL);
  if(fd!=SSH_INVALID_SOCKET){
#ifdef _WIN32
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,(const char*)&tmo_off,sizeof(tmo_off));
#else
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tmo_off,sizeof(tmo_off));
#endif
  }
  if(rc!=SSH_OK){
    runtime_error(ip,"SshError","could not listen on the remote port %d: %s",port,ssh_get_error(sess));
  }
  return V_nil();
}

/* Returns a channel handle, or nil if no incoming forwarded connection
 * arrived within timeout_ms - not distinguished from a real error here
 * (both currently read as "nothing yet, keep polling"), a real but minor
 * simplification for this first pass. */
static Value bi_ssh_accept_forward(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[0])||!is_num(a[1])) runtime_error(ip,"LarzTypeError","ssh_accept_forward() expects a session and a timeout in milliseconds");
  ssh_session sess=(ssh_session)(intptr_t)a[0].num;
  int timeout_ms=(int)a[1].num;
  int dest_port=0;
  ssh_channel ch=ssh_channel_accept_forward(sess,timeout_ms,&dest_port);
  if(!ch) return V_nil();
  return V_number((double)(intptr_t)ch);
}

/* Bridges one forwarded channel to a real TCP target, connect through
 * teardown, ENTIRELY in C with raw buffers and real byte counts - never
 * round-tripping the bulk data through a Larzscript string Value. This
 * matters because Larzscript strings have no stored length (see Str/
 * Value above - just a char* that's always NUL-terminated); any function
 * that needs "how long is this" falls back to strlen(), which stops at
 * the first embedded 0x00. Real protocols forwarded through here (SSH
 * itself, TLS, anything binary) routinely contain 0x00 bytes in their
 * own framing - found via a real live test: forwarding a genuine SSH
 * session through forward_remote_port()'s OLD Larzscript-level
 * ssh_channel_read()/ssh_channel_write()/socket_read()/socket_write()
 * loop let the plain-text banner (no NUL) through fine, then silently
 * truncated the very next binary KEX packet, corrupting the handshake
 * and killing the connection. ssh_channel_shell() already had to solve
 * this exact problem for the interactive-shell case (real fork()+pty,
 * bridged in C) - this is the same fix applied to the forwarding path,
 * not a new technique. Replaces forward_remote_port()'s entire inner
 * loop, including its previous `import "tcp"` dependency (this function
 * dials the TCP target itself, the same getaddrinfo-based connect
 * socket_connect() already uses). */
static Value bi_ssh_bridge_forward(Interp *ip, Value *a, int n){
  if(n!=3||!is_num(a[0])||a[1].t!=V_STR||!is_num(a[2])) runtime_error(ip,"LarzTypeError","ssh_bridge_forward() expects a channel, a target host string, and a target port number");
  ssh_channel ch=(ssh_channel)(intptr_t)a[0].num;
  const char *host=a[1].str;
  int port=(int)a[2].num;

  char portbuf[16]; snprintf(portbuf,sizeof(portbuf),"%d",port);
#ifdef _WIN32
  static int wsa_started=0;
  if(!wsa_started){ WSADATA wsa; WSAStartup(MAKEWORD(2,2),&wsa); wsa_started=1; }
#endif
  struct addrinfo hints; memset(&hints,0,sizeof(hints));
  hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
  struct addrinfo *res=NULL;
  int gai=getaddrinfo(host,portbuf,&hints,&res);
  if(gai!=0||!res) runtime_error(ip,"SocketError","could not resolve host '%s'",host);
  larz_sock_t fd=LARZ_INVALID_SOCK;
  for(struct addrinfo *rp=res; rp; rp=rp->ai_next){
    fd=socket(rp->ai_family,rp->ai_socktype,rp->ai_protocol);
    if(fd==LARZ_INVALID_SOCK) continue;
    if(connect(fd,rp->ai_addr,(int)rp->ai_addrlen)==0) break;
    larz_sock_close(fd); fd=LARZ_INVALID_SOCK;
  }
  freeaddrinfo(res);
  if(fd==LARZ_INVALID_SOCK) runtime_error(ip,"SocketError","could not connect to %s:%s",host,portbuf);

  char buf[16384];
  int broken=0;
  while(!broken){
    int avail=ssh_channel_poll_timeout(ch,20,0);
    if(avail>0){
      int nr=ssh_channel_read_nonblocking(ch,buf,sizeof(buf),0);
      if(nr>0){
        size_t off=0;
        while(off<(size_t)nr){
#ifdef _WIN32
          int w=send(fd,buf+off,(int)((size_t)nr-off),0);
#else
          long w=(long)send(fd,buf+off,(size_t)nr-off,0);
#endif
          if(w<=0){ broken=1; break; }
          off+=(size_t)w;
        }
      }
    }
    if(broken || ssh_channel_is_eof(ch)) break;

    fd_set rfds; FD_ZERO(&rfds); FD_SET(fd,&rfds);
    struct timeval tv={0,20000};
    int sel=select((int)fd+1,&rfds,NULL,NULL,&tv);
    if(sel>0 && FD_ISSET(fd,&rfds)){
#ifdef _WIN32
      int nr2=recv(fd,buf,(int)sizeof(buf),0);
#else
      long nr2=(long)recv(fd,buf,sizeof(buf),0);
#endif
      if(nr2>0){
        size_t sent=0;
        while(sent<(size_t)nr2){
          int w=ssh_channel_write(ch,buf+sent,(uint32_t)((size_t)nr2-sent));
          if(w<=0){ broken=1; break; }
          sent+=(size_t)w;
        }
      } else {
        break;   /* target closed or errored */
      }
    }
  }
  larz_sock_close(fd);
  return V_nil();
}

/* Bytes available to read within timeout_ms (0 = none ready, not an
 * error - the normal case in a poll loop, same convention as
 * socket_poll()'s empty-list-on-timeout). */
static Value bi_ssh_channel_poll(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[0])||!is_num(a[1])) runtime_error(ip,"LarzTypeError","ssh_channel_poll() expects a channel and a timeout in milliseconds");
  ssh_channel ch=(ssh_channel)(intptr_t)a[0].num;
  int timeout_ms=(int)a[1].num;
  int avail=ssh_channel_poll_timeout(ch,timeout_ms,0);
  if(avail<0) return V_number(0);   /* SSH_ERROR or SSH_EOF - caller checks ssh_channel_eof() separately */
  return V_number((double)avail);
}

static Value bi_ssh_channel_read(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[0])||!is_num(a[1])) runtime_error(ip,"LarzTypeError","ssh_channel_read() expects a channel and a max byte count");
  ssh_channel ch=(ssh_channel)(intptr_t)a[0].num;
  int maxlen=(int)a[1].num;
  if(maxlen<0) maxlen=0;
  char *buf=xmalloc((size_t)maxlen>0?(size_t)maxlen:1);
  int nr=ssh_channel_read_nonblocking(ch,buf,(uint32_t)maxlen,0);
  if(nr<0) nr=0;
  Value v=mkstr_n(buf,(size_t)nr);
  free(buf);
  return v;
}

static Value bi_ssh_channel_write(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[0])||a[1].t!=V_STR) runtime_error(ip,"LarzTypeError","ssh_channel_write() expects a channel and a string");
  ssh_channel ch=(ssh_channel)(intptr_t)a[0].num;
  size_t len=strlen(a[1].str);
  size_t sent=0;
  while(sent<len){
    int n_written=ssh_channel_write(ch,a[1].str+sent,(uint32_t)(len-sent));
    if(n_written<=0) runtime_error(ip,"SshError","channel write failed");
    sent+=(size_t)n_written;
  }
  return V_number((double)sent);
}

/* Byte-list variants of the two above - for code that needs to move
 * arbitrary binary payloads through a channel (e.g. hand-building a
 * protocol like SOCKS on top of a forwarded connection, where the
 * traffic itself routinely contains real 0x00 bytes) without the
 * whole-language strlen()-on-Str limitation ssh_channel_read()/write()
 * both still have (see those functions' own comments, and read_file_bytes()/
 * write_file()'s list branch, which solved the identical problem for file
 * I/O the same way: a Larzscript LIST of byte values has a real stored
 * count, unlike Str's bare NUL-terminated char[] with no length field at
 * all - so nothing here ever calls strlen() on the payload). */
static Value bi_ssh_channel_read_bytes(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[0])||!is_num(a[1])) runtime_error(ip,"LarzTypeError","ssh_channel_read_bytes() expects a channel and a max byte count");
  ssh_channel ch=(ssh_channel)(intptr_t)a[0].num;
  int maxlen=(int)a[1].num;
  if(maxlen<0) maxlen=0;
  unsigned char *buf=xmalloc((size_t)maxlen>0?(size_t)maxlen:1);
  int nr=ssh_channel_read_nonblocking(ch,(char*)buf,(uint32_t)maxlen,0);
  if(nr<0) nr=0;
  List *out=list_new();
  for(int i=0;i<nr;i++) list_push(out,V_number((double)buf[i]));
  free(buf);
  return V_list(out);
}

static Value bi_ssh_channel_write_bytes(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[0])||a[1].t!=V_LIST) runtime_error(ip,"LarzTypeError","ssh_channel_write_bytes() expects a channel and a byte list");
  ssh_channel ch=(ssh_channel)(intptr_t)a[0].num;
  int nb=a[1].list->n;
  unsigned char *buf=xmalloc((size_t)nb>0?(size_t)nb:1);
  for(int i=0;i<nb;i++){
    Value it=a[1].list->items[i];
    if(!is_num(it)){ free(buf); runtime_error(ip,"LarzTypeError","ssh_channel_write_bytes(): byte list must contain only numbers"); }
    buf[i]=(unsigned char)((long)it.num & 0xff);
  }
  size_t sent=0;
  while((int)sent<nb){
    int n_written=ssh_channel_write(ch,(const char*)buf+sent,(uint32_t)((size_t)nb-sent));
    if(n_written<=0){ free(buf); runtime_error(ip,"SshError","channel write failed"); }
    sent+=(size_t)n_written;
  }
  free(buf);
  return V_number((double)sent);
}

static Value bi_ssh_channel_eof(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_channel_eof() expects a channel");
  ssh_channel ch=(ssh_channel)(intptr_t)a[0].num;
  return V_bool(ssh_channel_is_eof(ch)!=0);
}

static Value bi_ssh_channel_free(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_channel_free() expects a channel");
  ssh_channel ch=(ssh_channel)(intptr_t)a[0].num;
  ssh_channel_free(ch);
  return V_nil();
}

/* Server role - exec-only for now (no interactive pty/shell), password
 * auth only (no server-side pubkey checking yet). The message-loop
 * primitives below mirror libssh's own "simple" blocking ssh_message_*
 * API rather than its callback-based one, to match this interpreter's
 * single-threaded blocking model - same reasoning `tcp.serve()` already
 * documents for why it's one connection at a time, not concurrent.
 * Channel I/O reuses the exact same ssh_channel_poll/read/write/eof/free
 * builtins the client side already has - an ssh_channel behaves
 * identically regardless of which side opened it. */

static Value bi_ssh_bind_open(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[0])||a[1].t!=V_STR) runtime_error(ip,"LarzTypeError","ssh_bind_open() expects a port number and a host key path");
#ifdef _WIN32
  /* Same fix as bi_ssh_open() above - the server role also has libssh
   * touch a real socket (ssh_bind_listen, later) before anything else on
   * Windows has necessarily called WSAStartup(). */
  static int wsa_started=0;
  if(!wsa_started){ WSADATA wsa; WSAStartup(MAKEWORD(2,2),&wsa); wsa_started=1; }
#endif
  ssh_bind b=ssh_bind_new();
  if(!b) runtime_error(ip,"SshError","could not allocate an ssh_bind");
  unsigned int port=(unsigned int)a[0].num;
  ssh_bind_options_set(b,SSH_BIND_OPTIONS_BINDPORT,&port);
  if(ssh_bind_options_set(b,SSH_BIND_OPTIONS_HOSTKEY,a[1].str)!=SSH_OK){
    char msg[256]; snprintf(msg,sizeof msg,"could not use host key %s: %s",a[1].str,ssh_get_error(b));
    ssh_bind_free(b);
    runtime_error(ip,"SshError","%s",msg);
  }
  if(ssh_bind_listen(b)!=SSH_OK){
    char msg[256]; snprintf(msg,sizeof msg,"could not listen on port %u: %s",port,ssh_get_error(b));
    ssh_bind_free(b);
    runtime_error(ip,"SshError","%s",msg);
  }
  return V_number((double)(intptr_t)b);
}

static Value bi_ssh_bind_accept_session(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_bind_accept_session() expects a bind handle");
  ssh_bind b=(ssh_bind)(intptr_t)a[0].num;
  ssh_session sess=ssh_new();
  if(!sess) runtime_error(ip,"SshError","could not allocate an ssh session");
  if(ssh_bind_accept(b,sess)!=SSH_OK){
    char msg[256]; snprintf(msg,sizeof msg,"accept failed: %s",ssh_get_error(b));
    ssh_free(sess);
    runtime_error(ip,"SshError","%s",msg);
  }
  if(ssh_handle_key_exchange(sess)!=SSH_OK){
    char msg[256]; snprintf(msg,sizeof msg,"key exchange failed: %s",ssh_get_error(sess));
    ssh_disconnect(sess); ssh_free(sess);
    runtime_error(ip,"SshError","%s",msg);
  }
  /* v1 scope: password auth only. */
  ssh_set_auth_methods(sess,SSH_AUTH_METHOD_PASSWORD);
  return V_number((double)(intptr_t)sess);
}

static Value bi_ssh_bind_free(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_bind_free() expects a bind handle");
  ssh_bind b=(ssh_bind)(intptr_t)a[0].num;
  ssh_bind_free(b);
  return V_nil();
}

/* Blocks until the client's next protocol message (auth attempt, channel
 * open, channel request, ...) or returns nil if the session has ended -
 * not distinguished from a real error here, same simplification
 * ssh_accept_forward() already makes on the client side. */
static Value bi_ssh_server_next_message(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_server_next_message() expects a session");
  ssh_session sess=(ssh_session)(intptr_t)a[0].num;
  ssh_message msg=ssh_message_get(sess);
  if(!msg) return V_nil();
  return V_number((double)(intptr_t)msg);
}

/* Categorizes a message into one plain string, so Larzscript code never
 * has to know libssh's own numeric type/subtype enums. */
static Value bi_ssh_msg_type(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_msg_type() expects a message");
  ssh_message msg=(ssh_message)(intptr_t)a[0].num;
  int t=ssh_message_type(msg);
  int st=ssh_message_subtype(msg);
  if(t==SSH_REQUEST_AUTH){
    if(st==SSH_AUTH_METHOD_PASSWORD) return V_string("auth_password");
    return V_string("auth_other");
  }
  if(t==SSH_REQUEST_CHANNEL_OPEN) return V_string("channel_open");
  if(t==SSH_REQUEST_CHANNEL){
    if(st==SSH_CHANNEL_REQUEST_EXEC) return V_string("exec");
    if(st==SSH_CHANNEL_REQUEST_PTY) return V_string("pty");
    if(st==SSH_CHANNEL_REQUEST_SHELL) return V_string("shell");
    return V_string("channel_request_other");
  }
  return V_string("other");
}

static Value bi_ssh_msg_auth_user(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_msg_auth_user() expects a message");
  ssh_message msg=(ssh_message)(intptr_t)a[0].num;
  const char *u=ssh_message_auth_user(msg);
  return V_string(u?u:"");
}

static Value bi_ssh_msg_auth_password(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_msg_auth_password() expects a message");
  ssh_message msg=(ssh_message)(intptr_t)a[0].num;
  const char *p=ssh_message_auth_password(msg);
  return V_string(p?p:"");
}

/* Accepts (frees the message - see file header comment on why). */
static Value bi_ssh_msg_auth_accept(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_msg_auth_accept() expects a message");
  ssh_message msg=(ssh_message)(intptr_t)a[0].num;
  ssh_message_auth_reply_success(msg,0);
  ssh_message_free(msg);
  return V_nil();
}

/* Generic deny/reject for anything this server doesn't want to allow -
 * frees the message. */
static Value bi_ssh_msg_deny(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_msg_deny() expects a message");
  ssh_message msg=(ssh_message)(intptr_t)a[0].num;
  ssh_message_reply_default(msg);
  ssh_message_free(msg);
  return V_nil();
}

/* Accepts a channel-open request, frees the message, returns the new
 * channel handle - the SAME kind of handle ssh_accept_forward() returns
 * on the client side, so ssh_channel_poll/read/write/eof/free all work
 * on it identically. */
static Value bi_ssh_msg_channel_accept(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_msg_channel_accept() expects a message");
  ssh_message msg=(ssh_message)(intptr_t)a[0].num;
  ssh_channel ch=ssh_message_channel_request_open_reply_accept(msg);
  ssh_message_free(msg);
  if(!ch) runtime_error(ip,"SshError","could not accept the channel-open request");
  return V_number((double)(intptr_t)ch);
}

static Value bi_ssh_msg_exec_command(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_msg_exec_command() expects a message");
  ssh_message msg=(ssh_message)(intptr_t)a[0].num;
  const char *cmd=ssh_message_channel_request_command(msg);
  return V_string(cmd?cmd:"");
}

/* The client's requested terminal size on a "pty" message - read these
 * BEFORE ssh_msg_request_accept()'ing the pty request, so the caller can
 * pass them into ssh_channel_shell() to size the pty correctly from the
 * start (a wrong initial size garbles full-screen terminal apps like
 * vim/htop until the user manually resizes). */
static Value bi_ssh_msg_pty_width(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_msg_pty_width() expects a message");
  ssh_message msg=(ssh_message)(intptr_t)a[0].num;
  return V_number((double)ssh_message_channel_request_pty_width(msg));
}
static Value bi_ssh_msg_pty_height(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_msg_pty_height() expects a message");
  ssh_message msg=(ssh_message)(intptr_t)a[0].num;
  return V_number((double)ssh_message_channel_request_pty_height(msg));
}

/* Accepts a channel REQUEST (exec/shell/pty - the sub-request within an
 * already-open channel), frees the message. */
static Value bi_ssh_msg_request_accept(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_msg_request_accept() expects a message");
  ssh_message msg=(ssh_message)(intptr_t)a[0].num;
  ssh_message_channel_request_reply_success(msg);
  ssh_message_free(msg);
  return V_nil();
}

static Value bi_ssh_channel_send_exit_status(Interp *ip, Value *a, int n){
  if(n!=2||!is_num(a[0])||!is_num(a[1])) runtime_error(ip,"LarzTypeError","ssh_channel_send_exit_status() expects a channel and an exit code");
  ssh_channel ch=(ssh_channel)(intptr_t)a[0].num;
  ssh_channel_request_send_exit_status(ch,(int)a[1].num);
  return V_nil();
}

/* Which channel a channel-request message (exec/pty/shell) belongs to -
 * ssh_server_next_message() polls at the session level, not per-channel,
 * so this is how the caller knows which already-open channel an "exec"
 * message is actually for. */
static Value bi_ssh_msg_channel(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_msg_channel() expects a message");
  ssh_message msg=(ssh_message)(intptr_t)a[0].num;
  ssh_channel ch=ssh_message_channel_request_channel(msg);
  if(!ch) return V_nil();
  return V_number((double)(intptr_t)ch);
}

/* Proper teardown before ssh_channel_free() - send_eof (tells the other
 * end no more data is coming) then close, the same sequence ssh_run()
 * already does internally on the client side. */
static Value bi_ssh_channel_close(Interp *ip, Value *a, int n){
  if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ssh_channel_close() expects a channel");
  ssh_channel ch=(ssh_channel)(intptr_t)a[0].num;
  ssh_channel_send_eof(ch);
  ssh_channel_close(ch);
  return V_nil();
}

/* Interactive shell for the server role - real fork()+forkpty() (POSIX
 * only - see the header comment near the pty includes for why Windows
 * doesn't have this yet; exec via ssh_run()/the "exec" message type is
 * unaffected and already works everywhere). Spawns the user's real
 * login shell attached to a real pseudoterminal sized cols x rows (from
 * the client's pty request - ssh_msg_pty_width/height above), then
 * blocks bridging data both ways until the channel closes or the shell
 * exits. Returns the shell's real exit code. One call does the whole
 * session - matches ssh_run()'s "C does the loop, Larzscript just calls
 * it" shape, since this interpreter has no non-blocking I/O primitives
 * at the Larzscript level fine-grained enough for interactive typing
 * latency. */
#ifdef LARZ_HAVE_PTY
static Value bi_ssh_channel_shell(Interp *ip, Value *a, int n){
  if(n!=3||!is_num(a[0])||!is_num(a[1])||!is_num(a[2])) runtime_error(ip,"LarzTypeError","ssh_channel_shell() expects a channel, terminal width, and terminal height");
  ssh_channel ch=(ssh_channel)(intptr_t)a[0].num;
  int cols=(int)a[1].num, rows=(int)a[2].num;
  if(cols<=0) cols=80;
  if(rows<=0) rows=24;

  struct winsize ws; memset(&ws,0,sizeof ws);
  ws.ws_col=(unsigned short)cols; ws.ws_row=(unsigned short)rows;

  int master_fd;
  pid_t pid=forkpty(&master_fd,NULL,NULL,&ws);
  if(pid<0) runtime_error(ip,"SshError","forkpty failed: %s",strerror(errno));
  if(pid==0){
    /* child: a real interactive login shell, attached to the pty slave
     * as its controlling terminal by forkpty()'s own login_tty() call. */
    const char *shell=getenv("SHELL");
    if(!shell||!*shell) shell="/bin/sh";
    execl(shell,shell,"-i",(char*)NULL);
    _exit(127);
  }

  /* parent: bridge channel <-> pty master until the channel closes or
   * the child exits - same shape as forward_remote_port()'s own poll
   * loop, just with a pty fd instead of a plain TCP socket on the other
   * side, and living entirely in C instead of Larzscript (the interpreter
   * has no non-blocking read/write at the Larzscript level fine-grained
   * enough for this). */
  int status=0, child_exited=0;
  char buf[4096];
  for(;;){
    int avail=ssh_channel_poll_timeout(ch,20,0);
    if(avail>0){
      int nr=ssh_channel_read_nonblocking(ch,buf,sizeof(buf),0);
      if(nr>0){
        ssize_t off=0;
        while(off<nr){ ssize_t w=write(master_fd,buf+off,(size_t)(nr-off)); if(w<=0) break; off+=w; }
      }
    }
    if(ssh_channel_is_eof(ch)) break;

    fd_set rfds; FD_ZERO(&rfds); FD_SET(master_fd,&rfds);
    struct timeval tv={0,20000};
    int sel=select(master_fd+1,&rfds,NULL,NULL,&tv);
    if(sel>0 && FD_ISSET(master_fd,&rfds)){
      ssize_t nr=read(master_fd,buf,sizeof(buf));
      if(nr>0){
        size_t sent=0;
        while(sent<(size_t)nr){
          int w=ssh_channel_write(ch,buf+sent,(uint32_t)((size_t)nr-sent));
          if(w<=0) break;
          sent+=(size_t)w;
        }
      } else if(nr==0 || (nr<0 && errno!=EAGAIN && errno!=EINTR)){
        break;   /* pty closed - shell exited (or its controlling process did) */
      }
    }

    pid_t w=waitpid(pid,&status,WNOHANG);
    if(w==pid){ child_exited=1; break; }
  }
  if(!child_exited){
    kill(pid,SIGHUP);
    waitpid(pid,&status,0);
  }
  close(master_fd);
  int exit_code=WIFEXITED(status)?WEXITSTATUS(status):1;
  return V_number((double)exit_code);
}
#else
static Value bi_ssh_channel_shell(Interp *ip, Value *a, int n){
  (void)a; (void)n;
  runtime_error(ip,"SshError","interactive shell is not available in this build (no pty support on this platform yet - exec commands still work via ssh_run()/the exec message type)");
  return V_nil();
}
#endif
#endif /* LARZ_HAVE_LIBSSH */
#endif /* hosted */

static Builtin B_print = {"print", bi_print};
static Builtin B_money = {"money", bi_money};
static Builtin B_len   = {"len",   bi_len};
static Builtin B_push  = {"push",  bi_push};
static Builtin B_range = {"range", bi_range};
static Builtin B_range_to = {"range_to", bi_range_to};
static Builtin B_str={"str",bi_str}, B_int={"int",bi_int}, B_float={"float",bi_float}, B_bool={"bool",bi_bool}, B_type={"type",bi_type};
static Builtin B_abs={"abs",bi_abs}, B_min={"min",bi_min}, B_max={"max",bi_max}, B_sum={"sum",bi_sum};
static Builtin B_sorted={"sorted",bi_sorted}, B_reversed={"reversed",bi_reversed};
static Builtin B_floor={"floor",bi_floor}, B_ceil={"ceil",bi_ceil}, B_round={"round",bi_round}, B_sqrt={"sqrt",bi_sqrt}, B_pow={"pow",bi_pow};
static Builtin B_native_biquad_process_buffer={"_native_biquad_process_buffer",bi_native_biquad_process_buffer};
static Builtin B_native_compressor_process_buffer={"_native_compressor_process_buffer",bi_native_compressor_process_buffer};
static Builtin B_native_limit_buffer={"_native_limit_buffer",bi_native_limit_buffer};
static Builtin B_native_peak_abs={"_native_peak_abs",bi_native_peak_abs};
static Builtin B_native_sum_sq={"_native_sum_sq",bi_native_sum_sq};
static Builtin B_native_scale_buffer={"_native_scale_buffer",bi_native_scale_buffer};
static Builtin B_native_mix_add={"_native_mix_add",bi_native_mix_add};
static Builtin B_native_fade_buffer={"_native_fade_buffer",bi_native_fade_buffer};
static Builtin B_native_find_first_above={"_native_find_first_above",bi_native_find_first_above};
static Builtin B_native_sidechain_process_buffer={"_native_sidechain_process_buffer",bi_native_sidechain_process_buffer};
static Builtin B_native_master_block={"_native_master_block",bi_native_master_block};
static Builtin B_native_saturate_buffer={"_native_saturate_buffer",bi_native_saturate_buffer};
static Builtin B_native_stereo_widen={"_native_stereo_widen",bi_native_stereo_widen};
static Builtin B_native_delay_process_buffer={"_native_delay_process_buffer",bi_native_delay_process_buffer};
static Builtin B_native_detect_pitch_track={"_native_detect_pitch_track",bi_native_detect_pitch_track};
static Builtin B_native_psola_shift={"_native_psola_shift",bi_native_psola_shift};
static Builtin B_file_size={"file_size",bi_file_size};
static Builtin B_read_file_bytes_range={"read_file_bytes_range",bi_read_file_bytes_range};
static Builtin B_patch_file_bytes={"patch_file_bytes",bi_patch_file_bytes};
static Builtin B_native_pcm16_encode={"_native_pcm16_encode",bi_native_pcm16_encode};
static Builtin B_native_pcm16_decode={"_native_pcm16_decode",bi_native_pcm16_decode};
static Builtin B_chr={"chr",bi_chr}, B_ord={"ord",bi_ord}, B_assert={"assert",bi_assert}, B_input={"input",bi_input};
static Builtin B_keys={"keys",bi_keys}, B_values={"values",bi_values};
static Builtin B_map={"map",bi_map}, B_filter={"filter",bi_filter}, B_reduce={"reduce",bi_reduce}, B_join={"join",bi_join}, B_enumerate={"enumerate",bi_enumerate};
static Builtin B_zip={"zip",bi_zip}, B_read_file={"read_file",bi_read_file}, B_read_file_bytes={"read_file_bytes",bi_read_file_bytes}, B_write_file={"write_file",bi_write_file}, B_append_file={"append_file",bi_append_file}, B_file_exists={"file_exists",bi_file_exists}, B_exit={"exit",bi_exit};
static Builtin B_all={"all",bi_all}, B_any={"any",bi_any}, B_count={"count",bi_count}, B_unique={"unique",bi_unique};
static Builtin B_hex={"hex",bi_hex}, B_bin={"bin",bi_bin}, B_oct={"oct",bi_oct}, B_gcd={"gcd",bi_gcd}, B_factorial={"factorial",bi_factorial}, B_sign={"sign",bi_sign}, B_clamp={"clamp",bi_clamp}, B_list={"list",bi_list}, B_dict={"dict",bi_dict};
static Builtin B_env={"env",bi_env}, B_run={"run",bi_run}, B_capture={"capture",bi_capture}, B_cwd={"cwd",bi_cwd}, B_chdir={"chdir",bi_chdir}, B_listdir={"listdir",bi_listdir}, B_mkdir={"mkdir",bi_mkdir}, B_remove={"remove",bi_remove}, B_rename={"rename",bi_rename}, B_time={"time",bi_time}, B_clock={"clock",bi_clock}, B_sleep={"sleep",bi_sleep};
#if !defined(__STDC_HOSTED__) || __STDC_HOSTED__
static Builtin B_socket_listen={"socket_listen",bi_socket_listen}, B_socket_accept={"socket_accept",bi_socket_accept}, B_socket_read={"socket_read",bi_socket_read}, B_socket_write={"socket_write",bi_socket_write}, B_socket_read_bytes={"socket_read_bytes",bi_socket_read_bytes}, B_socket_write_bytes={"socket_write_bytes",bi_socket_write_bytes}, B_socket_close={"socket_close",bi_socket_close}, B_socket_connect={"socket_connect",bi_socket_connect}, B_socket_poll={"socket_poll",bi_socket_poll};
static Builtin B_ssh_open={"ssh_open",bi_ssh_open}, B_ssh_auth_password={"ssh_auth_password",bi_ssh_auth_password}, B_ssh_auth_key={"ssh_auth_key",bi_ssh_auth_key}, B_ssh_run={"ssh_run",bi_ssh_run}, B_ssh_close={"ssh_close",bi_ssh_close}, B_ssh_is_connected={"ssh_is_connected",bi_ssh_is_connected};
static Builtin B_ssh_check_host={"ssh_check_host",bi_ssh_check_host}, B_ssh_trust_host={"ssh_trust_host",bi_ssh_trust_host};
static Builtin B_ssh_bridge_forward={"ssh_bridge_forward",bi_ssh_bridge_forward};
static Builtin B_ssh_listen_forward={"ssh_listen_forward",bi_ssh_listen_forward}, B_ssh_accept_forward={"ssh_accept_forward",bi_ssh_accept_forward}, B_ssh_channel_poll={"ssh_channel_poll",bi_ssh_channel_poll}, B_ssh_channel_read={"ssh_channel_read",bi_ssh_channel_read}, B_ssh_channel_write={"ssh_channel_write",bi_ssh_channel_write}, B_ssh_channel_read_bytes={"ssh_channel_read_bytes",bi_ssh_channel_read_bytes}, B_ssh_channel_write_bytes={"ssh_channel_write_bytes",bi_ssh_channel_write_bytes}, B_ssh_channel_eof={"ssh_channel_eof",bi_ssh_channel_eof}, B_ssh_channel_free={"ssh_channel_free",bi_ssh_channel_free};
static Builtin B_ssh_bind_open={"ssh_bind_open",bi_ssh_bind_open}, B_ssh_bind_accept_session={"ssh_bind_accept_session",bi_ssh_bind_accept_session}, B_ssh_bind_free={"ssh_bind_free",bi_ssh_bind_free}, B_ssh_server_next_message={"ssh_server_next_message",bi_ssh_server_next_message}, B_ssh_msg_type={"ssh_msg_type",bi_ssh_msg_type}, B_ssh_msg_auth_user={"ssh_msg_auth_user",bi_ssh_msg_auth_user}, B_ssh_msg_auth_password={"ssh_msg_auth_password",bi_ssh_msg_auth_password}, B_ssh_msg_auth_accept={"ssh_msg_auth_accept",bi_ssh_msg_auth_accept}, B_ssh_msg_deny={"ssh_msg_deny",bi_ssh_msg_deny}, B_ssh_msg_channel_accept={"ssh_msg_channel_accept",bi_ssh_msg_channel_accept}, B_ssh_msg_exec_command={"ssh_msg_exec_command",bi_ssh_msg_exec_command}, B_ssh_msg_request_accept={"ssh_msg_request_accept",bi_ssh_msg_request_accept}, B_ssh_channel_send_exit_status={"ssh_channel_send_exit_status",bi_ssh_channel_send_exit_status}, B_ssh_msg_channel={"ssh_msg_channel",bi_ssh_msg_channel}, B_ssh_channel_close={"ssh_channel_close",bi_ssh_channel_close};
static Builtin B_ssh_msg_pty_width={"ssh_msg_pty_width",bi_ssh_msg_pty_width}, B_ssh_msg_pty_height={"ssh_msg_pty_height",bi_ssh_msg_pty_height}, B_ssh_channel_shell={"ssh_channel_shell",bi_ssh_channel_shell};
#endif
static Builtin B_regex_match={"regex_match",bi_regex_match}, B_regex_find={"regex_find",bi_regex_find}, B_regex_replace={"regex_replace",bi_regex_replace}, B_regex_split={"regex_split",bi_regex_split};
static Builtin B_date={"date",bi_date}, B_datetime={"datetime",bi_datetime};

/* ===================== REPL ===================== */
/* net open brackets in s, ignoring string contents and comments (for the REPL) */
static int bracket_depth(const char *s){
  int depth=0, i=0;
  while(s[i]){
    char c=s[i];
    if(c=='#'){ while(s[i] && s[i]!='\n') i++; continue; }
    if(c=='"'){ i++; while(s[i] && s[i]!='"'){ if(s[i]=='\\' && s[i+1]) i+=2; else i++; } if(s[i]) i++; continue; }
    if(c=='{'||c=='('||c=='[') depth++;
    else if(c=='}'||c==')'||c==']') depth--;
    i++;
  }
  return depth;
}
static void repl(Interp *ip){
  char line[8192];
  static char buf[1<<16]; buf[0]=0;
  printf("Larzscript native REPL (v" LARZSCRIPT_VERSION ") - type statements; Ctrl-D to exit.\n"
         "Definitions can span multiple lines; the '..... ' prompt means more is expected.\n");
  for(;;){
    printf(buf[0] ? "..... " : "larz> "); fflush(stdout);
    if(!fgets(line, sizeof line, stdin)){ printf("\n"); break; }
    if(strlen(buf)+strlen(line)+1 >= sizeof buf){ fprintf(stderr,"input too long\n"); buf[0]=0; continue; }
    strcat(buf, line);
    if(bracket_depth(buf) > 0) continue;            /* keep reading until brackets balance */
    if(setjmp(g_err)){ fprintf(stderr,"SyntaxError: %s\n", g_errmsg); buf[0]=0; continue; }
    if(setjmp(ip->jb)){ fprintf(stderr,"%s: %s\n", ip->errname, ip->errmsg); buf[0]=0; continue; }
    Token *toks = lex(buf);
    Node *prog = parse_program(toks);
    ip->returning = 0;
    for(int i=0;i<prog->nkids;i++){
      Node *st = prog->kids[i];
      if(st->kind==N_EXPR){                        /* echo expression results */
        Value v = eval(ip, st->a, ip->globals);
        if(v.t!=V_NIL){ print_value(v); printf("\n"); }
      } else {
        exec(ip, st, ip->globals);
      }
    }
    buf[0]=0;
  }
}

/* ===================== main ===================== */
static char *read_all(const char *path){
  FILE *f = path ? fopen(path,"rb") : stdin;
  if(!f){ fprintf(stderr,"larzscript: cannot open %s\n", path); exit(1); }
  size_t cap=1<<16, len=0; char *buf=xmalloc(cap);
  size_t r;
  while((r=fread(buf+len,1,cap-len,f))>0){ len+=r; if(len==cap){ cap*=2; buf=realloc(buf,cap); } }
  buf[len]=0;
  if(path) fclose(f);
  return buf;
}

static void define_builtins(Env *g){
  env_define(g, "print", V_builtin(&B_print));
  env_define(g, "money", V_builtin(&B_money));
  env_define(g, "len",   V_builtin(&B_len));
  env_define(g, "push",  V_builtin(&B_push));
  env_define(g, "range", V_builtin(&B_range));
  env_define(g, "range_to", V_builtin(&B_range_to));
  env_define(g, "str",   V_builtin(&B_str));
  env_define(g, "int",   V_builtin(&B_int));
  env_define(g, "float", V_builtin(&B_float));
  env_define(g, "bool",  V_builtin(&B_bool));
  env_define(g, "type",  V_builtin(&B_type));
  env_define(g, "abs",   V_builtin(&B_abs));
  env_define(g, "min",   V_builtin(&B_min));
  env_define(g, "max",   V_builtin(&B_max));
  env_define(g, "sum",   V_builtin(&B_sum));
  env_define(g, "sorted",   V_builtin(&B_sorted));
  env_define(g, "reversed", V_builtin(&B_reversed));
  env_define(g, "floor", V_builtin(&B_floor));
  env_define(g, "ceil",  V_builtin(&B_ceil));
  env_define(g, "round", V_builtin(&B_round));
  env_define(g, "sqrt",  V_builtin(&B_sqrt));
  env_define(g, "pow",   V_builtin(&B_pow));
  env_define(g, "_native_biquad_process_buffer", V_builtin(&B_native_biquad_process_buffer));
  env_define(g, "_native_compressor_process_buffer", V_builtin(&B_native_compressor_process_buffer));
  env_define(g, "_native_limit_buffer", V_builtin(&B_native_limit_buffer));
  env_define(g, "_native_peak_abs", V_builtin(&B_native_peak_abs));
  env_define(g, "_native_sum_sq", V_builtin(&B_native_sum_sq));
  env_define(g, "_native_scale_buffer", V_builtin(&B_native_scale_buffer));
  env_define(g, "_native_mix_add", V_builtin(&B_native_mix_add));
  env_define(g, "_native_fade_buffer", V_builtin(&B_native_fade_buffer));
  env_define(g, "_native_find_first_above", V_builtin(&B_native_find_first_above));
  env_define(g, "_native_sidechain_process_buffer", V_builtin(&B_native_sidechain_process_buffer));
  env_define(g, "_native_master_block", V_builtin(&B_native_master_block));
  env_define(g, "_native_saturate_buffer", V_builtin(&B_native_saturate_buffer));
  env_define(g, "_native_stereo_widen", V_builtin(&B_native_stereo_widen));
  env_define(g, "_native_delay_process_buffer", V_builtin(&B_native_delay_process_buffer));
  env_define(g, "_native_detect_pitch_track", V_builtin(&B_native_detect_pitch_track));
  env_define(g, "_native_psola_shift", V_builtin(&B_native_psola_shift));
  env_define(g, "file_size", V_builtin(&B_file_size));
  env_define(g, "read_file_bytes_range", V_builtin(&B_read_file_bytes_range));
  env_define(g, "patch_file_bytes", V_builtin(&B_patch_file_bytes));
  env_define(g, "_native_pcm16_encode", V_builtin(&B_native_pcm16_encode));
  env_define(g, "_native_pcm16_decode", V_builtin(&B_native_pcm16_decode));
  env_define(g, "chr",   V_builtin(&B_chr));
  env_define(g, "ord",   V_builtin(&B_ord));
  env_define(g, "assert",V_builtin(&B_assert));
  env_define(g, "input", V_builtin(&B_input));
  env_define(g, "keys",  V_builtin(&B_keys));
  env_define(g, "values",V_builtin(&B_values));
  env_define(g, "map",    V_builtin(&B_map));
  env_define(g, "filter", V_builtin(&B_filter));
  env_define(g, "reduce", V_builtin(&B_reduce));
  env_define(g, "join",   V_builtin(&B_join));
  env_define(g, "enumerate", V_builtin(&B_enumerate));
  env_define(g, "zip",    V_builtin(&B_zip));
  env_define(g, "read_file",   V_builtin(&B_read_file));
  env_define(g, "read_file_bytes", V_builtin(&B_read_file_bytes));
  env_define(g, "write_file",  V_builtin(&B_write_file));
  env_define(g, "append_file", V_builtin(&B_append_file));
  env_define(g, "file_exists", V_builtin(&B_file_exists));
  env_define(g, "exit",   V_builtin(&B_exit));
  env_define(g, "all",    V_builtin(&B_all));
  env_define(g, "any",    V_builtin(&B_any));
  env_define(g, "count",  V_builtin(&B_count));
  env_define(g, "unique", V_builtin(&B_unique));
  env_define(g, "hex",    V_builtin(&B_hex));
  env_define(g, "bin",    V_builtin(&B_bin));
  env_define(g, "oct",    V_builtin(&B_oct));
  env_define(g, "gcd",    V_builtin(&B_gcd));
  env_define(g, "factorial", V_builtin(&B_factorial));
  env_define(g, "sign",   V_builtin(&B_sign));
  env_define(g, "clamp",  V_builtin(&B_clamp));
  env_define(g, "list",   V_builtin(&B_list));
  env_define(g, "dict",   V_builtin(&B_dict));
  env_define(g, "env",    V_builtin(&B_env));
  env_define(g, "run",    V_builtin(&B_run));
  env_define(g, "capture",V_builtin(&B_capture));
  env_define(g, "cwd",    V_builtin(&B_cwd));
  env_define(g, "chdir",  V_builtin(&B_chdir));
  env_define(g, "listdir",V_builtin(&B_listdir));
  env_define(g, "mkdir",  V_builtin(&B_mkdir));
  env_define(g, "remove", V_builtin(&B_remove));
  env_define(g, "rename", V_builtin(&B_rename));
  env_define(g, "time",   V_builtin(&B_time));
  env_define(g, "clock",  V_builtin(&B_clock));
  env_define(g, "sleep",  V_builtin(&B_sleep));
#if !defined(__STDC_HOSTED__) || __STDC_HOSTED__
  env_define(g, "socket_listen", V_builtin(&B_socket_listen));
  env_define(g, "socket_accept", V_builtin(&B_socket_accept));
  env_define(g, "socket_read",   V_builtin(&B_socket_read));
  env_define(g, "socket_write",  V_builtin(&B_socket_write));
  env_define(g, "socket_read_bytes",  V_builtin(&B_socket_read_bytes));
  env_define(g, "socket_write_bytes",  V_builtin(&B_socket_write_bytes));
  env_define(g, "socket_close",  V_builtin(&B_socket_close));
  env_define(g, "socket_connect",V_builtin(&B_socket_connect));
  env_define(g, "socket_poll",   V_builtin(&B_socket_poll));
  env_define(g, "ssh_open",         V_builtin(&B_ssh_open));
  env_define(g, "ssh_auth_password",V_builtin(&B_ssh_auth_password));
  env_define(g, "ssh_auth_key",     V_builtin(&B_ssh_auth_key));
  env_define(g, "ssh_run",          V_builtin(&B_ssh_run));
  env_define(g, "ssh_close",        V_builtin(&B_ssh_close));
  env_define(g, "ssh_is_connected", V_builtin(&B_ssh_is_connected));
  env_define(g, "ssh_check_host",   V_builtin(&B_ssh_check_host));
  env_define(g, "ssh_trust_host",   V_builtin(&B_ssh_trust_host));
  env_define(g, "ssh_bridge_forward", V_builtin(&B_ssh_bridge_forward));
  env_define(g, "ssh_listen_forward",V_builtin(&B_ssh_listen_forward));
  env_define(g, "ssh_accept_forward",V_builtin(&B_ssh_accept_forward));
  env_define(g, "ssh_channel_poll",  V_builtin(&B_ssh_channel_poll));
  env_define(g, "ssh_channel_read",  V_builtin(&B_ssh_channel_read));
  env_define(g, "ssh_channel_write", V_builtin(&B_ssh_channel_write));
  env_define(g, "ssh_channel_read_bytes", V_builtin(&B_ssh_channel_read_bytes));
  env_define(g, "ssh_channel_write_bytes", V_builtin(&B_ssh_channel_write_bytes));
  env_define(g, "ssh_channel_eof",   V_builtin(&B_ssh_channel_eof));
  env_define(g, "ssh_channel_free",  V_builtin(&B_ssh_channel_free));
  env_define(g, "ssh_bind_open",           V_builtin(&B_ssh_bind_open));
  env_define(g, "ssh_bind_accept_session", V_builtin(&B_ssh_bind_accept_session));
  env_define(g, "ssh_bind_free",           V_builtin(&B_ssh_bind_free));
  env_define(g, "ssh_server_next_message", V_builtin(&B_ssh_server_next_message));
  env_define(g, "ssh_msg_type",            V_builtin(&B_ssh_msg_type));
  env_define(g, "ssh_msg_auth_user",       V_builtin(&B_ssh_msg_auth_user));
  env_define(g, "ssh_msg_auth_password",   V_builtin(&B_ssh_msg_auth_password));
  env_define(g, "ssh_msg_auth_accept",     V_builtin(&B_ssh_msg_auth_accept));
  env_define(g, "ssh_msg_deny",            V_builtin(&B_ssh_msg_deny));
  env_define(g, "ssh_msg_channel_accept",  V_builtin(&B_ssh_msg_channel_accept));
  env_define(g, "ssh_msg_exec_command",    V_builtin(&B_ssh_msg_exec_command));
  env_define(g, "ssh_msg_request_accept",  V_builtin(&B_ssh_msg_request_accept));
  env_define(g, "ssh_channel_send_exit_status", V_builtin(&B_ssh_channel_send_exit_status));
  env_define(g, "ssh_msg_channel",         V_builtin(&B_ssh_msg_channel));
  env_define(g, "ssh_channel_close",       V_builtin(&B_ssh_channel_close));
  env_define(g, "ssh_msg_pty_width",       V_builtin(&B_ssh_msg_pty_width));
  env_define(g, "ssh_msg_pty_height",      V_builtin(&B_ssh_msg_pty_height));
  env_define(g, "ssh_channel_shell",       V_builtin(&B_ssh_channel_shell));
#endif
  env_define(g, "regex_match",   V_builtin(&B_regex_match));
  env_define(g, "regex_find",    V_builtin(&B_regex_find));
  env_define(g, "regex_replace", V_builtin(&B_regex_replace));
  env_define(g, "regex_split",   V_builtin(&B_regex_split));
  env_define(g, "date",     V_builtin(&B_date));
  env_define(g, "datetime", V_builtin(&B_datetime));
#ifdef __EMSCRIPTEN__
  register_ui_module(g);
#endif
#if defined(__STDC_HOSTED__) && !__STDC_HOSTED__
  register_ui_module(g);
#endif
}

static void install_builtins(Interp *ip){
  ip->globals = env_new(NULL);
  ip->has_gas = 0;                 /* unlimited by default */
  define_builtins(ip->globals);
}

#ifdef __EMSCRIPTEN__
/* ===================== browser: `ui` module + callback bridge ===================== *
 * Larzscript running client-side via WASM - see native/web/larzscript-web.js for
 * the page-side bootstrap that loads this module and feeds it
 * <script type="text/larzscript"> tags. `ui.*` are deliberately small,
 * money-native-flavored primitives, not a DOM clone: element get/set + one
 * genuinely new mechanism (ui.on's callback bridge). Anything higher-level -
 * a "pay button", a live balance badge - is ordinary Larzscript composing
 * these with wallet/pay/require; no UI framework is baked into C. */
EM_JS(void, ui_js_set_text, (const char *sel, const char *text), {
  document.querySelectorAll(UTF8ToString(sel)).forEach(function(el){ el.textContent = UTF8ToString(text); });
});
EM_JS(char*, ui_js_get_text, (const char *sel), {
  var el = document.querySelector(UTF8ToString(sel));
  var s = el ? el.textContent : "";
  var len = lengthBytesUTF8(s) + 1;
  var ptr = _malloc(len);
  stringToUTF8(s, ptr, len);
  return ptr;
});
EM_JS(char*, ui_js_get_value, (const char *sel), {
  var el = document.querySelector(UTF8ToString(sel));
  var s = (el && ('value' in el)) ? el.value : "";
  var len = lengthBytesUTF8(s) + 1;
  var ptr = _malloc(len);
  stringToUTF8(s, ptr, len);
  return ptr;
});
EM_JS(void, ui_js_set_value, (const char *sel, const char *v), {
  document.querySelectorAll(UTF8ToString(sel)).forEach(function(el){ el.value = UTF8ToString(v); });
});
EM_JS(void, ui_js_set_html, (const char *sel, const char *html), {
  document.querySelectorAll(UTF8ToString(sel)).forEach(function(el){ el.innerHTML = UTF8ToString(html); });
});
EM_JS(void, ui_js_add_class, (const char *sel, const char *cls), {
  document.querySelectorAll(UTF8ToString(sel)).forEach(function(el){ el.classList.add(UTF8ToString(cls)); });
});
EM_JS(void, ui_js_remove_class, (const char *sel, const char *cls), {
  document.querySelectorAll(UTF8ToString(sel)).forEach(function(el){ el.classList.remove(UTF8ToString(cls)); });
});
EM_JS(void, ui_js_on, (const char *sel, const char *event, int cb_id), {
  var ev = UTF8ToString(event);
  document.querySelectorAll(UTF8ToString(sel)).forEach(function(el){
    el.addEventListener(ev, function(){ Module.ccall('larz_invoke_callback', null, ['number'], [cb_id]); });
  });
});
/* ui.fetch(url, fn) - fn(status, body) is called once the request settles.
 * A network error (no response at all, e.g. offline/CORS) calls fn(0, message)
 * rather than dropping the callback silently - fn always fires exactly once. */
EM_JS(void, ui_js_fetch, (const char *url, int cb_id), {
  var u = UTF8ToString(url);
  fetch(u).then(function(resp){
    return resp.text().then(function(body){ return [resp.status, body]; });
  }).catch(function(err){
    return [0, String(err)];
  }).then(function(pair){
    var body = pair[1];
    var len = lengthBytesUTF8(body) + 1;
    var ptr = _malloc(len);
    stringToUTF8(body, ptr, len);
    Module.ccall('larz_invoke_fetch_callback', null, ['number','number','number'], [cb_id, pair[0], ptr]);
    _free(ptr);
  });
});

static Value bi_ui_set_text(Interp *ip, Value *a, int n){
  if(n!=2||a[0].t!=V_STR||a[1].t!=V_STR) runtime_error(ip,"LarzTypeError","ui.set_text() expects (selector, text)");
  ui_js_set_text(a[0].str, a[1].str); return V_nil();
}
static Value bi_ui_get_text(Interp *ip, Value *a, int n){
  if(n!=1||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","ui.get_text() expects (selector)");
  return V_take(ui_js_get_text(a[0].str));
}
static Value bi_ui_get_value(Interp *ip, Value *a, int n){
  if(n!=1||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","ui.get_value() expects (selector)");
  return V_take(ui_js_get_value(a[0].str));
}
static Value bi_ui_set_value(Interp *ip, Value *a, int n){
  if(n!=2||a[0].t!=V_STR||a[1].t!=V_STR) runtime_error(ip,"LarzTypeError","ui.set_value() expects (selector, value)");
  ui_js_set_value(a[0].str, a[1].str); return V_nil();
}
static Value bi_ui_set_html(Interp *ip, Value *a, int n){
  if(n!=2||a[0].t!=V_STR||a[1].t!=V_STR) runtime_error(ip,"LarzTypeError","ui.set_html() expects (selector, html)");
  ui_js_set_html(a[0].str, a[1].str); return V_nil();
}
static Value bi_ui_add_class(Interp *ip, Value *a, int n){
  if(n!=2||a[0].t!=V_STR||a[1].t!=V_STR) runtime_error(ip,"LarzTypeError","ui.add_class() expects (selector, class)");
  ui_js_add_class(a[0].str, a[1].str); return V_nil();
}
static Value bi_ui_remove_class(Interp *ip, Value *a, int n){
  if(n!=2||a[0].t!=V_STR||a[1].t!=V_STR) runtime_error(ip,"LarzTypeError","ui.remove_class() expects (selector, class)");
  ui_js_remove_class(a[0].str, a[1].str); return V_nil();
}
static Value bi_ui_on(Interp *ip, Value *a, int n){
  if(n!=3||a[0].t!=V_STR||a[1].t!=V_STR||(a[2].t!=V_FUNC&&a[2].t!=V_BUILTIN))
    runtime_error(ip,"LarzTypeError","ui.on() expects (selector, event, function)");
  if(g_ui_ncb==g_ui_cbcap){ g_ui_cbcap=g_ui_cbcap?g_ui_cbcap*2:16; g_ui_callbacks=realloc(g_ui_callbacks,g_ui_cbcap*sizeof(Value)); }
  int id=g_ui_ncb++;
  g_ui_callbacks[id]=a[2];
  ui_js_on(a[0].str, a[1].str, id);
  return V_nil();
}
static Value bi_ui_fetch(Interp *ip, Value *a, int n){
  if(n!=2||a[0].t!=V_STR||(a[1].t!=V_FUNC&&a[1].t!=V_BUILTIN))
    runtime_error(ip,"LarzTypeError","ui.fetch() expects (url, function) - function is called as fn(status, body)");
  if(g_ui_ncb==g_ui_cbcap){ g_ui_cbcap=g_ui_cbcap?g_ui_cbcap*2:16; g_ui_callbacks=realloc(g_ui_callbacks,g_ui_cbcap*sizeof(Value)); }
  int id=g_ui_ncb++;
  g_ui_callbacks[id]=a[1];
  ui_js_fetch(a[0].str, id);
  return V_nil();
}

static Builtin UB_set_text={"set_text",bi_ui_set_text}, UB_get_text={"get_text",bi_ui_get_text},
  UB_get_value={"get_value",bi_ui_get_value}, UB_set_value={"set_value",bi_ui_set_value},
  UB_set_html={"set_html",bi_ui_set_html}, UB_add_class={"add_class",bi_ui_add_class},
  UB_remove_class={"remove_class",bi_ui_remove_class}, UB_on={"on",bi_ui_on}, UB_fetch={"fetch",bi_ui_fetch};

static void register_ui_module(Env *g){
  Env *e=env_new(NULL);
  env_define(e,"set_text",V_builtin(&UB_set_text));
  env_define(e,"get_text",V_builtin(&UB_get_text));
  env_define(e,"get_value",V_builtin(&UB_get_value));
  env_define(e,"set_value",V_builtin(&UB_set_value));
  env_define(e,"set_html",V_builtin(&UB_set_html));
  env_define(e,"add_class",V_builtin(&UB_add_class));
  env_define(e,"remove_class",V_builtin(&UB_remove_class));
  env_define(e,"on",V_builtin(&UB_on));
  env_define(e,"fetch",V_builtin(&UB_fetch));
  env_define(g,"ui",V_module(e,xstrdup("ui")));
}

/* the persistent, page-lifetime interpreter every <script type="text/larzscript">
 * tag runs into (one shared global scope, same as ordinary <script> tags do),
 * and the interpreter every ui.on()-registered callback is invoked through
 * later, from JS, possibly long after the tag that registered it finished. */
static Interp g_web_ip; static int g_web_ip_ready=0;
static void ensure_web_ip(void){
  if(g_web_ip_ready) return;
  memset(&g_web_ip,0,sizeof(g_web_ip));
  install_builtins(&g_web_ip);
  g_web_ip.basedir=xstrdup(".");
  env_define(g_web_ip.globals,"args",V_list(list_new()));
  g_web_ip_ready=1;
}

EMSCRIPTEN_KEEPALIVE
int larz_eval_source(const char *src){
  ensure_web_ip();
  if(setjmp(g_err)){ fprintf(stderr,"SyntaxError: %s\n", g_errmsg); return 1; }
  Token *toks=lex(xstrdup(src));           /* never freed - same lifetime as the AST it feeds, like main()'s read_all() */
  Node *prog=parse_program(toks);
  if(setjmp(g_web_ip.jb)){ fprintf(stderr,"%s: %s\n", g_web_ip.errname, g_web_ip.errmsg); return 1; }
  g_web_ip.returning=0; g_web_ip.loopflow=0;
  for(int i=0;i<prog->nkids;i++){ exec(&g_web_ip, prog->kids[i], g_web_ip.globals); if(g_web_ip.returning) break; }
  return 0;
}

EMSCRIPTEN_KEEPALIVE
void larz_invoke_callback(int id){
  if(!g_web_ip_ready || id<0 || id>=g_ui_ncb) return;
  if(setjmp(g_web_ip.jb)){ fprintf(stderr,"%s: %s\n", g_web_ip.errname, g_web_ip.errmsg); return; }
  g_web_ip.returning=0; g_web_ip.loopflow=0;
  call_value(&g_web_ip, g_ui_callbacks[id], NULL, 0);
}

/* ui.fetch()'s callback: fn(status, body). status is a plain number (0 on a
 * network-level failure, never a real HTTP status - fetch() itself resolves
 * even for 4xx/5xx responses); body is copied into a proper GC string by
 * V_string before this returns, so the transient wasm-heap buffer JS passed
 * in (freed by the caller right after this call returns) is never retained. */
EMSCRIPTEN_KEEPALIVE
void larz_invoke_fetch_callback(int id, int status, const char *body){
  if(!g_web_ip_ready || id<0 || id>=g_ui_ncb) return;
  if(setjmp(g_web_ip.jb)){ fprintf(stderr,"%s: %s\n", g_web_ip.errname, g_web_ip.errmsg); return; }
  g_web_ip.returning=0; g_web_ip.loopflow=0;
  Value args[2]; args[0]=V_number((double)status); args[1]=V_string(body);
  call_value(&g_web_ip, g_ui_callbacks[id], args, 2);
}
#endif /* __EMSCRIPTEN__ */

#if defined(__STDC_HOSTED__) && !__STDC_HOSTED__
/* ===================== bare-metal LarzOS: kernel-native `ui` module ===================== *
 * Same `ui` module name and call shape as the browser build (native/WEB.md):
 * ui.set_text/ui.on, plus two creation primitives the browser didn't need
 * (ui.label/ui.button) - the kernel has no pre-existing markup to select
 * into the way the browser's ui module queries a real DOM, so widgets are
 * created via API calls. Backed by kernel/gfx.c's VGA Mode 13h framebuffer
 * and keyboard-driven widget model instead of EM_JS/DOM calls - a different
 * renderer behind the identical Larzscript-level vocabulary, exactly what
 * that module was designed for.
 *
 * Simpler than the browser's callback bridge: there's no foreign runtime
 * calling back in from outside the interpreter's own call stack. Per
 * kernel/kernel.c's boot dispatch, larz_main() runs one .lz program to
 * completion per boot stage - one Interp, no concurrency - so ui.run() is
 * an ordinary builtin call that already has `ip` in scope and can invoke a
 * stored closure through it directly, no separate persistent-interpreter
 * singleton needed the way the browser's g_web_ip is. Still needs the same
 * GC-rooting instinct that caught the browser's use-after-free -
 * g_kernel_ui_callbacks is marked in gc_collect() above, next to
 * ip->modcache's own rooting. */

static int g_kernel_gfx_ready = 0;
static void ensure_kernel_gfx(void){ if(!g_kernel_gfx_ready){ gfx_init(); g_kernel_gfx_ready=1; } }

static Value bi_ui_label(Interp *ip, Value *a, int n){
  if(n!=4 || a[0].t!=V_STR || !is_num(a[1]) || !is_num(a[2]) || a[3].t!=V_STR)
    runtime_error(ip,"LarzTypeError","ui.label() expects (id, x, y, text)");
  ensure_kernel_gfx();
  if(gfx_widget_label(a[0].str, (int)a[1].num, (int)a[2].num, a[3].str) < 0)
    runtime_error(ip,"LarzRuntimeError","ui.label(): widget table full (max %d)", GFX_MAX_WIDGETS);
  return V_nil();
}
static Value bi_ui_button(Interp *ip, Value *a, int n){
  if(n!=6 || a[0].t!=V_STR || !is_num(a[1]) || !is_num(a[2]) || !is_num(a[3]) || !is_num(a[4]) || a[5].t!=V_STR)
    runtime_error(ip,"LarzTypeError","ui.button() expects (id, x, y, w, h, text)");
  ensure_kernel_gfx();
  if(gfx_widget_button(a[0].str, (int)a[1].num, (int)a[2].num, (int)a[3].num, (int)a[4].num, a[5].str) < 0)
    runtime_error(ip,"LarzRuntimeError","ui.button(): widget table full (max %d)", GFX_MAX_WIDGETS);
  return V_nil();
}
static Value bi_ui_set_text(Interp *ip, Value *a, int n){
  if(n!=2 || a[0].t!=V_STR || a[1].t!=V_STR) runtime_error(ip,"LarzTypeError","ui.set_text() expects (id, text)");
  gfx_widget_set_text(a[0].str, a[1].str);
  return V_nil();
}
/* A scrolling log pane - not fed via ui.set_text (which replaces, not
 * appends). No separate "write" builtin: once created, it becomes the
 * active terminal (kernel/gfx.c) and every plain print() call anywhere -
 * including from code that has no idea a GUI exists - starts showing up in
 * it automatically, via the serial_putc hook in kernel/libk.c. */
static Value bi_ui_terminal(Interp *ip, Value *a, int n){
  if(n!=5 || a[0].t!=V_STR || !is_num(a[1]) || !is_num(a[2]) || !is_num(a[3]) || !is_num(a[4]))
    runtime_error(ip,"LarzTypeError","ui.terminal() expects (id, x, y, w, h)");
  ensure_kernel_gfx();
  if(gfx_widget_terminal(a[0].str, (int)a[1].num, (int)a[2].num, (int)a[3].num, (int)a[4].num) < 0)
    runtime_error(ip,"LarzRuntimeError","ui.terminal(): widget table full (max %d)", GFX_MAX_WIDGETS);
  return V_nil();
}
static Value bi_ui_on(Interp *ip, Value *a, int n){
  if(n!=3 || a[0].t!=V_STR || a[1].t!=V_STR || (a[2].t!=V_FUNC && a[2].t!=V_BUILTIN))
    runtime_error(ip,"LarzTypeError","ui.on() expects (id, event, function)");
  int idx = gfx_widget_index(a[0].str);
  if(idx<0) runtime_error(ip,"LarzNameError","ui.on(): no widget with id '%s'", a[0].str);
  g_kernel_ui_callbacks[idx] = a[2];
  return V_nil();
}
static int g_kernel_ui_quit = 0;
static Value bi_ui_quit(Interp *ip, Value *a, int n){
  (void)ip; (void)a; (void)n;
  g_kernel_ui_quit = 1;
  return V_nil();
}
static Value bi_ui_run(Interp *ip, Value *a, int n){
  (void)a; (void)n;
  g_kernel_ui_quit = 0;
  while(!g_kernel_ui_quit){
    const char *clicked = gfx_widget_poll();
    if(clicked){
      int idx = gfx_widget_index(clicked);
      if(idx>=0 && g_kernel_ui_callbacks[idx].t != V_NIL) call_value(ip, g_kernel_ui_callbacks[idx], NULL, 0);
    }
  }
  return V_nil();
}

/* ui.window(title, w, h) - a script creates its OWN window and becomes its
 * "current" window (see gfx.c's per-task g_current_window[]) - every
 * ui.label/ui.button/ui.terminal call the script makes after this lands
 * inside it automatically, with (x,y) as offsets from the window's client
 * origin, not absolute screen coordinates. This is what turns "add a
 * desktop app" into "write a .lz script that calls ui.window()" with no
 * bespoke C wrapper needed at all (kernel.c's task_generic_app just runs
 * whatever script it's told to, uninvolved in the window's title/size).
 * Position is NOT a parameter - the kernel picks a simple cascading
 * default (like a real desktop auto-placing new windows) so several
 * launched-on-demand apps don't all land in an identical spot. */
static Value bi_ui_window(Interp *ip, Value *a, int n){
  if(n!=3 || a[0].t!=V_STR || !is_num(a[1]) || !is_num(a[2]))
    runtime_error(ip,"LarzTypeError","ui.window() expects (title, w, h)");
  ensure_kernel_gfx();
  int w=(int)a[1].num, h=(int)a[2].num;
  int existing = gfx_window_count();
  int x = 120 + (existing % 5) * 30;
  int y = 90  + (existing % 5) * 30;
  int idx = gfx_window_create(a[0].str, x, y, w, h);
  if(idx < 0)
    runtime_error(ip,"LarzRuntimeError","ui.window(): too many windows open (max %d)", GFX_MAX_WINDOWS);
  return V_number(idx);
}

/* ui.window_size() - the CURRENT window's client area [w, h], the same
 * numbers kernel.c's old bespoke task_terminal() wrapper used to compute by
 * hand via gfx_window_client_rect() - lets a script size a full-bleed
 * widget (e.g. a terminal pane) inside its own window without needing its
 * own index or any C wrapper written for it. */
static Value bi_ui_window_size(Interp *ip, Value *a, int n){
  if(n!=0) runtime_error(ip,"LarzTypeError","ui.window_size() expects no arguments");
  ensure_kernel_gfx();
  int cx,cy,cw,ch;
  gfx_window_client_rect(gfx_current_window(), &cx,&cy,&cw,&ch);
  List *r=list_new(); int tr=ip->ntemp; gc_temp_push(ip,V_list(r));
  list_push(r, V_number(cw));
  list_push(r, V_number(ch));
  gc_temp_pop(ip,tr);
  return V_list(r);
}

/* ui.windows() - a list of {"index":N, "title":"...", "focused":bool}
 * dicts, one per open window, back-to-front - lets a script (the `windows`
 * shell command) enumerate the desktop without any C-side help beyond the
 * gfx_window_at()/gfx_window_title() accessors above. */
static Value bi_ui_windows(Interp *ip, Value *a, int n){
  if(n!=0) runtime_error(ip,"LarzTypeError","ui.windows() expects no arguments");
  ensure_kernel_gfx();
  List *r=list_new(); int tr=ip->ntemp; gc_temp_push(ip,V_list(r));
  int count = gfx_window_count(), focused = gfx_window_focused();
  for(int i=0;i<count;i++){
    int idx = gfx_window_at(i);
    Dict *d=dict_new();
    dict_set(d, V_string("index"),   V_number(idx));
    dict_set(d, V_string("title"),   V_string(gfx_window_title(idx)));
    dict_set(d, V_string("focused"), V_bool(idx==focused));
    list_push(r, V_dict(d));
  }
  gc_temp_pop(ip,tr);
  return V_list(r);
}

/* ui.close(index) - closes a window by index: read the owning task BEFORE
 * closing (closing clears it), gfx_window_close() the window itself, THEN
 * task_exit() that task - in that order, not task_exit() first. A script
 * can close its OWN window this way (owner == the calling task), and
 * task_exit() on your own currently-running task marks its slot unused
 * immediately, not "after this function returns" - if a timer tick landed
 * between task_exit() and gfx_window_close(), this task could be preempted
 * and never resumed (schedule() permanently skips unused slots), leaving
 * the window never actually closed. Closing the window FIRST, while the
 * task is still guaranteed alive to finish this call, avoids that race
 * entirely; task_exit() being called after doesn't need to "finish"
 * anything else afterward either way. */
static Value bi_ui_close(Interp *ip, Value *a, int n){
  if(n!=1 || !is_num(a[0])) runtime_error(ip,"LarzTypeError","ui.close() expects a window index");
  ensure_kernel_gfx();
  int idx=(int)a[0].num;
  int owner = gfx_window_owner_task(idx);
  if(owner < 0) runtime_error(ip,"LarzRuntimeError","ui.close(): no such window %d", idx);
  gfx_window_close(idx);
  task_exit(owner);
  return V_nil();
}

/* ui.launch(script) - a thin wrapper over kernel.c's launch_app(), the same
 * mechanism a desktop-icon click already uses - lets a script (the `launch`
 * shell command) open another app on demand. Returns true if a task slot
 * was actually available, false if every slot was full (so the caller can
 * report why nothing happened, instead of it silently doing nothing). */
static Value bi_ui_launch(Interp *ip, Value *a, int n){
  if(n!=1 || a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","ui.launch() expects a script path string");
  ensure_kernel_gfx();
  return V_bool(launch_app(a[0].str) >= 0);
}

static Builtin KB_label={"label",bi_ui_label}, KB_button={"button",bi_ui_button},
  KB_set_text={"set_text",bi_ui_set_text}, KB_on={"on",bi_ui_on},
  KB_run={"run",bi_ui_run}, KB_quit={"quit",bi_ui_quit},
  KB_terminal={"terminal",bi_ui_terminal}, KB_window={"window",bi_ui_window},
  KB_window_size={"window_size",bi_ui_window_size},
  KB_windows={"windows",bi_ui_windows}, KB_close={"close",bi_ui_close},
  KB_launch={"launch",bi_ui_launch};

static void register_ui_module(Env *g){
  Env *e = env_new(NULL);
  env_define(e, "label", V_builtin(&KB_label));
  env_define(e, "button", V_builtin(&KB_button));
  env_define(e, "set_text", V_builtin(&KB_set_text));
  env_define(e, "on", V_builtin(&KB_on));
  env_define(e, "run", V_builtin(&KB_run));
  env_define(e, "quit", V_builtin(&KB_quit));
  env_define(e, "terminal", V_builtin(&KB_terminal));
  env_define(e, "window", V_builtin(&KB_window));
  env_define(e, "window_size", V_builtin(&KB_window_size));
  env_define(e, "windows", V_builtin(&KB_windows));
  env_define(e, "close", V_builtin(&KB_close));
  env_define(e, "launch", V_builtin(&KB_launch));
  env_define(g, "ui", V_module(e, xstrdup("ui")));
}
#endif /* kernel-native ui module */

/* ===================== formatter (larzscript fmt) ===================== */
static void fmt_expr(Node *n, int minprec);
static void fmt_stmt(Node *n, int indent);
static void fmt_oneliner(Node *n);

static void fmt_indent(int k){ for(int i=0;i<k;i++) printf("  "); }
static void fmt_money_lit(long long c){ long long a=c<0?-c:c; printf("%s$%lld.%02lld", c<0?"-":"", a/100, a%100); }
static void fmt_str_lit(const char *s){
  putchar('"');
  for(const char *p=s;*p;p++){ char c=*p;
    if(c=='"') printf("\\\""); else if(c=='\\') printf("\\\\");
    else if(c=='\n') printf("\\n"); else if(c=='\t') printf("\\t"); else putchar(c); }
  putchar('"');
}
static int expr_prec(Node *n){
  if(n->kind==N_TERNARY) return 1;
  if(n->kind==N_BIN){ const char *o=n->op;
    if(!strcmp(o,"or")) return 2;
    if(!strcmp(o,"and")) return 3;
    if(!strcmp(o,"==")||!strcmp(o,"!=")) return 4;
    if(!strcmp(o,"has")||!strcmp(o,"in")) return 5;
    if(!strcmp(o,"<")||!strcmp(o,"<=")||!strcmp(o,">")||!strcmp(o,">=")) return 6;
    if(!strcmp(o,"+")||!strcmp(o,"-")) return 7;
    if(!strcmp(o,"**")) return 9;
    return 8;
  }
  if(n->kind==N_UN) return 10;
  return 11;
}
static void fmt_params(Node *n){
  putchar('(');
  for(int i=0;i<n->nparams;i++){ if(i) printf(", "); printf("%s", n->params[i]);
    if(n->pdefs && n->pdefs[i]){ printf(" = "); fmt_expr(n->pdefs[i], 1); } }
  putchar(')');
}
static void fmt_expr(Node *n, int minprec){
  int p=expr_prec(n), paren=p<minprec;
  if(paren) putchar('(');
  switch(n->kind){
    case N_NUM: print_number(n->num); break;
    case N_MONEY: fmt_money_lit(n->cents); break;
    case N_STR: fmt_str_lit(n->str); break;
    case N_FSTR: putchar('f'); fmt_str_lit(n->str); break;
    case N_BOOL: printf(n->boolean?"true":"false"); break;
    case N_NIL: printf("nil"); break;
    case N_NAME: printf("%s", n->name); break;
    case N_UN: printf("%s", strcmp(n->op,"not")==0?"not ":"-"); fmt_expr(n->a, 10); break;
    case N_BIN: fmt_expr(n->a, p); printf(" %s ", n->natural?n->natural:n->op); fmt_expr(n->b, p+1); break;
    case N_TERNARY: fmt_expr(n->a, 2); printf(" ? "); fmt_expr(n->b, 1); printf(" : "); fmt_expr(n->c, 1); break;
    case N_ARRAY: putchar('['); for(int i=0;i<n->nkids;i++){ if(i) printf(", "); fmt_expr(n->kids[i], 1); } putchar(']'); break;
    case N_DICT: putchar('{'); for(int i=0;i+1<n->nkids;i+=2){ if(i) printf(", "); fmt_expr(n->kids[i],1); printf(": "); fmt_expr(n->kids[i+1],1); } putchar('}'); break;
    case N_INDEX: fmt_expr(n->a,11); putchar('['); fmt_expr(n->b,1); putchar(']'); break;
    case N_SLICE: fmt_expr(n->a,11); putchar('['); if(n->b) fmt_expr(n->b,1); putchar(':'); if(n->c) fmt_expr(n->c,1); putchar(']'); break;
    case N_CALL: if(n->natural){ printf("%s ", n->natural); fmt_expr(n->kids[0],1); break; }
                 fmt_expr(n->a,11); putchar('('); for(int i=0;i<n->nkids;i++){ if(i) printf(", "); fmt_expr(n->kids[i],1); } putchar(')'); break;
    case N_GET: fmt_expr(n->a,11); printf(".%s", n->name); break;
    case N_METHOD: fmt_expr(n->a,11); printf(".%s(", n->name); for(int i=0;i<n->nkids;i++){ if(i) printf(", "); fmt_expr(n->kids[i],1); } putchar(')'); break;
    case N_FN: printf("fn"); fmt_params(n); if(n->has_gas) printf(" gas %lld", n->gas);
               printf(" { "); for(int i=0;i<n->b->nkids;i++){ if(i) printf("; "); fmt_oneliner(n->b->kids[i]); } printf(" }"); break;
    case N_LISTCOMP: putchar('['); fmt_expr(n->a,1); printf(" for %s in ", n->name); fmt_expr(n->b,1);
                     if(n->c){ printf(" if "); fmt_expr(n->c,1); } putchar(']'); break;
    case N_DICTCOMP: putchar('{'); fmt_expr(n->a,1); printf(": "); fmt_expr(n->b,1); printf(" for %s in ", n->name); fmt_expr(n->c,1);
                     if(n->nkids>0){ printf(" if "); fmt_expr(n->kids[0],1); } putchar('}'); break;
    default: printf("<expr>"); break;
  }
  if(paren) putchar(')');
}
/* the right-hand side of an assignment: shows compound ops (x += e) from cop */
static void fmt_assign_rhs(Node *asg, Node *rhs){
  if(asg->cop){ printf(" %s= ", asg->cop); fmt_expr(rhs->b, 1); }   /* rhs is (target cop e); print e */
  else { printf(" = "); fmt_expr(rhs, 1); }
}
/* print a statement's core (no indent, no newline) */
static void fmt_stmt_core(Node *n, int indent){
  switch(n->kind){
    case N_LET: printf("let %s = ", n->name); fmt_expr(n->a,1); break;
    case N_ASSIGN: printf("%s", n->name); fmt_assign_rhs(n, n->a); break;
    case N_SETINDEX: fmt_expr(n->a,11); putchar('['); fmt_expr(n->b,1); putchar(']'); fmt_assign_rhs(n, n->c); break;
    case N_PRICE: printf("price %s = ", n->name); fmt_expr(n->a,1); break;
    case N_WALLET: printf("wallet %s", n->name); if(n->a){ printf(" = "); fmt_expr(n->a,1); } break;
    case N_CAPABILITY: printf("capability %s", n->name); break;
    case N_GRANT: printf("grant %s", n->name); break;
    case N_REVOKE: printf("revoke %s", n->name); break;
    case N_PAY: printf("pay "); fmt_expr(n->a,1); printf(" from %s to %s", n->src, n->dst); if(n->str) printf(" requires %s", n->str); break;
    case N_SPLIT: printf("split "); fmt_expr(n->a,1); printf(" from %s {\n", n->src);
      for(int i=0;i<n->nkids;i++){ fmt_indent(indent+1); printf("%s %g%%\n", n->kids[i]->dst, n->kids[i]->num); }
      fmt_indent(indent); putchar('}'); break;
    case N_PAYWALL: printf("paywall %s = ", n->name); fmt_expr(n->a,10); printf(" / %s to %s", n->period, n->dst); break;
    case N_SUBSCRIBE: printf("subscribe %s to %s", n->src, n->dst); if(n->str) printf(" requires %s", n->str); break;
    case N_REQUIRE: printf("require "); fmt_expr(n->a,1); if(n->str){ printf(", "); fmt_str_lit(n->str); } break;
    case N_RETURN: printf("return"); if(n->a){ putchar(' '); fmt_expr(n->a,1); } break;
    case N_THROW: printf("throw "); fmt_expr(n->a,1); break;
    case N_BREAK: printf("break"); break;
    case N_CONTINUE: printf("continue"); break;
    case N_IMPORT: printf("import "); fmt_expr(n->a,1); if(n->name) printf(" as %s", n->name); break;
    case N_EXPR: fmt_expr(n->a,1); break;
    case N_FN: printf("fn %s", n->name?n->name:""); fmt_params(n); if(n->has_gas) printf(" gas %lld", n->gas);
               printf(" {\n"); for(int i=0;i<n->b->nkids;i++) fmt_stmt(n->b->kids[i], indent+1); fmt_indent(indent); putchar('}'); break;
    case N_IF: if(n->natural){ printf("unless "); fmt_expr(n->a->a,1); } else { printf("if "); fmt_expr(n->a,1); }
               printf(" {\n"); for(int i=0;i<n->b->nkids;i++) fmt_stmt(n->b->kids[i], indent+1); fmt_indent(indent); putchar('}');
               if(n->c){ printf(" else "); if(n->c->kind==N_IF) fmt_stmt_core(n->c, indent); else { printf("{\n"); for(int i=0;i<n->c->nkids;i++) fmt_stmt(n->c->kids[i], indent+1); fmt_indent(indent); putchar('}'); } } break;
    case N_WHILE: printf("while "); fmt_expr(n->a,1); printf(" {\n"); for(int i=0;i<n->b->nkids;i++) fmt_stmt(n->b->kids[i], indent+1); fmt_indent(indent); putchar('}'); break;
    case N_FOR: if(n->natural){ printf("for %s from ", n->name); fmt_expr(n->a->kids[0],1); printf(" to "); fmt_expr(n->a->kids[1],1); }
                else { printf("for %s in ", n->name); fmt_expr(n->a,1); }
                printf(" {\n"); for(int i=0;i<n->b->nkids;i++) fmt_stmt(n->b->kids[i], indent+1); fmt_indent(indent); putchar('}'); break;
    case N_TRY: printf("try {\n"); for(int i=0;i<n->a->nkids;i++) fmt_stmt(n->a->kids[i], indent+1); fmt_indent(indent); printf("} catch %s {\n", n->name); for(int i=0;i<n->b->nkids;i++) fmt_stmt(n->b->kids[i], indent+1); fmt_indent(indent); putchar('}'); break;
    case N_BLOCK: printf("{\n"); for(int i=0;i<n->nkids;i++) fmt_stmt(n->kids[i], indent+1); fmt_indent(indent); putchar('}'); break;
    default: fmt_expr(n, 1); break;
  }
}
static void fmt_stmt(Node *n, int indent){ fmt_indent(indent); fmt_stmt_core(n, indent); putchar('\n'); }
static void fmt_oneliner(Node *n){ fmt_stmt_core(n, 0); }   /* for lambda bodies */
static void format_program(Node *prog){ for(int i=0;i<prog->nkids;i++) fmt_stmt(prog->kids[i], 0); }

static const char *USAGE =
  "larzscript - the money-native, general-purpose language\n"
  "\n"
  "usage:\n"
  "  larzscript <program.lz>        run a program file\n"
  "  larzscript -e \"<code>\"          run a snippet of code\n"
  "  larzscript repl                start the interactive REPL\n"
  "  larzscript fmt <file.lz>       print the file, canonically formatted\n"
  "  larzscript --check <file.lz>   syntax-check a file (for editors / CI)\n"
  "  larzscript --emit-c <file.lz>  compile to C (larzc: gcc it for a native binary)\n"
  "  larzscript [--ledger] <file>   also print the money ledger afterwards\n"
  "  larzscript update              check for and install the latest release\n"
  "  larzscript pkg <args...>       run the package manager (install/list/publish/...)\n"
  "  larzscript --version | --help\n";

/* ======================================================================
 * larzc: a Larzscript -> C compiler backend (--emit-c). Handles a general-
 * purpose subset (numbers, strings, bools, functions, arithmetic/compare/logic,
 * if/while/for-in-range, ternary, and print/str/int/len) via a small tagged
 * value runtime, so gcc can compile it to a native binary.
 * ==================================================================== */
static void ec_expr(Node *n);
static int  ec_lc=0;

/* ============================ larzc closures ============================
 * Lambdas (anonymous fn) and nested named fns are hoisted to top-level C
 * functions with a uniform signature  LZ __lamN(LZ *cap, LZ *args). A closure
 * VALUE (t=9) pairs that fn pointer with a by-value snapshot of its captured
 * free variables. Calls through a value go via lz_call(); named top-level
 * functions used as values get an adapter closure. map/filter/reduce use these.
 * (Limitation: captures are by value, so mutating a captured outer local from
 * inside a closure is not shared back - the common factory/callback cases work.)
 * ==================================================================== */
typedef struct { char **v; int n, cap; } SSet;
static int ss_has(SSet *s, const char *x){ for(int i=0;i<s->n;i++) if(!strcmp(s->v[i],x)) return 1; return 0; }
static void ss_add(SSet *s, const char *x){ if(ss_has(s,x)) return; if(s->n>=s->cap){ s->cap=s->cap?s->cap*2:8; s->v=(char**)realloc(s->v,s->cap*sizeof(char*)); } s->v[s->n++]=(char*)x; }
static void ss_copy(SSet *dst, SSet *src){ for(int i=0;i<src->n;i++) ss_add(dst, src->v[i]); }

static Node **g_lams=0; static int g_nlams=0;      /* hoisted lambdas / nested fns (lam_id = index+1) */
static Node **g_topfn=0; static int g_ntopfn=0;     /* top-level function nodes */
static SSet   g_globals={0,0,0};                    /* top-level names (accessible everywhere) */
typedef struct { char *alias, *prefix; SSet fns, globals; } Import;   /* an inlined module */
static Import *g_imp=0; static int g_nimp=0;
static Import *imp_find(const char *alias){ for(int i=0;i<g_nimp;i++) if(!strcmp(g_imp[i].alias,alias)) return &g_imp[i]; return 0; }
static int is_topfn(const char *name, int *arity){ for(int i=0;i<g_ntopfn;i++) if(g_topfn[i]->name && !strcmp(g_topfn[i]->name,name)){ if(arity)*arity=g_topfn[i]->nparams; return 1; } return 0; }

static void fv_stmt(Node *n, SSet *bound, SSet *out);
static void fv_expr(Node *n, SSet *bound, SSet *out){
  if(!n) return;
  switch(n->kind){
    case N_NAME: if(!ss_has(bound,n->name)) ss_add(out,n->name); break;
    case N_FN: { SSet nb={0,0,0}; ss_copy(&nb,bound); for(int i=0;i<n->nparams;i++) ss_add(&nb,n->params[i]); if(n->name) ss_add(&nb,n->name); fv_stmt(n->b,&nb,out); free(nb.v); break; }
    case N_UN: case N_GET: case N_FSTR: fv_expr(n->a,bound,out); break;
    case N_BIN: case N_INDEX: fv_expr(n->a,bound,out); fv_expr(n->b,bound,out); break;
    case N_TERNARY: fv_expr(n->a,bound,out); fv_expr(n->b,bound,out); fv_expr(n->c,bound,out); break;
    case N_METHOD: case N_CALL: fv_expr(n->a,bound,out); for(int i=0;i<n->nkids;i++) fv_expr(n->kids[i],bound,out); break;
    case N_ARRAY: case N_DICT: for(int i=0;i<n->nkids;i++) fv_expr(n->kids[i],bound,out); break;
    default: break;
  }
}
static void fv_stmt(Node *n, SSet *bound, SSet *out){
  if(!n) return;
  switch(n->kind){
    case N_BLOCK: for(int i=0;i<n->nkids;i++) fv_stmt(n->kids[i],bound,out); break;
    case N_LET: fv_expr(n->a,bound,out); ss_add(bound,n->name); break;
    case N_ASSIGN: if(!ss_has(bound,n->name)) ss_add(out,n->name); fv_expr(n->a,bound,out); break;
    case N_SETINDEX: fv_expr(n->a,bound,out); fv_expr(n->b,bound,out); fv_expr(n->c,bound,out); break;
    case N_EXPR: case N_RETURN: case N_PAY: case N_REQUIRE: fv_expr(n->a,bound,out); break;
    case N_IF: fv_expr(n->a,bound,out); fv_stmt(n->b,bound,out); if(n->c) fv_stmt(n->c,bound,out); break;
    case N_WHILE: fv_expr(n->a,bound,out); fv_stmt(n->b,bound,out); break;
    case N_FOR: { fv_expr(n->a,bound,out); SSet nb={0,0,0}; ss_copy(&nb,bound); ss_add(&nb,n->name); fv_stmt(n->b,&nb,out); free(nb.v); break; }
    case N_FN: { SSet nb={0,0,0}; ss_copy(&nb,bound); for(int i=0;i<n->nparams;i++) ss_add(&nb,n->params[i]); if(n->name) ss_add(&nb,n->name); fv_stmt(n->b,&nb,out); free(nb.v); if(n->name) ss_add(bound,n->name); break; }
    default: break;
  }
}
/* captured names of a lambda: free vars of its body (minus its params/name) that are not globals */
static void lam_captures(Node *lam, SSet *out){
  SSet bound={0,0,0}, frees={0,0,0};
  for(int i=0;i<lam->nparams;i++) ss_add(&bound, lam->params[i]);
  if(lam->name) ss_add(&bound, lam->name);
  fv_stmt(lam->b, &bound, &frees);
  for(int i=0;i<frees.n;i++) if(!ss_has(&g_globals, frees.v[i])) ss_add(out, frees.v[i]);
  free(bound.v); free(frees.v);
}
/* walk a subtree, giving every nested lambda / nested named-fn a lam_id */
static void collect_lams(Node *n){
  if(!n) return;
  if(n->kind==N_FN){ if(!n->lam_id){ g_lams=(Node**)realloc(g_lams,(g_nlams+1)*sizeof(Node*)); g_lams[g_nlams++]=n; n->lam_id=g_nlams; } collect_lams(n->b); return; }
  collect_lams(n->a); collect_lams(n->b); collect_lams(n->c);
  for(int i=0;i<n->nkids;i++) collect_lams(n->kids[i]);
}
static void ec_str_lit(const char *s){ putchar('"'); for(const char*p=s;*p;p++){ if(*p=='"'||*p=='\\') putchar('\\'); if(*p=='\n'){ printf("\\n"); continue; } if(*p=='\t'){ printf("\\t"); continue; } putchar(*p); } putchar('"'); }
static void ec_raw(const char *s){ for(const char*p=s;*p;p++){ if(*p=='"'||*p=='\\') putchar('\\'); if(*p=='\n'){ printf("\\n"); continue; } putchar(*p); } }
static void ec_binop(Node *n, const char *fn){ printf("%s(",fn); ec_expr(n->a); printf(","); ec_expr(n->b); printf(")"); }
static void ec_cmp(Node *n, int op){ printf("lz_cmp("); ec_expr(n->a); printf(","); ec_expr(n->b); printf(",%d)",op); }
static void ec_expr(Node *n){
  switch(n->kind){
    case N_NUM: printf("lznum(%.17g)", n->num); break;
    case N_STR: printf("lzstr("); ec_str_lit(n->str); printf(")"); break;
    case N_BOOL: printf("lzbool(%d)", n->boolean); break;
    case N_NIL: printf("lznil()"); break;
    case N_FSTR: ec_expr(n->a); break;   /* desugared to a "+"-concat of literals and str(expr) */
    case N_NAME: {
      int ar; if(is_topfn(n->name,&ar)){ printf("lzclosure(__adapt_%s, %d, \"%s\", 0)", n->name, ar, n->name); }  /* function used as a value */
      else printf("%s", n->name);
      break;
    }
    case N_FN: {   /* lambda / nested fn used as an expression -> a closure value */
      SSet cap={0,0,0}; lam_captures(n, &cap);
      printf("lzclosure(__lam%d, %d, \"%s\", %d", n->lam_id, n->nparams, n->name?n->name:"?", cap.n);
      for(int i=0;i<cap.n;i++){ printf(", %s", cap.v[i]); }
      printf(")");
      free(cap.v);
      break;
    }
    case N_UN:
      if(strcmp(n->op,"not")==0){ printf("lzbool(!lztruthy("); ec_expr(n->a); printf("))"); }
      else { printf("lz_sub(lznum(0),"); ec_expr(n->a); printf(")"); }
      break;
    case N_TERNARY: printf("(lztruthy("); ec_expr(n->a); printf(")?("); ec_expr(n->b); printf("):("); ec_expr(n->c); printf("))"); break;
    case N_ARRAY: printf("lz_listn(%d",n->nkids); for(int i=0;i<n->nkids;i++){ printf(","); ec_expr(n->kids[i]); } printf(")"); break;
    case N_DICT: printf("lz_dictn(%d",n->nkids/2); for(int i=0;i<n->nkids;i++){ printf(","); ec_expr(n->kids[i]); } printf(")"); break;
    case N_INDEX: printf("lz_index("); ec_expr(n->a); printf(","); ec_expr(n->b); printf(")"); break;
    case N_METHOD: {
      if(n->a->kind==N_NAME){ Import*im=imp_find(n->a->name); if(im){ printf("%s%s(", im->prefix, n->name); for(int i=0;i<n->nkids;i++){ if(i)printf(","); ec_expr(n->kids[i]); } printf(")"); break; } }
      printf("lz_method("); ec_expr(n->a); printf(",\"%s\",%d", n->name, n->nkids); for(int i=0;i<n->nkids;i++){ printf(","); ec_expr(n->kids[i]); } printf(")"); break;
    }
    case N_MONEY: printf("lzmoney(%lld.0)", n->cents); break;
    case N_GET:
      if(n->a->kind==N_NAME){ Import*im=imp_find(n->a->name); if(im){
        if(ss_has(&im->fns, n->name)){ int ar=0; char pn[512]; snprintf(pn,sizeof pn,"%s%s",im->prefix,n->name); is_topfn(pn,&ar); printf("lzclosure(__adapt_%s, %d, \"%s\", 0)", pn, ar, n->name); }
        else printf("%s%s", im->prefix, n->name);   /* module global */
        break;
      } }
      if(!strcmp(n->name,"balance")){ printf("lz_wbal("); ec_expr(n->a); printf(")"); }
      else { fprintf(stderr,"larzc: unsupported property '.%s'\n", n->name); exit(1); }
      break;
    case N_BIN: {
      const char*o=n->op;
      if(!strcmp(o,"+")) ec_binop(n,"lz_add");
      else if(!strcmp(o,"-")) ec_binop(n,"lz_sub");
      else if(!strcmp(o,"*")) ec_binop(n,"lz_mul");
      else if(!strcmp(o,"/")) ec_binop(n,"lz_div");
      else if(!strcmp(o,"%")) ec_binop(n,"lz_mod");
      else if(!strcmp(o,"//")) ec_binop(n,"lz_idiv");
      else if(!strcmp(o,"**")) ec_binop(n,"lz_pow");
      else if(!strcmp(o,"and")) ec_binop(n,"lz_and");
      else if(!strcmp(o,"or")) ec_binop(n,"lz_or");
      else if(!strcmp(o,"==")) ec_cmp(n,0);
      else if(!strcmp(o,"!=")) ec_cmp(n,1);
      else if(!strcmp(o,"<")) ec_cmp(n,2);
      else if(!strcmp(o,">")) ec_cmp(n,3);
      else if(!strcmp(o,"<=")) ec_cmp(n,4);
      else if(!strcmp(o,">=")) ec_cmp(n,5);
      else if(!strcmp(o,"has")){ printf("lzbool(lz_has_sub("); ec_expr(n->a); printf(","); ec_expr(n->b); printf("))"); }
      else if(!strcmp(o,"in")){ printf("lz_in("); ec_expr(n->a); printf(","); ec_expr(n->b); printf(")"); }
      else { fprintf(stderr,"larzc: unsupported operator '%s'\n",o); exit(1); }
      break;
    }
    case N_CALL: {
      if(n->a->kind==N_NAME){
        const char*fn=n->a->name;
        if(!strcmp(fn,"print")){ printf("lz_print(%d",n->nkids); for(int i=0;i<n->nkids;i++){ printf(","); ec_expr(n->kids[i]); } printf(")"); break; }
        else if(!strcmp(fn,"sleep")){ printf("lz_sleep("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"str")){ printf("lz_str_("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"int")){ printf("lz_int("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"len")){ printf("lz_len("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"push")){ printf("lz_push("); ec_expr(n->kids[0]); printf(","); ec_expr(n->kids[1]); printf(")"); break; }
        else if(!strcmp(fn,"keys")){ printf("lz_keys("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"map")){ printf("lz_map("); ec_expr(n->kids[0]); printf(","); ec_expr(n->kids[1]); printf(")"); break; }
        else if(!strcmp(fn,"filter")){ printf("lz_filter("); ec_expr(n->kids[0]); printf(","); ec_expr(n->kids[1]); printf(")"); break; }
        else if(!strcmp(fn,"reduce")){ printf("lz_reduce("); ec_expr(n->kids[0]); printf(","); ec_expr(n->kids[1]); printf(","); if(n->nkids>=3) ec_expr(n->kids[2]); else printf("lznil()"); printf(",%d)", n->nkids>=3?1:0); break; }
        else if(!strcmp(fn,"read_file")){ printf("lz_read_file("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"write_file")){ printf("lz_write_file("); ec_expr(n->kids[0]); printf(","); ec_expr(n->kids[1]); printf(")"); break; }
        else if(!strcmp(fn,"append_file")){ printf("lz_append_file("); ec_expr(n->kids[0]); printf(","); ec_expr(n->kids[1]); printf(")"); break; }
        else if(!strcmp(fn,"file_exists")){ printf("lz_file_exists("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"mkdir")){ printf("lz_mkdir("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"remove")){ printf("lz_remove("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"cwd")){ printf("lz_cwd()"); break; }
        else if(!strcmp(fn,"listdir")){ printf("lz_listdir(%d,", n->nkids); if(n->nkids>=1) ec_expr(n->kids[0]); else printf("lznil()"); printf(")"); break; }
        else if(!strcmp(fn,"join")){ printf("lz_join(%d,", n->nkids); ec_expr(n->kids[0]); printf(","); if(n->nkids>=2) ec_expr(n->kids[1]); else printf("lznil()"); printf(")"); break; }
        else if(!strcmp(fn,"exit")){ printf("lz_exitf(%d,", n->nkids); if(n->nkids>=1) ec_expr(n->kids[0]); else printf("lznil()"); printf(")"); break; }
        else if(!strcmp(fn,"chr")){ printf("lz_chr("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"ord")){ printf("lz_ord("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"abs")){ printf("lz_absf("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"sqrt")){ printf("lz_sqrtf("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"floor")){ printf("lz_floorf("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"ceil")){ printf("lz_ceilf("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"round")){ printf("lz_roundf("); ec_expr(n->kids[0]); printf(")"); break; }
        else if(!strcmp(fn,"pow")){ printf("lz_powf("); ec_expr(n->kids[0]); printf(","); ec_expr(n->kids[1]); printf(")"); break; }
        else if(is_topfn(fn,0)){ printf("%s(",fn); for(int i=0;i<n->nkids;i++){ if(i)printf(","); ec_expr(n->kids[i]); } printf(")"); break; }
        /* else: a variable/param holding a closure -> fall through to lz_call */
      }
      printf("lz_call("); ec_expr(n->a); printf(", %d, ", n->nkids);
      if(n->nkids==0) printf("0"); else { printf("(LZ[]){"); for(int i=0;i<n->nkids;i++){ if(i)printf(","); ec_expr(n->kids[i]); } printf("}"); }
      printf(")");
      break;
    }
    default: fprintf(stderr,"larzc: unsupported expression (node %d)\n", n->kind); exit(1);
  }
}
static void ec_ind(int d){ for(int i=0;i<d;i++) printf("  "); }
static void ec_stmt(Node *n, int d){
  switch(n->kind){
    case N_LET: ec_ind(d); printf("LZ %s = ", n->name); ec_expr(n->a); printf(";\n"); break;
    case N_ASSIGN: ec_ind(d); printf("%s = ", n->name); ec_expr(n->a); printf(";\n"); break;
    case N_SETINDEX: ec_ind(d); printf("lz_setindex("); ec_expr(n->a); printf(","); ec_expr(n->b); printf(","); ec_expr(n->c); printf(");\n"); break;
    case N_PRICE: ec_ind(d); printf("LZ %s = ", n->name); ec_expr(n->a); printf(";\n"); break;
    case N_WALLET: ec_ind(d); printf("LZ %s = lzwallet(", n->name); if(n->a){ printf("("); ec_expr(n->a); printf(").n"); } else printf("0"); printf(");\n"); break;
    /* only reached for a NON-top-level capability (top-level ones are hoisted
     * to a global LZ and skipped here by emit_c's main()-loop, same as N_FN -
     * declaring `LZ NAME={11,0,0,0};` again here would shadow that global
     * instead of reusing it). A local capability is fully valid on its own
     * terms though, same as a wallet declared inside a function. */
    case N_CAPABILITY: ec_ind(d); printf("LZ %s = {11,0,0,0};\n", n->name); break;
    case N_GRANT: ec_ind(d); printf("%s.n = 1;\n", n->name); break;
    case N_REVOKE: ec_ind(d); printf("%s.n = 0;\n", n->name); break;
    case N_PAY:
      if(n->str){ ec_ind(d); printf("if(!%s.n){fputs(\"CapabilityError: capability '%s' is not granted\\n\",stderr);exit(1);}\n", n->str, n->str); }
      ec_ind(d); printf("lz_pay(%s, %s, ", n->src, n->dst); ec_expr(n->a); printf(");\n");
      break;
    /* the amount expr is evaluated exactly once (into __splitamt) even though
     * it may be referenced by every leg - matters if it has side effects.
     * Each split gets its own { } block, so __splitamt/__splitrem/__cut can
     * be reused verbatim across multiple split statements with no collision. */
    case N_SPLIT: {
      ec_ind(d); printf("{ LZ __splitamt = "); ec_expr(n->a); printf("; long long __splitrem = (long long)__splitamt.n;\n");
      for(int i=0;i<n->nkids;i++){
        Node *leg=n->kids[i];
        if(i==n->nkids-1){ ec_ind(d+1); printf("lz_pay(%s, %s, lzmoney((double)__splitrem));\n", n->src, leg->dst); }
        else { ec_ind(d+1); printf("{ long long __cut = (long long)lz_mround(__splitamt.n * %.10g / 100.0); __splitrem -= __cut; lz_pay(%s, %s, lzmoney((double)__cut)); }\n", leg->num, n->src, leg->dst); }
      }
      ec_ind(d); printf("}\n");
      break;
    }
    case N_PAYWALL: ec_ind(d); printf("LZ %s = lzpaywall((", n->name); ec_expr(n->a); printf(").n, \""); ec_raw(n->period); printf("\", \""); ec_raw(n->name); printf("\", %s);\n", n->dst); break;
    case N_SUBSCRIBE:
      if(n->str){ ec_ind(d); printf("if(!%s.n){fputs(\"CapabilityError: capability '%s' is not granted\\n\",stderr);exit(1);}\n", n->str, n->str); }
      ec_ind(d); printf("lz_subscribe(%s, %s);\n", n->src, n->dst);
      break;
    case N_FN: { SSet cap={0,0,0}; lam_captures(n,&cap); ec_ind(d); printf("LZ %s = lzclosure(__lam%d, %d, \"%s\", %d", n->name, n->lam_id, n->nparams, n->name?n->name:"?", cap.n); for(int i=0;i<cap.n;i++) printf(", %s", cap.v[i]); printf(");\n"); free(cap.v); break; }
    case N_REQUIRE: ec_ind(d); printf("if(!lztruthy("); ec_expr(n->a); printf(")){fputs(\"RequireError: "); ec_raw(n->str?n->str:"requirement not met"); printf("\\n\",stderr);exit(1);}\n"); break;
    case N_EXPR: ec_ind(d); ec_expr(n->a); printf(";\n"); break;
    case N_RETURN: ec_ind(d); printf("return "); if(n->a) ec_expr(n->a); else printf("lznil()"); printf(";\n"); break;
    case N_BLOCK: for(int i=0;i<n->nkids;i++) ec_stmt(n->kids[i],d); break;
    case N_IF:
      ec_ind(d); printf("if(lztruthy("); ec_expr(n->a); printf(")){\n"); ec_stmt(n->b,d+1); ec_ind(d); printf("}");
      if(n->c){ printf(" else {\n"); ec_stmt(n->c,d+1); ec_ind(d); printf("}"); } printf("\n"); break;
    case N_WHILE:
      ec_ind(d); printf("while(lztruthy("); ec_expr(n->a); printf(")){\n"); ec_stmt(n->b,d+1); ec_ind(d); printf("}\n"); break;
    case N_FOR: {
      Node *it=n->a;
      int lc=ec_lc++;
      if(it->kind==N_CALL && it->a->kind==N_NAME && !strcmp(it->a->name,"range")){
        Node *a0=it->nkids>0?it->kids[0]:NULL, *a1=it->nkids>1?it->kids[1]:NULL, *a2=it->nkids>2?it->kids[2]:NULL;
        ec_ind(d); printf("for(long long _i%d=", lc);
        if(a1){ printf("(long long)("); ec_expr(a0); printf(").n"); } else printf("0");
        printf("; _i%d<(long long)(", lc); if(a1) ec_expr(a1); else ec_expr(a0); printf(").n; _i%d+=", lc);
        if(a2){ printf("(long long)("); ec_expr(a2); printf(").n"); } else printf("1");
        printf("){\n"); ec_ind(d+1); printf("LZ %s = lznum((double)_i%d);\n", n->name, lc);
      } else if(it->kind==N_CALL && it->a->kind==N_NAME && !strcmp(it->a->name,"range_to") && it->nkids==2){
        /* "for i from A to B" - same native-C-loop fast path as range()
         * above, inclusive of B, direction computed at runtime since A/B
         * may not be compile-time constants (mirrors bi_range_to). */
        ec_ind(d); printf("{ long long _f%d=(long long)(", lc); ec_expr(it->kids[0]); printf(").n, _t%d=(long long)(", lc); ec_expr(it->kids[1]); printf(").n;\n");
        ec_ind(d); printf("long long _s%d = (_f%d<=_t%d) ? 1 : -1;\n", lc, lc, lc);
        ec_ind(d); printf("for(long long _i%d=_f%d; _s%d>0 ? _i%d<=_t%d : _i%d>=_t%d; _i%d+=_s%d){\n", lc,lc,lc,lc,lc,lc,lc,lc,lc);
        ec_ind(d+1); printf("LZ %s = lznum((double)_i%d);\n", n->name, lc);
      } else {                                        /* iterate a list (or string) */
        ec_ind(d); printf("LZ _L%d = ", lc); ec_expr(it); printf(";\n");
        ec_ind(d); printf("for(long long _i%d=0; _i%d<lz_lenN(_L%d); _i%d++){\n", lc,lc,lc,lc);
        ec_ind(d+1); printf("LZ %s = lz_index(_L%d, lznum((double)_i%d));\n", n->name, lc, lc);
      }
      ec_stmt(n->b,d+1); ec_ind(d); printf("}\n");
      if(it->kind==N_CALL && it->a->kind==N_NAME && !strcmp(it->a->name,"range_to") && it->nkids==2){ ec_ind(d); printf("}\n"); }
      break;
    }
    case N_IMPORT:
      if(!n->name){ fprintf(stderr,"larzc: dynamic import requires an explicit 'as alias' (line %d)\n", n->line); exit(1); }
      ec_ind(d); printf("LZ %s = lz_dynimport(", n->name); ec_expr(n->a); printf(");\n"); break;
    default: fprintf(stderr,"larzc: unsupported statement (node %d)\n", n->kind); exit(1);
  }
}
static void emit_runtime(void){
  puts("/* generated by larzc (larzscript --emit-c) */");
  puts("#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <stdarg.h>");
  puts("#include <math.h>\n#include <sys/stat.h>\n#include <dirent.h>\n#include <unistd.h>");
  puts("#ifdef _WIN32\n#define mkdir(path,mode) mkdir(path)\n#endif");
  puts("typedef struct LZ{int t;double n;char*s;void*p;}LZ;");   /* t:0 nil 1 num 2 str 3 bool 4 list */
  puts("typedef struct{LZ*items;int n,cap;}LST;");
  puts("static LZ lznil(){LZ v={0,0,0,0};return v;}");
  puts("static LZ lznum(double d){LZ v={1,d,0,0};return v;}");
  puts("static LZ lzbool(int b){LZ v={3,(double)(b!=0),0,0};return v;}");
  puts("static LZ lzstr(const char*s){LZ v={2,0,strdup(s),0};return v;}");
  puts("static LZ lzmoney(double cents){LZ v={6,cents,0,0};return v;}");
  puts("typedef struct{long long cents;}WAL;");
  puts("static LZ lzwallet(double cents){WAL*w=(WAL*)calloc(1,sizeof(WAL));w->cents=(long long)cents;LZ v={7,0,0,w};return v;}");
  puts("static double lz_mround(double x){return (double)(long long)(x>=0?x+0.5:x-0.5);}");
  puts("static LZ lz_wbal(LZ w){return lzmoney((double)((WAL*)w.p)->cents);}");
  puts("static LZ lz_pay(LZ src,LZ dst,LZ amt){WAL*s=(WAL*)src.p,*e=(WAL*)dst.p;long long c=(long long)amt.n;if(s->cents<c){fputs(\"MoneyError: insufficient funds\\n\",stderr);exit(1);}s->cents-=c;e->cents+=c;return lznil();}");
  /* paywall (t=8) + subscriptions: subscribe charges the wallet, pays the payee, records the (wallet,paywall) pair; `has` checks it */
  puts("typedef struct{long long price;char*period;char*name;void*payee;}PW;");
  puts("static LZ lzpaywall(double price,const char*period,const char*name,LZ payee){PW*p=(PW*)calloc(1,sizeof(PW));p->price=(long long)price;p->period=strdup(period);p->name=strdup(name);p->payee=payee.p;LZ v={8,0,0,p};return v;}");
  puts("static void**g_subs=0;static int g_nsub=0,g_subcap=0;");
  puts("static LZ lz_subscribe(LZ w,LZ pw){WAL*wl=(WAL*)w.p;PW*p=(PW*)pw.p;if(p->price>wl->cents){fputs(\"MoneyError: insufficient funds\\n\",stderr);exit(1);}wl->cents-=p->price;if(p->payee)((WAL*)p->payee)->cents+=p->price;if(g_nsub+2>g_subcap){g_subcap=g_subcap?g_subcap*2:16;g_subs=(void**)realloc(g_subs,g_subcap*sizeof(void*));}g_subs[g_nsub++]=w.p;g_subs[g_nsub++]=pw.p;return lznil();}");
  puts("static int lz_has_sub(LZ w,LZ pw){for(int i=0;i<g_nsub;i+=2)if(g_subs[i]==w.p&&g_subs[i+1]==pw.p)return 1;return 0;}");
  puts("static LZ lzlist(){LST*l=(LST*)calloc(1,sizeof(LST));LZ v={4,0,0,l};return v;}");
  puts("static LZ lz_push(LZ L,LZ e){LST*l=(LST*)L.p;if(l->n>=l->cap){l->cap=l->cap?l->cap*2:8;l->items=(LZ*)realloc(l->items,l->cap*sizeof(LZ));}l->items[l->n++]=e;return lznil();}");
  puts("static LZ lz_listn(int n,...){LZ L=lzlist();va_list ap;va_start(ap,n);for(int i=0;i<n;i++)lz_push(L,va_arg(ap,LZ));va_end(ap);return L;}");
  puts("typedef struct{char**keys;LZ*vals;int n,cap;}DCT;");
  puts("static LZ lzdict(){DCT*d=(DCT*)calloc(1,sizeof(DCT));LZ v={5,0,0,d};return v;}");
  puts("static int lz_dfind(LZ D,const char*k){DCT*d=(DCT*)D.p;for(int i=0;i<d->n;i++)if(!strcmp(d->keys[i],k))return i;return -1;}");
  puts("static void lz_dput(LZ D,const char*k,LZ val){int j=lz_dfind(D,k);DCT*d=(DCT*)D.p;if(j>=0){d->vals[j]=val;return;}if(d->n>=d->cap){d->cap=d->cap?d->cap*2:8;d->keys=(char**)realloc(d->keys,d->cap*sizeof(char*));d->vals=(LZ*)realloc(d->vals,d->cap*sizeof(LZ));}d->keys[d->n]=strdup(k);d->vals[d->n++]=val;}");
  puts("static int lz_lenN(LZ v){if(v.t==4)return((LST*)v.p)->n;if(v.t==5)return((DCT*)v.p)->n;if(v.t==2)return v.s?(int)strlen(v.s):0;return 0;}");
  puts("static LZ lz_index(LZ o,LZ i){if(o.t==5){if(i.t==2){int j=lz_dfind(o,i.s);return j>=0?((DCT*)o.p)->vals[j]:lznil();}int idx=(int)i.n;DCT*d=(DCT*)o.p;if(idx<0||idx>=d->n)return lznil();return lzstr(d->keys[idx]);}int idx=(int)i.n;if(o.t==4){LST*l=(LST*)o.p;if(idx<0)idx+=l->n;if(idx<0||idx>=l->n)return lznil();return l->items[idx];}if(o.t==2){int n=(int)strlen(o.s);if(idx<0)idx+=n;if(idx<0||idx>=n)return lznil();char b[2]={o.s[idx],0};return lzstr(b);}return lznil();}");
  puts("static int lztruthy(LZ v){if(v.t==1||v.t==3||v.t==11)return v.n!=0;if(v.t==2)return v.s&&v.s[0];if(v.t==4)return((LST*)v.p)->n>0;if(v.t==5)return((DCT*)v.p)->n>0;if(v.t==6)return v.n!=0;if(v.t==7||v.t==8||v.t==9||v.t==10)return 1;return 0;}");
  /* closures (t=9): fn pointer + by-value captured environment; map/filter/reduce use lz_call */
  puts("typedef struct{LZ(*fn)(LZ*,LZ*);LZ*cap;char*name;}CLO;");
  puts("static LZ lzclosure(LZ(*fn)(LZ*,LZ*),int arity,const char*name,int ncap,...){(void)arity;CLO*c=(CLO*)malloc(sizeof(CLO));c->fn=fn;c->name=strdup(name);c->cap=ncap?(LZ*)malloc(ncap*sizeof(LZ)):0;va_list ap;va_start(ap,ncap);for(int i=0;i<ncap;i++)c->cap[i]=va_arg(ap,LZ);va_end(ap);LZ v={9,0,0,c};return v;}");
  /* dynamic-module handle (t=10): DEXP/DMOD types + the registry they'll be backed by
   * (defined later in emit_c() once the discovered modules are known; forward-declared
   * here since lz_tostr/lz_dynimport/lz_method all reference them). */
  puts("typedef struct{const char*name;LZ(*fn)(LZ*,LZ*);}DEXP;");
  puts("typedef struct{const char*name;DEXP*exports;int nexports;}DMOD;");
  puts("extern DMOD g_dynmods[]; extern int g_ndynmods;");
  puts("static LZ lz_call(LZ f,int argc,LZ*args){(void)argc;if(f.t==9){CLO*c=(CLO*)f.p;return c->fn(c->cap,args);}fputs(\"LarzTypeError: value is not a function\\n\",stderr);exit(1);}");
  puts("static LZ lz_map(LZ f,LZ L){LZ r=lzlist();LST*l=(LST*)L.p;for(int i=0;i<l->n;i++){LZ a[1]={l->items[i]};lz_push(r,lz_call(f,1,a));}return r;}");
  puts("static LZ lz_filter(LZ f,LZ L){LZ r=lzlist();LST*l=(LST*)L.p;for(int i=0;i<l->n;i++){LZ a[1]={l->items[i]};if(lztruthy(lz_call(f,1,a)))lz_push(r,l->items[i]);}return r;}");
  puts("static LZ lz_reduce(LZ f,LZ L,LZ init,int has_init){LST*l=(LST*)L.p;int i=0;LZ acc;if(has_init)acc=init;else{acc=l->n?l->items[0]:lznil();i=1;}for(;i<l->n;i++){LZ a[2]={acc,l->items[i]};acc=lz_call(f,2,a);}return acc;}");
  puts("static char*lz_tostr(LZ v){");
  puts("  char b[64];");
  puts("  if(v.t==2)return v.s?v.s:(char*)\"\";");
  puts("  if(v.t==1){if(v.n==(long long)v.n)sprintf(b,\"%lld\",(long long)v.n);else sprintf(b,\"%g\",v.n);return strdup(b);}");
  puts("  if(v.t==3)return strdup(v.n?\"true\":\"false\");");
  puts("  if(v.t==4){LST*l=(LST*)v.p;size_t cap=64,len=0;char*r=(char*)malloc(cap);r[len++]='[';");
  puts("    for(int i=0;i<l->n;i++){if(i){r[len++]=',';r[len++]=' ';}char*e=lz_tostr(l->items[i]);size_t el=strlen(e);while(len+el+4>cap){cap*=2;r=(char*)realloc(r,cap);}memcpy(r+len,e,el);len+=el;}");
  puts("    r[len++]=']';r[len]=0;return r;}");
  puts("  if(v.t==5){DCT*d=(DCT*)v.p;size_t cap=64,len=0;char*r=(char*)malloc(cap);r[len++]='{';");
  puts("    for(int i=0;i<d->n;i++){if(i){r[len++]=',';r[len++]=' ';}char*e=lz_tostr(d->vals[i]);size_t kl=strlen(d->keys[i]),el=strlen(e);while(len+kl+el+8>cap){cap*=2;r=(char*)realloc(r,cap);}memcpy(r+len,d->keys[i],kl);len+=kl;r[len++]=':';r[len++]=' ';memcpy(r+len,e,el);len+=el;}");
  puts("    r[len++]='}';r[len]=0;return r;}");
  puts("  if(v.t==6){long long c=(long long)v.n,ac=c<0?-c:c;sprintf(b,\"%s$%lld.%02lld\",c<0?\"-\":\"\",ac/100,ac%100);return strdup(b);}");
  puts("  if(v.t==8){PW*p=(PW*)v.p;long long c=p->price<0?-p->price:p->price;char*r=(char*)malloc(strlen(p->name)+strlen(p->period)+48);sprintf(r,\"<paywall %s: $%lld.%02lld/%s>\",p->name,c/100,c%100,p->period);return r;}");
  puts("  if(v.t==9){CLO*c=(CLO*)v.p;char*r=(char*)malloc(strlen(c->name)+8);sprintf(r,\"<fn %s>\",c->name);return r;}");
  puts("  if(v.t==10){DMOD*m=(DMOD*)v.p;char*r=(char*)malloc(strlen(m->name)+16);sprintf(r,\"<module %s>\",m->name);return r;}");
  puts("  if(v.t==11)return strdup(v.n!=0?\"<capability granted>\":\"<capability revoked>\");");
  puts("  return strdup(\"nil\");}");
  /* dynamic import (t=10): a runtime name -> module handle, backed by a compile-time
   * closed-world registry (every .lz file the module search path can see when larzc
   * itself runs). g_dynmods/g_ndynmods are generated later in emit_c() once the set of
   * discovered modules is known. */
  puts("static LZ lz_dynimport(LZ namev){const char*want=lz_tostr(namev);for(int i=0;i<g_ndynmods;i++)if(!strcmp(g_dynmods[i].name,want)){LZ v={10,0,0,(void*)&g_dynmods[i]};return v;}fprintf(stderr,\"ImportError: cannot find module '%s' (not visible to larzc at compile time)\\n\",want);exit(1);}");
  /* file I/O + math/utility builtins (a compiled program runs on a hosted OS) */
  puts("static LZ lz_read_file(LZ p){FILE*f=fopen(lz_tostr(p),\"rb\");if(!f){fprintf(stderr,\"IOError: cannot read file '%s'\\n\",lz_tostr(p));exit(1);}size_t cap=65536,len=0;char*b=(char*)malloc(cap);size_t r;while((r=fread(b+len,1,cap-len,f))>0){len+=r;if(len==cap){cap*=2;b=(char*)realloc(b,cap);}}b[len]=0;fclose(f);LZ v={2,0,b,0};return v;}");
  puts("static LZ lz_write_file(LZ p,LZ c){FILE*f=fopen(lz_tostr(p),\"wb\");if(!f){fprintf(stderr,\"IOError: cannot write file '%s'\\n\",lz_tostr(p));exit(1);}fputs(lz_tostr(c),f);fclose(f);return lznil();}");
  puts("static LZ lz_append_file(LZ p,LZ c){FILE*f=fopen(lz_tostr(p),\"ab\");if(!f){fprintf(stderr,\"IOError: cannot append to file '%s'\\n\",lz_tostr(p));exit(1);}fputs(lz_tostr(c),f);fclose(f);return lznil();}");
  puts("static LZ lz_file_exists(LZ p){struct stat st;return lzbool(stat(lz_tostr(p),&st)==0);}");
  puts("static LZ lz_listdir(int n,LZ p){const char*path=n>=1?lz_tostr(p):\".\";DIR*d=opendir(path);if(!d){fprintf(stderr,\"IOError: cannot list directory '%s'\\n\",path);exit(1);}LZ r=lzlist();struct dirent*e;while((e=readdir(d))){if(!strcmp(e->d_name,\".\")||!strcmp(e->d_name,\"..\"))continue;lz_push(r,lzstr(e->d_name));}closedir(d);return r;}");
  puts("static LZ lz_mkdir(LZ p){return lzbool(mkdir(lz_tostr(p),0755)==0);}");
  puts("static LZ lz_remove(LZ p){return lzbool(remove(lz_tostr(p))==0);}");
  puts("static LZ lz_cwd(void){char b[4096];if(!getcwd(b,sizeof b)){fputs(\"IOError: cannot get cwd\\n\",stderr);exit(1);}return lzstr(b);}");
  puts("static LZ lz_join(int n,LZ L,LZ sep){const char*s=n>=2?lz_tostr(sep):\"\";LST*l=(LST*)L.p;size_t cap=64,len=0;char*r=(char*)malloc(cap);r[0]=0;for(int i=0;i<l->n;i++){if(i){size_t sl=strlen(s);while(len+sl+1>cap){cap*=2;r=(char*)realloc(r,cap);}memcpy(r+len,s,sl);len+=sl;}char*e=lz_tostr(l->items[i]);size_t el=strlen(e);while(len+el+1>cap){cap*=2;r=(char*)realloc(r,cap);}memcpy(r+len,e,el);len+=el;r[len]=0;}return lzstr(r);}");
  puts("static LZ lz_chr(LZ v){char b[2]={(char)(int)v.n,0};return lzstr(b);}");
  puts("static LZ lz_ord(LZ v){return lznum((double)(unsigned char)v.s[0]);}");
  puts("static LZ lz_absf(LZ v){if(v.t==6)return lzmoney(v.n<0?-v.n:v.n);return lznum(v.n<0?-v.n:v.n);}");
  puts("static LZ lz_sqrtf(LZ v){return lznum(sqrt(v.n));}");
  puts("static LZ lz_floorf(LZ v){return lznum(floor(v.n));}");
  puts("static LZ lz_ceilf(LZ v){return lznum(ceil(v.n));}");
  puts("static LZ lz_roundf(LZ v){return lznum(v.n>=0?floor(v.n+0.5):ceil(v.n-0.5));}");
  puts("static LZ lz_powf(LZ a,LZ b){return lznum(pow(a.n,b.n));}");
  puts("static LZ lz_exitf(int n,LZ c){exit(n>=1?(int)c.n:0);}");
  puts("static LZ lz_dictn(int n,...){LZ D=lzdict();va_list ap;va_start(ap,n);for(int i=0;i<n;i++){LZ k=va_arg(ap,LZ),val=va_arg(ap,LZ);lz_dput(D,lz_tostr(k),val);}va_end(ap);return D;}");
  puts("static LZ lz_keys(LZ D){LZ r=lzlist();if(D.t==5){DCT*d=(DCT*)D.p;for(int i=0;i<d->n;i++)lz_push(r,lzstr(d->keys[i]));}return r;}");
  puts("static LZ lz_setindex(LZ o,LZ k,LZ v){if(o.t==5)lz_dput(o,lz_tostr(k),v);else if(o.t==4){LST*l=(LST*)o.p;int idx=(int)k.n;if(idx<0)idx+=l->n;if(idx>=0&&idx<l->n)l->items[idx]=v;}return lznil();}");
  puts("static LZ lz_add(LZ a,LZ b){if(a.t==6&&b.t==6)return lzmoney(a.n+b.n);if(a.t==2||b.t==2){char*x=lz_tostr(a),*y=lz_tostr(b);char*r=malloc(strlen(x)+strlen(y)+1);strcpy(r,x);strcat(r,y);LZ v={2,0,r};return v;}return lznum(a.n+b.n);}");
  puts("static LZ lz_sub(LZ a,LZ b){if(a.t==6&&b.t==6)return lzmoney(a.n-b.n);return lznum(a.n-b.n);}");
  puts("static LZ lz_mul(LZ a,LZ b){if(a.t==6)return lzmoney(lz_mround(a.n*b.n));if(b.t==6)return lzmoney(lz_mround(b.n*a.n));return lznum(a.n*b.n);}");
  puts("static LZ lz_div(LZ a,LZ b){if(a.t==6)return lzmoney(lz_mround(a.n/b.n));return lznum(a.n/b.n);}");
  puts("static LZ lz_mod(LZ a,LZ b){return lznum((double)((long long)a.n%(long long)b.n));}");
  puts("static LZ lz_idiv(LZ a,LZ b){double d=a.n/b.n;long long f=(long long)d;if((double)f>d)f--;return lznum((double)f);}");
  puts("static LZ lz_pow(LZ a,LZ b){double r=1,x=a.n;long long e=(long long)b.n;for(long long i=0;i<e;i++)r*=x;return lznum(r);}");
  puts("static LZ lz_and(LZ a,LZ b){return lztruthy(a)?b:a;}");
  puts("static LZ lz_or(LZ a,LZ b){return lztruthy(a)?a:b;}");
  puts("static LZ lz_cmp(LZ a,LZ b,int op){double c;int eq;if(a.t==2&&b.t==2){int r=strcmp(a.s?a.s:\"\",b.s?b.s:\"\");c=r;eq=(r==0);}else{c=a.n-b.n;eq=(a.n==b.n);}int res=0;switch(op){case 0:res=eq;break;case 1:res=!eq;break;case 2:res=c<0;break;case 3:res=c>0;break;case 4:res=c<=0;break;case 5:res=c>=0;break;}return lzbool(res);}");
  puts("static LZ lz_in(LZ a,LZ b){if(b.t==4){LST*l=(LST*)b.p;for(int i=0;i<l->n;i++)if(lztruthy(lz_cmp(l->items[i],a,0)))return lzbool(1);return lzbool(0);}if(b.t==5)return lzbool(a.t==2&&lz_dfind(b,a.s)>=0);if(b.t==2)return lzbool(a.t==2&&a.s&&b.s&&strstr(b.s,a.s)!=0);return lzbool(0);}");
  puts("static LZ lz_method(LZ o,const char*m,int n,...){");
  puts("  va_list ap;va_start(ap,n);LZ A[4];for(int i=0;i<n&&i<4;i++)A[i]=va_arg(ap,LZ);va_end(ap);");
  puts("  if(o.t==4&&!strcmp(m,\"push\"))return lz_push(o,A[0]);");
  puts("  if(o.t==5){if(!strcmp(m,\"get\")){int j=lz_dfind(o,lz_tostr(A[0]));return j>=0?((DCT*)o.p)->vals[j]:(n>=2?A[1]:lznil());}if(!strcmp(m,\"has\"))return lzbool(lz_dfind(o,lz_tostr(A[0]))>=0);}");
  puts("  if(o.t==10){DMOD*dm=(DMOD*)o.p;for(int i=0;i<dm->nexports;i++)if(!strcmp(dm->exports[i].name,m))return dm->exports[i].fn(0,A);fprintf(stderr,\"LarzNameError: module '%s' has no member '%s'\\n\",dm->name,m);exit(1);}");
  puts("  if(o.t!=2)return lznil();");
  puts("  char*s=o.s?o.s:(char*)\"\";int L=(int)strlen(s);");
  puts("  if(!strcmp(m,\"upper\")){char*r=strdup(s);for(char*p=r;*p;p++)if(*p>='a'&&*p<='z')*p-=32;LZ v={2,0,r,0};return v;}");
  puts("  if(!strcmp(m,\"lower\")){char*r=strdup(s);for(char*p=r;*p;p++)if(*p>='A'&&*p<='Z')*p+=32;LZ v={2,0,r,0};return v;}");
  puts("  if(!strcmp(m,\"strip\")){int a=0,b=L;while(a<b&&(s[a]==' '||s[a]=='\\t'||s[a]=='\\n'||s[a]=='\\r'))a++;while(b>a&&(s[b-1]==' '||s[b-1]=='\\t'||s[b-1]=='\\n'||s[b-1]=='\\r'))b--;char*r=(char*)malloc(b-a+1);memcpy(r,s+a,b-a);r[b-a]=0;LZ v={2,0,r,0};return v;}");
  puts("  if(!strcmp(m,\"contains\"))return lzbool(A[0].s&&strstr(s,A[0].s)!=0);");
  puts("  if(!strcmp(m,\"starts_with\"))return lzbool(A[0].s&&strncmp(s,A[0].s,strlen(A[0].s))==0);");
  puts("  if(!strcmp(m,\"ends_with\")){int al=A[0].s?(int)strlen(A[0].s):0;return lzbool(al<=L&&strcmp(s+L-al,A[0].s)==0);}");
  puts("  if(!strcmp(m,\"split\")){LZ r=lzlist();const char*sep=A[0].s;int sl=(int)strlen(sep);const char*p=s;if(sl==0){lz_push(r,lzstr(s));return r;}for(;;){const char*q=strstr(p,sep);if(!q){lz_push(r,lzstr(p));break;}char*pt=(char*)malloc(q-p+1);memcpy(pt,p,q-p);pt[q-p]=0;LZ pv={2,0,pt,0};lz_push(r,pv);p=q+sl;}return r;}");
  puts("  if(!strcmp(m,\"replace\")){const char*a=A[0].s,*b=A[1].s;int al=(int)strlen(a),bl=(int)strlen(b);size_t cap=L+1,len=0;char*r=(char*)malloc(cap);const char*p=s;while(*p){if(al&&!strncmp(p,a,al)){while(len+bl+1>cap){cap*=2;r=(char*)realloc(r,cap);}memcpy(r+len,b,bl);len+=bl;p+=al;}else{if(len+2>cap){cap*=2;r=(char*)realloc(r,cap);}r[len++]=*p++;}}r[len]=0;LZ v={2,0,r,0};return v;}");
  puts("  if(!strcmp(m,\"ljust\")){int w=(int)A[0].n;if(w<=L)return lzstr(s);char*r=(char*)malloc(w+1);memcpy(r,s,L);for(int i=L;i<w;i++)r[i]=' ';r[w]=0;LZ v={2,0,r,0};return v;}");
  puts("  return lznil();}");
  puts("static void lz_p1(LZ v){char*s=lz_tostr(v);fputs(s,stdout);}");
  puts("static LZ lz_print(int n,...){va_list ap;va_start(ap,n);for(int i=0;i<n;i++){if(i)fputc(' ',stdout);lz_p1(va_arg(ap,LZ));}va_end(ap);fputc('\\n',stdout);return lznil();}");
  puts("static LZ lz_sleep(LZ secs){ usleep((useconds_t)(secs.n*1e6)); return lznil(); }");
  puts("static LZ lz_len(LZ v){return lznum((double)lz_lenN(v));}");
  puts("static LZ lz_int(LZ v){return v.t==2?lznum((double)atoll(v.s)):lznum((double)(long long)v.n);}");
  puts("static LZ lz_str_(LZ v){return lzstr(lz_tostr(v));}");
}
/* ---- larzc static import: parse a module, mangle its symbols, inline it ----
 * `import "m" as alias` is resolved + parsed at compile time; the module's
 * top-level symbols are prefixed (__m_alias_) and inlined into the output, and
 * alias.member references rewire to the prefixed name. (Nested module imports
 * are rejected with a clear error. Dynamic imports - `import <expr> as alias`
 * where the path isn't a literal string - are handled separately below.) */
static char g_srcdir[4096]="";

/* sprintf isn't declared in the LarzOS kernel's freestanding libc (only
 * snprintf is) - this is the exact size needed, so functionally identical. */
static char *prefixed(const char *prefix, const char *name){
  size_t n=strlen(prefix)+strlen(name)+1; char *x=(char*)malloc(n); snprintf(x,n,"%s%s",prefix,name); return x;
}
static void rename_syms(Node *n, SSet *syms, const char *prefix){
  if(!n) return;
  switch(n->kind){
    case N_NAME: case N_FN: case N_LET: case N_ASSIGN: case N_PRICE: case N_WALLET: case N_PAYWALL:
      if(n->name && ss_has(syms,n->name)){ n->name=prefixed(prefix,n->name); } break;
    default: break;
  }
  if(n->kind==N_PAY||n->kind==N_SUBSCRIBE){
    if(n->src && ss_has(syms,n->src)) n->src=prefixed(prefix,n->src);
    if(n->dst && ss_has(syms,n->dst)) n->dst=prefixed(prefix,n->dst);
  }
  rename_syms(n->a,syms,prefix); rename_syms(n->b,syms,prefix); rename_syms(n->c,syms,prefix);
  for(int i=0;i<n->nkids;i++) rename_syms(n->kids[i],syms,prefix);
}
static char *imp_resolve(const char *want){
  static char buf[4096]; FILE*f; const char *exts[2]={"",".lz"};
  for(int e=0;e<2;e++){
    if(g_srcdir[0]){ snprintf(buf,sizeof buf,"%s/%s%s",g_srcdir,want,exts[e]); if((f=fopen(buf,"rb"))){fclose(f);return buf;} }
    snprintf(buf,sizeof buf,"%s%s",want,exts[e]); if((f=fopen(buf,"rb"))){fclose(f);return buf;}
  }
  const char *lp=getenv("LARZSCRIPT_PATH");
  if(lp){ const char *s=lp; while(*s){ const char *e2=s; while(*e2 && *e2!=':') e2++; int len=(int)(e2-s); if(len>0 && len<3000){ char dir[3072]; memcpy(dir,s,(size_t)len); dir[len]=0; for(int e=0;e<2;e++){ snprintf(buf,sizeof buf,"%s/%s%s",dir,want,exts[e]); if((f=fopen(buf,"rb"))){fclose(f);return buf;} } } s = *e2 ? e2+1 : e2; } }
  return 0;
}
/* parse a module, prefix its symbols, and append its top-level nodes to *T */
static void load_module(Node *imp, Node ***T, int *nT){
  if(imp->a->kind!=N_STR){ fprintf(stderr,"larzc: dynamic import is unsupported (import needs a literal path)\n"); exit(1); }
  char *file=imp_resolve(imp->a->str);
  if(!file){ fprintf(stderr,"larzc: cannot find module '%s'\n", imp->a->str); exit(1); }
  Node *mp=parse_program(lex(read_all(file)));
  Import im; im.alias=imp->name?imp->name:imp->a->str; memset(&im.fns,0,sizeof im.fns); memset(&im.globals,0,sizeof im.globals);
  char pfx[256]; snprintf(pfx,sizeof pfx,"__m_%s_", im.alias); im.prefix=strdup(pfx);
  SSet syms={0,0,0};
  for(int i=0;i<mp->nkids;i++){ Node*k=mp->kids[i];
    if(k->kind==N_IMPORT){ fprintf(stderr,"larzc: nested import in module '%s' is unsupported\n", im.alias); exit(1); }
    if(k->kind==N_FN && k->name){ ss_add(&syms,k->name); ss_add(&im.fns,k->name); }
    else if((k->kind==N_LET||k->kind==N_PRICE||k->kind==N_WALLET||k->kind==N_PAYWALL)&&k->name){ ss_add(&syms,k->name); ss_add(&im.globals,k->name); }
  }
  rename_syms(mp, &syms, im.prefix);
  for(int i=0;i<mp->nkids;i++){ *T=(Node**)realloc(*T,(*nT+1)*sizeof(Node*)); (*T)[(*nT)++]=mp->kids[i]; }
  g_imp=(Import*)realloc(g_imp,(g_nimp+1)*sizeof(Import)); g_imp[g_nimp++]=im;
}

/* ---- larzc dynamic import: `import <expr> as alias` where <expr> isn't a
 * literal string. The alias is only known at RUNTIME, so this can't inline a
 * single fixed module the way load_module() does. Instead larzc compiles a
 * CLOSED WORLD: every .lz file visible on the module search path (the same
 * directories the interpreter's resolve_import searches) AT COMPILE TIME is
 * parsed + inlined (like a static import, own symbol prefix `__d_<name>_`)
 * and registered by its basename in a runtime name->module-handle table
 * (DMOD/g_dynmods, declared in emit_runtime()). `import cmd as m` then
 * compiles to a runtime table lookup (lz_dynimport), and `m.fn(args)` already
 * falls through to the generic lz_method() dispatcher (see N_METHOD in
 * ec_expr) which was taught to search a module handle's export table.
 * LIMITATION: a module dropped into the search path AFTER compiling (e.g. by
 * an on-device package manager) is invisible to the compiled binary - it only
 * sees what larzc could see. Modules with a nested `import` of their own are
 * rejected, same as static imports. */
typedef struct { char *name, *prefix; SSet fns; } DynMod;
static DynMod *g_dynmod=0; static int g_ndynmod=0;

/* true if `n` contains an N_IMPORT that ISN'T a literal-path top-level import
 * of `prog` (i.e. one load_module() already/will handle) - covers both a
 * dynamic-path import and any import nested inside a fn/if/while/for body. */
static int has_extra_import(Node *n, Node *prog){
  if(!n) return 0;
  if(n->kind==N_IMPORT){
    int top_literal=0;
    if(n->a->kind==N_STR) for(int i=0;i<prog->nkids;i++) if(prog->kids[i]==n){ top_literal=1; break; }
    if(!top_literal) return 1;
  }
  if(has_extra_import(n->a,prog)||has_extra_import(n->b,prog)||has_extra_import(n->c,prog)) return 1;
  for(int i=0;i<n->nkids;i++) if(has_extra_import(n->kids[i],prog)) return 1;
  return 0;
}
static int needs_dynamic_registry(Node *prog){
  for(int i=0;i<prog->nkids;i++) if(has_extra_import(prog->kids[i],prog)) return 1;
  return 0;
}
static char *basename_noext(const char *path){
  const char *base=strrchr(path,'/'); base=base?base+1:path;
  char *r=strdup(base); char *dot=strrchr(r,'.'); if(dot && !strcmp(dot,".lz")) *dot=0;
  return r;
}
static void add_uniq_lz_file(char ***names, int *nn, const char *path){
  const char *base=strrchr(path,'/'); base=base?base+1:path;
  for(int i=0;i<*nn;i++){ const char*b2=strrchr((*names)[i],'/'); b2=b2?b2+1:(*names)[i]; if(!strcmp(b2,base)) return; }
  *names=(char**)realloc(*names,(*nn+1)*sizeof(char*)); (*names)[(*nn)++]=strdup(path);
}
static void scan_dir_for_lz(const char *dir, char ***names, int *nn, const char *skip_path){
  DIR *d=opendir(dir); if(!d) return;
  struct dirent *e;
  while((e=readdir(d))){
    size_t l=strlen(e->d_name);
    if(l<=3 || strcmp(e->d_name+l-3,".lz")!=0) continue;
    char path[4096]; snprintf(path,sizeof path,"%s/%s",dir,e->d_name);
    if(skip_path){ const char*b1=strrchr(path,'/'); b1=b1?b1+1:path; const char*b2=strrchr(skip_path,'/'); b2=b2?b2+1:skip_path; if(!strcmp(b1,b2)) continue; }
    add_uniq_lz_file(names,nn,path);
  }
  closedir(d);
}
/* enumerate every .lz file the runtime's resolve_import() could ever find -
 * source dir, $LARZSCRIPT_PATH, ~/.larzscript/lib, ./lz_modules - excluding
 * the entry program itself (compile-time equivalent of the interpreter's
 * search order; deliberately no strtok, see commit eb92ec2). */
static char **discover_dynamic_modules(int *outn, const char *main_path){
  char **names=0; int nn=0;
  scan_dir_for_lz(g_srcdir[0]?g_srcdir:".", &names,&nn, main_path);
  const char *lp=getenv("LARZSCRIPT_PATH");
  if(lp){ const char *s=lp; while(*s){ const char *e2=s; while(*e2 && *e2!=':') e2++; int len=(int)(e2-s); if(len>0 && len<4000){ char dir[4096]; memcpy(dir,s,(size_t)len); dir[len]=0; scan_dir_for_lz(dir,&names,&nn,main_path); } s = *e2 ? e2+1 : e2; } }
  const char *home=lz_home_dir();
  if(home){ char dir[4096]; snprintf(dir,sizeof dir,"%s/.larzscript/lib",home); scan_dir_for_lz(dir,&names,&nn,main_path); }
  scan_dir_for_lz("lz_modules",&names,&nn,main_path);
  *outn=nn; return names;
}
/* parse+prefix+inline one discovered module file, same shape as load_module()
 * but recorded into g_dynmod (keyed by runtime name) instead of g_imp (which
 * is keyed by a compile-time-known surface alias, not applicable here). */
static void inline_dynamic_module(const char *file, const char *modname, Node ***T, int *nT){
  Node *mp=parse_program(lex(read_all(file)));
  char pfx[256]; snprintf(pfx,sizeof pfx,"__d_%s_",modname);
  SSet syms={0,0,0}, fns={0,0,0};
  /* Discovery is speculative (every .lz the search path can see, whether or not this
   * program actually dynamic-imports it by that name) - so a discovered module may
   * NOT have loose top-level statements. Unlike a static `import "x" as y` (an
   * explicit, one-time request to run x's body), unconditionally inlining and
   * running an unrelated bystander script's side effects into every other compiled
   * program would be silently wrong. Require modules on the search path to be pure
   * fn/let/price/wallet/paywall declarations; reject anything else with a clear error
   * rather than guess when (if ever) it's safe to run. */
  for(int i=0;i<mp->nkids;i++){ Node*k=mp->kids[i];
    if(k->kind==N_IMPORT){ fprintf(stderr,"larzc: nested import in dynamic module '%s' is unsupported\n", modname); exit(1); }
    if(k->kind==N_FN && k->name){ ss_add(&syms,k->name); ss_add(&fns,k->name); }
    else if((k->kind==N_LET||k->kind==N_PRICE||k->kind==N_WALLET||k->kind==N_PAYWALL)&&k->name){ ss_add(&syms,k->name); }
    else { fprintf(stderr,"larzc: '%s' (found on the module search path) has top-level code that isn't a fn/let/price/wallet/paywall declaration, so larzc can't safely treat it as a dynamic-import target (it would run as a side effect of ANY program that dynamically imports anything); move it out of the search path if it's not meant to be imported\n", file); exit(1); }
  }
  rename_syms(mp, &syms, pfx);
  for(int i=0;i<mp->nkids;i++){ *T=(Node**)realloc(*T,(*nT+1)*sizeof(Node*)); (*T)[(*nT)++]=mp->kids[i]; }
  g_dynmod=(DynMod*)realloc(g_dynmod,(g_ndynmod+1)*sizeof(DynMod));
  g_dynmod[g_ndynmod].name=strdup(modname); g_dynmod[g_ndynmod].prefix=strdup(pfx); g_dynmod[g_ndynmod].fns=fns;
  g_ndynmod++;
}
static void emit_topfn_sig(Node *k){ printf("LZ %s(", k->name); for(int j=0;j<k->nparams;j++){ if(j)printf(","); printf("LZ %s", k->params[j]); } if(k->nparams==0) printf("void"); printf(")"); }
static void emit_c(Node *prog, const char *main_path){
  emit_runtime();
  /* Build the combined top-level: inlined module nodes (in import order) then the
   * main program's non-import nodes. Everything downstream treats this uniformly. */
  Node **T=0; int nT=0;
  for(int i=0;i<prog->nkids;i++){ Node*k=prog->kids[i]; if(k->kind==N_IMPORT && k->a->kind==N_STR) load_module(k, &T, &nT); }
  /* dynamic import: if any import (top-level or nested) needs a runtime lookup,
   * compile the whole closed world of discoverable modules and register them. */
  if(needs_dynamic_registry(prog)){
    int nfiles=0; char **files=discover_dynamic_modules(&nfiles, main_path);
    for(int i=0;i<nfiles;i++){ char *modname=basename_noext(files[i]); inline_dynamic_module(files[i], modname, &T, &nT); free(modname); }
  }
  for(int i=0;i<prog->nkids;i++){ Node*k=prog->kids[i]; if(k->kind==N_IMPORT && k->a->kind==N_STR) continue; T=(Node**)realloc(T,(nT+1)*sizeof(Node*)); T[nT++]=k; }

  /* gather top-level globals + functions */
  for(int i=0;i<nT;i++){ Node*k=T[i];
    if((k->kind==N_LET||k->kind==N_PRICE||k->kind==N_WALLET||k->kind==N_PAYWALL||k->kind==N_CAPABILITY) && k->name) ss_add(&g_globals, k->name);
    if(k->kind==N_FN && k->name){ ss_add(&g_globals, k->name); g_topfn=(Node**)realloc(g_topfn,(g_ntopfn+1)*sizeof(Node*)); g_topfn[g_ntopfn++]=k; }
  }
  /* give every nested lambda / nested named fn a hoist id */
  for(int i=0;i<nT;i++){ Node*k=T[i]; if(k->kind==N_FN) collect_lams(k->b); else collect_lams(k); }

  /* top-level let/price/wallet/paywall become C globals so functions can reference them */
  for(int i=0;i<nT;i++){ Node*k=T[i]; if((k->kind==N_LET||k->kind==N_PRICE||k->kind==N_WALLET||k->kind==N_PAYWALL) && k->name) printf("LZ %s;\n", k->name); }
  /* top-level capabilities become LZ-typed C globals too (t=11, .n=0/1 for
   * revoked/granted) - NOT a plain int: every other value in this codegen is
   * a uniform LZ struct (print/expressions/function args all assume it), so
   * a raw int here would corrupt any variadic call it's passed through (this
   * broke print(capability) with a real segfault before being caught).
   * A braced struct literal is a valid *constant* initializer (no function
   * call), so - unlike wallet/paywall, whose balance may be a runtime
   * expression - this needs no separate init step in main() either. */
  for(int i=0;i<nT;i++){ Node*k=T[i]; if(k->kind==N_CAPABILITY && k->name) printf("LZ %s = {11,0,0,0};\n", k->name); }
  /* forward declarations: top-level fns + their value-adapters + hoisted lambdas */
  for(int i=0;i<g_ntopfn;i++){ emit_topfn_sig(g_topfn[i]); printf(";\n"); printf("LZ __adapt_%s(LZ*,LZ*);\n", g_topfn[i]->name); }
  for(int i=0;i<g_nlams;i++) printf("LZ __lam%d(LZ*,LZ*);\n", g_lams[i]->lam_id);
  /* dynamic-import registry: one DEXP[] per discovered module (name -> its
   * value-adapter, already generated above like any other top-level fn),
   * then the DMOD[] table lz_dynimport()/lz_method() search by runtime name. */
  for(int i=0;i<g_ndynmod;i++){
    DynMod *dm=&g_dynmod[i];
    printf("DEXP __dexp_%s[] = {", dm->name);
    for(int j=0;j<dm->fns.n;j++){ if(j) printf(","); printf("{\"%s\",__adapt_%s%s}", dm->fns.v[j], dm->prefix, dm->fns.v[j]); }
    if(dm->fns.n==0) printf("{0,0}");
    printf("};\n");
  }
  printf("DMOD g_dynmods[] = {");
  for(int i=0;i<g_ndynmod;i++){ if(i) printf(","); printf("{\"%s\",__dexp_%s,%d}", g_dynmod[i].name, g_dynmod[i].name, g_dynmod[i].fns.n); }
  if(g_ndynmod==0) printf("{0,0,0}");
  printf("};\nint g_ndynmods = %d;\n", g_ndynmod);
  /* top-level fn definitions */
  for(int i=0;i<g_ntopfn;i++){ Node*k=g_topfn[i]; emit_topfn_sig(k); printf("{\n"); ec_stmt(k->b,1); printf("  return lznil();\n}\n"); }
  /* value-adapters: wrap a named fn under the uniform closure calling convention */
  for(int i=0;i<g_ntopfn;i++){ Node*k=g_topfn[i]; printf("LZ __adapt_%s(LZ*cap,LZ*a){(void)cap;(void)a;return %s(", k->name, k->name); for(int j=0;j<k->nparams;j++){ if(j)printf(","); printf("a[%d]", j); } printf(");}\n"); }
  /* hoisted lambda / nested-fn definitions: params from __args, captures from __cap */
  for(int i=0;i<g_nlams;i++){ Node*lam=g_lams[i];
    printf("LZ __lam%d(LZ*__cap,LZ*__args){(void)__cap;(void)__args;\n", lam->lam_id);
    for(int j=0;j<lam->nparams;j++) printf("  LZ %s = __args[%d];\n", lam->params[j], j);
    SSet cap={0,0,0}; lam_captures(lam,&cap);
    for(int j=0;j<cap.n;j++) printf("  LZ %s = __cap[%d];\n", cap.v[j], j);
    free(cap.v);
    ec_stmt(lam->b,1);
    printf("  return lznil();\n}\n");
  }
  printf("int main(void){\n");
  for(int i=0;i<nT;i++){ Node*k=T[i];
    if(k->kind==N_FN) continue;
    if(k->kind==N_CAPABILITY) continue;   /* already declared+initialized as a global above */
    if(k->kind==N_LET||k->kind==N_PRICE){ ec_ind(1); printf("%s = ", k->name); ec_expr(k->a); printf(";\n"); }
    else if(k->kind==N_WALLET){ ec_ind(1); printf("%s = lzwallet(", k->name); if(k->a){ printf("("); ec_expr(k->a); printf(").n"); } else printf("0"); printf(");\n"); }
    else if(k->kind==N_PAYWALL){ ec_ind(1); printf("%s = lzpaywall((", k->name); ec_expr(k->a); printf(").n, \""); ec_raw(k->period); printf("\", \""); ec_raw(k->name); printf("\", %s);\n", k->dst); }
    else ec_stmt(k,1);
  }
  printf("  return 0;\n}\n");
}

/* ======================================================================
 * larzscript update: self-update, verified via a from-scratch SHA-256.
 * Single-shot (hash a whole in-memory buffer at once) - update() only ever
 * hashes short-lived files, no streaming API needed. No new #include: uses
 * plain `unsigned int`/`unsigned long long` instead of <stdint.h>, since
 * that header has no replacement in the LarzOS kernel's freestanding libc
 * and this file is compiled into the kernel too - matches how the rest of
 * this file already avoids fixed-width types everywhere else.
 *
 * The whole feature (this hash included, not just the OS-specific fetch/
 * replace logic below) is hosted-OS-only: readlink, system(), chmod and
 * friends have no meaning (and often no declaration at all) in the LarzOS
 * kernel's freestanding libk.c, and even the hash would be dead code there
 * (unused-function warnings under -Wall). __STDC_HOSTED__ is the portable,
 * standard way to detect that (0 under -ffreestanding) - checked ahead of
 * any OS-specific #ifdef, since the freestanding kernel build is still done
 * with a normal Linux-targeted gcc and so still defines __linux__ etc.,
 * which would wrongly select the hosted-Linux branch if checked first. A
 * broken build here was caught by actually building the kernel, not just
 * the three hosted platforms native.yml already covers.
 * ==================================================================== */
#if !defined(__STDC_HOSTED__) || __STDC_HOSTED__
typedef struct { unsigned int state[8]; unsigned long long bitlen; unsigned char data[64]; unsigned datalen; } SHA256_CTX;

static const unsigned int sha256_k[64] = {
  0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
  0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
  0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
  0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
  0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
  0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
  0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
  0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};
#define SHA256_ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void sha256_transform(SHA256_CTX *ctx, const unsigned char data[]){
  unsigned int a,b,c,d,e,f,g,h,t1,t2,m[64]; int i,j;
  for(i=0,j=0;i<16;i++,j+=4) m[i]=((unsigned)data[j]<<24)|((unsigned)data[j+1]<<16)|((unsigned)data[j+2]<<8)|(unsigned)data[j+3];
  for(;i<64;i++){
    unsigned int s0=SHA256_ROTR(m[i-15],7)^SHA256_ROTR(m[i-15],18)^(m[i-15]>>3);
    unsigned int s1=SHA256_ROTR(m[i-2],17)^SHA256_ROTR(m[i-2],19)^(m[i-2]>>10);
    m[i]=m[i-16]+s0+m[i-7]+s1;
  }
  a=ctx->state[0];b=ctx->state[1];c=ctx->state[2];d=ctx->state[3];
  e=ctx->state[4];f=ctx->state[5];g=ctx->state[6];h=ctx->state[7];
  for(i=0;i<64;i++){
    unsigned int S1=SHA256_ROTR(e,6)^SHA256_ROTR(e,11)^SHA256_ROTR(e,25);
    unsigned int ch=(e&f)^((~e)&g);
    t1=h+S1+ch+sha256_k[i]+m[i];
    unsigned int S0=SHA256_ROTR(a,2)^SHA256_ROTR(a,13)^SHA256_ROTR(a,22);
    unsigned int maj=(a&b)^(a&c)^(b&c);
    t2=S0+maj;
    h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
  }
  ctx->state[0]+=a;ctx->state[1]+=b;ctx->state[2]+=c;ctx->state[3]+=d;
  ctx->state[4]+=e;ctx->state[5]+=f;ctx->state[6]+=g;ctx->state[7]+=h;
}
static void sha256_init(SHA256_CTX *ctx){
  ctx->datalen=0; ctx->bitlen=0;
  ctx->state[0]=0x6a09e667u;ctx->state[1]=0xbb67ae85u;ctx->state[2]=0x3c6ef372u;ctx->state[3]=0xa54ff53au;
  ctx->state[4]=0x510e527fu;ctx->state[5]=0x9b05688cu;ctx->state[6]=0x1f83d9abu;ctx->state[7]=0x5be0cd19u;
}
static void sha256_update(SHA256_CTX *ctx, const unsigned char *data, size_t len){
  for(size_t i=0;i<len;i++){
    ctx->data[ctx->datalen]=data[i]; ctx->datalen++;
    if(ctx->datalen==64){ sha256_transform(ctx,ctx->data); ctx->bitlen+=512; ctx->datalen=0; }
  }
}
static void sha256_final(SHA256_CTX *ctx, unsigned char hash[32]){
  unsigned i=ctx->datalen;
  if(ctx->datalen<56){ ctx->data[i++]=0x80; while(i<56) ctx->data[i++]=0; }
  else { ctx->data[i++]=0x80; while(i<64) ctx->data[i++]=0; sha256_transform(ctx,ctx->data); memset(ctx->data,0,56); }
  ctx->bitlen += (unsigned long long)ctx->datalen*8;
  for(i=0;i<8;i++) ctx->data[63-i]=(unsigned char)(ctx->bitlen>>(i*8));
  sha256_transform(ctx,ctx->data);
  for(i=0;i<4;i++) for(int s=0;s<8;s++) hash[s*4+i]=(unsigned char)((ctx->state[s]>>(24-i*8))&0xffu);
}
static void sha256_hex(const unsigned char *data, size_t len, char out[65]){
  SHA256_CTX ctx; unsigned char hash[32];
  sha256_init(&ctx); sha256_update(&ctx,data,len); sha256_final(&ctx,hash);
  for(int i=0;i<32;i++) sprintf(out+i*2,"%02x",hash[i]);
  out[64]=0;
}
/* NIST test vectors - run every time cmd_update() is invoked, before trusting
 * any comparison; a broken hash routine would otherwise silently defeat the
 * one safety property this whole feature exists for. */
static int sha256_selftest(void){
  char out[65];
  sha256_hex((const unsigned char*)"", 0, out);
  if(strcmp(out,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")!=0) return 0;
  sha256_hex((const unsigned char*)"abc", 3, out);
  if(strcmp(out,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")!=0) return 0;
  return 1;
}

/* platform asset name - must exactly match what native.yml's release job
 * produces and what install.sh/install.ps1 already request. */
static const char *update_asset_name(void){
#if defined(_WIN32)
  return "larzscript-windows-x86_64.exe";
#elif defined(__APPLE__)
  #if defined(__aarch64__)
    return "larzscript-macos-arm64";
  #else
    return "larzscript-macos-x86_64";
  #endif
#elif defined(__aarch64__)
  return "larzscript-linux-aarch64";
#elif defined(__x86_64__)
  return "larzscript-linux-x86_64";
#else
  return NULL;
#endif
}

static char *update_read_file(const char *path, size_t *outlen){
  FILE *f=fopen(path,"rb"); if(!f) return NULL;
  size_t cap=1<<20,len=0; char *b=xmalloc(cap); size_t r;
  while((r=fread(b+len,1,cap-len,f))>0){ len+=r; if(len==cap){ cap*=2; b=realloc(b,cap); } }
  fclose(f); if(outlen) *outlen=len; return b;
}

/* the currently running binary's own path - argv[0] can be relative,
 * PATH-resolved, or otherwise unreliable, and acting on the wrong path here
 * is the one mistake in this whole feature that isn't cheaply reversible. */
static char *update_self_path(void){
#if defined(_WIN32)
  char buf[4096];
  DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
  if(n==0 || n>=sizeof buf) return NULL;
  return xstrdup(buf);
#elif defined(__linux__)
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if(n<=0) return NULL;
  buf[n]=0; return xstrdup(buf);
#elif defined(__APPLE__)
  /* macOS has no /proc - _NSGetExecutablePath is the documented way to
   * get argv[0]'s resolved path; it can still be a symlink, so resolve
   * that too (matches readlink's behavior on Linux above, and matters
   * here since Homebrew-style installs commonly symlink into a Cellar). */
  char raw[4096]; uint32_t sz = sizeof raw;
  if(_NSGetExecutablePath(raw, &sz)!=0) return NULL;
  char real[4096];
  if(realpath(raw, real)==NULL) return xstrdup(raw);
  return xstrdup(real);
#else
  return NULL;
#endif
}

/* curl on Linux (matches install.sh); PowerShell's Invoke-WebRequest on
 * Windows, NOT curl - install.ps1 already uses Invoke-WebRequest (verified
 * working), and a plain `curl` isn't guaranteed present (confirmed absent
 * under Wine while testing this; even on real Windows 10/11, System32's
 * curl.exe is a relatively recent addition (1803+) that some locked-down or
 * Server Core images strip - PowerShell itself is the safer universal bet). */
static int update_fetch(const char *url, const char *outpath){
  char cmd[8448];
#ifdef _WIN32
  snprintf(cmd,sizeof cmd,"powershell -NoProfile -Command \"Invoke-WebRequest -UseBasicParsing -Uri '%s' -OutFile '%s'\"",url,outpath);
#else
  snprintf(cmd,sizeof cmd,"curl -fsSL \"%s\" -o \"%s\"",url,outpath);
#endif
  return system(cmd);
}

static int cmd_update(void){
  if(!sha256_selftest()){ fprintf(stderr,"larzscript update: internal SHA-256 self-test failed - aborting, nothing touched\n"); return 1; }

  const char *asset = update_asset_name();
  if(!asset){ fprintf(stderr,"larzscript update: self-update isn't available for this platform yet; rebuild from source\n"); return 1; }

  char *self = update_self_path();
  if(!self){ fprintf(stderr,"larzscript update: could not determine this program's own path\n"); return 1; }

  char selfdir[4096]; snprintf(selfdir,sizeof selfdir,"%s",self);
  { char *sl=strrchr(selfdir,'/'); if(!sl) sl=strrchr(selfdir,'\\'); if(sl) *sl=0; else snprintf(selfdir,sizeof selfdir,"."); }

  /* fail fast: can we even write here, before any network round-trip? */
  char probe[4096]; snprintf(probe,sizeof probe,"%s/.larzscript_update_probe",selfdir);
  FILE *pf=fopen(probe,"wb");
  if(!pf){ fprintf(stderr,"larzscript update: cannot write to '%s' - reinstall to a user-writable location\n", selfdir); free(self); return 1; }
  fclose(pf); remove(probe);

  /* clean up a leftover Windows *.old from a previous update (see the atomic-
   * replace step below) - only checked here, not on every launch, since a
   * stray .old is wasted disk space, not a correctness issue. */
  { char oldpath[4160]; snprintf(oldpath,sizeof oldpath,"%s.old",self); remove(oldpath); }

  const char *sumsurl = "https://github.com/larz-scripter/larzscript/releases/latest/download/SHA256SUMS";
  char sumstmp[4096]; snprintf(sumstmp,sizeof sumstmp,"%s/.larzscript_SHA256SUMS",selfdir);
  if(update_fetch(sumsurl,sumstmp)!=0){ fprintf(stderr,"larzscript update: could not reach GitHub to check for updates\n"); free(self); return 1; }
  size_t sumslen; char *sums=update_read_file(sumstmp,&sumslen);
  remove(sumstmp);
  if(!sums){ fprintf(stderr,"larzscript update: could not read the downloaded SHA256SUMS\n"); free(self); return 1; }

  char expected[65]="";
  { char *p=sums;
    while(p && *p){
      char *nl=strchr(p,'\n'); size_t linelen = nl ? (size_t)(nl-p) : strlen(p);
      if(linelen>66 && strstr(p,asset)==p+66){ memcpy(expected,p,64); expected[64]=0; break; }
      p = nl ? nl+1 : NULL;
    }
  }
  free(sums);
  if(!expected[0]){ fprintf(stderr,"larzscript update: no entry for '%s' in the release's SHA256SUMS (release incomplete?)\n", asset); free(self); return 1; }

  size_t selflen; char *selfbuf=update_read_file(self,&selflen);
  if(!selfbuf){ fprintf(stderr,"larzscript update: could not read the running binary at '%s'\n", self); free(self); return 1; }
  char selfhash[65]; sha256_hex((unsigned char*)selfbuf,selflen,selfhash);
  free(selfbuf);
  if(strcmp(selfhash,expected)==0){ printf("larzscript is already up to date (%s)\n", LARZSCRIPT_VERSION); free(self); return 0; }

  char url[512]; snprintf(url,sizeof url,
    "https://github.com/larz-scripter/larzscript/releases/latest/download/%s", asset);
  char tmp[4160]; snprintf(tmp,sizeof tmp,"%s.new",self);   /* same directory => the later rename is atomic */
  printf("downloading update...\n");
  if(update_fetch(url,tmp)!=0){ fprintf(stderr,"larzscript update: download failed\n"); remove(tmp); free(self); return 1; }

  size_t newlen; char *newbuf=update_read_file(tmp,&newlen);
  if(!newbuf){ fprintf(stderr,"larzscript update: could not read the downloaded file\n"); remove(tmp); free(self); return 1; }
  char newhash[65]; sha256_hex((unsigned char*)newbuf,newlen,newhash);
  free(newbuf);
  if(strcmp(newhash,expected)!=0){
    fprintf(stderr,"larzscript update: downloaded file failed checksum verification, aborting - no changes made\n");
    remove(tmp); free(self); return 1;
  }

#ifndef _WIN32
  chmod(tmp,0755);
#endif

#ifdef _WIN32
  char oldpath[4160]; snprintf(oldpath,sizeof oldpath,"%s.old",self);
  remove(oldpath);                 /* best-effort; fine if nothing's there */
  if(rename(self,oldpath)!=0){ fprintf(stderr,"larzscript update: could not move the running binary aside\n"); remove(tmp); free(self); return 1; }
  if(rename(tmp,self)!=0){ fprintf(stderr,"larzscript update: could not place the new binary - restoring the original\n"); rename(oldpath,self); free(self); return 1; }
  remove(oldpath);                 /* normally fails while still running - that's fine, cleaned up next update */
#else
  if(rename(tmp,self)!=0){ fprintf(stderr,"larzscript update: could not replace the running binary\n"); remove(tmp); free(self); return 1; }
#endif

  printf("updated larzscript -> %s\n", self);
  free(self);
  return 0;
}

#else /* !__STDC_HOSTED__ (the LarzOS kernel build) - self-update needs a hosted OS */
static int cmd_update(void){
  fprintf(stderr,"larzscript update: not available on this build\n");
  return 1;
}
#endif /* __STDC_HOSTED__ */

int main(int argc, char **argv){
  if(getenv("LZ_GC_STRESS")) g_gc_threshold=0;   /* collect on every statement (test mode) */
  const char *path=NULL, *eval_code=NULL; int show_ledger=0, want_repl=0, want_fmt=0, want_check=0, want_emit_c=0;
  int i=1;
  for(; i<argc; i++){
    const char *a=argv[i];
    if(strcmp(a,"--version")==0 || strcmp(a,"-v")==0){ printf("larzscript (native) " LARZSCRIPT_VERSION "\n"); return 0; }
    if(strcmp(a,"--help")==0 || strcmp(a,"-h")==0){ printf("%s", USAGE); return 0; }
    if(strcmp(a,"update")==0){ return cmd_update(); }
    if(strcmp(a,"--ledger")==0){ show_ledger=1; continue; }
    if(strcmp(a,"fmt")==0){ want_fmt=1; continue; }
    if(strcmp(a,"--check")==0 || strcmp(a,"check")==0){ want_check=1; continue; }
    if(strcmp(a,"--emit-c")==0){ want_emit_c=1; continue; }
    if(strcmp(a,"-e")==0 || strcmp(a,"--eval")==0){ if(i+1>=argc){ fprintf(stderr,"larzscript: -e needs code\n"); return 1; } eval_code=argv[++i]; i++; break; }
    if(strcmp(a,"repl")==0){ want_repl=1; i++; break; }
    /* `larzscript pkg install X` resolves to the copy of larzpkg.lz that
     * install.sh drops at $HOME/.larzscript/larzpkg.lz, from ANY working
     * directory - previously the only way to run it was to know and
     * type that full path yourself (`larzscript larzpkg.lz install X`
     * only worked if you happened to be sitting in that directory),
     * which is exactly the kind of thing a real package manager command
     * shouldn't require. */
    if(strcmp(a,"pkg")==0){
      const char *home=lz_home_dir();
      if(!home){ fprintf(stderr,"larzscript pkg: neither $HOME nor %%USERPROFILE%% is set, can't find larzpkg.lz\n"); return 1; }
      static char pkgpath[4096];
      snprintf(pkgpath,sizeof pkgpath,"%s/.larzscript/larzpkg.lz",home);
      if(access(pkgpath,0)!=0){ fprintf(stderr,"larzscript pkg: %s not found - re-run the installer (curl -fsSL <install-url> | sh)\n", pkgpath); return 1; }
      path=pkgpath; i++; break;
    }
    path=a; i++; break;                 /* the source file; the rest are program args */
  }
  /* remaining argv[i..] are the program's own arguments */
  List *prog_args=list_new();
  for(; i<argc; i++) list_push(prog_args, V_string(argv[i]));

  /* Bare `larzscript` at an interactive terminal behaves like bare `python`:
   * drop into the REPL. Piped/redirected stdin (no tty) keeps the older
   * behavior of reading a whole program from stdin and running it.
   * isatty/fileno aren't declared in the LarzOS kernel's freestanding libc
   * (there's no real terminal-vs-pipe distinction there anyway - the kernel
   * always calls larz_main with a fixed argv, never bare). */
#if !defined(__STDC_HOSTED__) || __STDC_HOSTED__
  if(!path && !eval_code && !want_fmt && !want_check && !want_emit_c && !want_repl && isatty(fileno(stdin))) want_repl=1;
#endif

  if(want_fmt){
    if(!path){ fprintf(stderr,"larzscript fmt: needs a file\n"); return 1; }
    char *src=read_all(path);
    if(setjmp(g_err)){ fprintf(stderr,"SyntaxError: %s\n", g_errmsg); return 1; }
    format_program(parse_program(lex(src)));
    return 0;
  }

  if(want_check){                        /* parse-check only: report syntax errors, don't run */
    if(!path){ fprintf(stderr,"larzscript --check: needs a file\n"); return 1; }
    char *src=read_all(path);
    if(setjmp(g_err)){ fprintf(stderr,"%s: SyntaxError: %s\n", path, g_errmsg); return 1; }
    parse_program(lex(src));
    printf("%s: ok\n", path);
    return 0;
  }

  if(want_emit_c){                       /* larzc: transpile to C on stdout */
    if(!path){ fprintf(stderr,"larzscript --emit-c: needs a file\n"); return 1; }
    { const char*sl=strrchr(path,'/'); if(sl){ size_t d=(size_t)(sl-path); if(d<sizeof g_srcdir){ memcpy(g_srcdir,path,d); g_srcdir[d]=0; } } }  /* module base dir */
    char *src=read_all(path);
    if(setjmp(g_err)){ fprintf(stderr,"%s: SyntaxError: %s\n", path, g_errmsg); return 1; }
    emit_c(parse_program(lex(src)), path);
    return 0;
  }

  if(want_repl){
    Interp ip; memset(&ip,0,sizeof(ip));
    install_builtins(&ip); ip.basedir=xstrdup(".");
    env_define(ip.globals, "args", V_list(prog_args));
    repl(&ip);
    return 0;
  }

  char *src, *basedir;
  if(eval_code){ src=xstrdup(eval_code); basedir=xstrdup("."); }
  else {
    src = read_all(path);
    if(path){ char db[4096]; snprintf(db,sizeof db,"%s",path); char *sl=strrchr(db,'/'); if(sl)*sl=0; else snprintf(db,sizeof db,"."); basedir=xstrdup(db); }
    else basedir=xstrdup(".");
  }

  if(setjmp(g_err)){ fprintf(stderr,"SyntaxError: %s\n", g_errmsg); return 1; }
  Token *toks = lex(src);
  Node *prog = parse_program(toks);

  Interp ip; memset(&ip,0,sizeof(ip));
  install_builtins(&ip); ip.basedir=basedir;
  env_define(ip.globals, "args", V_list(prog_args));

  if(setjmp(ip.jb)){ fprintf(stderr,"%s: %s\n", ip.errname, ip.errmsg); return 1; }
  for(int i=0;i<prog->nkids;i++){ exec(&ip, prog->kids[i], ip.globals); if(ip.returning) break; }

  if(show_ledger){
    fprintf(stderr, "--- ledger (%d) ---\n", ip.nled);
    for(int i=0;i<ip.nled;i++){
      long long c=ip.ledger[i].cents<0?-ip.ledger[i].cents:ip.ledger[i].cents;
      fprintf(stderr, "  %s -> %s  $%lld.%02lld\n", ip.ledger[i].src, ip.ledger[i].dst, c/100, c%100);
    }
    fprintf(stderr, "gas used: %lld\n", ip.gas_used);
  }
  return 0;
}
