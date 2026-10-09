#pragma once
#include <stdint.h>
#define __bitcount16(x) __builtin_popcount((uint16_t)(x))
#define __bitcount32(x) __builtin_popcount((uint32_t)(x))
#define __bitcount64(x) __builtin_popcountll((uint64_t)(x))
#define __bitcountl(x)  __builtin_popcountl((unsigned long)(x))
