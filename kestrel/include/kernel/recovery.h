/* include/kernel/recovery.h -- boot failure diagnostics + recovery shell
 *
 * recovery_enter() clears the 1024x768 terminal, prints a diagnostic trace
 * (the failure, the clock source, the xHCI operational/runtime registers
 * and every root port's PORTSC decoded, USB mass-storage transport
 * statistics with the last SCSI sense data, the block devices and the tail
 * of the kernel log), mirrors all of it to COM1, and then runs a small
 * command interpreter that accepts input from the keyboard (PS/2 or USB)
 * and from the serial line at the same time. */
#ifndef KESTREL_RECOVERY_H
#define KESTREL_RECOVERY_H

enum recovery_action { RECOVERY_RETRY, RECOVERY_CONTINUE };

enum recovery_action recovery_enter(const char *reason, const char *detail);

#endif
