/* include/kernel/report.h -- diagnostic report written to the boot stick
 *
 * Machines without a serial port still need a way to hand a log to a
 * developer. tools/mkusbimg.py puts a small FAT volume labelled
 * "KESTREL RPT" on the stick with two preallocated files; this module finds
 * that volume on any block device, follows the files' cluster chains and
 * overwrites their contents in place:
 *
 *   REPORT.TXT   header (version, firmware, command line, CPU), every
 *                registered section (e.g. the NVIDIA report) and the
 *                complete kernel log
 *   VBIOS.ROM    a registered binary blob (the graphics card's video BIOS)
 *
 * File sizes, the FAT and the directory are never changed, so the volume
 * stays valid for every OS. Only a volume with exactly that label is
 * touched. */
#ifndef KESTREL_REPORT_H
#define KESTREL_REPORT_H

#include <stddef.h>
#include <stdbool.h>

/* A section of REPORT.TXT: fn fills buf (at most cap bytes) and returns the
 * length. Sections appear in registration order. */
void report_add_section(const char *title, size_t (*fn)(char *buf, size_t cap));
/* Contents of VBIOS.ROM (the pointer must stay valid). */
void report_set_blob(const void *data, size_t len, const char *what);

/* Find the report volume (boot: after USB storage has settled). */
bool report_locate(void);
/* Write REPORT.TXT (and VBIOS.ROM if a blob is set). Returns 0 or -errno. */
int  report_save(const char *reason);
/* One line for the shell: where the report goes, or why it cannot. */
const char *report_where(void);

#endif
