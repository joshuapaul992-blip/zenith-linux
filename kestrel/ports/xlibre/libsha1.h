/* libsha1.h -- see libsha1.c */
#ifndef LIBSHA1_H
#define LIBSHA1_H
#include <stdint.h>
#define SHA1_DIGEST_SIZE 20
typedef struct { uint32_t h[5]; uint64_t len; unsigned char buf[64]; unsigned long used; } sha1_ctx;
void sha1_begin(sha1_ctx *ctx);
void sha1_hash(const unsigned char *data, unsigned long len, sha1_ctx *ctx);
void sha1_end(unsigned char hval[SHA1_DIGEST_SIZE], sha1_ctx *ctx);
#endif
