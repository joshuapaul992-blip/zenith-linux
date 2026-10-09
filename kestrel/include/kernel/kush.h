/* include/kernel/kush.h -- 'kush', the Kestrel command interpreter */
#ifndef KESTREL_KUSH_H
#define KESTREL_KUSH_H

/* Read-eval-print loop on the calling task's TTY (tty.h); each call is an
 * independent shell instance. Never returns. */
void kush_main(void) __attribute__((noreturn));

#endif
