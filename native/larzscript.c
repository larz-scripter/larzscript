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
 *   strings/lists also support repetition: "ab"*3, [0]*5
 *   money:    $ = integer cents; price; pay .. from .. to ..; require;
 *             paywall / subscribe / has (money-native primitives)
 *   errors:   reported with line numbers; catchable with try/catch.
 * Memory: a precise mark-sweep garbage collector reclaims container objects
 * (lists, dicts, envs, closures, wallets, paywalls) so long-running programs
 * stay bounded; it runs between statements and protects in-flight temporaries
 * with a temp-root stack. (Strings are not yet collected.) Verified under
 * AddressSanitizer with the GC forced on every statement. Zero third-party
 * dependencies (libc only).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <setjmp.h>

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
enum { GC_LIST=1, GC_DICT, GC_ENV, GC_ENTRY, GC_CLOSURE, GC_WALLET, GC_PAYWALL };
static GCObj *g_gc_head=NULL;
static long g_gc_count=0, g_gc_threshold=200000;
static void gc_register(void *p, unsigned char kind){ GCObj *o=(GCObj*)p; o->gc_kind=kind; o->gc_marked=0; o->gc_next=g_gc_head; g_gc_head=o; g_gc_count++; }

/* ===================== values ===================== */
typedef enum { V_NIL, V_BOOL, V_NUM, V_MONEY, V_STR, V_WALLET, V_FUNC, V_BUILTIN, V_LIST, V_PAYWALL, V_DICT, V_MODULE } VType;
struct Node; struct Env; struct Interp; struct List; struct Paywall; struct Dict;
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
};

static Value V_nil(void){ Value v; v.t=V_NIL; return v; }
static Value V_bool(int b){ Value v; v.t=V_BOOL; v.b=b!=0; return v; }
static Value V_number(double d){ Value v; v.t=V_NUM; v.num=d; return v; }
static Value V_money(long long c){ Value v; v.t=V_MONEY; v.cents=c; return v; }
static Value V_string(char *s){ Value v; v.t=V_STR; v.str=s; return v; }
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
static List *list_new(void){ List *l=xmalloc(sizeof(List)); l->items=NULL; l->n=0; l->cap=0; gc_register(l,GC_LIST); return l; }
static void list_push(List *l, Value v){ if(l->n==l->cap){ l->cap=l->cap?l->cap*2:8; l->items=realloc(l->items,l->cap*sizeof(Value)); } l->items[l->n++]=v; }
static Dict *dict_new(void){ Dict *d=xmalloc(sizeof(Dict)); d->items=NULL; d->n=0; d->cap=0; gc_register(d,GC_DICT); return d; }

static long long money_round(double x){ return (long long)(x>=0 ? x+0.5 : x-0.5); }

static int is_num(Value v){ return v.t==V_NUM; }

/* value equality (used by ==, dict keys, list contains) - by value, not identity */
static int values_equal(Value a, Value b){
  if(a.t!=b.t) return 0;
  switch(a.t){
    case V_NIL: return 1;
    case V_BOOL: return a.b==b.b;
    case V_NUM: return a.num==b.num;
    case V_MONEY: return a.cents==b.cents;
    case V_STR: return strcmp(a.str,b.str)==0;
    case V_WALLET: return a.wal==b.wal;
    case V_PAYWALL: return a.pw==b.pw;
    case V_LIST: return a.list==b.list;
    case V_DICT: return a.dict==b.dict;
    case V_MODULE: return a.mod==b.mod;
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
    case V_NUM: return v.num!=0;
    case V_MONEY: return v.cents!=0;
    case V_STR: return v.str[0]!=0;
    case V_LIST: return v.list->n!=0;
    case V_DICT: return v.dict->n!=0;
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
  }
}
static char *str_of(Value v){ SB b; b.s=NULL; b.n=0; b.cap=0; val_to_sb(&b,v); sb_putc(&b,0); return b.s; }

/* ===================== AST ===================== */
typedef enum {
  N_LET, N_ASSIGN, N_PRICE, N_WALLET, N_PAY, N_REQUIRE, N_FN, N_RETURN,
  N_IF, N_WHILE, N_BLOCK, N_EXPR, N_PAYWALL, N_SUBSCRIBE,
  N_NUM, N_MONEY, N_STR, N_BOOL, N_NIL, N_NAME, N_BIN, N_UN, N_CALL, N_GET, N_METHOD,
  N_ARRAY, N_INDEX, N_DICT, N_SETINDEX, N_BREAK, N_CONTINUE, N_FOR,
  N_SLICE, N_TRY, N_THROW, N_FSTR, N_IMPORT, N_TERNARY, N_LISTCOMP, N_DICTCOMP
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
  long long gas; int has_gas;
  char *src, *dst;            /* pay / subscribe */
  char *period;               /* paywall */
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
  "try","catch","throw","import","as", NULL
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
          buf[b++]= e=='n'?'\n': e=='t'?'\t': e;
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
static Node *equality(Parser *p){ const char *o[]={"==","!="}; return bin_lvl(p,membership,o,2,0); }
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
  if(is_kw(t,"pay")){ padv(p); Node *n=node(N_PAY); n->a=expression(p); expect_kw(p,"from"); n->src=padv(p)->text; expect_kw(p,"to"); n->dst=padv(p)->text; return n; }
  if(is_kw(t,"require")){ padv(p); Node *n=node(N_REQUIRE); n->a=expression(p);
      if(pk(p)->type==T_COMMA){ padv(p); if(pk(p)->type!=T_STR) fail("expected a message string on line %d",pk(p)->line); n->str=padv(p)->text; } return n; }
  if(is_kw(t,"fn")){ return parse_fn(p, 1); }
  if(is_kw(t,"try")){ padv(p); Node *n=node(N_TRY); n->a=block(p); expect_kw(p,"catch"); n->name=padv(p)->text; n->b=block(p); return n; }
  if(is_kw(t,"throw")){ padv(p); Node *n=node(N_THROW); n->a=expression(p); return n; }
  if(is_kw(t,"import")){ padv(p); Node *n=node(N_IMPORT);
      if(pk(p)->type!=T_STR) fail("expected a module path string after 'import' on line %d", pk(p)->line);
      n->str=padv(p)->text;
      if(is_kw(pk(p),"as")){ padv(p); n->name=padv(p)->text; }
      return n; }
  if(is_kw(t,"return")){ padv(p); Node *n=node(N_RETURN); if(starts_expr(pk(p))) n->a=expression(p); return n; }
  if(is_kw(t,"if")){ padv(p); Node *n=node(N_IF); n->a=expression(p); n->b=block(p);
      if(is_kw(pk(p),"else")){ padv(p); n->c = is_kw(pk(p),"if") ? statement(p) : block(p); } return n; }
  if(is_kw(t,"while")){ padv(p); Node *n=node(N_WHILE); n->a=expression(p); n->b=block(p); return n; }
  if(is_kw(t,"paywall")){ padv(p); Node *n=node(N_PAYWALL); n->name=padv(p)->text;
      if(!is_op(pk(p),"=")){ fail("expected '=' on line %d",pk(p)->line); }
      padv(p);
      n->a=unary(p);            /* the price (stops before the '/') */
      if(!is_op(pk(p),"/")){ fail("expected '/' before the period on line %d",pk(p)->line); }
      padv(p);
      n->period=padv(p)->text; expect_kw(p,"to"); n->dst=padv(p)->text; return n; }
  if(is_kw(t,"subscribe")){ padv(p); Node *n=node(N_SUBSCRIBE); n->src=padv(p)->text; expect_kw(p,"to"); n->dst=padv(p)->text; return n; }
  if(is_kw(t,"for")){
      padv(p); Node *n=node(N_FOR); n->name=padv(p)->text; expect_kw(p,"in");
      n->a=expression(p); n->b=block(p); return n;
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

static void gc_mark_env(Env *e);
static void gc_mark_value(Value v){
  switch(v.t){
    case V_LIST: { GCObj *o=(GCObj*)v.list; if(!o->gc_marked){ o->gc_marked=1; for(int i=0;i<v.list->n;i++) gc_mark_value(v.list->items[i]); } break; }
    case V_DICT: { GCObj *o=(GCObj*)v.dict; if(!o->gc_marked){ o->gc_marked=1; for(int i=0;i<v.dict->n;i++){ gc_mark_value(v.dict->items[i].key); gc_mark_value(v.dict->items[i].val); } } break; }
    case V_WALLET: ((GCObj*)v.wal)->gc_marked=1; break;
    case V_PAYWALL: ((GCObj*)v.pw)->gc_marked=1; break;
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

static void define_builtins(Env *e);   /* forward: used by import */

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
    if(bs){ size_t la=strlen(a.str), lb=strlen(b.str); char *s=xmalloc(la+lb+1); memcpy(s,a.str,la); memcpy(s+la,b.str,lb+1); return V_string(s); }
    runtime_error(ip,"LarzTypeError","cannot add those values");
  }
  if(strcmp(op,"-")==0){ if(bm) return V_money(a.cents-b.cents); if(bn) return V_number(a.num-b.num); runtime_error(ip,"LarzTypeError","cannot subtract those values"); }
  if(strcmp(op,"*")==0){
    if(a.t==V_MONEY && is_num(b)) return V_money(money_round((double)a.cents*b.num));
    if(is_num(a) && b.t==V_MONEY) return V_money(money_round((double)b.cents*a.num));
    if(bn) return V_number(a.num*b.num);
    /* string/list repetition: "ab" * 3, [0] * 4 (either order) */
    { Value s=a, k=b; if(is_num(a)&&(b.t==V_STR||b.t==V_LIST)){ s=b; k=a; }
      if(s.t==V_STR && is_num(k)){ long long m=(long long)k.num; if(m<0) m=0; size_t la=strlen(s.str); char *out=xmalloc(la*m+1); for(long long i=0;i<m;i++) memcpy(out+i*la, s.str, la); out[la*m]=0; return V_string(out); }
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
        if(strcmp(m,"upper")==0||strcmp(m,"lower")==0){ int up=m[0]=='u'; char *r=xstrdup(s); for(char *p=r;*p;p++) *p= up?toupper((unsigned char)*p):tolower((unsigned char)*p); return V_string(r); }
        if(strcmp(m,"strip")==0){ int a=0,b=(int)strlen(s); while(a<b&&isspace((unsigned char)s[a])) a++; while(b>a&&isspace((unsigned char)s[b-1])) b--; return V_string(xstrndup(s+a,b-a)); }
        if(strcmp(m,"contains")==0||strcmp(m,"find")==0){ if(na!=1||args[0].t!=V_STR) runtime_error(ip,"LarzTypeError","%s expects a string",m); const char *f=strstr(s,args[0].str); if(m[0]=='c') return V_bool(f!=NULL); return V_number(f?(double)(f-s):-1); }
        if(strcmp(m,"starts_with")==0){ if(na!=1||args[0].t!=V_STR) runtime_error(ip,"LarzTypeError","starts_with expects a string"); size_t ln=strlen(args[0].str); return V_bool(strncmp(s,args[0].str,ln)==0); }
        if(strcmp(m,"ends_with")==0){ if(na!=1||args[0].t!=V_STR) runtime_error(ip,"LarzTypeError","ends_with expects a string"); size_t ls=strlen(s), le=strlen(args[0].str); return V_bool(ls>=le && strcmp(s+ls-le,args[0].str)==0); }
        if(strcmp(m,"replace")==0){ if(na!=2||args[0].t!=V_STR||args[1].t!=V_STR) runtime_error(ip,"LarzTypeError","replace expects two strings"); const char *from=args[0].str,*to=args[1].str; size_t lf=strlen(from); if(lf==0) return V_string(xstrdup(s)); SB b; b.s=NULL;b.n=0;b.cap=0; const char *p=s; while(*p){ if(strncmp(p,from,lf)==0){ sb_puts(&b,to); p+=lf; } else sb_putc(&b,*p++); } sb_putc(&b,0); return V_string(b.s?b.s:xstrdup("")); }
        if(strcmp(m,"split")==0){ List *r=list_new(); if(na==0||args[0].t!=V_STR||args[0].str[0]==0){ for(const char *p=s;*p;p++){ char *c=xstrndup(p,1); list_push(r,V_string(c)); } return V_list(r); } const char *sep=args[0].str; size_t ls=strlen(sep); const char *p=s,*q; while((q=strstr(p,sep))){ list_push(r,V_string(xstrndup(p,q-p))); p=q+ls; } list_push(r,V_string(xstrdup(p))); return V_list(r); }
        if(strcmp(m,"capitalize")==0){ char *r=xstrdup(s); if(r[0]){ r[0]=toupper((unsigned char)r[0]); for(char *p=r+1;*p;p++) *p=tolower((unsigned char)*p); } return V_string(r); }
        if(strcmp(m,"title")==0){ char *r=xstrdup(s); int start=1; for(char *p=r;*p;p++){ if(isspace((unsigned char)*p)){ start=1; } else { *p = start?toupper((unsigned char)*p):tolower((unsigned char)*p); start=0; } } return V_string(r); }
        if(strcmp(m,"ljust")==0||strcmp(m,"rjust")==0){ if(na<1||!is_num(args[0])) runtime_error(ip,"LarzTypeError","%s expects a width",m); int w=(int)args[0].num; char fill=(na>=2&&args[1].t==V_STR&&args[1].str[0])?args[1].str[0]:' '; int ls=(int)strlen(s); int pad=w>ls?w-ls:0; SB b; b.s=NULL;b.n=0;b.cap=0; if(m[0]=='r'){ for(int i=0;i<pad;i++) sb_putc(&b,fill); sb_puts(&b,s); } else { sb_puts(&b,s); for(int i=0;i<pad;i++) sb_putc(&b,fill); } sb_putc(&b,0); return V_string(b.s?b.s:xstrdup("")); }
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
        runtime_error(ip,"LarzTypeError","'in' needs a list, dict or string on the right");
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
      int len; Value *arr=NULL; Dict *dd=NULL; const char *sp=NULL;
      if(it.t==V_LIST){ len=it.list->n; arr=it.list->items; }
      else if(it.t==V_DICT){ len=it.dict->n; dd=it.dict; }
      else if(it.t==V_STR){ len=(int)strlen(it.str); sp=it.str; }
      else { gc_temp_pop(ip,tr); runtime_error(ip,"LarzTypeError","cannot iterate that value"); return V_nil(); }
      for(int i=0;i<len;i++){
        Value item = arr?arr[i] : dd?dd->items[i].key : V_string(xstrndup(sp+i,1));
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
      int len; Value *arr=NULL; Dict *dd=NULL; const char *sp=NULL;
      if(it.t==V_LIST){ len=it.list->n; arr=it.list->items; }
      else if(it.t==V_DICT){ len=it.dict->n; dd=it.dict; }
      else if(it.t==V_STR){ len=(int)strlen(it.str); sp=it.str; }
      else { gc_temp_pop(ip,tr); runtime_error(ip,"LarzTypeError","cannot iterate that value"); return V_nil(); }
      for(int i=0;i<len;i++){
        Value item = arr?arr[i] : dd?dd->items[i].key : V_string(xstrndup(sp+i,1));
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
      Value obj=eval(ip,n->a,env); gc_temp_push(ip,obj);
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
      return V_string(xstrndup(obj.str+start, end-start));
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
      if(obj.t==V_STR){ int len=(int)strlen(obj.str); if(idx<0) idx+=len; if(idx<0||idx>=len) runtime_error(ip,"LarzRuntimeError","index out of range"); char *s=xmalloc(2); s[0]=obj.str[idx]; s[1]=0; return V_string(s); }
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

static Value call_value(Interp *ip, Value callee, Value *args, int nargs){
  if(callee.t==V_BUILTIN) return callee.bi->fn(ip,args,nargs);
  if(callee.t==V_FUNC){
    Node *decl=callee.fn->decl;
    const char *fname = decl->name ? decl->name : "function";
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
    gc_root_push(ip, call_env);
    for(int i=0;i<decl->b->nkids;i++){ exec(ip, decl->b->kids[i], call_env); if(ip->returning) break; }
    gc_root_pop(ip);
    Value r = ip->returning ? ip->retval : V_nil();
    ip->returning=0;
    return r;
  }
  runtime_error(ip,"LarzTypeError","that value is not callable");
  return V_nil();
}

static void exec(Interp *ip, Node *n, Env *env){
  maybe_gc(ip);                 /* safe point: between statements, nothing half-built is unrooted */
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
    case N_PAY: {
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
    case N_PAYWALL: {
      Value v=eval(ip,n->a,env);
      if(v.t!=V_MONEY) runtime_error(ip,"LarzTypeError","a paywall price must be money");
      Paywall *pw=xmalloc(sizeof(Paywall)); gc_register(pw,GC_PAYWALL);
      pw->name=xstrdup(n->name); pw->price=v.cents; pw->period=xstrdup(n->period); pw->payee=xstrdup(n->dst);
      env_define(env, n->name, V_paywall(pw));
      return;
    }
    case N_SUBSCRIBE: {
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
      int len; Value *arr=NULL; Dict *d=NULL; const char *sp=NULL;
      if(it.t==V_LIST){ len=it.list->n; arr=it.list->items; }
      else if(it.t==V_DICT){ len=it.dict->n; d=it.dict; }
      else if(it.t==V_STR){ len=(int)strlen(it.str); sp=it.str; }
      else { gc_temp_pop(ip,fortr); runtime_error(ip,"LarzTypeError","cannot iterate that value"); return; }
      for(int i=0;i<len;i++){
        Value item = arr ? arr[i] : d ? d->items[i].key : V_string(xstrndup(sp+i,1));
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
      int nr=ip->nroots, nt=ip->ntemp;              /* restore GC roots if the try unwinds */
      if(setjmp(ip->jb)==0){
        exec(ip, n->a, env);
        memcpy(ip->jb, saved, sizeof(jmp_buf));       /* normal exit: restore outer */
      } else {
        memcpy(ip->jb, saved, sizeof(jmp_buf));       /* error: restore outer first */
        ip->nroots=nr; ip->ntemp=nt;
        ip->returning=0; ip->loopflow=0;
        Dict *d=dict_new();
        dict_set(d, V_string(xstrdup("type")),    V_string(xstrdup(ip->errname?ip->errname:"Error")));
        dict_set(d, V_string(xstrdup("message")), V_string(xstrdup(ip->errmsg)));
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
      char full[4096];
      if(n->str[0]=='/') snprintf(full,sizeof full,"%s",n->str);
      else snprintf(full,sizeof full,"%s/%s", ip->basedir?ip->basedir:".", n->str);
      char resolved[4096];
      { char *rp=realpath(full,NULL); if(rp){ snprintf(resolved,sizeof resolved,"%s",rp); free(rp); } else snprintf(resolved,sizeof resolved,"%s",full); }
      const char *alias=n->name; char abuf[256];
      if(!alias){ const char *base=strrchr(n->str,'/'); base=base?base+1:n->str; snprintf(abuf,sizeof abuf,"%s",base); char *dot=strrchr(abuf,'.'); if(dot)*dot=0; alias=abuf; }
      for(int i=0;i<ip->nmod;i++) if(strcmp(ip->modcache[i].path,resolved)==0){ env_define(env, alias, ip->modcache[i].val); return; }
      FILE *f=fopen(resolved,"rb"); if(!f) runtime_error(ip,"ImportError","cannot import '%s'", n->str);
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
  runtime_error(ip,"LarzTypeError","len() expects a string, list or dict");
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
  List *l=list_new();
  if(step>0) for(long long i=start;i<stop;i+=step) list_push(l,V_number((double)i));
  else for(long long i=start;i>stop;i+=step) list_push(l,V_number((double)i));
  return V_list(l);
}
static const char *type_name(Value v){
  switch(v.t){ case V_NIL:return "nil"; case V_BOOL:return "bool"; case V_NUM:return "number";
    case V_MONEY:return "money"; case V_STR:return "string"; case V_WALLET:return "wallet";
    case V_FUNC:return "function"; case V_BUILTIN:return "function"; case V_LIST:return "list";
    case V_DICT:return "dict"; case V_PAYWALL:return "paywall"; case V_MODULE:return "module"; default:return "value"; }
}
static Value bi_str(Interp *ip, Value *a, int n){ if(n!=1) runtime_error(ip,"LarzTypeError","str() expects one argument"); return V_string(str_of(a[0])); }
static Value bi_type(Interp *ip, Value *a, int n){ if(n!=1) runtime_error(ip,"LarzTypeError","type() expects one argument"); return V_string(xstrdup(type_name(a[0]))); }
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
static Value _minmax(Interp *ip, Value *a, int n, int want_max){
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
static Value bi_sum(Interp *ip, Value *a, int n){
  if(n!=1 || a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","sum() expects a list");
  List *l=a[0].list; if(l->n==0) return V_number(0);
  if(l->items[0].t==V_MONEY){ long long c=0; for(int i=0;i<l->n;i++){ if(l->items[i].t!=V_MONEY) runtime_error(ip,"LarzTypeError","sum(): mixed types"); c+=l->items[i].cents; } return V_money(c); }
  double s=0; for(int i=0;i<l->n;i++){ if(!is_num(l->items[i])) runtime_error(ip,"LarzTypeError","sum(): expects numbers or money"); s+=l->items[i].num; } return V_number(s);
}
static Value bi_sorted(Interp *ip, Value *a, int n){
  if(n!=1 || a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","sorted() expects a list");
  List *r=list_new(); for(int i=0;i<a[0].list->n;i++) list_push(r, a[0].list->items[i]);
  qsort(r->items, r->n, sizeof(Value), qsort_value_cmp);
  return V_list(r);
}
static Value bi_reversed(Interp *ip, Value *a, int n){
  if(n!=1 || a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","reversed() expects a list");
  List *r=list_new(); for(int i=a[0].list->n-1;i>=0;i--) list_push(r, a[0].list->items[i]);
  return V_list(r);
}
static Value bi_floor(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","floor() expects a number"); double x=a[0].num; long long d=(long long)x; if(x<0 && (double)d!=x) d--; return V_number((double)d); }
static Value bi_ceil(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","ceil() expects a number"); double x=a[0].num; long long d=(long long)x; if(x>0 && (double)d!=x) d++; return V_number((double)d); }
static Value bi_round(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","round() expects a number"); double x=a[0].num; return V_number((double)(long long)(x>=0?x+0.5:x-0.5)); }
static Value bi_sqrt(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","sqrt() expects a number"); double x=a[0].num; if(x<0) runtime_error(ip,"LarzValueError","sqrt() of a negative number"); if(x==0) return V_number(0); double g=x>1?x:1; for(int i=0;i<60;i++) g=0.5*(g+x/g); return V_number(g); }
static Value bi_pow(Interp *ip, Value *a, int n){ if(n!=2||!is_num(a[0])||!is_num(a[1])) runtime_error(ip,"LarzTypeError","pow() expects two numbers"); double b=a[0].num, e=a[1].num; if(e!=(long long)e) runtime_error(ip,"LarzValueError","pow(): exponent must be a whole number"); long long ex=(long long)e; double r=1, base=b; int neg=ex<0; if(neg) ex=-ex; for(long long i=0;i<ex;i++) r*=base; if(neg){ if(b==0) runtime_error(ip,"LarzRuntimeError","0 to a negative power"); r=1/r; } return V_number(r); }
static Value bi_chr(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","chr() expects a number"); char *s=xmalloc(2); s[0]=(char)(int)a[0].num; s[1]=0; return V_string(s); }
static Value bi_ord(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_STR||a[0].str[0]==0) runtime_error(ip,"LarzTypeError","ord() expects a non-empty string"); return V_number((unsigned char)a[0].str[0]); }
static Value bi_assert(Interp *ip, Value *a, int n){ if(n<1) runtime_error(ip,"LarzTypeError","assert() expects a condition"); if(!truthy(a[0])) runtime_error(ip,"AssertionError","%s", (n>=2&&a[1].t==V_STR)?a[1].str:"assertion failed"); return V_nil(); }
static Value bi_input(Interp *ip, Value *a, int n){
  (void)ip; if(n>=1 && a[0].t==V_STR){ printf("%s", a[0].str); fflush(stdout); }
  char buf[8192]; if(!fgets(buf,sizeof buf,stdin)) return V_nil();
  int len=(int)strlen(buf); while(len>0 && (buf[len-1]=='\n'||buf[len-1]=='\r')) buf[--len]=0;
  return V_string(xstrndup(buf,len));
}
static Value bi_keys(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_DICT) runtime_error(ip,"LarzTypeError","keys() expects a dict"); List *r=list_new(); for(int i=0;i<a[0].dict->n;i++) list_push(r,a[0].dict->items[i].key); return V_list(r); }
static Value bi_values(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_DICT) runtime_error(ip,"LarzTypeError","values() expects a dict"); List *r=list_new(); for(int i=0;i<a[0].dict->n;i++) list_push(r,a[0].dict->items[i].val); return V_list(r); }
static Value bi_map(Interp *ip, Value *a, int n){ if(n!=2||a[1].t!=V_LIST) runtime_error(ip,"LarzTypeError","map() expects a function and a list"); List *r=list_new(); int tr=ip->ntemp; gc_temp_push(ip,V_list(r)); for(int i=0;i<a[1].list->n;i++){ Value arg=a[1].list->items[i]; list_push(r, call_value(ip,a[0],&arg,1)); } gc_temp_pop(ip,tr); return V_list(r); }
static Value bi_filter(Interp *ip, Value *a, int n){ if(n!=2||a[1].t!=V_LIST) runtime_error(ip,"LarzTypeError","filter() expects a function and a list"); List *r=list_new(); int tr=ip->ntemp; gc_temp_push(ip,V_list(r)); for(int i=0;i<a[1].list->n;i++){ Value arg=a[1].list->items[i]; if(truthy(call_value(ip,a[0],&arg,1))) list_push(r,arg); } gc_temp_pop(ip,tr); return V_list(r); }
static Value bi_reduce(Interp *ip, Value *a, int n){ if(n<2||a[1].t!=V_LIST) runtime_error(ip,"LarzTypeError","reduce() expects a function, a list and an optional initial value"); List *l=a[1].list; int i=0; Value acc; if(n>=3) acc=a[2]; else { if(l->n==0) runtime_error(ip,"LarzValueError","reduce() of empty list with no initial value"); acc=l->items[0]; i=1; } int tr=ip->ntemp; gc_temp_push(ip,acc); for(; i<l->n; i++){ Value args[2]; args[0]=acc; args[1]=l->items[i]; acc=call_value(ip,a[0],args,2); ip->temproots[tr]=acc; } gc_temp_pop(ip,tr); return acc; }
static Value bi_join(Interp *ip, Value *a, int n){ if(n<1||a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","join() expects a list and an optional separator"); const char *sep=(n>=2&&a[1].t==V_STR)?a[1].str:""; SB b; b.s=NULL;b.n=0;b.cap=0; for(int i=0;i<a[0].list->n;i++){ if(i) sb_puts(&b,sep); char *s=str_of(a[0].list->items[i]); sb_puts(&b,s); } sb_putc(&b,0); return V_string(b.s?b.s:xstrdup("")); }
static Value bi_enumerate(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","enumerate() expects a list"); List *r=list_new(); for(int i=0;i<a[0].list->n;i++){ List *pair=list_new(); list_push(pair,V_number(i)); list_push(pair,a[0].list->items[i]); list_push(r,V_list(pair)); } return V_list(r); }
static Value bi_zip(Interp *ip, Value *a, int n){ if(n!=2||a[0].t!=V_LIST||a[1].t!=V_LIST) runtime_error(ip,"LarzTypeError","zip() expects two lists"); int m=a[0].list->n<a[1].list->n?a[0].list->n:a[1].list->n; List *r=list_new(); for(int i=0;i<m;i++){ List *pair=list_new(); list_push(pair,a[0].list->items[i]); list_push(pair,a[1].list->items[i]); list_push(r,V_list(pair)); } return V_list(r); }
static Value bi_read_file(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","read_file() expects a path string"); FILE *f=fopen(a[0].str,"rb"); if(!f) runtime_error(ip,"IOError","cannot read file '%s'", a[0].str); size_t cap=1<<16,len=0; char *b=xmalloc(cap); size_t r; while((r=fread(b+len,1,cap-len,f))>0){ len+=r; if(len==cap){ cap*=2; b=realloc(b,cap); } } b[len]=0; fclose(f); return V_string(b); }
static Value bi_write_file(Interp *ip, Value *a, int n){ if(n!=2||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","write_file() expects a path and content"); FILE *f=fopen(a[0].str,"wb"); if(!f) runtime_error(ip,"IOError","cannot write file '%s'", a[0].str); char *s=str_of(a[1]); fputs(s,f); fclose(f); return V_nil(); }
static Value bi_append_file(Interp *ip, Value *a, int n){ if(n!=2||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","append_file() expects a path and content"); FILE *f=fopen(a[0].str,"ab"); if(!f) runtime_error(ip,"IOError","cannot append to file '%s'", a[0].str); char *s=str_of(a[1]); fputs(s,f); fclose(f); return V_nil(); }
static Value bi_file_exists(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_STR) runtime_error(ip,"LarzTypeError","file_exists() expects a path string"); FILE *f=fopen(a[0].str,"rb"); if(f){ fclose(f); return V_bool(1);} return V_bool(0); }
static Value bi_exit(Interp *ip, Value *a, int n){ (void)ip; int code = (n>=1&&is_num(a[0]))?(int)a[0].num:0; exit(code); }
static Value bi_all(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","all() expects a list"); for(int i=0;i<a[0].list->n;i++) if(!truthy(a[0].list->items[i])) return V_bool(0); return V_bool(1); }
static Value bi_any(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","any() expects a list"); for(int i=0;i<a[0].list->n;i++) if(truthy(a[0].list->items[i])) return V_bool(1); return V_bool(0); }
static Value bi_count(Interp *ip, Value *a, int n){ if(n!=2) runtime_error(ip,"LarzTypeError","count() expects a list/string and a value"); long long c=0; if(a[0].t==V_LIST){ for(int i=0;i<a[0].list->n;i++) if(values_equal(a[0].list->items[i],a[1])) c++; } else if(a[0].t==V_STR&&a[1].t==V_STR&&a[1].str[0]){ const char *p=a[0].str,*q; size_t l=strlen(a[1].str); while((q=strstr(p,a[1].str))){ c++; p=q+l; } } else runtime_error(ip,"LarzTypeError","count() expects a list, or two strings"); return V_number((double)c); }
static Value bi_unique(Interp *ip, Value *a, int n){ if(n!=1||a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","unique() expects a list"); List *r=list_new(); for(int i=0;i<a[0].list->n;i++){ int seen=0; for(int j=0;j<r->n;j++) if(values_equal(r->items[j],a[0].list->items[i])){ seen=1; break; } if(!seen) list_push(r,a[0].list->items[i]); } return V_list(r); }
static Value _base_str(long long v, int base, const char *prefix){ char buf[80]; int neg=v<0; unsigned long long u=neg?(unsigned long long)(-v):(unsigned long long)v; int k=0; if(u==0) buf[k++]='0'; while(u){ int d=u%base; buf[k++]= d<10 ? '0'+d : 'a'+(d-10); u/=base; } SB b; b.s=NULL;b.n=0;b.cap=0; if(neg) sb_putc(&b,'-'); sb_puts(&b,prefix); for(int i=k-1;i>=0;i--) sb_putc(&b,buf[i]); sb_putc(&b,0); return V_string(b.s?b.s:xstrdup("")); }
static Value bi_hex(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","hex() expects a number"); return _base_str((long long)a[0].num,16,"0x"); }
static Value bi_bin(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","bin() expects a number"); return _base_str((long long)a[0].num,2,"0b"); }
static Value bi_oct(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","oct() expects a number"); return _base_str((long long)a[0].num,8,"0o"); }
static Value bi_gcd(Interp *ip, Value *a, int n){ if(n!=2||!is_num(a[0])||!is_num(a[1])) runtime_error(ip,"LarzTypeError","gcd() expects two numbers"); long long x=(long long)a[0].num, y=(long long)a[1].num; if(x<0)x=-x; if(y<0)y=-y; while(y){ long long t=x%y; x=y; y=t; } return V_number((double)x); }
static Value bi_factorial(Interp *ip, Value *a, int n){ if(n!=1||!is_num(a[0])) runtime_error(ip,"LarzTypeError","factorial() expects a number"); long long k=(long long)a[0].num; if(k<0) runtime_error(ip,"LarzValueError","factorial() of a negative number"); double r=1; for(long long i=2;i<=k;i++) r*=i; return V_number(r); }
static Value bi_sign(Interp *ip, Value *a, int n){ if(n!=1) runtime_error(ip,"LarzTypeError","sign() expects one argument"); if(a[0].t==V_MONEY) return V_number(a[0].cents<0?-1:(a[0].cents>0?1:0)); if(is_num(a[0])) return V_number(a[0].num<0?-1:(a[0].num>0?1:0)); runtime_error(ip,"LarzTypeError","sign() expects a number or money"); return V_nil(); }
static Value bi_clamp(Interp *ip, Value *a, int n){ if(n!=3) runtime_error(ip,"LarzTypeError","clamp() expects a value, low and high"); if(value_compare(a[0],a[1])<0) return a[1]; if(value_compare(a[0],a[2])>0) return a[2]; return a[0]; }
static Value bi_list(Interp *ip, Value *a, int n){ if(n!=1) runtime_error(ip,"LarzTypeError","list() expects one argument"); List *r=list_new(); if(a[0].t==V_LIST){ for(int i=0;i<a[0].list->n;i++) list_push(r,a[0].list->items[i]); } else if(a[0].t==V_DICT){ for(int i=0;i<a[0].dict->n;i++) list_push(r,a[0].dict->items[i].key); } else if(a[0].t==V_STR){ for(const char *p=a[0].str;*p;p++) list_push(r,V_string(xstrndup(p,1))); } else runtime_error(ip,"LarzTypeError","list() expects a list, dict or string"); return V_list(r); }
static Value bi_dict(Interp *ip, Value *a, int n){ Dict *d=dict_new(); if(n==0) return V_dict(d); if(n!=1||a[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","dict() expects a list of [key, value] pairs"); for(int i=0;i<a[0].list->n;i++){ Value pr=a[0].list->items[i]; if(pr.t!=V_LIST||pr.list->n!=2) runtime_error(ip,"LarzTypeError","dict() pairs must be [key, value] lists"); dict_set(d, pr.list->items[0], pr.list->items[1]); } return V_dict(d); }

static Builtin B_print = {"print", bi_print};
static Builtin B_money = {"money", bi_money};
static Builtin B_len   = {"len",   bi_len};
static Builtin B_push  = {"push",  bi_push};
static Builtin B_range = {"range", bi_range};
static Builtin B_str={"str",bi_str}, B_int={"int",bi_int}, B_float={"float",bi_float}, B_bool={"bool",bi_bool}, B_type={"type",bi_type};
static Builtin B_abs={"abs",bi_abs}, B_min={"min",bi_min}, B_max={"max",bi_max}, B_sum={"sum",bi_sum};
static Builtin B_sorted={"sorted",bi_sorted}, B_reversed={"reversed",bi_reversed};
static Builtin B_floor={"floor",bi_floor}, B_ceil={"ceil",bi_ceil}, B_round={"round",bi_round}, B_sqrt={"sqrt",bi_sqrt}, B_pow={"pow",bi_pow};
static Builtin B_chr={"chr",bi_chr}, B_ord={"ord",bi_ord}, B_assert={"assert",bi_assert}, B_input={"input",bi_input};
static Builtin B_keys={"keys",bi_keys}, B_values={"values",bi_values};
static Builtin B_map={"map",bi_map}, B_filter={"filter",bi_filter}, B_reduce={"reduce",bi_reduce}, B_join={"join",bi_join}, B_enumerate={"enumerate",bi_enumerate};
static Builtin B_zip={"zip",bi_zip}, B_read_file={"read_file",bi_read_file}, B_write_file={"write_file",bi_write_file}, B_append_file={"append_file",bi_append_file}, B_file_exists={"file_exists",bi_file_exists}, B_exit={"exit",bi_exit};
static Builtin B_all={"all",bi_all}, B_any={"any",bi_any}, B_count={"count",bi_count}, B_unique={"unique",bi_unique};
static Builtin B_hex={"hex",bi_hex}, B_bin={"bin",bi_bin}, B_oct={"oct",bi_oct}, B_gcd={"gcd",bi_gcd}, B_factorial={"factorial",bi_factorial}, B_sign={"sign",bi_sign}, B_clamp={"clamp",bi_clamp}, B_list={"list",bi_list}, B_dict={"dict",bi_dict};

/* ===================== REPL ===================== */
static void repl(Interp *ip){
  char line[8192];
  printf("Larzscript native REPL (v1.7.0) - type statements; Ctrl-D to exit.\n");
  for(;;){
    printf("larz> "); fflush(stdout);
    if(!fgets(line, sizeof line, stdin)){ printf("\n"); break; }
    /* re-arm the error handlers for each line so an error doesn't exit */
    if(setjmp(g_err)){ fprintf(stderr,"SyntaxError: %s\n", g_errmsg); continue; }
    if(setjmp(ip->jb)){ fprintf(stderr,"%s: %s\n", ip->errname, ip->errmsg); continue; }
    Token *toks = lex(line);
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
}

static void install_builtins(Interp *ip){
  ip->globals = env_new(NULL);
  ip->has_gas = 0;                 /* unlimited by default */
  define_builtins(ip->globals);
}

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
    case N_BIN: fmt_expr(n->a, p); printf(" %s ", n->op); fmt_expr(n->b, p+1); break;
    case N_TERNARY: fmt_expr(n->a, 2); printf(" ? "); fmt_expr(n->b, 1); printf(" : "); fmt_expr(n->c, 1); break;
    case N_ARRAY: putchar('['); for(int i=0;i<n->nkids;i++){ if(i) printf(", "); fmt_expr(n->kids[i], 1); } putchar(']'); break;
    case N_DICT: putchar('{'); for(int i=0;i+1<n->nkids;i+=2){ if(i) printf(", "); fmt_expr(n->kids[i],1); printf(": "); fmt_expr(n->kids[i+1],1); } putchar('}'); break;
    case N_INDEX: fmt_expr(n->a,11); putchar('['); fmt_expr(n->b,1); putchar(']'); break;
    case N_SLICE: fmt_expr(n->a,11); putchar('['); if(n->b) fmt_expr(n->b,1); putchar(':'); if(n->c) fmt_expr(n->c,1); putchar(']'); break;
    case N_CALL: fmt_expr(n->a,11); putchar('('); for(int i=0;i<n->nkids;i++){ if(i) printf(", "); fmt_expr(n->kids[i],1); } putchar(')'); break;
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
    case N_PAY: printf("pay "); fmt_expr(n->a,1); printf(" from %s to %s", n->src, n->dst); break;
    case N_PAYWALL: printf("paywall %s = ", n->name); fmt_expr(n->a,10); printf(" / %s to %s", n->period, n->dst); break;
    case N_SUBSCRIBE: printf("subscribe %s to %s", n->src, n->dst); break;
    case N_REQUIRE: printf("require "); fmt_expr(n->a,1); if(n->str){ printf(", "); fmt_str_lit(n->str); } break;
    case N_RETURN: printf("return"); if(n->a){ putchar(' '); fmt_expr(n->a,1); } break;
    case N_THROW: printf("throw "); fmt_expr(n->a,1); break;
    case N_BREAK: printf("break"); break;
    case N_CONTINUE: printf("continue"); break;
    case N_IMPORT: printf("import "); fmt_str_lit(n->str); if(n->name) printf(" as %s", n->name); break;
    case N_EXPR: fmt_expr(n->a,1); break;
    case N_FN: printf("fn %s", n->name?n->name:""); fmt_params(n); if(n->has_gas) printf(" gas %lld", n->gas);
               printf(" {\n"); for(int i=0;i<n->b->nkids;i++) fmt_stmt(n->b->kids[i], indent+1); fmt_indent(indent); putchar('}'); break;
    case N_IF: printf("if "); fmt_expr(n->a,1); printf(" {\n"); for(int i=0;i<n->b->nkids;i++) fmt_stmt(n->b->kids[i], indent+1); fmt_indent(indent); putchar('}');
               if(n->c){ printf(" else "); if(n->c->kind==N_IF) fmt_stmt_core(n->c, indent); else { printf("{\n"); for(int i=0;i<n->c->nkids;i++) fmt_stmt(n->c->kids[i], indent+1); fmt_indent(indent); putchar('}'); } } break;
    case N_WHILE: printf("while "); fmt_expr(n->a,1); printf(" {\n"); for(int i=0;i<n->b->nkids;i++) fmt_stmt(n->b->kids[i], indent+1); fmt_indent(indent); putchar('}'); break;
    case N_FOR: printf("for %s in ", n->name); fmt_expr(n->a,1); printf(" {\n"); for(int i=0;i<n->b->nkids;i++) fmt_stmt(n->b->kids[i], indent+1); fmt_indent(indent); putchar('}'); break;
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
  "  larzscript [--ledger] <file>   also print the money ledger afterwards\n"
  "  larzscript --version | --help\n";

int main(int argc, char **argv){
  if(getenv("LZ_GC_STRESS")) g_gc_threshold=0;   /* collect on every statement (test mode) */
  const char *path=NULL, *eval_code=NULL; int show_ledger=0, want_repl=0, want_fmt=0;
  int i=1;
  for(; i<argc; i++){
    const char *a=argv[i];
    if(strcmp(a,"--version")==0 || strcmp(a,"-v")==0){ printf("larzscript (native) 1.7.0\n"); return 0; }
    if(strcmp(a,"--help")==0 || strcmp(a,"-h")==0){ printf("%s", USAGE); return 0; }
    if(strcmp(a,"--ledger")==0){ show_ledger=1; continue; }
    if(strcmp(a,"fmt")==0){ want_fmt=1; continue; }
    if(strcmp(a,"-e")==0 || strcmp(a,"--eval")==0){ if(i+1>=argc){ fprintf(stderr,"larzscript: -e needs code\n"); return 1; } eval_code=argv[++i]; i++; break; }
    if(strcmp(a,"repl")==0){ want_repl=1; i++; break; }
    path=a; i++; break;                 /* the source file; the rest are program args */
  }
  /* remaining argv[i..] are the program's own arguments */
  List *prog_args=list_new();
  for(; i<argc; i++) list_push(prog_args, V_string(xstrdup(argv[i])));

  if(want_fmt){
    if(!path){ fprintf(stderr,"larzscript fmt: needs a file\n"); return 1; }
    char *src=read_all(path);
    if(setjmp(g_err)){ fprintf(stderr,"SyntaxError: %s\n", g_errmsg); return 1; }
    format_program(parse_program(lex(src)));
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
