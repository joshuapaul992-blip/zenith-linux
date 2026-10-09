#pragma once
#include <stdint.h>
typedef void *APTR; typedef const void *CONST_APTR; typedef uintptr_t IPTR; typedef intptr_t SIPTR;
typedef uint64_t UQUAD; typedef int64_t QUAD; typedef uint32_t ULONG; typedef int32_t LONG;
typedef uint16_t UWORD; typedef int16_t WORD; typedef uint8_t UBYTE; typedef int8_t BYTE;
typedef short BOOL; typedef void VOID; typedef unsigned char *STRPTR; typedef const unsigned char *CONST_STRPTR;
typedef unsigned char TEXT; typedef void (*VOID_FUNC)(void); typedef IPTR BPTR;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define AROS_LE2WORD(x) ((UWORD)(x))
#define AROS_LE2LONG(x) ((ULONG)(x))
#define AROS_WORD2LE(x) ((UWORD)(x))
#define AROS_LONG2LE(x) ((ULONG)(x))
