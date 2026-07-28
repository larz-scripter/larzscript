/* console.h - low-level console the kernel and libk share */
#ifndef _LARZOS_CONSOLE_H
#define _LARZOS_CONSOLE_H
void console_init(void);
void serial_putc(char c);
char serial_getc(void);
int  serial_can_read(void);
void qemu_exit(unsigned char code);
#endif
