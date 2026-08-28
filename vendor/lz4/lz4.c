/*
 * LZ4 block-format decompression implementation.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "lz4.h"
#include <stdint.h>
#include <string.h>

#define LZ4_MINMATCH 4

static int LZ4_decompress_block(const uint8_t* src, const uint8_t* srcEnd,
                                 uint8_t* dst, uint8_t* dstEnd)
{
    const uint8_t* srcPtr = src;
    uint8_t* dstPtr = dst;
    const uint8_t* dstStart = dst;

    while (srcPtr < srcEnd) {
        uint8_t token = *srcPtr++;
        unsigned litLen = token >> 4;

        if (litLen == 0x0F) {
            while (srcPtr < srcEnd && *srcPtr == 0xFF) {
                litLen += 255;
                srcPtr++;
            }
            if (srcPtr >= srcEnd) return -1;
            litLen += *srcPtr++;
        }

        if (litLen > 0) {
            if (srcPtr + litLen > srcEnd) return -1;
            if (dstPtr + litLen > dstEnd) return -1;
            memcpy(dstPtr, srcPtr, litLen);
            srcPtr += litLen;
            dstPtr += litLen;
        }

        if (srcPtr >= srcEnd) break;

        if (srcPtr + 2 > srcEnd) return -1;
        unsigned offset = (unsigned)(srcPtr[0] | (srcPtr[1] << 8));
        srcPtr += 2;
        if (offset == 0 || offset > (unsigned)(dstPtr - dstStart)) return -1;

        unsigned matchLen = token & 0x0F;
        if (matchLen == 0x0F) {
            while (srcPtr < srcEnd && *srcPtr == 0xFF) {
                matchLen += 255;
                srcPtr++;
            }
            if (srcPtr >= srcEnd) return -1;
            matchLen += *srcPtr++;
        }
        matchLen += LZ4_MINMATCH;

        if (dstPtr + matchLen > dstEnd) return -1;

        const uint8_t* matchSrc = dstPtr - offset;
        if (offset >= matchLen) {
            memcpy(dstPtr, matchSrc, matchLen);
            dstPtr += matchLen;
        } else if (offset == 1) {
            memset(dstPtr, matchSrc[0], matchLen);
            dstPtr += matchLen;
        } else {
            while (matchLen--) {
                *dstPtr++ = *matchSrc++;
            }
        }
    }

    return (int)(dstPtr - dstStart);
}

int LZ4_decompress_safe(const char* src, char* dst, int srcSize, int dstCapacity)
{
    if (srcSize <= 0 || dstCapacity < 0) return -1;
    uint8_t* dstBuf = (uint8_t*)dst;
    return LZ4_decompress_block((const uint8_t*)src, (const uint8_t*)src + srcSize,
                                dstBuf, dstBuf + dstCapacity);
}
