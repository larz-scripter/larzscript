/* setjmp.S - real x86_64 setjmp/longjmp (System V ABI), freestanding.
 * jmp_buf is long[8]: rbx, rbp, r12, r13, r14, r15, rsp, rip.
 * The interpreter save/restores jmp_buf with memcpy, so these behave exactly
 * like libc's (unlike __builtin_setjmp).
 */
.section .text

.global setjmp
.type setjmp, @function
setjmp:
    movq %rbx,  0(%rdi)
    movq %rbp,  8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax          /* caller's rsp (undo the call push) */
    movq %rax, 48(%rdi)
    movq (%rsp), %rax           /* return address */
    movq %rax, 56(%rdi)
    xorl %eax, %eax             /* setjmp returns 0 */
    ret
.size setjmp, . - setjmp

.global longjmp
.type longjmp, @function
longjmp:
    movq  0(%rdi), %rbx
    movq  8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    movq 56(%rdi), %rdx         /* saved rip */
    movl %esi, %eax             /* return the value passed to longjmp */
    testl %eax, %eax
    jnz 1f
    movl $1, %eax               /* ...but never 0 */
1:  jmp *%rdx
.size longjmp, . - longjmp

.section .note.GNU-stack,"",@progbits
