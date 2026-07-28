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
 * Core (v0.1 native): numbers, money ($ = integer cents), strings, booleans,
 * nil; let/assign; if/else; while; functions + recursion + closures; gas-metered
 * functions; wallets; price; pay ... from ... to ...; require; print/money.
 * Memory: a simple grow-only allocator (freed by the OS on exit) - a bootstrap
 * interpreter, honest about it. Zero third-party dependencies (libc only).
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

/* ===================== values ===================== */
typedef enum { V_NIL, V_BOOL, V_NUM, V_MONEY, V_STR, V_WALLET, V_FUNC, V_BUILTIN, V_LIST, V_PAYWALL } VType;
struct Node; struct Env; struct Interp; struct List; struct Paywall;
typedef struct Wallet { char *name; long long cents; } Wallet;
typedef struct Closure { struct Node *decl; struct Env *env; const char *name; } Closure;
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
};

static Value V_nil(void){ Value v; v.t=V_NIL; return v; }
static Value V_bool(int b){ Value v; v.t=V_BOOL; v.b=b!=0; return v; }
static Value V_number(double d){ Value v; v.t=V_NUM; v.num=d; return v; }
static Value V_money(long long c){ Value v; v.t=V_MONEY; v.cents=c; return v; }
static Value V_string(char *s){ Value v; v.t=V_STR; v.str=s; return v; }
static Value V_wallet(Wallet *w){ Value v; v.t=V_WALLET; v.wal=w; return v; }
static Value V_func(Closure *c){ Value v; v.t=V_FUNC; v.fn=c; return v; }
static Value V_builtin(Builtin *b){ Value v; v.t=V_BUILTIN; v.bi=b; return v; }

typedef struct List { Value *items; int n, cap; } List;
typedef struct Paywall { char *name; long long price; char *period; char *payee; } Paywall;
static Value V_list(List *l){ Value v; v.t=V_LIST; v.list=l; return v; }
static Value V_paywall(Paywall *pw){ Value v; v.t=V_PAYWALL; v.pw=pw; return v; }
static List *list_new(void){ List *l=xmalloc(sizeof(List)); l->items=NULL; l->n=0; l->cap=0; return l; }
static void list_push(List *l, Value v){ if(l->n==l->cap){ l->cap=l->cap?l->cap*2:8; l->items=realloc(l->items,l->cap*sizeof(Value)); } l->items[l->n++]=v; }

static long long money_round(double x){ return (long long)(x>=0 ? x+0.5 : x-0.5); }

static int is_num(Value v){ return v.t==V_NUM; }

static int truthy(Value v){
  switch(v.t){
    case V_NIL: return 0;
    case V_BOOL: return v.b;
    case V_NUM: return v.num!=0;
    case V_MONEY: return v.cents!=0;
    case V_STR: return v.str[0]!=0;
    case V_LIST: return v.list->n!=0;
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
  }
}

/* ===================== AST ===================== */
typedef enum {
  N_LET, N_ASSIGN, N_PRICE, N_WALLET, N_PAY, N_REQUIRE, N_FN, N_RETURN,
  N_IF, N_WHILE, N_BLOCK, N_EXPR, N_PAYWALL, N_SUBSCRIBE,
  N_NUM, N_MONEY, N_STR, N_BOOL, N_NIL, N_NAME, N_BIN, N_UN, N_CALL, N_GET, N_METHOD,
  N_ARRAY, N_INDEX
} NodeKind;

typedef struct Node {
  NodeKind kind;
  double num; long long cents; char *str; int boolean;
  char *name;                 /* identifier / member / let-name / op holder */
  char *op;
  struct Node *a, *b, *c;
  struct Node **kids; int nkids;
  char **params; int nparams;
  long long gas; int has_gas;
  char *src, *dst;            /* pay / subscribe */
  char *period;               /* paywall */
} Node;

static Node *node(NodeKind k){ Node *n = xmalloc(sizeof(Node)); memset(n,0,sizeof(Node)); n->kind=k; return n; }
static Node *mkname(const char *s){ Node *n=node(N_NAME); n->name=xstrdup(s); return n; }
static Node *mknum(double d){ Node *n=node(N_NUM); n->num=d; return n; }
static void push_kid(Node *b, Node *k){
  if(b->nkids % 8 == 0) b->kids = realloc(b->kids, (b->nkids+8)*sizeof(Node*));
  b->kids[b->nkids++]=k;
}

/* ===================== lexer ===================== */
typedef enum { T_EOF, T_NUM, T_MONEY, T_STR, T_IDENT, T_KW, T_OP,
               T_LP, T_RP, T_LB, T_RB, T_COMMA, T_DOT, T_LBK, T_RBK } TokType;
typedef struct { TokType type; char *text; double num; long long cents; int line; } Token;

static const char *KEYWORDS[] = {
  "let","fn","return","if","else","while","and","or","not","true","false","nil",
  "price","wallet","pay","from","to","require","gas",
  "paywall","subscribe","has","for","in", NULL
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
    if(c==' '||c=='\t'||c=='\r'){ i++; continue; }
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
    if(c=='"'){
      int j=i+1; char buf[4096]; int b=0;
      while(src[j] && src[j]!='"'){
        if(src[j]=='\\' && src[j+1]){
          char e=src[j+1];
          buf[b++]= e=='n'?'\n': e=='t'?'\t': e;
          j+=2; continue;
        }
        buf[b++]=src[j++];
      }
      if(!src[j]) fail("unterminated string on line %d", line);
      buf[b]=0; t.type=T_STR; t.text=xstrdup(buf); i=j+1; tk_push(&tl,t); continue;
    }
    if(isalpha((unsigned char)c)||c=='_'){
      int j=i; while(isalnum((unsigned char)src[j])||src[j]=='_') j++;
      t.text=xstrndup(src+i,j-i);
      t.type = is_keyword(src+i,j-i) ? T_KW : T_IDENT;
      i=j; tk_push(&tl,t); continue;
    }
    /* two-char operators */
    if((c=='='&&src[i+1]=='=')||(c=='!'&&src[i+1]=='=')||(c=='<'&&src[i+1]=='=')||(c=='>'&&src[i+1]=='=')){
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
static Token *pk2(Parser *p){ return &p->t[p->i+1]; }
static Token *padv(Parser *p){ return &p->t[p->i++]; }
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
  if(is_kw(t,"true")){ padv(p); Node *n=node(N_BOOL); n->boolean=1; return n; }
  if(is_kw(t,"false")){ padv(p); Node *n=node(N_BOOL); n->boolean=0; return n; }
  if(is_kw(t,"nil")){ padv(p); return node(N_NIL); }
  if(t->type==T_IDENT){ padv(p); Node *n=node(N_NAME); n->name=t->text; return n; }
  if(t->type==T_LP){ padv(p); Node *e=expression(p); expect(p,T_RP,"')'"); return e; }
  if(t->type==T_LBK){
    padv(p); Node *n=node(N_ARRAY);
    if(pk(p)->type!=T_RBK){
      push_kid(n, expression(p));
      while(pk(p)->type==T_COMMA){ padv(p); if(pk(p)->type==T_RBK) break; push_kid(n, expression(p)); }
    }
    expect(p, T_RBK, "']'");
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
      if(pk(p)->type!=T_IDENT) fail("expected a property name on line %d", pk(p)->line);
      char *name = padv(p)->text;
      if(pk(p)->type==T_LP){
        Node *m=node(N_METHOD); m->a=n; m->name=name; arglist(p,&m->kids,&m->nkids); n=m;
      } else {
        Node *g=node(N_GET); g->a=n; g->name=name; n=g;
      }
    } else if(pk(p)->type==T_LP){
      Node *c=node(N_CALL); c->a=n; arglist(p,&c->kids,&c->nkids); n=c;
    } else if(pk(p)->type==T_LBK){
      padv(p); Node *ix=node(N_INDEX); ix->a=n; ix->b=expression(p); expect(p,T_RBK,"']'"); n=ix;
    } else break;
  }
  return n;
}

static Node *unary(Parser *p){
  if(is_kw(pk(p),"not")){ padv(p); Node *n=node(N_UN); n->op="not"; n->a=unary(p); return n; }
  if(is_op(pk(p),"-")){ padv(p); Node *n=node(N_UN); n->op="-"; n->a=unary(p); return n; }
  return postfix(p);
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
static Node *factor(Parser *p){ const char *o[]={"*","/","%"}; return bin_lvl(p,unary,o,3,0); }
static Node *term(Parser *p){ const char *o[]={"+","-"}; return bin_lvl(p,factor,o,2,0); }
static Node *comparison(Parser *p){ const char *o[]={"<","<=",">",">="}; return bin_lvl(p,term,o,4,0); }
static Node *membership(Parser *p){ const char *o[]={"has"}; return bin_lvl(p,comparison,o,1,1); }
static Node *equality(Parser *p){ const char *o[]={"==","!="}; return bin_lvl(p,membership,o,2,0); }
static Node *logic_and(Parser *p){ const char *o[]={"and"}; return bin_lvl(p,equality,o,1,1); }
static Node *logic_or(Parser *p){ const char *o[]={"or"}; return bin_lvl(p,logic_and,o,1,1); }
static Node *expression(Parser *p){ return logic_or(p); }

static int starts_expr(Token *t){
  if(t->type==T_NUM||t->type==T_MONEY||t->type==T_STR||t->type==T_IDENT||t->type==T_LP||t->type==T_LBK) return 1;
  if(is_kw(t,"true")||is_kw(t,"false")||is_kw(t,"nil")||is_kw(t,"not")) return 1;
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
  if(is_kw(t,"fn")){ padv(p); Node *n=node(N_FN); n->name=padv(p)->text; expect(p,T_LP,"'('");
      int cap=0; if(pk(p)->type!=T_RP){ do{ if(n->nparams==cap){cap=cap?cap*2:4; n->params=realloc(n->params,cap*sizeof(char*));} n->params[n->nparams++]=padv(p)->text; }while(pk(p)->type==T_COMMA&&(padv(p),1)); }
      expect(p,T_RP,"')'");
      if(is_kw(pk(p),"gas")){ padv(p); if(pk(p)->type!=T_NUM) fail("expected a gas amount on line %d",pk(p)->line); n->gas=(long long)padv(p)->num; n->has_gas=1; }
      n->b=block(p); return n; }
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
      padv(p); char *var=padv(p)->text; expect_kw(p,"in");
      Node *iter=expression(p); Node *body=block(p);
      p->loopn++;
      char xs[32], ix[32];
      snprintf(xs,sizeof(xs),"__for%d_xs",p->loopn);
      snprintf(ix,sizeof(ix),"__for%d_i",p->loopn);
      /* inner block: let var = xs[ix]; <body>; ix = ix + 1 */
      Node *inner=node(N_BLOCK);
      Node *letx=node(N_LET); letx->name=var;
      Node *idxn=node(N_INDEX); idxn->a=mkname(xs); idxn->b=mkname(ix); letx->a=idxn;
      push_kid(inner, letx);
      for(int i=0;i<body->nkids;i++) push_kid(inner, body->kids[i]);
      Node *inc=node(N_ASSIGN); inc->name=xstrdup(ix);
      Node *add=node(N_BIN); add->op="+"; add->a=mkname(ix); add->b=mknum(1); inc->a=add;
      push_kid(inner, inc);
      /* while ix < len(xs) { inner } */
      Node *cond=node(N_BIN); cond->op="<"; cond->a=mkname(ix);
      Node *call=node(N_CALL); call->a=mkname("len"); push_kid(call, mkname(xs)); cond->b=call;
      Node *wh=node(N_WHILE); wh->a=cond; wh->b=inner;
      /* outer block: let xs = iter; let ix = 0; while ... */
      Node *outer=node(N_BLOCK);
      Node *letxs=node(N_LET); letxs->name=xstrdup(xs); letxs->a=iter; push_kid(outer, letxs);
      Node *leti=node(N_LET); leti->name=xstrdup(ix); leti->a=mknum(0); push_kid(outer, leti);
      push_kid(outer, wh);
      return outer;
  }
  if(t->type==T_LB){ return block(p); }
  if(t->type==T_IDENT && is_op(pk2(p),"=")){ Node *n=node(N_ASSIGN); n->name=padv(p)->text; padv(p); n->a=expression(p); return n; }
  Node *n=node(N_EXPR); n->a=expression(p); return n;
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
typedef struct Entry { char *name; Value val; struct Entry *next; } Entry;
typedef struct Env { Entry *head; struct Env *parent; } Env;
static Env *env_new(Env *parent){ Env *e=xmalloc(sizeof(Env)); e->head=NULL; e->parent=parent; return e; }
static Value *env_find(Env *e, const char *name){
  for(; e; e=e->parent) for(Entry *it=e->head; it; it=it->next) if(strcmp(it->name,name)==0) return &it->val;
  return NULL;
}
static void env_define(Env *e, const char *name, Value v){
  for(Entry *it=e->head; it; it=it->next) if(strcmp(it->name,name)==0){ it->val=v; return; }
  Entry *n=xmalloc(sizeof(Entry)); n->name=xstrdup(name); n->val=v; n->next=e->head; e->head=n;
}

/* ===================== interpreter ===================== */
typedef struct Txn { const char *src, *dst; long long cents; } Txn;
typedef struct Sub { const char *w, *p; } Sub;
typedef struct Interp {
  Env *globals;
  int has_gas; long long gas; long long gas_used;
  int returning; Value retval;
  Txn *ledger; int nled, ledcap;
  Sub *subs; int nsub, subcap;
  jmp_buf jb; char errmsg[256]; const char *errname;
} Interp;

static void runtime_error(Interp *ip, const char *name, const char *fmt, ...){
  ip->errname=name;
  va_list ap; va_start(ap,fmt); vsnprintf(ip->errmsg,sizeof(ip->errmsg),fmt,ap); va_end(ap);
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
    int eq=0;
    if(a.t!=b.t) eq=0;
    else switch(a.t){
      case V_NIL: eq=1; break;
      case V_BOOL: eq=a.b==b.b; break;
      case V_NUM: eq=a.num==b.num; break;
      case V_MONEY: eq=a.cents==b.cents; break;
      case V_STR: eq=strcmp(a.str,b.str)==0; break;
      default: eq=0;
    }
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
    runtime_error(ip,"LarzTypeError","cannot multiply those values");
  }
  if(strcmp(op,"/")==0){
    if(a.t==V_MONEY && is_num(b)){ if(b.num==0) runtime_error(ip,"MoneyError","cannot divide money by zero"); return V_money(money_round((double)a.cents/b.num)); }
    if(bn){ if(b.num==0) runtime_error(ip,"LarzRuntimeError","division by zero"); return V_number(a.num/b.num); }
    runtime_error(ip,"LarzTypeError","cannot divide those values");
  }
  if(strcmp(op,"%")==0){ if(bn){ if(b.num==0) runtime_error(ip,"LarzRuntimeError","division by zero"); return V_number((double)((long long)a.num % (long long)b.num)); } runtime_error(ip,"LarzTypeError","cannot take modulo"); }
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

static Value eval(Interp *ip, Node *n, Env *env){
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
      return do_binop(ip,n->op, eval(ip,n->a,env), eval(ip,n->b,env));
    }
    case N_ARRAY: {
      List *l=list_new();
      for(int i=0;i<n->nkids;i++) list_push(l, eval(ip,n->kids[i],env));
      return V_list(l);
    }
    case N_INDEX: {
      Value obj=eval(ip,n->a,env), iv=eval(ip,n->b,env);
      if(!is_num(iv) || iv.num!=(long long)iv.num) runtime_error(ip,"LarzTypeError","index must be a whole number");
      long long idx=(long long)iv.num;
      if(obj.t==V_LIST){ if(idx<0||idx>=obj.list->n) runtime_error(ip,"LarzRuntimeError","index %lld out of range (length %d)", idx, obj.list->n); return obj.list->items[idx]; }
      if(obj.t==V_STR){ int len=(int)strlen(obj.str); if(idx<0||idx>=len) runtime_error(ip,"LarzRuntimeError","index out of range"); char *s=xmalloc(2); s[0]=obj.str[idx]; s[1]=0; return V_string(s); }
      runtime_error(ip,"LarzTypeError","cannot index that value");
    }
    case N_CALL: {
      Value callee=eval(ip,n->a,env);
      Value args[64]; if(n->nkids>64) runtime_error(ip,"LarzTypeError","too many arguments");
      for(int i=0;i<n->nkids;i++) args[i]=eval(ip,n->kids[i],env);
      return call_value(ip, callee, args, n->nkids);
    }
    case N_GET: {
      Value obj=eval(ip,n->a,env);
      if(obj.t==V_WALLET){
        if(strcmp(n->name,"balance")==0) return V_money(obj.wal->cents);
        if(strcmp(n->name,"name")==0) return V_string(obj.wal->name);
        runtime_error(ip,"LarzTypeError","a wallet has no property '%s'", n->name);
      }
      runtime_error(ip,"LarzTypeError","cannot read '%s'", n->name);
    }
    case N_METHOD: {
      Value obj=eval(ip,n->a,env);
      Value args[8]; for(int i=0;i<n->nkids && i<8;i++) args[i]=eval(ip,n->kids[i],env);
      if(obj.t==V_WALLET){
        if(strcmp(n->name,"credit")==0 || strcmp(n->name,"debit")==0){
          if(n->nkids!=1 || args[0].t!=V_MONEY) runtime_error(ip,"LarzTypeError","wallet.%s expects one money argument", n->name);
          if(strcmp(n->name,"credit")==0) obj.wal->cents += args[0].cents;
          else { if(args[0].cents>obj.wal->cents) runtime_error(ip,"MoneyError","wallet '%s' has insufficient funds", obj.wal->name); obj.wal->cents -= args[0].cents; }
          return V_nil();
        }
        runtime_error(ip,"LarzTypeError","a wallet has no method '%s'", n->name);
      }
      runtime_error(ip,"LarzTypeError","cannot call a method on that value");
    }
    default: runtime_error(ip,"LarzRuntimeError","cannot evaluate that node"); return V_nil();
  }
}

static Value call_value(Interp *ip, Value callee, Value *args, int nargs){
  if(callee.t==V_BUILTIN) return callee.bi->fn(ip,args,nargs);
  if(callee.t==V_FUNC){
    Node *decl=callee.fn->decl;
    if(nargs!=decl->nparams) runtime_error(ip,"LarzTypeError","%s expects %d argument(s), got %d", decl->name, decl->nparams, nargs);
    if(decl->has_gas && decl->gas){
      ip->gas_used += decl->gas;
      if(ip->has_gas){ if(decl->gas>ip->gas) runtime_error(ip,"OutOfGasError","out of gas calling '%s'", decl->name); ip->gas -= decl->gas; }
    }
    Env *call_env=env_new(callee.fn->env);
    for(int i=0;i<decl->nparams;i++) env_define(call_env, decl->params[i], args[i]);
    ip->returning=0;
    for(int i=0;i<decl->b->nkids;i++){ exec(ip, decl->b->kids[i], call_env); if(ip->returning) break; }
    Value r = ip->returning ? ip->retval : V_nil();
    ip->returning=0;
    return r;
  }
  runtime_error(ip,"LarzTypeError","that value is not callable");
  return V_nil();
}

static void exec(Interp *ip, Node *n, Env *env){
  switch(n->kind){
    case N_LET: env_define(env, n->name, eval(ip,n->a,env)); return;
    case N_ASSIGN: { Value *slot=env_find(env,n->name); if(!slot) runtime_error(ip,"LarzNameError","cannot assign to undefined '%s' (use 'let')", n->name); *slot=eval(ip,n->a,env); return; }
    case N_PRICE: { Value v=eval(ip,n->a,env); if(v.t!=V_MONEY) runtime_error(ip,"LarzTypeError","a price must be money"); env_define(env,n->name,v); return; }
    case N_WALLET: {
      long long c=0;
      if(n->a){ Value v=eval(ip,n->a,env); if(v.t!=V_MONEY) runtime_error(ip,"LarzTypeError","a wallet balance must be money"); c=v.cents; }
      Wallet *w=xmalloc(sizeof(Wallet)); w->name=xstrdup(n->name); w->cents=c;
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
      Paywall *pw=xmalloc(sizeof(Paywall));
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
    case N_FN: { Closure *c=xmalloc(sizeof(Closure)); c->decl=n; c->env=env; c->name=n->name; env_define(env,n->name,V_func(c)); return; }
    case N_RETURN: { ip->retval = n->a ? eval(ip,n->a,env) : V_nil(); ip->returning=1; return; }
    case N_IF: {
      if(truthy(eval(ip,n->a,env))) exec(ip,n->b,env);
      else if(n->c) exec(ip,n->c,env);
      return;
    }
    case N_WHILE: { while(truthy(eval(ip,n->a,env))){ exec(ip,n->b,env); if(ip->returning) break; } return; }
    case N_BLOCK: { Env *child=env_new(env); for(int i=0;i<n->nkids;i++){ exec(ip,n->kids[i],child); if(ip->returning) break; } return; }
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
  runtime_error(ip,"LarzTypeError","len() expects a string or list");
  return V_nil();
}
static Value bi_push(Interp *ip, Value *args, int n){
  if(n!=2 || args[0].t!=V_LIST) runtime_error(ip,"LarzTypeError","push() expects a list and an item");
  list_push(args[0].list, args[1]);
  return V_nil();
}
static Value bi_range(Interp *ip, Value *args, int n){
  if(n!=1 || !is_num(args[0])) runtime_error(ip,"LarzTypeError","range() expects one number");
  List *l=list_new(); long long m=(long long)args[0].num;
  for(long long i=0;i<m;i++) list_push(l, V_number((double)i));
  return V_list(l);
}
static Builtin B_print = {"print", bi_print};
static Builtin B_money = {"money", bi_money};
static Builtin B_len   = {"len",   bi_len};
static Builtin B_push  = {"push",  bi_push};
static Builtin B_range = {"range", bi_range};

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

int main(int argc, char **argv){
  const char *path=NULL; int show_ledger=0;
  for(int i=1;i<argc;i++){
    if(strcmp(argv[i],"--version")==0 || strcmp(argv[i],"-v")==0){ printf("larzscript (native) 0.2.0\n"); return 0; }
    if(strcmp(argv[i],"--help")==0 || strcmp(argv[i],"-h")==0){ printf("usage: larzscript [--ledger] <program.lz>\n"); return 0; }
    if(strcmp(argv[i],"--ledger")==0){ show_ledger=1; continue; }
    path=argv[i];
  }
  char *src = read_all(path);

  if(setjmp(g_err)){ fprintf(stderr,"SyntaxError: %s\n", g_errmsg); return 1; }
  Token *toks = lex(src);
  Node *prog = parse_program(toks);

  Interp ip; memset(&ip,0,sizeof(ip));
  ip.globals = env_new(NULL);
  ip.has_gas = 0;                 /* unlimited by default */
  env_define(ip.globals, "print", V_builtin(&B_print));
  env_define(ip.globals, "money", V_builtin(&B_money));
  env_define(ip.globals, "len",   V_builtin(&B_len));
  env_define(ip.globals, "push",  V_builtin(&B_push));
  env_define(ip.globals, "range", V_builtin(&B_range));

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
