/* console.h - low-level console the kernel and libk share */
#ifndef _LARZOS_CONSOLE_H
#define _LARZOS_CONSOLE_H
void console_init(void);
void ints_init(void);            /* IDT + PIC + timer/keyboard IRQs */
void sched_init(void);           /* preemptive scheduler + demo background tasks */
void task_create(void (*fn)(void));   /* spawn fn as a new preemptively-scheduled task */
int  current_task_id(void);           /* which task is running right now (schedule()'s g_cur) */
int  next_task_slot(void);            /* index task_create() will assign next, or -1 if full */
void task_exit(int tid);              /* kill task tid - see kernel/libk.c for what this does and doesn't do */
void desktop_icon_activate(int i);    /* kernel.c: launch the i-th desktop-icon manifest entry, or no-op */
int  launch_app(const char *script);  /* kernel.c: launch script as a new app task, returns its slot or -1 */
void vfs_init(void);             /* build the writable filesystem from the initramfs */
void serial_putc(char c);
char serial_getc(void);
char console_getc(void);          /* blocks on serial OR the PS/2 keyboard */
int  serial_can_read(void);
void qemu_exit(unsigned char code);
void kbd_diag(void);              /* raw 8042 keyboard diagnostic (never returns) */
#endif
