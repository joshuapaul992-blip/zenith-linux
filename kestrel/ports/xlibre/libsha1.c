/* libsha1.c -- the "libsha1" API (sha1_begin/sha1_hash/sha1_end) that the
 * X server's os/xsha1.c can use, for builds without a crypto library.
 * A straightforward FIPS 180-1 implementation; written for Kestrel. */
#include "libsha1.h"
#include <string.h>

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void block(sha1_ctx *c, const unsigned char *p)
{
    uint32_t w[80], a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4], cc = c->h[2];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);          k = 0x5a827999; }
        else if (i < 40) { f = b ^ cc ^ d;                   k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8f1bbcdc; }
        else             { f = b ^ cc ^ d;                   k = 0xca62c1d6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void sha1_begin(sha1_ctx *c)
{
    static const uint32_t init[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    memcpy(c->h, init, sizeof init);
    c->len = 0;
    c->used = 0;
}

void sha1_hash(const unsigned char *data, unsigned long len, sha1_ctx *c)
{
    c->len += len;
    while (len) {
        unsigned long n = 64 - c->used < len ? 64 - c->used : len;
        memcpy(c->buf + c->used, data, n);
        c->used += n; data += n; len -= n;
        if (c->used == 64) { block(c, c->buf); c->used = 0; }
    }
}

void sha1_end(unsigned char hval[20], sha1_ctx *c)
{
    uint64_t bits = c->len * 8;
    unsigned char pad = 0x80, zero = 0, lenb[8];
    sha1_hash(&pad, 1, c);
    while (c->used != 56) sha1_hash(&zero, 1, c);
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha1_hash(lenb, 8, c);
    for (int i = 0; i < 20; i++) hval[i] = (unsigned char)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}
