#include "core/mesh_loader.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

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

void byteSwap(float* val) {
    char* bytes = reinterpret_cast<char*>(val);
    std::swap(bytes[0], bytes[3]);
    std::swap(bytes[1], bytes[2]);
}

void byteSwap(int* val) {
    char* bytes = reinterpret_cast<char*>(val);
    std::swap(bytes[0], bytes[3]);
    std::swap(bytes[1], bytes[2]);
}

bool isLittleEndian() {
    int test = 1;
    return *(reinterpret_cast<char*>(&test)) == 1;
}

// ── Geometry utilities ──────────────────────────────────────────────────────

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
        // first face; a subsequent face joins the current group only if its normal
        // dot-product with every already-accepted face in that group stays >=
        // SHARP_ANGLE_COS. Otherwise start a new group. This honors the intended
        // ~25 deg smooth threshold exactly (a sphere's near-tangent faces all land in
        // one group -> no split -> smooth shading; a cube corner's 90 deg faces stay
        // in separate groups -> split).
        std::vector<int> groupOf(fEnd - fStart, 0); // face slot -> group id
        int numGroups = 1;
        for (int fi = fStart + 1; fi < fEnd; fi++) {
            int f = adjFaces[fi];
            bool merged = false;
            for (int g = 0; g < numGroups; g++) {
                bool ok = true;
                for (int fj = fStart; fj < fi; fj++) {
                    if (groupOf[fj - fStart] != g) continue;
                    int of = adjFaces[fj];
                    if (dot3(faceNormals[f], faceNormals[of]) < SHARP_ANGLE_COS) {
                        ok = false;
                        break;
                    }
                }
                if (ok) { groupOf[fi - fStart] = g; merged = true; break; }
            }
            if (!merged) groupOf[fi - fStart] = numGroups++;
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

    // Sync per-point vectors the same way: a duplicated vertex must carry the
    // same vector as its source, otherwise glyph code indexed by vertex count
    // would read past the end of the (smaller) vector arrays and crash.
    if (!mesh.pointVectors.empty()) {
        size_t newVertCount = newVertices.size() / 3;
        std::map<std::string, std::vector<float>> newPointVectors;
        for (auto& [name, vecArr] : mesh.pointVectors) {
            std::vector<float> newVec(newVertCount * 3, 0.0f);
            for (size_t oldV = 0; oldV < numVerts; ++oldV) {
                float vx = 0.0f, vy = 0.0f, vz = 0.0f;
                if (oldV * 3 + 2 < vecArr.size()) {
                    vx = vecArr[oldV * 3 + 0];
                    vy = vecArr[oldV * 3 + 1];
                    vz = vecArr[oldV * 3 + 2];
                }
                for (int newV : vertexRemap[oldV]) {
                    if (static_cast<size_t>(newV) < newVertCount) {
                        newVec[newV * 3 + 0] = vx;
                        newVec[newV * 3 + 1] = vy;
                        newVec[newV * 3 + 2] = vz;
                    }
                }
            }
            newPointVectors[name] = std::move(newVec);
        }
        mesh.pointVectors = std::move(newPointVectors);
    }

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
    if (!mesh.pointVectors.empty()) {
        for (auto& [name, vecArr] : mesh.pointVectors) {
            if (vecArr.size() != finalVerts * 3) {
                vecArr.resize(finalVerts * 3, 0.0f);
            }
        }
    }

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

} // namespace mesh_utils
