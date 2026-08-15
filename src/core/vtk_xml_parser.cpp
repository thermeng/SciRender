#include "core/mesh_loader.h"
#include "pugixml.hpp"
#include <lz4.h>
#include <LzmaDec.h>
#include <7zAlloc.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>
#include <cctype>
#include <cstdint>
#include <zlib.h>

#ifndef VTK_XML_VERBOSE
#define VTK_XML_VERBOSE 0
#endif

#define VTK_XML_WARN(msg) do { if (VTK_XML_VERBOSE) std::cerr << "VTK XML Parser: " << msg << std::endl; } while(0)

// ============================================================================
// Compression format support
// ============================================================================

enum class Compression {
    None = 0,
    Zlib,
    Lz4,
    Lzma
};

static Compression detectCompression(const std::string& compressorStr) {
    std::string upper = mesh_utils::toUpper(compressorStr);
    if (upper.empty()) return Compression::None;
    if (upper.find("ZLIB") != std::string::npos) return Compression::Zlib;
    if (upper.find("LZ4") != std::string::npos) return Compression::Lz4;
    if (upper.find("LZMA") != std::string::npos) return Compression::Lzma;
    return Compression::None;
}

// VTK's compressed block format (standard VTK XML):
// [block_count: header_type][per_block: uncompressed_size: header_type, compressed_size: header_type, compressed_data]
// Each block is independently decompressible.

static bool decompressZlib(const char* src, size_t srcLen, size_t& offset,
                           uint32_t numBlocks, bool swapHeader, bool header64Flag,
                           std::vector<char>& out) {
    out.clear();

    for (uint32_t block = 0; block < numBlocks; ++block) {
        if (header64Flag) {
            if (offset + 2 * sizeof(uint64_t) > srcLen) break;
            uint64_t uncomp = 0, comp = 0;
            std::memcpy(&uncomp, src + offset, sizeof(uint64_t));
            std::memcpy(&comp, src + offset + sizeof(uint64_t), sizeof(uint64_t));
            if (swapHeader) { mesh_utils::byteSwap(&uncomp); mesh_utils::byteSwap(&comp); }
            offset += 2 * sizeof(uint64_t);
            if (comp == 0 || offset + comp > srcLen) break;

            z_stream strm;
            std::memset(&strm, 0, sizeof(strm));
            if (inflateInit(&strm) != Z_OK) break;
            strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(src + offset));
            strm.avail_in = static_cast<uInt>(comp);
            char scratch[65536];
            int ret = Z_OK;
            while (ret != Z_STREAM_END) {
                strm.next_out = reinterpret_cast<Bytef*>(scratch);
                strm.avail_out = sizeof(scratch);
                ret = inflate(&strm, Z_NO_FLUSH);
                if (ret != Z_OK && ret != Z_STREAM_END) break;
                size_t produced = sizeof(scratch) - strm.avail_out;
                out.insert(out.end(), scratch, scratch + produced);
                if (ret == Z_STREAM_END) break;
                if (strm.avail_out != 0) { ret = Z_BUF_ERROR; break; }
            }
            inflateEnd(&strm);
            offset += comp;
            if (ret != Z_STREAM_END) break;
        } else {
            if (offset + 2 * sizeof(uint32_t) > srcLen) break;
            uint32_t uncomp = 0, comp = 0;
            std::memcpy(&uncomp, src + offset, sizeof(uint32_t));
            std::memcpy(&comp, src + offset + sizeof(uint32_t), sizeof(uint32_t));
            if (swapHeader) { mesh_utils::byteSwap(&uncomp); mesh_utils::byteSwap(&comp); }
            offset += 2 * sizeof(uint32_t);
            if (comp == 0 || offset + comp > srcLen) break;

            z_stream strm;
            std::memset(&strm, 0, sizeof(strm));
            if (inflateInit(&strm) != Z_OK) break;
            strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(src + offset));
            strm.avail_in = comp;
            char scratch[65536];
            int ret = Z_OK;
            while (ret != Z_STREAM_END) {
                strm.next_out = reinterpret_cast<Bytef*>(scratch);
                strm.avail_out = sizeof(scratch);
                ret = inflate(&strm, Z_NO_FLUSH);
                if (ret != Z_OK && ret != Z_STREAM_END) break;
                size_t produced = sizeof(scratch) - strm.avail_out;
                out.insert(out.end(), scratch, scratch + produced);
                if (ret == Z_STREAM_END) break;
                if (strm.avail_out != 0) { ret = Z_BUF_ERROR; break; }
            }
            inflateEnd(&strm);
            offset += comp;
            if (ret != Z_STREAM_END) break;
        }
    }
    return true;
}

static bool decompressLz4(const char* src, size_t srcLen, size_t& offset,
                          uint32_t numBlocks, bool swapHeader, bool header64Flag,
                          std::vector<char>& out) {
    out.clear();
    size_t lz4Block = 128 * 1024;  // VTK uses 128KB blocks

    for (uint32_t block = 0; block < numBlocks; ++block) {
        if (header64Flag) {
            if (offset + 2 * sizeof(uint64_t) > srcLen) break;
            uint64_t uncomp = 0, comp = 0;
            std::memcpy(&uncomp, src + offset, sizeof(uint64_t));
            std::memcpy(&comp, src + offset + sizeof(uint64_t), sizeof(uint64_t));
            if (swapHeader) { mesh_utils::byteSwap(&uncomp); mesh_utils::byteSwap(&comp); }
            offset += 2 * sizeof(uint64_t);
            if (comp == 0 || offset + comp > srcLen) break;

            size_t remaining = static_cast<size_t>(uncomp);
            size_t blockSize = (remaining > lz4Block) ? lz4Block : remaining;
            size_t base = out.size();
            out.resize(base + blockSize);
            char* dstPtr = out.data() + base;
            int decoded = LZ4_decompress_safe(src + offset, dstPtr, static_cast<int>(comp), static_cast<int>(blockSize));
            if (decoded < 0) {
                out.resize(out.size() - blockSize);
                VTK_XML_WARN("LZ4 decompression failed at block " << block);
                break;
            }
            out.resize(out.size() - blockSize + decoded);
            offset += comp;
        } else {
            if (offset + 2 * sizeof(uint32_t) > srcLen) break;
            uint32_t uncomp = 0, comp = 0;
            std::memcpy(&uncomp, src + offset, sizeof(uint32_t));
            std::memcpy(&comp, src + offset + sizeof(uint32_t), sizeof(uint32_t));
            if (swapHeader) { mesh_utils::byteSwap(&uncomp); mesh_utils::byteSwap(&comp); }
            offset += 2 * sizeof(uint32_t);
            if (comp == 0 || offset + comp > srcLen) break;

            size_t remaining = static_cast<size_t>(uncomp);
            size_t blockSize = (remaining > lz4Block) ? lz4Block : remaining;
            size_t base = out.size();
            out.resize(base + blockSize);
            char* dstPtr = out.data() + base;
            int decoded = LZ4_decompress_safe(src + offset, dstPtr, static_cast<int>(comp), static_cast<int>(blockSize));
            if (decoded < 0) {
                out.resize(out.size() - blockSize);
                VTK_XML_WARN("LZ4 decompression failed at block " << block);
                break;
            }
            out.resize(out.size() - blockSize + decoded);
            offset += comp;
        }
    }
    return true;
}

static bool decompressLzma(const char* src, size_t srcLen, size_t& offset,
                           uint32_t numBlocks, bool swapHeader, bool header64Flag,
                           std::vector<char>& out) {
    out.clear();

    for (uint32_t block = 0; block < numBlocks; ++block) {
        if (header64Flag) {
            if (offset + 2 * sizeof(uint64_t) > srcLen) break;
            uint64_t uncomp = 0, comp = 0;
            std::memcpy(&uncomp, src + offset, sizeof(uint64_t));
            std::memcpy(&comp, src + offset + sizeof(uint64_t), sizeof(uint64_t));
            if (swapHeader) { mesh_utils::byteSwap(&uncomp); mesh_utils::byteSwap(&comp); }
            offset += 2 * sizeof(uint64_t);
            if (comp == 0 || offset + comp > srcLen) break;
            if (comp < LZMA_PROPS_SIZE) {
                VTK_XML_WARN("LZMA block too small for props at block " << block);
                break;
            }

            const Byte* props = reinterpret_cast<const Byte*>(src + offset);
            const Byte* lzmaSrc = reinterpret_cast<const Byte*>(src + offset + LZMA_PROPS_SIZE);
            size_t lzmaSrcLen = static_cast<size_t>(comp) - LZMA_PROPS_SIZE;
            size_t destLen = static_cast<size_t>(uncomp);

            std::vector<char> blockOut(destLen);
            ELzmaStatus status;
            size_t srcLen2 = lzmaSrcLen;
            size_t destLen2 = destLen;
            static ISzAlloc g_Alloc = { SzAlloc, SzFree };
            SRes res = LzmaDecode(reinterpret_cast<Byte*>(blockOut.data()), &destLen2,
                                  lzmaSrc, &srcLen2, props, LZMA_PROPS_SIZE,
                                  LZMA_FINISH_ANY, &status, &g_Alloc);
            if (res != SZ_OK) {
                VTK_XML_WARN("LZMA decompression failed: res=" << res << " status=" << status);
                break;
            }
            out.insert(out.end(), blockOut.data(), blockOut.data() + destLen2);
            offset += comp;
        } else {
            if (offset + 2 * sizeof(uint32_t) > srcLen) break;
            uint32_t uncomp = 0, comp = 0;
            std::memcpy(&uncomp, src + offset, sizeof(uint32_t));
            std::memcpy(&comp, src + offset + sizeof(uint32_t), sizeof(uint32_t));
            if (swapHeader) { mesh_utils::byteSwap(&uncomp); mesh_utils::byteSwap(&comp); }
            offset += 2 * sizeof(uint32_t);
            if (comp == 0 || offset + comp > srcLen) break;
            if (comp < LZMA_PROPS_SIZE) {
                VTK_XML_WARN("LZMA block too small for props at block " << block);
                break;
            }

            const Byte* props = reinterpret_cast<const Byte*>(src + offset);
            const Byte* lzmaSrc = reinterpret_cast<const Byte*>(src + offset + LZMA_PROPS_SIZE);
            size_t lzmaSrcLen = static_cast<size_t>(comp) - LZMA_PROPS_SIZE;
            size_t destLen = static_cast<size_t>(uncomp);

            std::vector<char> blockOut(destLen);
            ELzmaStatus status;
            size_t srcLen2 = lzmaSrcLen;
            size_t destLen2 = destLen;
            static ISzAlloc g_Alloc = { SzAlloc, SzFree };
            SRes res = LzmaDecode(reinterpret_cast<Byte*>(blockOut.data()), &destLen2,
                                  lzmaSrc, &srcLen2, props, LZMA_PROPS_SIZE,
                                  LZMA_FINISH_ANY, &status, &g_Alloc);
            if (res != SZ_OK) {
                VTK_XML_WARN("LZMA decompression failed: res=" << res << " status=" << status);
                break;
            }
            out.insert(out.end(), blockOut.data(), blockOut.data() + destLen2);
            offset += comp;
        }
    }
    return true;
}

static bool decompressBlocks(const char* src, size_t srcLen, size_t& offset,
                             uint32_t numBlocks, bool swapHeader, bool header64Flag,
                             Compression comp, std::vector<char>& out) {
    switch (comp) {
    case Compression::Zlib: return decompressZlib(src, srcLen, offset, numBlocks, swapHeader, header64Flag, out);
    case Compression::Lz4:  return decompressLz4(src, srcLen, offset, numBlocks, swapHeader, header64Flag, out);
    case Compression::Lzma: return decompressLzma(src, srcLen, offset, numBlocks, swapHeader, header64Flag, out);
    default: return false;
    }
}

// ============================================================================
// Base64 decode
// ============================================================================

static const char* base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_char_to_value(char c) {
    const char* p = std::strchr(base64_chars, c);
    return p ? int(p - base64_chars) : -1;
}

static void decodeBase64(const std::string& input, std::vector<char>& out) {
    out.clear();
    out.reserve(input.size() * 3 / 4);
    std::vector<int> tmp;
    tmp.reserve(input.size());
    for (char c : input) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t' || c == '_') continue;
        if (c == '=') { tmp.push_back(-1); continue; }
        int val = base64_char_to_value(c);
        if (val >= 0) tmp.push_back(val);
    }
    for (size_t i = 0; i + 1 < tmp.size(); i += 4) {
        int a = tmp[i], b = (i + 1 < tmp.size()) ? tmp[i + 1] : 0;
        int c = (i + 2 < tmp.size()) ? tmp[i + 2] : 0;
        int d = (i + 3 < tmp.size()) ? tmp[i + 3] : 0;
        if (a < 0 || b < 0) break;
        out.push_back(char((a << 2) | (b >> 4)));
        if (c >= 0) out.push_back(char((b << 4) | (c >> 2)));
        if (d >= 0) out.push_back(char((c << 6) | d));
    }
}

// ============================================================================
// VTK XML data type dispatch
// ============================================================================

enum VtkType {
    VTK_TYPE_UNDEFINED,
    VTK_TYPE_INT8, VTK_TYPE_UINT8,
    VTK_TYPE_INT16, VTK_TYPE_UINT16,
    VTK_TYPE_INT32, VTK_TYPE_UINT32,
    VTK_TYPE_INT64, VTK_TYPE_UINT64,
    VTK_TYPE_FLOAT32, VTK_TYPE_FLOAT64
};

static VtkType vtkTypeFromString(const std::string& upperType) {
    if (upperType == "INT8") return VTK_TYPE_INT8;
    if (upperType == "UINT8") return VTK_TYPE_UINT8;
    if (upperType == "INT16") return VTK_TYPE_INT16;
    if (upperType == "UINT16") return VTK_TYPE_UINT16;
    if (upperType == "INT32") return VTK_TYPE_INT32;
    if (upperType == "UINT32") return VTK_TYPE_UINT32;
    if (upperType == "INT64") return VTK_TYPE_INT64;
    if (upperType == "UINT64") return VTK_TYPE_UINT64;
    if (upperType == "FLOAT32") return VTK_TYPE_FLOAT32;
    if (upperType == "FLOAT64") return VTK_TYPE_FLOAT64;
    return VTK_TYPE_UNDEFINED;
}

static int vtkTypeSize(VtkType t) {
    switch (t) {
    case VTK_TYPE_INT8: case VTK_TYPE_UINT8: return 1;
    case VTK_TYPE_INT16: case VTK_TYPE_UINT16: return 2;
    case VTK_TYPE_INT32: case VTK_TYPE_UINT32: case VTK_TYPE_FLOAT32: return 4;
    case VTK_TYPE_INT64: case VTK_TYPE_UINT64: case VTK_TYPE_FLOAT64: return 8;
    default: return 4; // conservative fallback
    }
}

// Convert a raw, correctly-typed byte buffer into floats for the renderer.
template<typename T>
static void appendTypedAsFloat(const char* p, size_t count, bool swap, std::vector<float>& out) {
    out.reserve(out.size() + count);
    for (size_t i = 0; i < count; ++i) {
        T v;
        std::memcpy(&v, p + i * sizeof(T), sizeof(T));
        if constexpr (sizeof(T) > 1) {
            if (swap) mesh_utils::byteSwap(&v);
        }
        out.push_back(static_cast<float>(v));
    }
}

static void convertBytesToFloat(const std::vector<char>& bytes, VtkType t, bool swap, std::vector<float>& out) {
    const char* p = bytes.data();
    size_t count = t != VTK_TYPE_UNDEFINED ? bytes.size() / static_cast<size_t>(vtkTypeSize(t)) : 0;
    switch (t) {
    case VTK_TYPE_INT8:   appendTypedAsFloat<int8_t>(p, count, swap, out); break;
    case VTK_TYPE_UINT8:  appendTypedAsFloat<uint8_t>(p, count, swap, out); break;
    case VTK_TYPE_INT16:  appendTypedAsFloat<int16_t>(p, count, swap, out); break;
    case VTK_TYPE_UINT16: appendTypedAsFloat<uint16_t>(p, count, swap, out); break;
    case VTK_TYPE_INT32:  appendTypedAsFloat<int32_t>(p, count, swap, out); break;
    case VTK_TYPE_UINT32: appendTypedAsFloat<uint32_t>(p, count, swap, out); break;
    case VTK_TYPE_INT64:  appendTypedAsFloat<int64_t>(p, count, swap, out); break;
    case VTK_TYPE_UINT64: appendTypedAsFloat<uint64_t>(p, count, swap, out); break;
    case VTK_TYPE_FLOAT32: appendTypedAsFloat<float>(p, count, swap, out); break;
    case VTK_TYPE_FLOAT64: appendTypedAsFloat<double>(p, count, swap, out); break;
    default: break;
    }
}

// Convert a raw, correctly-typed byte buffer into int32 for topology arrays.
template<typename T>
static void appendTypedAsInt32(const char* p, size_t count, bool swap, std::vector<int32_t>& out) {
    out.reserve(out.size() + count);
    for (size_t i = 0; i < count; ++i) {
        T v;
        std::memcpy(&v, p + i * sizeof(T), sizeof(T));
        if constexpr (sizeof(T) > 1) {
            if (swap) mesh_utils::byteSwap(&v);
        }
        out.push_back(static_cast<int32_t>(v));
    }
}

static void convertBytesToInt32(const std::vector<char>& bytes, VtkType t, bool swap, std::vector<int32_t>& out) {
    const char* p = bytes.data();
    size_t count = t != VTK_TYPE_UNDEFINED ? bytes.size() / static_cast<size_t>(vtkTypeSize(t)) : 0;
    switch (t) {
    case VTK_TYPE_INT8:   appendTypedAsInt32<int8_t>(p, count, swap, out); break;
    case VTK_TYPE_UINT8:  appendTypedAsInt32<uint8_t>(p, count, swap, out); break;
    case VTK_TYPE_INT16:  appendTypedAsInt32<int16_t>(p, count, swap, out); break;
    case VTK_TYPE_UINT16: appendTypedAsInt32<uint16_t>(p, count, swap, out); break;
    case VTK_TYPE_INT32:  appendTypedAsInt32<int32_t>(p, count, swap, out); break;
    case VTK_TYPE_UINT32: appendTypedAsInt32<uint32_t>(p, count, swap, out); break;
    case VTK_TYPE_INT64:  appendTypedAsInt32<int64_t>(p, count, swap, out); break;
    case VTK_TYPE_UINT64: appendTypedAsInt32<uint64_t>(p, count, swap, out); break;
    case VTK_TYPE_FLOAT32: appendTypedAsInt32<float>(p, count, swap, out); break;
    case VTK_TYPE_FLOAT64: appendTypedAsInt32<double>(p, count, swap, out); break;
    default: break;
    }
}

template<typename T>
static void appendTypedAsInt64(const char* p, size_t count, bool swap, std::vector<int64_t>& out) {
    out.reserve(out.size() + count);
    for (size_t i = 0; i < count; ++i) {
        T v;
        std::memcpy(&v, p + i * sizeof(T), sizeof(T));
        if constexpr (sizeof(T) > 1) {
            if (swap) mesh_utils::byteSwap(&v);
        }
        out.push_back(static_cast<int64_t>(v));
    }
}

static void convertBytesToInt64(const std::vector<char>& bytes, VtkType t, bool swap, std::vector<int64_t>& out) {
    const char* p = bytes.data();
    size_t count = t != VTK_TYPE_UNDEFINED ? bytes.size() / static_cast<size_t>(vtkTypeSize(t)) : 0;
    switch (t) {
    case VTK_TYPE_INT8:   appendTypedAsInt64<int8_t>(p, count, swap, out); break;
    case VTK_TYPE_UINT8:  appendTypedAsInt64<uint8_t>(p, count, swap, out); break;
    case VTK_TYPE_INT16:  appendTypedAsInt64<int16_t>(p, count, swap, out); break;
    case VTK_TYPE_UINT16: appendTypedAsInt64<uint16_t>(p, count, swap, out); break;
    case VTK_TYPE_INT32:  appendTypedAsInt64<int32_t>(p, count, swap, out); break;
    case VTK_TYPE_UINT32: appendTypedAsInt64<uint32_t>(p, count, swap, out); break;
    case VTK_TYPE_INT64:  appendTypedAsInt64<int64_t>(p, count, swap, out); break;
    case VTK_TYPE_UINT64: appendTypedAsInt64<uint64_t>(p, count, swap, out); break;
    case VTK_TYPE_FLOAT32: appendTypedAsInt64<float>(p, count, swap, out); break;
    case VTK_TYPE_FLOAT64: appendTypedAsInt64<double>(p, count, swap, out); break;
    default: break;
    }
}

// ============================================================================
// Topology helpers (standalone versions of legacy parser internals)
// ============================================================================

static void generateStructuredGridSurface(RenderMesh& mesh, int dX, int dY, int dZ) {
    std::vector<std::vector<uint32_t>> cellToVertices;
    auto idx = [&](int x, int y, int z) { return x + y * dX + z * dX * dY; };
    auto addQuad = [&](int a, int b, int c, int d) {
        mesh.indices.push_back(static_cast<uint32_t>(a));
        mesh.indices.push_back(static_cast<uint32_t>(b));
        mesh.indices.push_back(static_cast<uint32_t>(c));
        mesh.indices.push_back(static_cast<uint32_t>(a));
        mesh.indices.push_back(static_cast<uint32_t>(c));
        mesh.indices.push_back(static_cast<uint32_t>(d));
        cellToVertices.push_back({
            static_cast<uint32_t>(a), static_cast<uint32_t>(b),
            static_cast<uint32_t>(c), static_cast<uint32_t>(d)
        });
    };

    int cx = std::max(1, dX - 1);
    int cy = std::max(1, dY - 1);
    int cz = std::max(1, dZ - 1);

    const bool is3D = (dX > 1 && dY > 1 && dZ > 1);
    if (is3D) {
        for (int y = 0; y < cy; ++y)
            for (int x = 0; x < cx; ++x)
                addQuad(idx(x, y, 0), idx(x, y + 1, 0), idx(x + 1, y + 1, 0), idx(x + 1, y, 0));
        for (int y = 0; y < cy; ++y)
            for (int x = 0; x < cx; ++x)
                addQuad(idx(x, y, dZ - 1), idx(x + 1, y, dZ - 1), idx(x + 1, y + 1, dZ - 1), idx(x, y + 1, dZ - 1));
        for (int z = 0; z < cz; ++z)
            for (int y = 0; y < cy; ++y)
                addQuad(idx(0, y, z), idx(0, y, z + 1), idx(0, y + 1, z + 1), idx(0, y + 1, z));
        for (int z = 0; z < cz; ++z)
            for (int y = 0; y < cy; ++y)
                addQuad(idx(dX - 1, y, z), idx(dX - 1, y + 1, z), idx(dX - 1, y + 1, z + 1), idx(dX - 1, y, z + 1));
        for (int z = 0; z < cz; ++z)
            for (int x = 0; x < cx; ++x)
                addQuad(idx(x, 0, z), idx(x + 1, 0, z), idx(x + 1, 0, z + 1), idx(x, 0, z + 1));
        for (int z = 0; z < cz; ++z)
            for (int x = 0; x < cx; ++x)
                addQuad(idx(x, dY - 1, z), idx(x, dY - 1, z + 1), idx(x + 1, dY - 1, z + 1), idx(x + 1, dY - 1, z));
    } else if (dZ == 1) {
        for (int y = 0; y + 1 < dY; ++y)
            for (int x = 0; x + 1 < dX; ++x)
                addQuad(idx(x, y, 0), idx(x + 1, y, 0), idx(x + 1, y + 1, 0), idx(x, y + 1, 0));
    } else if (dY == 1) {
        for (int z = 0; z + 1 < dZ; ++z)
            for (int x = 0; x + 1 < dX; ++x)
                addQuad(idx(x, 0, z), idx(x + 1, 0, z), idx(x + 1, 0, z + 1), idx(x, 0, z + 1));
    } else if (dX == 1) {
        for (int z = 0; z + 1 < dZ; ++z)
            for (int y = 0; y + 1 < dY; ++y)
                addQuad(idx(0, y, z), idx(0, y + 1, z), idx(0, y + 1, z + 1), idx(0, y, z + 1));
    }
    (void)cellToVertices;
}

static std::vector<std::vector<uint32_t>> triangulatePolygons(RenderMesh& mesh,
                                                              const std::vector<int64_t>& rawPolygonData,
                                                              int numPolys) {
    int idx = 0;
    std::vector<std::vector<uint32_t>> cellToVertices(numPolys);
    for (int p = 0; p < numPolys; ++p) {
        if (idx >= static_cast<int>(rawPolygonData.size())) break;
        int nPoints = rawPolygonData[idx++];
        if (nPoints < 0 || idx + nPoints > static_cast<int>(rawPolygonData.size())) break;
        for (int i = 0; i < nPoints; ++i) {
            cellToVertices[p].push_back(static_cast<uint32_t>(rawPolygonData[idx + i]));
        }
        for (int i = 1; i < nPoints - 1; ++i) {
            mesh.indices.push_back(static_cast<uint32_t>(rawPolygonData[idx + 0]));
            mesh.indices.push_back(static_cast<uint32_t>(rawPolygonData[idx + i]));
            mesh.indices.push_back(static_cast<uint32_t>(rawPolygonData[idx + i + 1]));
        }
        idx += nPoints;
    }
    return cellToVertices;
}

static std::vector<std::vector<uint32_t>> triangulateTriangleStrips(RenderMesh& mesh,
                                                                     const std::vector<int64_t>& rawStripData,
                                                                     int numStrips) {
    std::vector<std::vector<uint32_t>> cellToVertices;
    cellToVertices.reserve(numStrips > 0 ? numStrips * 3 : 0);
    int idx = 0;
    for (int s = 0; s < numStrips; ++s) {
        if (idx >= static_cast<int>(rawStripData.size())) break;
        int nPoints = rawStripData[idx++];
        if (nPoints < 0 || idx + nPoints > static_cast<int>(rawStripData.size())) break;
        for (int i = 0; i < nPoints - 2; ++i) {
            uint32_t i0 = static_cast<uint32_t>(rawStripData[idx + i]);
            uint32_t i1 = static_cast<uint32_t>(rawStripData[idx + i + 1]);
            uint32_t i2 = static_cast<uint32_t>(rawStripData[idx + i + 2]);
            cellToVertices.push_back({ i0, i1, i2 });
            if (i % 2 == 0) mesh.indices.insert(mesh.indices.end(), { i0, i1, i2 });
            else mesh.indices.insert(mesh.indices.end(), { i0, i2, i1 });
        }
        idx += nPoints;
    }
    return cellToVertices;
}

static std::vector<std::vector<uint32_t>> triangulateLines(RenderMesh& mesh,
                                                            const std::vector<int64_t>& rawLineData,
                                                            int numLines) {
    int idx = 0;
    std::vector<std::vector<uint32_t>> cellToVertices(numLines);
    for (int l = 0; l < numLines; ++l) {
        if (idx >= static_cast<int>(rawLineData.size())) break;
        int nPoints = rawLineData[idx++];
        if (nPoints < 0 || idx + nPoints > static_cast<int>(rawLineData.size())) break;
        for (int i = 0; i < nPoints; ++i) {
            cellToVertices[l].push_back(static_cast<uint32_t>(rawLineData[idx + i]));
        }
        for (int i = 0; i + 1 < nPoints; ++i) {
            uint32_t a = static_cast<uint32_t>(rawLineData[idx + i]);
            uint32_t b = static_cast<uint32_t>(rawLineData[idx + i + 1]);
            mesh.indices.insert(mesh.indices.end(), { a, b });
        }
        idx += nPoints;
    }
    return cellToVertices;
}

static std::vector<std::vector<uint32_t>> triangulateUnstructuredCells(
    RenderMesh& mesh,
    const std::vector<int64_t>& rawCellData,
    const std::vector<int64_t>& cellTypes,
    int totalCells) {
    mesh.indices.clear();
    std::vector<std::vector<uint32_t>> cellToVertices(totalCells);
    int idx = 0;
    for (int c = 0; c < totalCells; ++c) {
        if (idx >= static_cast<int>(rawCellData.size())) break;
        int numPointsInCell = rawCellData[idx++];
        if (numPointsInCell < 0 || idx + numPointsInCell > static_cast<int>(rawCellData.size())) break;
        for (int i = 0; i < numPointsInCell; ++i) {
            cellToVertices[c].push_back(static_cast<uint32_t>(rawCellData[idx + i]));
        }
        int type = (c < static_cast<int>(cellTypes.size())) ? cellTypes[c] : 0;
        if (type == 0) {
            if (numPointsInCell == 3) type = 5;
            if (numPointsInCell == 4) type = 9;
            if (numPointsInCell == 8) type = 12;
        }
        switch (type) {
        case 1:  // VTK_VERTEX
            break;
        case 2:  // VTK_POLY_VERTEX
            break;
        case 3:  // VTK_LINE
            break;
        case 4:  // VTK_POLY_LINE
            break;
        case 5:
            if (idx + 2 < static_cast<int>(rawCellData.size())) {
                mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 0]));
                mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 1]));
                mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 2]));
            }
            break;
        case 6: {  // VTK_TRIANGLE_STRIP
            for (int i = 0; i + 2 < numPointsInCell; ++i) {
                if (idx + i + 2 < static_cast<int>(rawCellData.size())) {
                    if (i & 1) {
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 1]));
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i]));
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 2]));
                    } else {
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i]));
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 1]));
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 2]));
                    }
                }
            }
            break;
        }
        case 7: {  // VTK_POLYGON (triangle fan)
            for (int i = 1; i < numPointsInCell - 1; ++i) {
                if (idx + i + 1 < static_cast<int>(rawCellData.size())) {
                    mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 0]));
                    mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i]));
                    mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 1]));
                }
            }
            break;
        }
        case 8: {  // VTK_PIXEL (rectangle)
            // VTK_PIXEL ordering: i0=BL, i1=BR, i2=TL, i3=TR
            if (idx + 3 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]);
                uint32_t i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                mesh.indices.insert(mesh.indices.end(), { i0, i2, i1, i0, i1, i3 });
            }
            break;
        }
        case 9:
            if (idx + 3 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]);
                uint32_t i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                mesh.indices.insert(mesh.indices.end(), { i0, i1, i2, i0, i2, i3 });
            }
            break;
        case 14:
            if (idx + 4 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]);
                uint32_t i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                uint32_t i4 = static_cast<uint32_t>(rawCellData[idx + 4]);
                mesh.indices.insert(mesh.indices.end(), {
                    i0, i2, i1, i0, i3, i2,
                    i0, i1, i4, i1, i2, i4, i2, i3, i4, i3, i0, i4
                });
            }
            break;
        case 15: {  // VTK_WEDGE (triangular prism, 6 vertices)
            if (idx + 5 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]);
                uint32_t i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                uint32_t i4 = static_cast<uint32_t>(rawCellData[idx + 4]);
                uint32_t i5 = static_cast<uint32_t>(rawCellData[idx + 5]);
                mesh.indices.insert(mesh.indices.end(), {
                    i0, i2, i1,
                    i3, i4, i5,
                    i0, i1, i3,
                    i1, i4, i3,
                    i1, i2, i4,
                    i2, i5, i4,
                    i0, i3, i2,
                    i2, i3, i5,
                    i0, i5, i2
                });
            }
            break;
        }
        case 10:  // VTK_TETRA
            if (idx + 3 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]);
                uint32_t i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                mesh.indices.insert(mesh.indices.end(), {
                    i0, i2, i1,
                    i0, i1, i3,
                    i1, i2, i3,
                    i0, i3, i2
                });
            }
            break;
        case 11: {  // VTK_VOXEL (8 vertices, reordered to hex)
            if (idx + 7 < static_cast<int>(rawCellData.size())) {
                uint32_t h0 = static_cast<uint32_t>(rawCellData[idx + 0]);
                uint32_t h1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t h2 = static_cast<uint32_t>(rawCellData[idx + 3]);
                uint32_t h3 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t h4 = static_cast<uint32_t>(rawCellData[idx + 4]);
                uint32_t h5 = static_cast<uint32_t>(rawCellData[idx + 5]);
                uint32_t h6 = static_cast<uint32_t>(rawCellData[idx + 7]);
                uint32_t h7 = static_cast<uint32_t>(rawCellData[idx + 6]);
                cellToVertices[c] = {h0, h1, h2, h3, h4, h5, h6, h7};
                mesh.indices.insert(mesh.indices.end(), {
                    h0, h3, h1, h1, h3, h2, h4, h5, h7, h5, h6, h7,
                    h0, h1, h4, h1, h5, h4, h2, h3, h6, h3, h7, h6,
                    h0, h4, h3, h3, h4, h7, h1, h2, h5, h2, h6, h5
                });
            }
            break;
        }
        case 12:  // VTK_HEXAHEDRON (8 vertices, in order)
            if (idx + 7 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]);
                uint32_t i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                uint32_t i4 = static_cast<uint32_t>(rawCellData[idx + 4]);
                uint32_t i5 = static_cast<uint32_t>(rawCellData[idx + 5]);
                uint32_t i6 = static_cast<uint32_t>(rawCellData[idx + 6]);
                uint32_t i7 = static_cast<uint32_t>(rawCellData[idx + 7]);
                mesh.indices.insert(mesh.indices.end(), {
                    i0, i3, i1, i1, i3, i2, i4, i5, i7, i5, i6, i7,
                    i0, i1, i4, i1, i5, i4, i2, i3, i6, i3, i7, i6,
                    i0, i4, i3, i3, i4, i7, i1, i2, i5, i2, i6, i5
                });
            }
            break;
        case 13:  // VTK_PENTAGONAL_PRISM (10 vertices)
            if (idx + 9 < static_cast<int>(rawCellData.size())) {
                uint32_t v0 = static_cast<uint32_t>(rawCellData[idx + 0]);
                uint32_t v1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t v2 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t v3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                uint32_t v4 = static_cast<uint32_t>(rawCellData[idx + 4]);
                uint32_t v5 = static_cast<uint32_t>(rawCellData[idx + 5]);
                uint32_t v6 = static_cast<uint32_t>(rawCellData[idx + 6]);
                uint32_t v7 = static_cast<uint32_t>(rawCellData[idx + 7]);
                uint32_t v8 = static_cast<uint32_t>(rawCellData[idx + 8]);
                uint32_t v9 = static_cast<uint32_t>(rawCellData[idx + 9]);
                cellToVertices[c] = {v0, v1, v2, v3, v4, v5, v6, v7, v8, v9};
                mesh.indices.insert(mesh.indices.end(), {
                    // top cap (fan from v5)
                    v5, v7, v6, v5, v8, v7, v5, v9, v8,
                    // bottom cap (fan from v4, reversed winding)
                    v4, v2, v3, v4, v3, v1, v4, v1, v0,
                    // side rectangles split into triangles
                    v0, v1, v6, v0, v6, v5,
                    v1, v2, v7, v1, v7, v6,
                    v2, v3, v8, v2, v8, v7,
                    v3, v4, v9, v3, v9, v8,
                    v0, v5, v9, v0, v9, v4
                });
            }
            break;
        default:
            if (type >= 21) {
                VTK_XML_WARN("Skipping unsupported higher-order cell type " << type
                            << " (requires subdivision/refinement)");
            } else {
                for (int i = 1; i < numPointsInCell - 1; ++i) {
                    if (idx + i + 1 < static_cast<int>(rawCellData.size())) {
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 0]));
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i]));
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 1]));
                    }
                }
            }
            break;
        }
    }
    return cellToVertices;
}

static void extrapolateCellDataToPointsMerged(
    RenderMesh& mesh,
    const std::vector<std::vector<uint32_t>>& globalCellToVertices,
    const std::unordered_map<std::string, std::vector<float>>& cellScalarsStorage,
    const std::unordered_map<std::string, std::vector<float>>& cellVectorsStorage) {
    if (globalCellToVertices.empty()) return;
    if (cellScalarsStorage.empty() && cellVectorsStorage.empty()) return;

    int vCount = static_cast<int>(mesh.vertices.size() / 3);
    if (vCount == 0) return;
    if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();

    struct FieldAcc { std::vector<float> sum; };
    std::vector<FieldAcc> accs;
    accs.reserve(cellScalarsStorage.size() + cellVectorsStorage.size());
    std::map<std::string, size_t> scalarAccIdx;
    std::map<std::string, size_t> vectorAccIdx;

    for (const auto& [name, raw] : cellScalarsStorage) {
        scalarAccIdx[name] = accs.size();
        accs.push_back(FieldAcc{ std::vector<float>(static_cast<size_t>(vCount), 0.0f) });
    }
    for (const auto& [name, raw] : cellVectorsStorage) {
        vectorAccIdx[name] = accs.size();
        accs.push_back(FieldAcc{ std::vector<float>(static_cast<size_t>(vCount) * 3, 0.0f) });
    }
    std::vector<float> contributionCounts(static_cast<size_t>(vCount), 0.0f);

    for (size_t c = 0; c < globalCellToVertices.size(); ++c) {
        for (const auto& [name, raw] : cellScalarsStorage) {
            if (c >= raw.size()) continue;
            float val = raw[c];
            size_t si = scalarAccIdx[name];
            for (int vIdx : globalCellToVertices[c]) {
                if (vIdx >= 0 && vIdx < vCount) accs[si].sum[vIdx] += val;
            }
        }
        for (const auto& [name, raw] : cellVectorsStorage) {
            if (c >= raw.size() / 3) continue;
            float vx = raw[c * 3 + 0];
            float vy = raw[c * 3 + 1];
            float vz = raw[c * 3 + 2];
            size_t vi = vectorAccIdx[name];
            for (int vIdx : globalCellToVertices[c]) {
                if (vIdx >= 0 && vIdx < vCount) {
                    accs[vi].sum[static_cast<size_t>(vIdx) * 3 + 0] += vx;
                    accs[vi].sum[static_cast<size_t>(vIdx) * 3 + 1] += vy;
                    accs[vi].sum[static_cast<size_t>(vIdx) * 3 + 2] += vz;
                }
            }
        }
        for (int vIdx : globalCellToVertices[c]) {
            if (vIdx >= 0 && vIdx < vCount) contributionCounts[vIdx] += 1.0f;
        }
    }

    for (const auto& [name, raw] : cellScalarsStorage) {
        std::vector<float>& sum = accs[scalarAccIdx[name]].sum;
        for (int i = 0; i < vCount; ++i) {
            if (contributionCounts[i] > 0.0f) sum[i] /= contributionCounts[i];
        }
        mesh.attributes->pointScalars[name] = std::move(sum);
    }
    for (const auto& [name, raw] : cellVectorsStorage) {
        std::vector<float>& sum = accs[vectorAccIdx[name]].sum;
        for (int i = 0; i < vCount; ++i) {
            if (contributionCounts[i] > 0.0f) {
                float inv = 1.0f / contributionCounts[i];
                sum[static_cast<size_t>(i) * 3 + 0] *= inv;
                sum[static_cast<size_t>(i) * 3 + 1] *= inv;
                sum[static_cast<size_t>(i) * 3 + 2] *= inv;
            }
        }
        mesh.attributes->pointVectors[name] = std::move(sum);
    }
}

// ============================================================================
// VTK XML Parser Context
// ============================================================================

class VTKXMLParserContext {
public:
    explicit VTKXMLParserContext(const std::string& path) : filePath(path) {}

    RenderMesh parse() {
        mesh.bounds.centerX = mesh.bounds.centerY = mesh.bounds.centerZ = 0.0;
        mesh.bounds.extent = 1.0;

        // Keep the raw bytes around: raw-appended binary payloads must be
        // sliced byte-wise (pugixml's text value is NUL-terminated and would
        // truncate at the first embedded 0x00).
        std::string xmlBytes = readFileIntoString(filePath);
        if (xmlBytes.empty()) {
            std::cerr << "VTK XML Parser Error: Failed to open or empty file: " << filePath << std::endl;
            return mesh;
        }

        pugi::xml_document doc;
        // Raw-appended payloads would break pugixml; sanitize the parse copy.
        std::string xmlText = xmlBytes;
        sanitizeAppendedPayload(xmlText);
        pugi::xml_parse_result result = doc.load_buffer(xmlText.data(), xmlText.size(),
                                                        pugi::parse_default | pugi::parse_ws_pcdata);
        if (!result) {
            std::cerr << "VTK XML Parser Error: XML parse failed at offset " << result.offset
                      << " (" << result.description() << "): " << filePath << std::endl;
            return mesh;
        }

        pugi::xml_node root = doc.child("VTKFile");
        if (!root) {
            std::cerr << "VTK XML Parser Error: Missing <VTKFile> tag." << std::endl;
            return mesh;
        }

        datasetType = mesh_utils::toUpper(root.attribute("type").value());
        std::string bo = mesh_utils::toUpper(root.attribute("byte_order").value());
        bigEndian = (bo == "BIGENDIAN");
        std::string ht = mesh_utils::toUpper(root.attribute("header_type").value());
        header64 = (ht == "UINT64");
        std::string fileCompressorStr = root.attribute("compressor").value();
        fileCompression = detectCompression(fileCompressorStr);
        fileHasCompression = (fileCompression != Compression::None);

        std::string innerTag;
        if (datasetType == "UNSTRUCTUREDGRID") innerTag = "UnstructuredGrid";
        else if (datasetType == "STRUCTUREDGRID") innerTag = "StructuredGrid";
        else if (datasetType == "IMAGEDATA") innerTag = "ImageData";
        else if (datasetType == "POLYDATA") innerTag = "PolyData";
        else if (datasetType == "RECTILINEARGRID") innerTag = "RectilinearGrid";
        else {
            std::cerr << "VTK XML Parser Error: Unsupported dataset type: " << datasetType << std::endl;
            return mesh;
        }

        pugi::xml_node datasetNode = root.child(innerTag.c_str());
        if (!datasetNode) {
            std::cerr << "VTK XML Parser Error: Missing <" << innerTag << "> tag." << std::endl;
            return mesh;
        }

        // Dataset-level extent/origin/spacing (ImageData / StructuredGrid /
        // RectilinearGrid). Pieces may repeat these; per-piece values win.
        readExtentAttributes(datasetNode);

        // Extract raw-appended binary payload from the original (unsanitized) file bytes.
        extractRawAppended(xmlBytes, appendedData);

        // Root-level FieldData (metadata arrays like Time, etc.)
        if (pugi::xml_node rootFieldData = root.child("FieldData")) {
            if (!mesh.attributes) mesh.attributes = DatasetAttributes();
            for (pugi::xml_node da : rootFieldData.children("DataArray")) {
                DataArrayMeta meta = readDataArrayMeta(da);
                std::vector<float> vals;
                readArrayFloats(da, meta, vals);
                std::string fieldName = meta.name.empty() ? "unnamed" : meta.name;
                mesh.attributes->fieldData[fieldName].insert(
                    mesh.attributes->fieldData[fieldName].end(),
                    vals.begin(), vals.end());
            }
        }

        // Phase 1: discover every named PointData/CellData array across all
        // pieces so multi-piece data can be padded to a consistent global size.
        collectFieldSpecs(datasetNode);

        // Phase 2: process each piece, accumulating geometry + topology +
        // attributes across pieces.
        for (pugi::xml_node piece : datasetNode.children("Piece")) {
            processPiece(piece);
        }

        buildTopology();
        finalizeMeshData();

        mesh.datasetType = datasetType;
        mesh.fileFormat = "VTKXML";
        if (datasetType == "STRUCTUREDGRID" || datasetType == "IMAGEDATA" || datasetType == "RECTILINEARGRID") {
            int dX = 0, dY = 0, dZ = 0;
            if (datasetType == "RECTILINEARGRID") {
                dX = static_cast<int>(rectX.size());
                dY = static_cast<int>(rectY.size());
                dZ = static_cast<int>(rectZ.size());
            } else {
                dX = wholeExtent[1] - wholeExtent[0] + 1;
                dY = wholeExtent[3] - wholeExtent[2] + 1;
                dZ = wholeExtent[5] - wholeExtent[4] + 1;
            }
            if (dX <= 0) dX = 1;
            if (dY <= 0) dY = 1;
            if (dZ <= 0) dZ = 1;
            mesh.gridDimX = dX;
            mesh.gridDimY = dY;
            mesh.gridDimZ = dZ;
        }
        return mesh;
    }

private:
    // ========================================================================
    // Raw file / XML plumbing
    // ========================================================================

    static std::string readFileIntoString(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

     // Extract raw-appended binary payload from the original (unsanitized) file bytes.
    // If the <AppendedData> tag has encoding="base64", the payload text is decoded
    // into binary first.  offset attributes in <DataArray> are relative to the first
    // payload byte (i.e. the start of the decoded binary block).
    static void extractRawAppended(const std::string& bytes, std::vector<char>& out) {
        out.clear();
        size_t openStart = bytes.find("<AppendedData");
        if (openStart == std::string::npos) return;
        size_t gt = bytes.find('>', openStart);
        if (gt == std::string::npos) return;

        bool isBase64 = std::string(bytes.begin() + openStart, bytes.begin() + gt).find("base64") != std::string::npos;

        size_t closeTag = bytes.find("</AppendedData>", gt);
        if (closeTag == std::string::npos) return;

        size_t payloadStart = gt + 1;
        while (payloadStart < closeTag && std::isspace(static_cast<unsigned char>(bytes[payloadStart])))
            ++payloadStart;
        if (payloadStart < closeTag && bytes[payloadStart] == '_') ++payloadStart;
        size_t payloadEnd = closeTag;
        while (payloadEnd > payloadStart && std::isspace(static_cast<unsigned char>(bytes[payloadEnd - 1])))
            --payloadEnd;

        if (isBase64) {
            std::string b64(bytes.begin() + payloadStart, bytes.begin() + payloadEnd);
            decodeBase64(b64, out);
        } else {
            out.assign(bytes.begin() + payloadStart, bytes.begin() + payloadEnd);
        }
    }

    // Raw-appended payloads hold arbitrary bytes ('<', '&', quotes, non-ASCII)
    // that are not valid XML character data, so pugixml would fail to parse the
    // file. Blank out the payload region in the copy handed to pugixml while
    // keeping the original bytes for byte-wise extraction above. Base64 payloads
    // are safe text and are left untouched.
    static void sanitizeAppendedPayload(std::string& xmlText) {
        size_t openStart = xmlText.find("<AppendedData");
        if (openStart == std::string::npos) return;
        size_t gt = xmlText.find('>', openStart);
        if (gt == std::string::npos) return;
        std::string tag(xmlText.begin() + openStart, xmlText.begin() + gt);
        if (mesh_utils::toUpper(tag).find("BASE64") != std::string::npos) return;
        size_t closeTag = xmlText.find("</AppendedData>", gt);
        if (closeTag == std::string::npos) return;
        size_t payloadStart = gt + 1;
        while (payloadStart < closeTag && std::isspace(static_cast<unsigned char>(xmlText[payloadStart])))
            ++payloadStart;
        if (payloadStart < closeTag && xmlText[payloadStart] == '_') ++payloadStart;
        if (payloadStart < closeTag)
            std::fill(xmlText.begin() + payloadStart, xmlText.begin() + closeTag, ' ');
    }

    struct DataArrayMeta {
        std::string name;
        VtkType type = VTK_TYPE_FLOAT32;
        int numComponents = 1;
        std::string format = "ASCII";
        size_t offset = 0;
        bool compressed = false;
        Compression compression = Compression::None;
        size_t numberOfTuples = 0;
    };

    DataArrayMeta readDataArrayMeta(const pugi::xml_node& da) {
        DataArrayMeta meta;
        meta.name = da.attribute("Name").value();
        meta.type = vtkTypeFromString(mesh_utils::toUpper(da.attribute("type").value()));
        if (meta.type == VTK_TYPE_UNDEFINED) meta.type = VTK_TYPE_FLOAT32;
        meta.numComponents = da.attribute("NumberOfComponents").as_int(1);
        std::string fmt = mesh_utils::toUpper(da.attribute("format").value());
        if (!fmt.empty()) meta.format = fmt;
        meta.offset = static_cast<size_t>(da.attribute("offset").as_ullong(0));
        meta.compressed = fileHasCompression || (da.attribute("compressor").value()[0] != '\0');
        if (meta.compressed) {
            meta.compression = fileCompression;
            if (da.attribute("compressor").value()[0] != '\0')
                meta.compression = detectCompression(da.attribute("compressor").value());
        }
        meta.numberOfTuples = da.attribute("NumberOfTuples").as_ullong(0);
        return meta;
    }

    static pugi::xml_node findDataArray(const pugi::xml_node& block, const std::string& name) {
        for (pugi::xml_node da : block.children("DataArray")) {
            if (name == da.attribute("Name").value()) return da;
        }
        return pugi::xml_node();
    }

    // ========================================================================
    // Raw byte extraction (binary / appended, with zlib support)
    // ========================================================================

    std::vector<char> readArrayBytes(const pugi::xml_node& da, const DataArrayMeta& meta) {
        std::vector<char> inlineBuf;
        const char* base = nullptr;
        size_t totalLen = 0;
        size_t dataOffset = 0;
        size_t expected = 0;

        if (meta.format == "BINARY") {
            decodeBase64(da.child_value(), inlineBuf);
            base = inlineBuf.data();
            totalLen = inlineBuf.size();
            if (header64) {
                uint64_t hdr = 0;
                if (totalLen >= sizeof(uint64_t)) {
                    std::memcpy(&hdr, base, sizeof(uint64_t));
                    if (bigEndian == mesh_utils::isLittleEndian()) mesh_utils::byteSwap(&hdr);
                }
                expected = static_cast<size_t>(hdr);
                dataOffset = sizeof(uint64_t);
            } else {
                uint32_t hdr = 0;
                if (totalLen >= sizeof(uint32_t)) {
                    std::memcpy(&hdr, base, sizeof(uint32_t));
                    if (bigEndian == mesh_utils::isLittleEndian()) mesh_utils::byteSwap(&hdr);
                }
                expected = static_cast<size_t>(hdr);
                dataOffset = sizeof(uint32_t);
            }
        } else { // APPENDED
            base = appendedData.data();
            totalLen = appendedData.size();
            dataOffset = meta.offset;
            if (header64) {
                uint64_t hdr = 0;
                if (dataOffset + sizeof(uint64_t) <= totalLen) {
                    std::memcpy(&hdr, base + dataOffset, sizeof(uint64_t));
                    if (bigEndian == mesh_utils::isLittleEndian()) mesh_utils::byteSwap(&hdr);
                }
                expected = static_cast<size_t>(hdr);
                dataOffset += sizeof(uint64_t);
            } else {
                uint32_t hdr = 0;
                if (dataOffset + sizeof(uint32_t) <= totalLen) {
                    std::memcpy(&hdr, base + dataOffset, sizeof(uint32_t));
                    if (bigEndian == mesh_utils::isLittleEndian()) mesh_utils::byteSwap(&hdr);
                }
                expected = static_cast<size_t>(hdr);
                dataOffset += sizeof(uint32_t);
            }
        }

         std::vector<char> out;
        if (meta.compressed) {
            // VTK compressed format: first header_type value is block count,
            // then per-block: [uncompressed_size: header_type][compressed_size: header_type][data]
            // The common code above already read the first header into `expected`
            // (which is the block count for compressed data) and advanced dataOffset.
            uint32_t numBlocks = static_cast<uint32_t>(expected);
            size_t offset = dataOffset;
            bool ok = decompressBlocks(base, totalLen, offset, numBlocks,
                                      bigEndian == mesh_utils::isLittleEndian(),
                                      header64, meta.compression, out);
            if (!ok) {
                VTK_XML_WARN("decompressBlocks failed: numBlocks=" << numBlocks
                          << " compression=" << static_cast<int>(meta.compression));
            }
        } else {
            if (dataOffset + expected <= totalLen) {
                out.assign(base + dataOffset, base + dataOffset + expected);
            }
        }
        return out;
    }

    // ========================================================================
    // Typed array readers -> float / int32
    // ========================================================================

    void readArrayFloats(const pugi::xml_node& da, const DataArrayMeta& meta, std::vector<float>& out) {
        if (meta.format == "ASCII") {
            std::istringstream iss(da.child_value());
            double v;
            while (iss >> v) out.push_back(static_cast<float>(v));
            return;
        }
        std::vector<char> bytes = readArrayBytes(da, meta);
        if (!bytes.empty()) convertBytesToFloat(bytes, meta.type, bigEndian == mesh_utils::isLittleEndian(), out);
    }

    void readArrayInt32(const pugi::xml_node& da, const DataArrayMeta& meta, std::vector<int32_t>& out) {
        if (meta.format == "ASCII") {
            std::istringstream iss(da.child_value());
            long long v;
            while (iss >> v) out.push_back(static_cast<int32_t>(v));
            return;
        }
        std::vector<char> bytes = readArrayBytes(da, meta);
        if (!bytes.empty()) convertBytesToInt32(bytes, meta.type, bigEndian == mesh_utils::isLittleEndian(), out);
    }

    void readArrayInt64(const pugi::xml_node& da, const DataArrayMeta& meta, std::vector<int64_t>& out) {
        if (meta.format == "ASCII") {
            std::istringstream iss(da.child_value());
            long long v;
            while (iss >> v) out.push_back(static_cast<int64_t>(v));
            return;
        }
        std::vector<char> bytes = readArrayBytes(da, meta);
        if (!bytes.empty()) convertBytesToInt64(bytes, meta.type, bigEndian == mesh_utils::isLittleEndian(), out);
    }

    // ========================================================================
    // Multi-piece accumulation
    // ========================================================================

    struct FieldSpec {
        std::string name;
        int numComponents = 1;
    };
    std::vector<FieldSpec> pointFields;
    std::vector<FieldSpec> cellFields;

    void collectFieldSpecs(const pugi::xml_node& datasetNode) {
        for (pugi::xml_node piece : datasetNode.children("Piece")) {
            collectFieldSpecsFrom(piece.child("PointData"), pointFields);
            collectFieldSpecsFrom(piece.child("CellData"), cellFields);
        }
    }

    static void collectFieldSpecsFrom(const pugi::xml_node& block, std::vector<FieldSpec>& specs) {
        for (pugi::xml_node da : block.children("DataArray")) {
            std::string name = da.attribute("Name").value();
            if (name.empty()) continue;
            auto it = std::find_if(specs.begin(), specs.end(),
                                   [&](const FieldSpec& s) { return s.name == name; });
            if (it == specs.end()) {
                specs.push_back({ name, da.attribute("NumberOfComponents").as_int(1) });
            }
        }
    }

    void processPiece(const pugi::xml_node& piece) {
        int piecePoints = piece.attribute("NumberOfPoints").as_int(0);
        int pieceCells = piece.attribute("NumberOfCells").as_int(0);
        numPoints += piecePoints;
        numCells += pieceCells;

        if (datasetType == "POLYDATA") {
            numVerts += piece.attribute("NumberOfVerts").as_int(0);
            numLines += piece.attribute("NumberOfLines").as_int(0);
            numStrips += piece.attribute("NumberOfStrips").as_int(0);
            numPolys += piece.attribute("NumberOfPolys").as_int(0);
        }

        // Piece-local indices become global once points are concatenated.
        int pointBase = static_cast<int>(points.size() / 3);
        int cellBase = static_cast<int>(connectivity.size());

        readExtentAttributes(piece); // Piece-level Extent/Origin/Spacing override dataset level.

        if (pugi::xml_node pointsNode = piece.child("Points")) {
            if (pugi::xml_node da = pointsNode.child("DataArray")) {
                DataArrayMeta meta = readDataArrayMeta(da);
                std::vector<float> vals;
                readArrayFloats(da, meta, vals);
                size_t n = vals.size();
                if (meta.numComponents == 3) {
                    points.insert(points.end(), vals.begin(), vals.end());
                } else if (meta.numComponents == 1) {
                    for (size_t i = 0; i < n; ++i) {
                        points.push_back(vals[i]);
                        points.push_back(0.0f);
                        points.push_back(0.0f);
                    }
                } else if (meta.numComponents == 2) {
                    for (size_t i = 0; i + 1 < n; i += 2) {
                        points.push_back(vals[i]);
                        points.push_back(vals[i + 1]);
                        points.push_back(0.0f);
                    }
                } else if (meta.numComponents >= 4) {
                    for (size_t i = 0; i + 3 < n; i += 4) {
                        points.push_back(vals[i]);
                        points.push_back(vals[i + 1]);
                        points.push_back(vals[i + 2]);
                    }
                }
            }
        }

        if (datasetType == "UNSTRUCTUREDGRID") {
            if (pugi::xml_node cellsNode = piece.child("Cells")) {
                if (pugi::xml_node conn = findDataArray(cellsNode, "connectivity")) {
                    DataArrayMeta cm = readDataArrayMeta(conn);
                    std::vector<int64_t> tmp;
                    readArrayInt64(conn, cm, tmp);
                    for (int64_t v : tmp) connectivity.push_back(v + pointBase);
                }
                if (pugi::xml_node off = findDataArray(cellsNode, "offsets")) {
                    DataArrayMeta om = readDataArrayMeta(off);
                    std::vector<int64_t> tmp;
                    readArrayInt64(off, om, tmp);
                    for (int64_t v : tmp) offsets.push_back(v + cellBase);
                }
                if (pugi::xml_node types = findDataArray(cellsNode, "types")) {
                    DataArrayMeta tm = readDataArrayMeta(types);
                    readArrayInt64(types, tm, cellTypes);
                }
            }
        }

        if (datasetType == "RECTILINEARGRID") {
            if (pugi::xml_node coords = piece.child("Coordinates")) {
                int coordIdx = 0;
                for (pugi::xml_node da : coords.children("DataArray")) {
                    DataArrayMeta meta = readDataArrayMeta(da);
                    std::vector<float> vals;
                    readArrayFloats(da, meta, vals);
                    if (coordIdx == 0) rectX.insert(rectX.end(), vals.begin(), vals.end());
                    else if (coordIdx == 1) rectY.insert(rectY.end(), vals.begin(), vals.end());
                    else if (coordIdx == 2) rectZ.insert(rectZ.end(), vals.begin(), vals.end());
                    ++coordIdx;
                }
            }
        }

        if (datasetType == "POLYDATA") {
            parseTopologyBlock(piece.child("Verts"), verts, pointBase);
            parseTopologyBlock(piece.child("Lines"), lines, pointBase);
            parseTopologyBlock(piece.child("Strips"), strips, pointBase);
            parseTopologyBlock(piece.child("Polys"), polys, pointBase);
        }

        parseDataBlocks(piece);
    }

    // Topology blocks are [count p0 p1 ...] records; keep the count and rebase
    // each point index by the cumulative point base so concatenated pieces
    // reference global points.
    void parseTopologyBlock(const pugi::xml_node& blockNode, std::vector<int64_t>& out, int pointBase) {
        if (!blockNode) return;
        if (!blockNode.child("DataArray")) return;
        pugi::xml_node da = blockNode.child("DataArray");
        DataArrayMeta meta = readDataArrayMeta(da);
        std::vector<int64_t> tmp;
        readArrayInt64(da, meta, tmp);
        size_t i = 0;
        while (i < tmp.size()) {
            int64_t n = tmp[i];
            int nPoints = static_cast<int>(n);
            out.push_back(tmp[i]);
            ++i;
            for (int k = 0; k < nPoints && i < tmp.size(); ++k) {
                out.push_back(tmp[i] + pointBase);
                ++i;
            }
        }
    }

    void parseDataBlocks(const pugi::xml_node& piece) {
        int piecePoints = piece.attribute("NumberOfPoints").as_int(0);
        int pieceCells = piece.attribute("NumberOfCells").as_int(0);
        if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();

        // PointData
        parseOneDataBlock(piece.child("PointData"), pointFields, piecePoints, true);
        // CellData
        parseOneDataBlock(piece.child("CellData"), cellFields, pieceCells, false);
        // FieldData (arbitrary metadata; appended across pieces)
        if (pugi::xml_node fieldBlock = piece.child("FieldData")) {
            for (pugi::xml_node da : fieldBlock.children("DataArray")) {
                DataArrayMeta meta = readDataArrayMeta(da);
                std::vector<float> vals;
                readArrayFloats(da, meta, vals);
                std::vector<float>& arr = mesh.attributes->fieldData[meta.name.empty() ? "unnamed" : meta.name];
                arr.insert(arr.end(), vals.begin(), vals.end());
            }
        }
    }

    // Appends one named field across pieces; arrays absent in a piece are
    // zero-padded so every field lines up with the global point/cell count.
    void parseOneDataBlock(const pugi::xml_node& block, const std::vector<FieldSpec>& specs,
                           int pieceTuples, bool isPoint) {
        if (specs.empty()) return;
        for (const FieldSpec& spec : specs) {
            std::vector<float> vals;
            pugi::xml_node da = findDataArray(block, spec.name);
            if (da) {
                DataArrayMeta meta = readDataArrayMeta(da);
                readArrayFloats(da, meta, vals);
            }
            size_t expectedCount = static_cast<size_t>(pieceTuples) * static_cast<size_t>(spec.numComponents);
            // Only pad/truncate when the piece's declared tuple count is known
            // (e.g. NumberOfPoints on UnstructuredGrid/PolyData pieces). For
            // structured grids the count is implied by the extent and the piece
            // has no NumberOfPoints attribute, so append the raw values as-is.
            if (expectedCount > 0) {
                if (vals.size() < expectedCount) vals.resize(expectedCount, 0.0f);
                else if (vals.size() > expectedCount) vals.resize(expectedCount);
            }

            if (spec.numComponents == 1) {
                if (isPoint) {
                    std::vector<float>& arr = mesh.attributes->pointScalars[spec.name];
                    arr.insert(arr.end(), vals.begin(), vals.end());
                    if (mesh.scalarName.empty()) mesh.scalarName = spec.name;
                } else {
                    std::vector<float>& arr = mesh.attributes->cellScalars[spec.name];
                    arr.insert(arr.end(), vals.begin(), vals.end());
                    cellScalarsStorage[spec.name].insert(cellScalarsStorage[spec.name].end(), vals.begin(), vals.end());
                }
            } else if (spec.numComponents == 3) {
                if (isPoint) {
                    std::vector<float>& arr = mesh.attributes->pointVectors[spec.name];
                    arr.insert(arr.end(), vals.begin(), vals.end());
                    if (mesh.vectorName.empty()) mesh.vectorName = spec.name;
                } else {
                    cellVectorsStorage[spec.name].insert(cellVectorsStorage[spec.name].end(), vals.begin(), vals.end());
                }
            } else if (spec.numComponents >= 2) {
                // Multi-component field data (2, 4+)
                if (isPoint) {
                    mesh.attributes->pointFieldData[spec.name].insert(
                        mesh.attributes->pointFieldData[spec.name].end(),
                        vals.begin(), vals.end());
                    mesh.attributes->pointFieldComponents[spec.name] = spec.numComponents;
                } else {
                    mesh.attributes->cellFieldData[spec.name].insert(
                        mesh.attributes->cellFieldData[spec.name].end(),
                        vals.begin(), vals.end());
                    mesh.attributes->cellFieldComponents[spec.name] = spec.numComponents;
                }
            }
        }
    }

    void readExtentAttributes(const pugi::xml_node& node) {
        std::string we = node.attribute("WholeExtent").value();
        if (!we.empty()) {
            std::istringstream iss(we);
            iss >> wholeExtent[0] >> wholeExtent[1] >> wholeExtent[2] >> wholeExtent[3] >> wholeExtent[4] >> wholeExtent[5];
        }
        std::string org = node.attribute("Origin").value();
        if (!org.empty()) {
            std::istringstream iss(org);
            iss >> origin[0] >> origin[1] >> origin[2];
        }
        std::string sp = node.attribute("Spacing").value();
        if (!sp.empty()) {
            std::istringstream iss(sp);
            iss >> spacing[0] >> spacing[1] >> spacing[2];
        }
    }

    // ========================================================================
    // Topology / finalize
    // ========================================================================

     void buildTopology() {
        if (datasetType == "UNSTRUCTUREDGRID" && !connectivity.empty() && !offsets.empty() && !cellTypes.empty()) {
            mesh.vertices = points;
            // Convert XML connectivity+offsets+types to legacy flat cell format
            std::vector<int64_t> legacyCells;
            for (size_t c = 0; c < offsets.size(); ++c) {
                int64_t start = (c == 0) ? 0 : offsets[c - 1];
                int64_t end = offsets[c];
                legacyCells.push_back(end - start);
                for (int64_t i = start; i < end; ++i) {
                    legacyCells.push_back(connectivity[i]);
                }
            }
            int totalCells = static_cast<int>(offsets.size());
            globalCellToVertices = triangulateUnstructuredCells(mesh, legacyCells, cellTypes, totalCells);
        } else if (datasetType == "STRUCTUREDGRID" && !points.empty()) {
            mesh.vertices = points;
            int dX = wholeExtent[1] - wholeExtent[0] + 1;
            int dY = wholeExtent[3] - wholeExtent[2] + 1;
            int dZ = wholeExtent[5] - wholeExtent[4] + 1;
            if (dX <= 0) dX = 1;
            if (dY <= 0) dY = 1;
            if (dZ <= 0) dZ = 1;
            generateStructuredGridSurface(mesh, dX, dY, dZ);
        } else if (datasetType == "IMAGEDATA") {
            int dX = wholeExtent[1] - wholeExtent[0] + 1;
            int dY = wholeExtent[3] - wholeExtent[2] + 1;
            int dZ = wholeExtent[5] - wholeExtent[4] + 1;
            if (dX <= 0) dX = 1;
            if (dY <= 0) dY = 1;
            if (dZ <= 0) dZ = 1;
            numPoints = dX * dY * dZ;
            mesh.vertices.resize(static_cast<size_t>(numPoints) * 3);
            int vIdx = 0;
            for (int z = 0; z < dZ; ++z) {
                for (int y = 0; y < dY; ++y) {
                    for (int x = 0; x < dX; ++x) {
                        mesh.vertices[vIdx++] = origin[0] + (wholeExtent[0] + x) * spacing[0];
                        mesh.vertices[vIdx++] = origin[1] + (wholeExtent[2] + y) * spacing[1];
                        mesh.vertices[vIdx++] = origin[2] + (wholeExtent[4] + z) * spacing[2];
                    }
                }
            }
            if (!points.empty() && points.size() == mesh.vertices.size()) {
                mesh.vertices = points;
            }
            generateStructuredGridSurface(mesh, dX, dY, dZ);
            int cx = std::max(1, dX - 1), cy = std::max(1, dY - 1), cz = std::max(1, dZ - 1);
            globalCellToVertices.reserve(static_cast<size_t>(cx) * cy * cz);
            auto idx = [&](int x, int y, int z) { return x + y * dX + z * dX * dY; };
            for (int z = 0; z + 1 < dZ; ++z)
                for (int y = 0; y + 1 < dY; ++y)
                    for (int x = 0; x + 1 < dX; ++x) {
                        uint32_t i0 = static_cast<uint32_t>(idx(x, y, z));
                        uint32_t i1 = static_cast<uint32_t>(idx(x + 1, y, z));
                        uint32_t i2 = static_cast<uint32_t>(idx(x + 1, y + 1, z));
                        uint32_t i3 = static_cast<uint32_t>(idx(x, y + 1, z));
                        uint32_t i4 = static_cast<uint32_t>(idx(x, y, z + 1));
                        uint32_t i5 = static_cast<uint32_t>(idx(x + 1, y, z + 1));
                        uint32_t i6 = static_cast<uint32_t>(idx(x + 1, y + 1, z + 1));
                        uint32_t i7 = static_cast<uint32_t>(idx(x, y + 1, z + 1));
                         globalCellToVertices.push_back({ i0, i1, i2, i3, i4, i5, i6, i7 });
                     }
            mesh.renderAsPoints = true;
        } else if (datasetType == "RECTILINEARGRID" && !rectX.empty() && !rectY.empty() && !rectZ.empty()) {
            int dX = static_cast<int>(rectX.size());
            int dY = static_cast<int>(rectY.size());
            int dZ = static_cast<int>(rectZ.size());
            numPoints = dX * dY * dZ;
            mesh.vertices.resize(static_cast<size_t>(numPoints) * 3);
            int vIdx = 0;
            for (int z = 0; z < dZ; ++z) {
                for (int y = 0; y < dY; ++y) {
                    for (int x = 0; x < dX; ++x) {
                        mesh.vertices[vIdx++] = rectX[x];
                        mesh.vertices[vIdx++] = rectY[y];
                        mesh.vertices[vIdx++] = rectZ[z];
                    }
                }
            }
            generateStructuredGridSurface(mesh, dX, dY, dZ);
            int cx = std::max(1, dX - 1), cy = std::max(1, dY - 1), cz = std::max(1, dZ - 1);
            globalCellToVertices.reserve(static_cast<size_t>(cx) * cy * cz);
            auto idx = [&](int x, int y, int z) { return x + y * dX + z * dX * dY; };
            for (int z = 0; z + 1 < dZ; ++z)
                for (int y = 0; y + 1 < dY; ++y)
                    for (int x = 0; x + 1 < dX; ++x) {
                        uint32_t i0 = static_cast<uint32_t>(idx(x, y, z));
                        uint32_t i1 = static_cast<uint32_t>(idx(x + 1, y, z));
                        uint32_t i2 = static_cast<uint32_t>(idx(x + 1, y + 1, z));
                        uint32_t i3 = static_cast<uint32_t>(idx(x, y + 1, z));
                        uint32_t i4 = static_cast<uint32_t>(idx(x, y, z + 1));
                        uint32_t i5 = static_cast<uint32_t>(idx(x + 1, y, z + 1));
                        uint32_t i6 = static_cast<uint32_t>(idx(x + 1, y + 1, z + 1));
                        uint32_t i7 = static_cast<uint32_t>(idx(x, y + 1, z + 1));
                         globalCellToVertices.push_back({ i0, i1, i2, i3, i4, i5, i6, i7 });
                     }
        } else if (datasetType == "POLYDATA") {
            mesh.vertices = points;
            if (!polys.empty()) {
                globalCellToVertices = triangulatePolygons(mesh, polys, numPolys);
            }
            if (!lines.empty()) {
                triangulateLines(mesh, lines, numLines);
            }
            if (!strips.empty()) {
                triangulateTriangleStrips(mesh, strips, numStrips);
            }
            if (mesh.indices.empty() && !mesh.vertices.empty()) {
                std::cerr << "VTK XML Parser Warning: POLYDATA has points but no polys/lines/strips; rendering points only." << std::endl;
                mesh.renderAsPoints = true;
            }
        }
    }

    void finalizeMeshData() {
        if (mesh.vertices.empty()) {
            std::cerr << "VTK XML Parser Error: Empty data sequence." << std::endl;
            return;
        }
        if (mesh.indices.empty() && datasetType != "POLYDATA" && !mesh.renderAsPoints) {
            std::cerr << "VTK XML Parser Error: topology produced no triangles and not a point set; mesh will not render." << std::endl;
            return;
        }

        {
            uint32_t vCount = static_cast<uint32_t>(mesh.vertices.size() / 3);
            bool badIndex = false;
            for (uint32_t idx : mesh.indices) {
                if (idx >= vCount) { badIndex = true; break; }
            }
            if (badIndex) {
                std::cerr << "VTK XML Parser Error: topology references vertex index >= vertex count ("
                          << vCount << "); dropping indices." << std::endl;
                mesh.indices.clear();
                return;
            }
        }

        extrapolateCellDataToPointsMerged(mesh, globalCellToVertices, cellScalarsStorage, cellVectorsStorage);

        if (mesh.attributes.has_value()) {
            size_t perVertex = mesh.vertices.size() / 3;
            mesh.pointVectorCount = perVertex;
            for (const auto& [name, vecArr] : mesh.attributes->pointVectors) {
                if (vecArr.size() < perVertex * 3) continue;
                mesh.pointVectorOffset[name] = mesh.pointVectorsData.size();
                for (size_t v = 0; v < perVertex; ++v) {
                    mesh.pointVectorsData.emplace_back(
                        vecArr[v * 3 + 0], vecArr[v * 3 + 1], vecArr[v * 3 + 2]);
                }
            }
        }

        bool hasAttributes = mesh.attributes.has_value();
        if (hasAttributes && !mesh.attributes->pointScalars.empty()) {
            if (mesh.scalarName.empty() || !mesh.attributes->pointScalars.count(mesh.scalarName)) {
                mesh.scalarName = mesh.attributes->pointScalars.begin()->first;
            }
            const std::vector<float>& active = mesh.attributes->pointScalars[mesh.scalarName];
            size_t vCount = mesh.vertices.size() / 3;
            if (!active.empty() && active.size() == vCount) {
                mesh.scalars = active;
            } else {
                std::cerr << "VTK XML Parser Warning: active scalar '" << mesh.scalarName
                          << "' is not 1-component per vertex; scalar coloring disabled." << std::endl;
                mesh.scalars.clear();
            }
        } else {
            mesh.scalars.clear();
        }

        if (hasAttributes) {
            for (const auto& [name, _] : mesh.attributes->pointScalars) {
                mesh.availableScalarNames.push_back(name);
            }
            for (const auto& [name, _] : mesh.attributes->pointVectors) {
                mesh.availableVectorNames.push_back(name);
            }
        }
        if (mesh.vectorName.empty() && !mesh.availableVectorNames.empty()) {
            mesh.vectorName = mesh.availableVectorNames.front();
        }

        mesh_utils::computeBounds(mesh);
        mesh.sourcePointCount = static_cast<int>(mesh.vertices.size() / 3);

        mesh.flatVerts.reserve(mesh.indices.size() * 3);
        for (uint32_t i : mesh.indices) {
            const float* p = &mesh.vertices[i * 3];
            mesh.flatVerts.insert(mesh.flatVerts.end(), { p[0], p[1], p[2] });
        }

        if (mesh.normals.empty() && !mesh.indices.empty()) {
            mesh_utils::computeNormals(mesh);
        }
    }

    // ========================================================================
    // State
    // ========================================================================

    std::string filePath;
    RenderMesh mesh;
    std::string datasetType;
    bool bigEndian = false;
    bool header64 = false;
    bool fileHasCompression = false;
    Compression fileCompression = Compression::None;

    int numPoints = 0;
    int numCells = 0;

    // ImageData / StructuredGrid extent
    int wholeExtent[6] = {0, -1, 0, -1, 0, -1};
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float spacing[3] = {1.0f, 1.0f, 1.0f};

    // RectilinearGrid coordinates
    std::vector<float> rectX, rectY, rectZ;

    // Points
    std::vector<float> points;

    // UnstructuredGrid cells
    std::vector<int64_t> connectivity;
    std::vector<int64_t> offsets;
    std::vector<int64_t> cellTypes;

    // PolyData
    std::vector<int64_t> polys;
    std::vector<int64_t> lines;
    std::vector<int64_t> strips;
    std::vector<int64_t> verts;
    int numPolys = 0, numLines = 0, numStrips = 0, numVerts = 0;

    // Cell data storage (pre-extrapolation)
    std::unordered_map<std::string, std::vector<float>> cellScalarsStorage;
    std::unordered_map<std::string, std::vector<float>> cellVectorsStorage;
    std::vector<std::vector<uint32_t>> globalCellToVertices;

    // Appended binary buffer
    std::vector<char> appendedData;
};

RenderMesh parseVTKXML(const std::string& filePath) {
    VTKXMLParserContext parser(filePath);
    return parser.parse();
}

// ── MultiBlock (.vtm) support ────────────────────────────────────────────────

static std::string resolveVtmPath(const std::string& vtmPath, const std::string& dataSetFile) {
    if (dataSetFile.empty()) return dataSetFile;
    // If the DataSet @c file is relative, resolve against the .vtm's directory.
    if (dataSetFile.find(':') != std::string::npos || dataSetFile.find('/') == 0 || dataSetFile.find('\\') == 0) {
        return dataSetFile;
    }
    size_t lastSlash = vtmPath.find_last_of("/\\");
    if (lastSlash == std::string::npos) return dataSetFile;
    return vtmPath.substr(0, lastSlash + 1) + dataSetFile;
}

RenderMesh parseMultiBlockXML(const std::string& filePath) {
    pugi::xml_document doc;
    {
        std::ifstream f(filePath, std::ios::binary);
        if (!f) {
            std::cerr << "VTK XML Parser Error: Failed to open .vtm file: " << filePath << std::endl;
            return RenderMesh();
        }
        std::stringstream ss;
        ss << f.rdbuf();
        std::string xmlBytes = ss.str();
        // Strip NUL bytes from raw binary payloads (vtm files rarely have them,
        // but be safe).
        size_t nullPos = xmlBytes.find('\0');
        if (nullPos != std::string::npos) xmlBytes.resize(nullPos);
        pugi::xml_parse_result result = doc.load_buffer(xmlBytes.data(), xmlBytes.size(),
            pugi::parse_default | pugi::parse_ws_pcdata);
        if (!result) {
            std::cerr << "VTK XML Parser Error: .vtm XML parse failed: " << result.description() << std::endl;
            return RenderMesh();
        }
    }

    pugi::xml_node root = doc.child("VTKFile");
    if (!root) {
        std::cerr << "VTK XML Parser Error: Missing <VTKFile> tag in .vtm." << std::endl;
        return RenderMesh();
    }

    pugi::xml_node multiBlock = root.child("MultiBlock");
    if (!multiBlock) {
        std::cerr << "VTK XML Parser Error: Missing <MultiBlock> tag in .vtm." << std::endl;
        return RenderMesh();
    }

    // Collect all <DataSet file="..."/> references
    std::vector<std::string> dataSetFiles;
    for (pugi::xml_node node : multiBlock.children()) {
        std::string nodeName = node.name();
        if (nodeName == "DataSet") {
            std::string fileAttr = node.attribute("file").value();
            if (!fileAttr.empty()) {
                dataSetFiles.push_back(fileAttr);
            }
        }
    }

    if (dataSetFiles.empty()) {
        std::cerr << "VTK XML Parser Warning: No <DataSet file=\"...\"/> references found in .vtm." << std::endl;
    }

    std::vector<RenderMesh> pieces;
    for (const std::string& dsFile : dataSetFiles) {
        std::string resolved = resolveVtmPath(filePath, dsFile);
        RenderMesh pieceMesh = parseVTKXML(resolved);
        if (!pieceMesh.vertices.empty()) {
            pieces.push_back(std::move(pieceMesh));
        }
    }

    if (pieces.empty()) {
        std::cerr << "VTK XML Parser Error: No valid datasets loaded from .vtm: " << filePath << std::endl;
        return RenderMesh();
    }

    return mergeRenderMeshes(pieces);
}

// ── mergeRenderMeshes ────────────────────────────────────────────────────────
// Concatenates multiple meshes into one, rebasing index buffers.
RenderMesh mergeRenderMeshes(const std::vector<RenderMesh>& meshes) {
    if (meshes.empty()) return RenderMesh();
    if (meshes.size() == 1) return meshes[0];

    RenderMesh merged;
    uint32_t indexBase = 0;
    size_t maxVectorCount = 0;

    for (const RenderMesh& m : meshes) {
        size_t vStart = merged.vertices.size();
        merged.vertices.insert(merged.vertices.end(), m.vertices.begin(), m.vertices.end());

        for (uint32_t idx : m.indices) {
            merged.indices.push_back(idx + indexBase);
        }

        if (m.normals.size() == m.vertices.size()) {
            merged.normals.insert(merged.normals.end(), m.normals.begin(), m.normals.end());
        }

        if (!m.scalars.empty()) {
            merged.scalars.insert(merged.scalars.end(), m.scalars.begin(), m.scalars.end());
            if (m.scalarName.empty()) merged.scalarName = m.scalarName;
        }
        for (const auto& name : m.availableScalarNames) {
            merged.availableScalarNames.push_back(name);
        }

         merged.pointVectorsData.insert(merged.pointVectorsData.end(),
                                        m.pointVectorsData.begin(), m.pointVectorsData.end());
         for (const auto& [name, offset] : m.pointVectorOffset) {
             merged.pointVectorOffset[name] = static_cast<uint32_t>(vStart) / 3 + offset;
         }
        for (const auto& name : m.availableVectorNames) {
            if (std::find(merged.availableVectorNames.begin(), merged.availableVectorNames.end(), name)
                == merged.availableVectorNames.end()) {
                merged.availableVectorNames.push_back(name);
            }
        }
        maxVectorCount = std::max(maxVectorCount, m.pointVectorCount);

        if (!m.datasetType.empty() && merged.datasetType.empty()) {
            merged.datasetType = m.datasetType;
        }
        if (!m.fileFormat.empty() && merged.fileFormat.empty()) {
            merged.fileFormat = m.fileFormat;
        }
        merged.renderAsPoints = merged.renderAsPoints || m.renderAsPoints;

        indexBase = static_cast<uint32_t>(merged.vertices.size() / 3);
    }

    merged.pointVectorCount = maxVectorCount;

    // Merge attributes
    if (meshes[0].attributes.has_value()) {
        merged.attributes = DatasetAttributes();
        for (const RenderMesh& m : meshes) {
            if (!m.attributes) continue;
            for (const auto& [name, vals] : m.attributes->pointScalars) {
                auto& arr = merged.attributes->pointScalars[name];
                arr.insert(arr.end(), vals.begin(), vals.end());
            }
            for (const auto& [name, vals] : m.attributes->cellScalars) {
                auto& arr = merged.attributes->cellScalars[name];
                arr.insert(arr.end(), vals.begin(), vals.end());
            }
            for (const auto& [name, vals] : m.attributes->pointVectors) {
                auto& arr = merged.attributes->pointVectors[name];
                arr.insert(arr.end(), vals.begin(), vals.end());
            }
            for (const auto& [name, vals] : m.attributes->pointFieldData) {
                auto& arr = merged.attributes->pointFieldData[name];
                arr.insert(arr.end(), vals.begin(), vals.end());
            }
            for (const auto& [name, comp] : m.attributes->pointFieldComponents) {
                merged.attributes->pointFieldComponents[name] = comp;
            }
            for (const auto& [name, vals] : m.attributes->cellFieldData) {
                auto& arr = merged.attributes->cellFieldData[name];
                arr.insert(arr.end(), vals.begin(), vals.end());
            }
            for (const auto& [name, comp] : m.attributes->cellFieldComponents) {
                merged.attributes->cellFieldComponents[name] = comp;
            }
            for (const auto& [name, vals] : m.attributes->fieldData) {
                auto& arr = merged.attributes->fieldData[name];
                arr.insert(arr.end(), vals.begin(), vals.end());
            }
        }
    }

    mesh_utils::computeBounds(merged);
    merged.sourcePointCount = static_cast<int>(merged.vertices.size() / 3);
    return merged;
}
