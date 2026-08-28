/*
 * Compact LZ4 block-format decompressor (decompression only).
 *
 * Implements the LZ4 Block Format Specification:
 * https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md
 *
 * This is a minimal, self-contained decoder sufficient for reading
 * VTK XML files compressed with the "LZ4" compressor. It does not
 * support the LZ4 Frame format (lz4frame.h); VTK uses raw blocks.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef LZ4_H
#define LZ4_H

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Decompresses an LZ4 compressed block.
 *
 * src         : pointer to compressed data (block payload, excluding the
 *               preceding [uint32 compressedSize] header that VTK adds per block)
 * srcSize     : size of compressed data
 * dst         : output buffer (must be pre-allocated, size >= dstCapacity)
 * dstCapacity : size of output buffer
 *
 * Returns: number of bytes written to dst, or -1 on error.
 */
int LZ4_decompress_safe(const char* src, char* dst, int srcSize, int dstCapacity);

#if defined(__cplusplus)
}
#endif

#endif /* LZ4_H */
