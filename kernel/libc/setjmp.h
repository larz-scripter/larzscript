/* freestanding setjmp.h for the LarzOS kernel (see setjmp.S) */
#ifndef _LARZOS_SETJMP_H
#define _LARZOS_SETJMP_H
typedef long jmp_buf[8];          /* rbx,rbp,r12,r13,r14,r15,rsp,rip */
int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);
#endif
