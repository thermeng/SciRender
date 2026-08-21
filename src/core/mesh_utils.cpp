#include "core/mesh_loader.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <map>

namespace mesh_utils {

// ── String helpers ──────────────────────────────────────────────────────────

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string toUpper(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
    return result;
}

// ── Byte-swap helpers (VTK binary is big-endian) ────────────────────────────

template<typename T>
static inline void swapBytes(T* val) {
    char* bytes = reinterpret_cast<char*>(val);
    std::size_t n = sizeof(T);
    for (std::size_t i = 0; i < n / 2; ++i) {
        std::swap(bytes[i], bytes[n - 1 - i]);
    }
}

void byteSwap(float* val)     { swapBytes(val); }
void byteSwap(double* val)    { swapBytes(val); }
void byteSwap(int* val)       { swapBytes(val); }
void byteSwap(uint32_t* val)  { swapBytes(val); }
void byteSwap(int16_t* val)   { swapBytes(val); }
void byteSwap(uint16_t* val)  { swapBytes(val); }
void byteSwap(uint8_t* val)   { swapBytes(val); }
void byteSwap(int64_t* val)   { swapBytes(val); }
void byteSwap(uint64_t* val)  { swapBytes(val); }

bool isLittleEndian() {
    int test = 1;
    return *(reinterpret_cast<char*>(&test)) == 1;
}

// ── Geometry utilities ──────────────────────────────────────────────────────

void indexFlatTriangles(const std::vector<float>& flatVerts,
                        std::vector<float>& outVertices,
                        std::vector<uint32_t>& outIndices) {
    if (flatVerts.empty()) return;
    const size_t cornerCount = flatVerts.size() / 3;
    const size_t triCount = cornerCount / 3;

    struct VertEntry {
        PositionKey key;
        uint32_t flatIdx; // index into flatVerts (corner index * 3)
    };
    std::vector<VertEntry> entries(cornerCount);
    for (size_t i = 0; i < cornerCount; ++i) {
        size_t base = i * 3;
        entries[i] = { positionKey(flatVerts[base], flatVerts[base + 1], flatVerts[base + 2]),
                       static_cast<uint32_t>(i) };
    }

    // Sort by quantized position — groups identical vertices together
    std::sort(entries.begin(), entries.end(), [](const VertEntry& a, const VertEntry& b) {
        if (std::get<0>(a.key) != std::get<0>(b.key)) return std::get<0>(a.key) < std::get<0>(b.key);
        if (std::get<1>(a.key) != std::get<1>(b.key)) return std::get<1>(a.key) < std::get<1>(b.key);
        return std::get<2>(a.key) < std::get<2>(b.key);
    });

    // Scan sorted entries: first occurrence of each unique key gets a new vertex
    // index; subsequent occurrences reuse that index.
    std::vector<uint32_t> cornerToIdx(cornerCount); // flat corner → indexed vertex
    outVertices.reserve(cornerCount * 3);           // upper bound
    for (size_t i = 0; i < cornerCount; ) {
        size_t j = i + 1;
        while (j < cornerCount && entries[j].key == entries[i].key) ++j;
        // All entries[i..j) share the same position — emit one vertex
        uint32_t vidx = static_cast<uint32_t>(outVertices.size() / 3);
        size_t base = entries[i].flatIdx * 3;
        outVertices.push_back(flatVerts[base + 0]);
        outVertices.push_back(flatVerts[base + 1]);
        outVertices.push_back(flatVerts[base + 2]);
        for (size_t k = i; k < j; ++k) {
            cornerToIdx[entries[k].flatIdx] = vidx;
        }
        i = j;
    }

    outIndices.reserve(cornerCount);
    for (size_t t = 0; t < triCount; ++t) {
        outIndices.push_back(cornerToIdx[t * 3 + 0]);
        outIndices.push_back(cornerToIdx[t * 3 + 1]);
        outIndices.push_back(cornerToIdx[t * 3 + 2]);
    }
}

void finalizeSurfaceMesh(RenderMesh& mesh,
                         const std::string& datasetType,
                         const std::string& fileFormat,
                         const char* logLabel) {
    if (!mesh.hasBounds) {
        computeBounds(mesh);
    }

    // Record the topological point count (deduped, pre-normal-split) so the UI
    // can report the true vertex count ParaView shows. computeNormals() below
    // will duplicate vertices at sharp edges for shading, which would otherwise
    // inflate the displayed "Points" value.
    mesh.sourcePointCount = static_cast<int>(mesh.vertices.size() / 3);
    mesh.datasetType = datasetType;
    mesh.fileFormat = fileFormat;
    if (mesh.normals.empty() && !mesh.vertices.empty() && !mesh.indices.empty()) {
        computeNormals(mesh);
    }

    std::cout << logLabel << mesh.indices.size() / 3 << " triangles, "
              << mesh.vertices.size() / 3 << " unique vertices" << std::endl;
}

void computeBounds(RenderMesh& mesh) {
    if (mesh.vertices.empty()) {
        mesh.bounds = BoundingVolume{};
        return;
    }
    double minX = 1e300, minY = 1e300, minZ = 1e300;
    double maxX = -1e300, maxY = -1e300, maxZ = -1e300;
    for (size_t i = 0; i < mesh.vertices.size(); i += 3) {
        double x = static_cast<double>(mesh.vertices[i]);
        double y = static_cast<double>(mesh.vertices[i + 1]);
        double z = static_cast<double>(mesh.vertices[i + 2]);
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);
        minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
    }
    mesh.bounds.minX = minX; mesh.bounds.maxX = maxX;
    mesh.bounds.minY = minY; mesh.bounds.maxY = maxY;
    mesh.bounds.minZ = minZ; mesh.bounds.maxZ = maxZ;

    mesh.bounds.centerX = (minX + maxX) * 0.5;
    mesh.bounds.centerY = (minY + maxY) * 0.5;
    mesh.bounds.centerZ = (minZ + maxZ) * 0.5;

    double dx = maxX - minX, dy = maxY - minY, dz = maxZ - minZ;
    mesh.bounds.extent = std::max({ dx, dy, dz });
    if (mesh.bounds.extent < 0.001) mesh.bounds.extent = 1.0;
    mesh.bounds.worldRadius = mesh.bounds.extent * 0.5;
}

// Angle-based sharp-edge normal computation.
// Vertices at sharp edges (angle between adjacent faces > threshold) are
// duplicated so each side gets its own flat normal. Smooth regions keep
// averaged normals. This eliminates the "bloating" artifact on cube edges
// and other sharp features.
void computeNormals(RenderMesh& mesh) {
    if (mesh.vertices.empty() || mesh.indices.empty()) return;

    const float SHARP_ANGLE_COS = 0.9f;  // ~25 degrees — faces more divergent than this split

    size_t numVerts = mesh.vertices.size() / 3;
    size_t numTris = mesh.indices.size() / 3;

    // Step 1: Compute per-face normals
    struct FaceNormal {
        float nx, ny, nz;
    };
    std::vector<FaceNormal> faceNormals(numTris);
    for (size_t t = 0; t < numTris; t++) {
        int i0 = mesh.indices[t * 3];
        int i1 = mesh.indices[t * 3 + 1];
        int i2 = mesh.indices[t * 3 + 2];

        float v0x = mesh.vertices[i0 * 3], v0y = mesh.vertices[i0 * 3 + 1], v0z = mesh.vertices[i0 * 3 + 2];
        float v1x = mesh.vertices[i1 * 3], v1y = mesh.vertices[i1 * 3 + 1], v1z = mesh.vertices[i1 * 3 + 2];
        float v2x = mesh.vertices[i2 * 3], v2y = mesh.vertices[i2 * 3 + 1], v2z = mesh.vertices[i2 * 3 + 2];

        float e1x = v1x - v0x, e1y = v1y - v0y, e1z = v1z - v0z;
        float e2x = v2x - v0x, e2y = v2y - v0y, e2z = v2z - v0z;

        float nx = e1y * e2z - e1z * e2y;
        float ny = e1z * e2x - e1x * e2z;
        float nz = e1x * e2y - e1y * e2x;

        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-10f) {
            nx /= len; ny /= len; nz /= len;
        }
        faceNormals[t] = {nx, ny, nz};
    }

    // Step 2: Build vertex -> face adjacency as a flat CSR (Compressed Sparse
    // Row) structure to avoid the vector-of-vectors heap fragmentation that
    // previously dominated CPU time on multi-million-vertex meshes (B8).
    std::vector<int> adjOffsets(numVerts + 1, 0);
    for (size_t t = 0; t < numTris; t++) {
        for (int c = 0; c < 3; c++) adjOffsets[mesh.indices[t * 3 + c] + 1]++;
    }
    for (size_t v = 1; v <= numVerts; v++) adjOffsets[v] += adjOffsets[v - 1];
    std::vector<int> adjFaces(adjOffsets[numVerts]);
    std::vector<int> adjCursor = adjOffsets; // running write cursor
    for (size_t t = 0; t < numTris; t++) {
        for (int c = 0; c < 3; c++) {
            int v = mesh.indices[t * 3 + c];
            adjFaces[adjCursor[v]++] = static_cast<int>(t);
        }
    }

    // Step 3: For each vertex, group adjacent faces into smooth groups using the
    // ACTUAL face-normal angle (dot product) against SHARP_ANGLE_COS (~25 deg).
    // A vertex is split only when its incident faces genuinely diverge beyond that
    // threshold (a real sharp edge). The previous sign-octant heuristic (bucket by
    // the sign of each normal component, spanning up to 90 deg) wrongly split smooth
    // surfaces wherever any normal component crossed zero — e.g. along every meridian
    // of a sphere or cone — producing the reported spurious "sharp lines" artifacts.
    // vertexRemap[originalVert] = {newVertForGroup0, newVertForGroup1, ...}
    std::vector<std::vector<int>> vertexRemap(numVerts);
    for (size_t v = 0; v < numVerts; v++) {
        vertexRemap[v].push_back(static_cast<int>(v));  // default: original vertex
    }

    // A vertex gains (numGroups - 1) duplicates at most. Size the output to
    // ~2x the input once so the later push_backs don't reallocate (B7).
    std::vector<float> newVertices;
    newVertices.reserve(mesh.vertices.size() * 2);
    newVertices = mesh.vertices;  // will grow with duplicates
    int nextVert = static_cast<int>(numVerts);

    auto dot3 = [](const FaceNormal& a, const FaceNormal& b) {
        return a.nx * b.nx + a.ny * b.ny + a.nz * b.nz;
    };

    for (size_t v = 0; v < numVerts; v++) {
        int fStart = adjOffsets[v], fEnd = adjOffsets[v + 1];
        if (fStart == fEnd) continue;

        // Greedily cluster incident faces into smooth groups. Seed group 0 with the
        // first face; a subsequent face joins a group only if its normal
        // dot-product with that group's REPRESENTATIVE face stays >= SHARP_ANGLE_COS.
        // Comparing against a single representative (instead of all already-accepted
        // faces) reduces per-vertex cost from O(F²) to O(F×G) where G is the number of
        // groups (typically 1–2). This is the standard smooth-shading approach and
        // honors the ~25 deg threshold: a sphere's near-tangent faces all share a
        // representative within the threshold → one group → smooth shading; a cube
        // corner's 90 deg faces diverge → separate groups → split.
        std::vector<int> groupOf(fEnd - fStart, 0); // face slot -> group id
        std::vector<int> groupRep;                   // adjFaces index of each group's rep face
        groupRep.reserve(4);
        groupRep.push_back(fStart);                  // group 0's rep is the first incident face
        int numGroups = 1;
        for (int fi = fStart + 1; fi < fEnd; fi++) {
            int f = adjFaces[fi];
            bool merged = false;
            for (int g = 0; g < numGroups; g++) {
                int of = adjFaces[groupRep[g]];
                if (dot3(faceNormals[f], faceNormals[of]) >= SHARP_ANGLE_COS) {
                    groupOf[fi - fStart] = g;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                groupOf[fi - fStart] = numGroups;
                groupRep.push_back(fi);
                numGroups++;
            }
        }

        if (numGroups <= 1) continue;  // smooth vertex, no split needed

        // Create duplicate vertices: exactly one per extra group (group 0 keeps the
        // original vertex). The index remap below routes every face in a group to its
        // duplicate, so producing more than one per group would only waste memory on
        // unreferenced vertices (S3).
        for (int g = 1; g < numGroups; g++) {
            newVertices.push_back(mesh.vertices[v * 3]);
            newVertices.push_back(mesh.vertices[v * 3 + 1]);
            newVertices.push_back(mesh.vertices[v * 3 + 2]);
            vertexRemap[v].push_back(nextVert++);
        }

        // Remap indices: route each adjacent face to its group's duplicate.
        for (int fi = fStart; fi < fEnd; fi++) {
            int f = adjFaces[fi];
            int g = groupOf[fi - fStart];
            int newV = vertexRemap[v][g];
            if (newV == static_cast<int>(v)) continue;  // original vertex, no remap
            int base = f * 3;
            for (int c = 0; c < 3; c++) {
                if (mesh.indices[base + c] == static_cast<int>(v)) {
                    mesh.indices[base + c] = newV;
                }
            }
        }
    }

    // Step 4: Compute per-group normals (averaged within group)
    std::vector<float> newNormals(newVertices.size(), 0.0f);
    for (size_t t = 0; t < numTris; t++) {
        int i0 = mesh.indices[t * 3];
        int i1 = mesh.indices[t * 3 + 1];
        int i2 = mesh.indices[t * 3 + 2];

        float fnx = faceNormals[t].nx, fny = faceNormals[t].ny, fnz = faceNormals[t].nz;
        newNormals[i0 * 3] += fnx; newNormals[i0 * 3 + 1] += fny; newNormals[i0 * 3 + 2] += fnz;
        newNormals[i1 * 3] += fnx; newNormals[i1 * 3 + 1] += fny; newNormals[i1 * 3 + 2] += fnz;
        newNormals[i2 * 3] += fnx; newNormals[i2 * 3 + 1] += fny; newNormals[i2 * 3 + 2] += fnz;
    }

    // Normalize
    for (size_t i = 0; i < newNormals.size(); i += 3) {
        float nx = newNormals[i], ny = newNormals[i + 1], nz = newNormals[i + 2];
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-10f) {
            newNormals[i] /= len; newNormals[i + 1] /= len; newNormals[i + 2] /= len;
        }
        else {
            newNormals[i] = 0.0f; newNormals[i + 1] = 0.0f; newNormals[i + 2] = 1.0f;
        }
    }

    // Sync scalars: expand the scalars array to match new duplicated vertices.
    // For each duplicate vertex created, copy the original vertex's scalar value.
    if (!mesh.scalars.empty()) {
        size_t origVertCount = numVerts;
        size_t newVertCount = newVertices.size() / 3;
        std::vector<float> newScalars(newVertCount, 0.5f); // one scalar per vertex, default mid-range
        for (size_t oldV = 0; oldV < origVertCount; oldV++) {
            float scalarVal = (oldV < mesh.scalars.size()) ? mesh.scalars[oldV] : 0.5f;
            // Copy scalar to all remapped duplicates of this vertex
            for (int newV : vertexRemap[oldV]) {
                if (static_cast<size_t>(newV) < newVertCount) {
                    newScalars[newV] = scalarVal;
                }
            }
        }
        mesh.scalars = std::move(newScalars);
    }

    // Sync the per-field point-scalar maps the same way: a duplicated vertex must
    // carry the same scalar as its source, so field switches handed to the GPU
    // (which is indexed by the final vertex count) stay in bounds and correct.
    if (mesh.attributes.has_value() && !mesh.attributes->pointScalars.empty()) {
        const size_t newVertCount = newVertices.size() / 3;
        for (auto& [name, arr] : mesh.attributes->pointScalars) {
            if (arr.empty()) continue;
            std::vector<float> newArr(newVertCount, 0.5f);
            for (size_t oldV = 0; oldV < numVerts; oldV++) {
                float scalarVal = (oldV < arr.size()) ? arr[oldV] : 0.5f;
                for (int newV : vertexRemap[oldV]) {
                    if (static_cast<size_t>(newV) < newVertCount) {
                        newArr[newV] = scalarVal;
                    }
                }
            }
            arr = std::move(newArr);
        }
    }

    // NOTE: per-point vectors are intentionally NOT remapped here. The glyph
    // builder samples them by ORIGINAL point index (limit = min(verts/3,
    // runCount)) and a split/duplicate vertex sits at its source's exact xyz,
    // so leaving the vector run at the pre-split per-point count (one vec3 per
    // original point) is both correct and complete — expanding to the split
    // vertex count corrupts multi-field offsets and desyncs the buffer size
    // from the documented pointVectorsData.size() == originalPointCount
    // invariant. Scalars above are expanded because they are GPU-indexed by
    // the final vertex count.

    // Apply changes
    mesh.vertices = std::move(newVertices);
    mesh.normals = std::move(newNormals);

    // B3: enforce the per-vertex array-size invariants AFTER the split. Any code
    // downstream (GL upload, glyph sampling) indexes these by the final vertex
    // count, so a desynced array is a latent out-of-range read. Repair rather
    // than trust: pad short arrays, truncate long ones, drop unusable ones.
    const size_t finalVerts = mesh.vertices.size() / 3;
    if (!mesh.scalars.empty() && mesh.scalars.size() != finalVerts) {
        mesh.scalars.resize(finalVerts, 0.5f);
    }
    // per-point vectors: left at the original per-point count (see note above);
    // no vertex-count resize.

    // Ensure scalarMin/scalarMax remain valid after vertex splitting
    if (!mesh.scalars.empty()) {
        float actualMin = 1e30f, actualMax = -1e30f;
        for (float s : mesh.scalars) {
            if (s < actualMin) actualMin = s;
            if (s > actualMax) actualMax = s;
        }
        if (mesh.attributes.has_value()) {
            mesh.attributes->scalarMin = actualMin;
            mesh.attributes->scalarMax = actualMax;
        }
    }
}

// ── Cell-data → point-data extrapolation ─────────────────────────────────────

void extrapolateCellDataToPoints(
    RenderMesh& mesh,
    const std::vector<std::vector<uint32_t>>& globalCellToVertices,
    const std::unordered_map<std::string, std::vector<float>>& cellScalarsStorage,
    const std::unordered_map<std::string, std::vector<float>>& cellVectorsStorage
) {
    if (globalCellToVertices.empty()) return;
    if (cellScalarsStorage.empty() && cellVectorsStorage.empty()) return;

    int vCount = static_cast<int>(mesh.vertices.size() / 3);
    if (vCount == 0) return;
    if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();

    // Pre-collect all fields into a flat array. This avoids per-cell hash lookups
    // (scalarAccIdx[name] / vectorAccIdx[name]) that the old code performed inside
    // the cell loop — each was O(fields) lookups per cell.
    struct FieldInfo {
        std::string name;
        bool isVector;
        const std::vector<float>* raw;
        std::vector<float> sum;
    };
    std::vector<FieldInfo> fields;
    fields.reserve(cellScalarsStorage.size() + cellVectorsStorage.size());
    for (const auto& [name, raw] : cellScalarsStorage) {
        fields.push_back({name, false, &raw, std::vector<float>(static_cast<size_t>(vCount), 0.0f)});
    }
    for (const auto& [name, raw] : cellVectorsStorage) {
        fields.push_back({name, true, &raw, std::vector<float>(static_cast<size_t>(vCount) * 3, 0.0f)});
    }

    // Single pass over all cells to compute per-vertex contribution counts.
    // The old code computed this inside the field loop (repeatedly per field).
    std::vector<float> contributionCounts(static_cast<size_t>(vCount), 0.0f);
    for (const auto& cellVerts : globalCellToVertices) {
        for (uint32_t vIdx : cellVerts) {
            if (vIdx < static_cast<uint32_t>(vCount)) contributionCounts[vIdx] += 1.0f;
        }
    }

    // For each field: iterate all cells once, accumulate cell values into
    // incident points. Bounds checked once via the && loop condition (no
    // per-inner-iteration branch on the cell index).
    for (auto& f : fields) {
        if (f.isVector) {
            size_t numVecs = f.raw->size() / 3;
            for (size_t c = 0; c < globalCellToVertices.size() && c < numVecs; ++c) {
                const float vx = (*f.raw)[c * 3 + 0];
                const float vy = (*f.raw)[c * 3 + 1];
                const float vz = (*f.raw)[c * 3 + 2];
                const auto& verts = globalCellToVertices[c];
                for (uint32_t vIdx : verts) {
                    if (vIdx >= static_cast<uint32_t>(vCount)) continue;
                    size_t base = static_cast<size_t>(vIdx) * 3;
                    f.sum[base + 0] += vx;
                    f.sum[base + 1] += vy;
                    f.sum[base + 2] += vz;
                }
            }
        } else {
            size_t numScalars = f.raw->size();
            for (size_t c = 0; c < globalCellToVertices.size() && c < numScalars; ++c) {
                const float val = (*f.raw)[c];
                const auto& verts = globalCellToVertices[c];
                for (uint32_t vIdx : verts) {
                    if (vIdx < static_cast<uint32_t>(vCount)) f.sum[vIdx] += val;
                }
            }
        }
    }

    // Normalize sums by contribution count and store as point data.
    for (auto& f : fields) {
        if (f.isVector) {
            for (int i = 0; i < vCount; ++i) {
                if (contributionCounts[i] > 0.0f) {
                    float inv = 1.0f / contributionCounts[i];
                    size_t base = static_cast<size_t>(i) * 3;
                    f.sum[base + 0] *= inv;
                    f.sum[base + 1] *= inv;
                    f.sum[base + 2] *= inv;
                }
            }
            if (mesh.scalarName.empty()) mesh.scalarName = f.name;
            mesh.attributes->pointVectors[f.name] = f.sum;
        } else {
            for (int i = 0; i < vCount; ++i) {
                if (contributionCounts[i] > 0.0f) f.sum[i] /= contributionCounts[i];
            }
            if (mesh.scalarName.empty()) mesh.scalarName = f.name;
            mesh.attributes->pointScalars[f.name] = std::move(f.sum);
        }
    }
}

// ── Cell-edge extraction (ParaView-style wireframe) ─────────────────────────
std::vector<uint32_t> extractTriEdges(
    const std::vector<uint32_t>& indices) {
    if (indices.size() % 3 != 0) return {};

    std::vector<uint64_t> edgeKeys;
    size_t triCount = indices.size() / 3;
    edgeKeys.reserve(triCount * 3);

    for (size_t t = 0; t < triCount; ++t) {
        uint32_t a = indices[t * 3 + 0];
        uint32_t b = indices[t * 3 + 1];
        uint32_t c = indices[t * 3 + 2];
        auto emit = [&](uint32_t x, uint32_t y) {
            if (x > y) std::swap(x, y);
            edgeKeys.push_back((static_cast<uint64_t>(x) << 32) | y);
        };
        emit(a, b);
        emit(b, c);
        emit(c, a);
    }

    std::sort(edgeKeys.begin(), edgeKeys.end());
    auto last = std::unique(edgeKeys.begin(), edgeKeys.end());
    edgeKeys.erase(last, edgeKeys.end());

    std::vector<uint32_t> out;
    out.reserve(edgeKeys.size() * 2);
    for (uint64_t k : edgeKeys) {
        out.push_back(static_cast<uint32_t>((k >> 32) & 0xFFFFFFFF));
        out.push_back(static_cast<uint32_t>(k & 0xFFFFFFFF));
    }
    return out;
}
}
