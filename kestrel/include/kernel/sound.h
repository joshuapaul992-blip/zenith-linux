/* include/kernel/sound.h -- sound cards (drivers/sound)
 *
 * Intel High Definition Audio controllers and their codecs (Realtek ALC and
 * the others), played through the OSS interface: /dev/dsp and /dev/mixer. */
#ifndef KESTREL_SOUND_H
#define KESTREL_SOUND_H

#include <stddef.h>

void   hda_init(void);                      /* after PCI enumeration and devfs */
size_t hda_proc(char *buf, size_t cap);     /* /proc/sound */

#endif
