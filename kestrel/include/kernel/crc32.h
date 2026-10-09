/* include/kernel/crc32.h -- CRC-32 (IEEE 802.3, reflected 0xEDB88320), as
 * used by GPT headers/entry arrays and the Kestrel boot volume header. */
#ifndef KESTREL_CRC32_H
#define KESTREL_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* crc32(0, buf, n) for a fresh checksum; feed the result back to continue. */
uint32_t crc32(uint32_t crc, const void *buf, size_t n);

#endif
