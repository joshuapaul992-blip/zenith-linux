/* include/kernel/vt.h -- a Unix shell on every text terminal
 *
 * Each monitor's TTY becomes a terminal in the Unix sense: /dev/ttyN is the
 * slave of a console pty (fs/pty.c), so termios, job control and the
 * keyboard signals work as on Linux, and /bin/sh (BusyBox ash) runs on it
 * as a login shell. See kernel/vt.c. */
#ifndef KESTREL_VT_H
#define KESTREL_VT_H

#include <stdbool.h>

/* Use /bin/sh on the terminals? False with kestrel.shell=kush on the kernel
 * command line, or when there is no /bin/sh (kush is used instead). */
bool vt_shell_available(void);

/* Turn TTY `index` into a terminal and keep a shell running on it: start
 * one, wait for it, hang the terminal up, start the next. Never returns;
 * run it in a thread of its own (or as init on TTY 0). */
void vt_session(int index) __attribute__((noreturn));

#endif
