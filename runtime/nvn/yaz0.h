// Yaz0 (Nintendo's LZ77 variant: the .szs archives) decompression, header only: the shader indexer
// (shader_prep.c) and tools/shader_corpus.m read the game's compressed shader archives with it.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The decompressed size of a Yaz0 stream (0: not one)
static inline size_t yaz0_size(const uint8_t *src, size_t n) {
    if (n < 16 || memcmp(src, "Yaz0", 4)) return 0;
    return (size_t)src[4] << 24 | (size_t)src[5] << 16 | (size_t)src[6] << 8 | src[7];
}

// Decompresses src (n bytes, with its header) into a new buffer of *out_n bytes; NULL when it isn't valid Yaz0
static inline uint8_t *yaz0_decompress(const uint8_t *src, size_t n, size_t *out_n) {
    size_t size = yaz0_size(src, n);
    if (!size || size > (1u << 30)) return NULL;
    uint8_t *dst = (uint8_t *)malloc(size);
    if (!dst) return NULL;
    size_t s = 16, d = 0;
    uint8_t code = 0;
    int bits = 0;
    while (d < size) {
        if (!bits) {
            if (s >= n) break;
            code = src[s++], bits = 8;
        }
        if (code & 0x80) {  // a literal byte
            if (s >= n) break;
            dst[d++] = src[s++];
        } else {  // a back reference: distance (12 bits) + length (4 bits, or a third byte)
            if (s + 2 > n) break;
            size_t b1 = src[s], b2 = src[s + 1];
            s += 2;
            size_t dist = ((b1 & 0xF) << 8 | b2) + 1, len = b1 >> 4;
            if (!len) {
                if (s >= n) break;
                len = (size_t)src[s++] + 0x12;
            } else len += 2;
            if (dist > d) break;
            for (size_t i = 0; i < len && d < size; i++, d++) dst[d] = dst[d - dist];
        }
        code <<= 1, bits--;
    }
    if (d != size) { free(dst); return NULL; }
    *out_n = size;
    return dst;
}
