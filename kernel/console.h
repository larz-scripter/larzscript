/* console.h - low-level console the kernel and libk share */
#ifndef _LARZOS_CONSOLE_H
#define _LARZOS_CONSOLE_H
void console_init(void);
void ints_init(void);            /* IDT + PIC + timer/keyboard IRQs */
void sched_init(void);           /* preemptive scheduler + demo background tasks */
void vfs_init(void);             /* build the writable filesystem from the initramfs */
void serial_putc(char c);
char serial_getc(void);
char console_getc(void);          /* blocks on serial OR the PS/2 keyboard */
int  serial_can_read(void);
void qemu_exit(unsigned char code);
#endif
