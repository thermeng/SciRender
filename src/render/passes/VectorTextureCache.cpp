#include "render/passes/VectorTextureCache.h"
#include "core/FieldStore.h"
#include <glad/gl.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <new>
#include <unordered_set>

namespace {

// Hard cap on the number of texels of the LIC vector texture (8M vec3 = 96 MB
// on the CPU). Shared by the cache's expected-dims test and the grid builder:
// if the two disagree, the texture is rebuilt on every frame.
constexpr std::size_t kMaxGridTexels = 8u * 1024u * 1024u;

// Per-axis cap for the LIC vector texture.
// A 2D field (every CFD case) stays at its native resolution up to 512 per
// axis: 512*512*1 texels * 12 B = 3 MB. A 3D field is capped at 64 per axis
// (262k texels) because the resample cost grows with the texel count.
constexpr int kMaxLicDim2D = 512;
constexpr int kMaxLicDim3D = 64;
// Above this many distinct values along an axis the point set is not a grid
// and the exact grid detection is abandoned (the generic resample is used).
constexpr std::size_t kDistinctCoordCap = 4096;

inline std::uint32_t floatBits(float f) {
    std::uint32_t b = 0;
    std::memcpy(&b, &f, sizeof(b));
    if (b == 0x80000000u) return 0u;   // -0.0 -> +0.0 so both hash the same
    return b;
}

// Number of distinct coordinate values along one axis. Hashed on the float
// bits, so it is O(n) and exact for the repeated coordinates of a regular
// grid. Returns cap+1 when the axis carries more distinct values than cap,
// i.e. the point set is not a grid.
template <typename PosAt>
std::size_t distinctCoordCount(PosAt&& posAt, std::size_t n, std::size_t axis, std::size_t cap) {
    std::unordered_set<std::uint32_t> seen;
    seen.reserve(std::min<std::size_t>(n, 2048) * 2 + 16);
    for (std::size_t i = 0; i < n; ++i) {
        seen.insert(floatBits(posAt(i, axis)));
        if (seen.size() > cap) return cap + 1;
    }
    return seen.size();
}

} // namespace

bool VectorTextureCache::resolveField(const RenderMesh& mesh, const std::string& name, int placement,
                                       const glm::vec3*& outData, size_t& outCount, bool& outIsCell,
                                       const postfields::ComputedSnapshot* snap) const {
    int effPlacement = placement;
    if (effPlacement < 0) {
        effPlacement = (mesh.meshHasCellVectors() && !mesh.meshHasVectors()) ? 1 : 0;
    }
    auto p = effPlacement == 1 ? FieldStore::Placement::CellCenter : FieldStore::Placement::Vertex;
    FieldStore::VectorField vf = FieldStore::resolveVector(mesh, name, p, snap);
    // Strict placement: when the UI explicitly asks for Cell placement (1),
    // do NOT silently fall back to point vectors. That would hide a missing
    // cellCenters / cell data error behind a dim/mode mismatch.
    // FieldStore::resolveVector itself falls through to the point field when
    // the cell field is absent, so check isCell, not just presence.
    if (effPlacement == 1) {
        if (!vf.data || vf.count == 0 || !vf.isCell) return false;
    } else if (!vf.data || vf.count == 0) {
        vf = FieldStore::resolveVector(mesh, name, FieldStore::Placement::Vertex, snap);
    }
    if (!vf.data || vf.count == 0) return false;
    outData = vf.data;
    outCount = vf.count;
    outIsCell = vf.isCell;
    return true;
}

VectorTextureCache::GridDims VectorTextureCache::chooseGridDims(const RenderMesh& mesh, size_t count, bool isCell) {
    GridDims d;

    // Source positions of the field: the cell centres for a cell-centred
    // field, the vertices otherwise.
    const glm::vec3* cellPos = nullptr;
    if (isCell) {
        if (mesh.cellCenters.empty() || mesh.cellCenters.size() != count) return d;
        cellPos = mesh.cellCenters.data();
    }
    const float* verts = mesh.vertices.data();
    const std::size_t numVerts = mesh.vertices.size() / 3;
    // A cell-centred field has one entry per CELL: clipping it to the vertex
    // count (cells outnumber vertices on tri/tet meshes) drops most of the
    // domain and leaves the texture empty where no sample survived.
    const std::size_t n = isCell ? count : std::min(numVerts, count);
    if (n == 0) return d;

    auto posAt = [&](std::size_t v, std::size_t axis) -> float {
        if (cellPos) {
            const glm::vec3& c = cellPos[v];
            return axis == 0 ? c.x : (axis == 1 ? c.y : c.z);
        }
        return verts[v * 3 + axis];
    };

    // 1) A regular grid: use the grid's OWN resolution. The distinct
    //    coordinates along an axis are the grid's node counts, so a 41x41
    //    cavity field yields a 41x41 texture - no resampling, no loss of the
    //    small structures.
    const std::size_t nx = distinctCoordCount(posAt, n, 0, kDistinctCoordCap);
    const std::size_t ny = distinctCoordCount(posAt, n, 1, kDistinctCoordCap);
    const std::size_t nz = distinctCoordCount(posAt, n, 2, kDistinctCoordCap);
    if (nx >= 2 && ny >= 2 && nz >= 1 &&
        nx <= kDistinctCoordCap && ny <= kDistinctCoordCap && nz <= kDistinctCoordCap) {
        const long double prod = static_cast<long double>(nx) * static_cast<long double>(ny) *
                                 static_cast<long double>(nz);
        // Grid-like: the distinct coordinates multiply to about the point
        // count (a grid with every node occupied, or a few empty cells).
        if (prod >= static_cast<long double>(n) && prod <= static_cast<long double>(n) * 4.0L) {
            d.x = static_cast<int>(std::min<std::size_t>(nx, kMaxLicDim2D));
            d.y = static_cast<int>(std::min<std::size_t>(ny, kMaxLicDim2D));
            d.z = static_cast<int>(std::min<std::size_t>(nz, kMaxLicDim3D));
            if (d.x > 0 && d.y > 0 && d.z > 0) return d;
        }
    }

    // 2) Generic point set. A SLAB (a 2D field: every CFD case) has sqrt(n)
    //    points per axis, not cbrt(n). Using cbrt for a slab is what reduced
    //    a 41x41 cavity field to a 16x16 texture and smeared the secondary
    //    corner vortices into the primary vortex.
    const float rangeX = mesh.bounds.maxX - mesh.bounds.minX;
    const float rangeY = mesh.bounds.maxY - mesh.bounds.minY;
    const float rangeZ = mesh.bounds.maxZ - mesh.bounds.minZ;
    const float diag = std::sqrt(rangeX * rangeX + rangeY * rangeY + rangeZ * rangeZ);
    const float minRange = std::min(std::min(rangeX, rangeY), rangeZ);
    const bool slab = diag > 0.0f && minRange <= diag * 1e-6f;
    if (slab) {
        int r = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
        r = std::clamp(r, 16, kMaxLicDim2D);
        d.x = r; d.y = r; d.z = 1;
    } else {
        int r = static_cast<int>(std::ceil(std::cbrt(static_cast<double>(n))));
        r = std::clamp(r, 16, kMaxLicDim3D);
        d.x = d.y = d.z = r;
    }
    return d;
}

bool VectorTextureCache::buildGrid(const RenderMesh& mesh, const glm::vec3* data, size_t count, bool isCell,
                                    std::vector<glm::vec3>& outGrid, int& outDimX, int& outDimY, int& outDimZ) const {
    int dimX = mesh.gridDimX, dimY = mesh.gridDimY, dimZ = mesh.gridDimZ;
    const size_t numVerts = mesh.vertices.size() / 3;
    // Cell-centred data is indexed by cell, not by vertex. Never clamp it to
    // numVerts or a tri/tet mesh (cells > vertices) is silently truncated.
    const size_t limit = isCell ? count : std::min(numVerts, count);

    // Non-finite samples would propagate NaN through the whole integration,
    // so they are scrubbed before they reach the texture.
    auto sanitize = [](std::vector<glm::vec3>& grid) {
        for (glm::vec3& v : grid) {
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
                v = glm::vec3(0.0f);
        }
    };

    if (dimX > 0 && dimY > 0 && dimZ > 0 && !isCell) {
        const size_t need = static_cast<size_t>(dimX) * static_cast<size_t>(dimY) * static_cast<size_t>(dimZ);
        if (need == limit && need <= kMaxGridTexels) {
            outGrid.resize(need);
            memcpy(outGrid.data(), data, outGrid.size() * sizeof(glm::vec3));
            sanitize(outGrid);
            outDimX = dimX; outDimY = dimY; outDimZ = dimZ;
            return true;
        }
        // else: fall through to resampled grid; oversized/mismatched structured
        // dims must never allocate blindly (OOM / int-overflow guard)
    }

    // General-purpose cell-grid handling: structured cell count is dim-1; for slab where dim==1 there are no cells along that axis,
    // but we keep 1 to maintain a valid 3D texture (depth 1) until rank-aware 2D path is implemented.
    if (dimX > 0 && dimY > 0 && dimZ > 0 && isCell) {
        int cDimX = (dimX > 1) ? dimX - 1 : 1;
        int cDimY = (dimY > 1) ? dimY - 1 : 1;
        int cDimZ = (dimZ > 1) ? dimZ - 1 : 1;
        const size_t need = static_cast<size_t>(cDimX) * static_cast<size_t>(cDimY) * static_cast<size_t>(cDimZ);
        if (need == count && need <= kMaxGridTexels) {
            outGrid.resize(need);
            memcpy(outGrid.data(), data, outGrid.size() * sizeof(glm::vec3));
            sanitize(outGrid);
            outDimX = cDimX; outDimY = cDimY; outDimZ = cDimZ;
            return true;
        }
    }

    // Resolution from the DATA itself, not from a cubic guess.
    //
    // The old heuristic was ceil(cbrt(numVerts)) clamped to [16, 32]. For a
    // 2D field - which is every CFD case - the per-axis count is sqrt(n), not
    // cbrt(n): a 41x41 cavity field (1681 points) collapsed to a 16x16
    // texture, and the small structures (the secondary corner vortices) were
    // averaged away. chooseGridDims() detects a regular grid exactly and
    // otherwise uses the 2D-aware estimate.
    const GridDims gd = chooseGridDims(mesh, count, isCell);
    if (gd.x <= 0 || gd.y <= 0 || gd.z <= 0) return false;
    size_t total = static_cast<size_t>(gd.x) * gd.y * gd.z;
    if (total == 0 || total > kMaxGridTexels) return false;
    outGrid.assign(total, glm::vec3(0.0f));

    float minX = static_cast<float>(mesh.bounds.minX), maxX = static_cast<float>(mesh.bounds.maxX);
    float minY = static_cast<float>(mesh.bounds.minY), maxY = static_cast<float>(mesh.bounds.maxY);
    float minZ = static_cast<float>(mesh.bounds.minZ), maxZ = static_cast<float>(mesh.bounds.maxZ);
    float rangeX = maxX - minX, rangeY = maxY - minY, rangeZ = maxZ - minZ;

    const float* verts = mesh.vertices.data();
    const glm::vec3* cellPos = nullptr;
    bool useCellCenters = false;
    if (isCell) {
        if (!mesh.cellCenters.empty() && mesh.cellCenters.size() == count) {
            cellPos = mesh.cellCenters.data();
            useCellCenters = true;
        } else {
            return false;
        }
    }

    struct SpatialEntry {
        float x, y, z;
        const glm::vec3* vec;
    };
    // Cap the resample source points so a huge field cannot blow up the
    // render thread. The cap has to scale with the TARGET grid: the spatial
    // hash now has one cell per texel, so the interpolation cost is
    // O(27 * number of source points) - linear in the field, independent of
    // the texture resolution. Subsampling below ~4 points per texel would
    // leave holes in the texture (a texel whose neighbourhood holds no
    // sample), so the floor is tied to the texel count.
    const size_t kMaxResamplePoints = std::max<size_t>(65536, total * 4);
    size_t stride = 1;
    if (limit > kMaxResamplePoints) stride = (limit + kMaxResamplePoints - 1) / kMaxResamplePoints;
    std::vector<SpatialEntry> spatial;
    spatial.reserve((limit + stride - 1) / stride);
    for (size_t v = 0; v < limit; v += stride) {
        SpatialEntry e;
        if (useCellCenters) {
            e.x = cellPos[v].x;
            e.y = cellPos[v].y;
            e.z = cellPos[v].z;
        } else {
            e.x = verts[v * 3 + 0];
            e.y = verts[v * 3 + 1];
            e.z = verts[v * 3 + 2];
        }
        e.vec = &data[v];
        spatial.push_back(e);
    }

    // Spatial hash sized to the TARGET grid, not to a fixed 8^3 cube.
    //
    // The fixed cube made the interpolation radius 1/8 of the DOMAIN: a
    // 41x41 field was smoothed over five cells and every structure smaller
    // than that (the secondary corner vortices) disappeared. With the hash at
    // the texel size the radius is a couple of texels - a local
    // interpolation, not a domain-wide blur.
    const int hashX = std::clamp(gd.x, 1, 256);
    const int hashY = std::clamp(gd.y, 1, 256);
    const int hashZ = std::clamp(gd.z, 1, 32);
    // General-purpose thin-axis handling: use relative epsilon based on scene diag, not absolute 1e-8 world units.
    float diagRange = std::sqrt(rangeX*rangeX + rangeY*rangeY + rangeZ*rangeZ);
    float epsRange = std::max(diagRange * 1e-7f, 1e-7f);
    std::vector<std::vector<size_t>> grid(static_cast<size_t>(hashX) * hashY * hashZ);
    auto hashIndex = [&](int gx, int gy, int gz) -> size_t {
        return (static_cast<size_t>(gx) + static_cast<size_t>(gy) * hashX +
                static_cast<size_t>(gz) * hashX * hashY);
    };
    for (size_t v = 0; v < spatial.size(); ++v) {
        int gx = static_cast<int>(std::clamp((spatial[v].x - minX) / (rangeX > epsRange ? rangeX : epsRange), 0.0f, 0.999f) * hashX);
        int gy = static_cast<int>(std::clamp((spatial[v].y - minY) / (rangeY > epsRange ? rangeY : epsRange), 0.0f, 0.999f) * hashY);
        int gz = static_cast<int>(std::clamp((spatial[v].z - minZ) / (rangeZ > epsRange ? rangeZ : epsRange), 0.0f, 0.999f) * hashZ);
        grid[hashIndex(gx, gy, gz)].push_back(v);
    }

    // Sparse regions (big cells) can leave texels farther than one hash cell
    // from any sample. Such a texel used to become a zero vector; the shader
    // then sees no direction and falls back to raw white noise, which reads as
    // background speckle inside large cells. Precompute, once per hash cell,
    // the nearest populated cell (multi-source BFS) so a texel with no local
    // sample is extrapolated from real data instead of zero.
    const int cellsXY = hashX * hashY;
    std::vector<int> nearestCell(grid.size(), -1);
    std::deque<int> bfs;
    for (size_t c = 0; c < grid.size(); ++c) {
        if (!grid[c].empty()) {
            nearestCell[c] = static_cast<int>(c);
            bfs.push_back(static_cast<int>(c));
        }
    }
    while (!bfs.empty()) {
        const int c = bfs.front();
        bfs.pop_front();
        const int cz = c / cellsXY;
        const int rem = c % cellsXY;
        const int cy = rem / hashX;
        const int cx = rem % hashX;
        for (int dz = -1; dz <= 1; ++dz) {
            const int nz = cz + dz;
            if (nz < 0 || nz >= hashZ) continue;
            for (int dy = -1; dy <= 1; ++dy) {
                const int ny = cy + dy;
                if (ny < 0 || ny >= hashY) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = cx + dx;
                    if (nx < 0 || nx >= hashX) continue;
                    const int nc = static_cast<int>(hashIndex(nx, ny, nz));
                    if (nearestCell[nc] < 0) {
                        nearestCell[nc] = nearestCell[c];
                        bfs.push_back(nc);
                    }
                }
            }
        }
    }

    // Linear resampling: inverse-distance weighted (IDW) average over the
    // 27-cell neighborhood of the spatial hash replaces pure nearest-neighbor
    // Voronoi. Matches Streamline's trilinear philosophy and gives smooth
    // GL_LINEAR interpolation. eps avoids singularity at texel center.
    const float idwEps = 1e-6f * (rangeX*rangeX + rangeY*rangeY + rangeZ*rangeZ + 1.0f);
    auto accumulateIdw = [&](int gx, int gy, int gz, float px, float py, float pz,
                             glm::vec3& acc, float& wSum) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = gx + dx, ny = gy + dy, nz = gz + dz;
                    if (nx < 0 || nx >= hashX || ny < 0 || ny >= hashY || nz < 0 || nz >= hashZ) continue;
                    const auto& cell = grid[hashIndex(nx, ny, nz)];
                    for (size_t v : cell) {
                        const auto& e = spatial[v];
                        float ddx = e.x - px, ddy = e.y - py, ddz = e.z - pz;
                        float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
                        // IDW p=2: w = 1/(d2+eps) ; close samples dominate but neighbors blend
                        float w = 1.0f / (d2 + idwEps);
                        acc += (*e.vec) * w;
                        wSum += w;
                    }
                }
            }
        }
    };
    for (int iz = 0; iz < gd.z; ++iz) {
        for (int iy = 0; iy < gd.y; ++iy) {
            for (int ix = 0; ix < gd.x; ++ix) {
                // Texel centres follow the same convention as the structured
                // fast path (texel i sits at i/(n-1) of the range), so the same
                // field is sampled at the same world positions no matter which
                // path built the texture.
                const float wx = (gd.x > 1) ? static_cast<float>(ix) / static_cast<float>(gd.x - 1) : 0.5f;
                const float wy = (gd.y > 1) ? static_cast<float>(iy) / static_cast<float>(gd.y - 1) : 0.5f;
                const float wz = (gd.z > 1) ? static_cast<float>(iz) / static_cast<float>(gd.z - 1) : 0.5f;
                const float px = minX + wx * rangeX;
                const float py = minY + wy * rangeY;
                const float pz = minZ + wz * rangeZ;

                glm::vec3 acc(0.0f);
                float wSum = 0.0f;
                int gx = static_cast<int>(std::clamp((px - minX) / (rangeX > epsRange ? rangeX : epsRange), 0.0f, 0.999f) * hashX);
                int gy = static_cast<int>(std::clamp((py - minY) / (rangeY > epsRange ? rangeY : epsRange), 0.0f, 0.999f) * hashY);
                int gz = static_cast<int>(std::clamp((pz - minZ) / (rangeZ > epsRange ? rangeZ : epsRange), 0.0f, 0.999f) * hashZ);
                accumulateIdw(gx, gy, gz, px, py, pz, acc, wSum);
                if (wSum <= 0.0f) {
                    // No sample in the local 27-cell block: extrapolate from the
                    // nearest populated block. This is what keeps large cells
                    // filled with a smooth direction instead of zero/noise.
                    const int near = nearestCell[hashIndex(gx, gy, gz)];
                    if (near >= 0) {
                        const int ncz = near / cellsXY;
                        const int nrem = near % cellsXY;
                        accumulateIdw(nrem % hashX, nrem / hashX, ncz, px, py, pz, acc, wSum);
                    }
                }
                const size_t o = static_cast<size_t>(ix) + static_cast<size_t>(iy) * gd.x +
                                 static_cast<size_t>(iz) * gd.x * gd.y;
                glm::vec3 texel = (wSum > 0.0f) ? (acc / wSum) : glm::vec3(0.0f);
                if (!std::isfinite(texel.x) || !std::isfinite(texel.y) || !std::isfinite(texel.z))
                    texel = glm::vec3(0.0f);
                outGrid[o] = texel;
            }
        }
    }
    outDimX = gd.x; outDimY = gd.y; outDimZ = gd.z;
    return true;
}

std::string VectorTextureCache::makeKey(const std::string& name, bool isCell) {
    // Canonical key: no boundary mode (always Repeat). Legacy "#B*" suffix dropped.
    return name + (isCell ? "#C" : "#P");
}
std::string VectorTextureCache::makeKey(const std::string& name, bool isCell, int /*licBoundaryMode*/) {
    return makeKey(name, isCell);
}

void VectorTextureCache::touch(Entry& e, const std::string& key) {
    if (e.hasLru) {
        m_lru.erase(e.lruIt);
        e.hasLru = false;
    }
    m_lru.push_front(key);
    e.lruIt = m_lru.begin();
    e.hasLru = true;
}

void VectorTextureCache::evictIfNeeded() {
    // hasLru keeps the list and the map in sync even if an allocation throws
    // between entry insertion and the LRU update, so never pop an empty list.
    while (m_entries.size() > kMaxEntries && !m_lru.empty()) {
        const std::string oldest = m_lru.back();
        m_entries.erase(oldest);
        m_lru.pop_back();
    }
}

GLuint VectorTextureCache::textureForField(const std::string& name, const RenderMesh* mesh,
                                             const glm::vec3& boxMin, const glm::vec3& boxMax,
                                             int placement, int licBoundaryMode,
                                             const postfields::ComputedSnapshot* snap) {
    if (!mesh || name.empty()) return 0;

    const glm::vec3* data = nullptr;
    size_t count = 0;
    bool isCell = false;
    if (!resolveField(*mesh, name, placement, data, count, isCell, snap)) return 0;

    // Only the cheap, exact structured-grid test runs per frame. The expensive
    // data-driven grid detection (chooseGridDims scans every source position
    // three times) stays in buildGrid, i.e. it runs only on a cache miss.
    const int dimX = mesh->gridDimX, dimY = mesh->gridDimY, dimZ = mesh->gridDimZ;
    const size_t numVerts = mesh->vertices.size() / 3;
    const size_t limit = isCell ? count : std::min(numVerts, count);
    int expectedDimX = 0, expectedDimY = 0, expectedDimZ = 0;
    if (!isCell && dimX > 0 && dimY > 0 && dimZ > 0) {
        const size_t need = static_cast<size_t>(dimX) * static_cast<size_t>(dimY) * static_cast<size_t>(dimZ);
        // The builder refuses structured grids above kMaxGridTexels and falls
        // back to chooseGridDims; mirror that here or the entry can never match.
        if (need == limit && need <= kMaxGridTexels) {
            expectedDimX = dimX; expectedDimY = dimY; expectedDimZ = dimZ;
        }
    } else if (isCell && dimX > 0 && dimY > 0 && dimZ > 0) {
        int cDimX = (dimX > 1) ? dimX - 1 : 1;
        int cDimY = (dimY > 1) ? dimY - 1 : 1;
        int cDimZ = (dimZ > 1) ? dimZ - 1 : 1;
        const size_t need = static_cast<size_t>(cDimX) * static_cast<size_t>(cDimY) * static_cast<size_t>(cDimZ);
        if (need == count && need <= kMaxGridTexels) {
            expectedDimX = cDimX; expectedDimY = cDimY; expectedDimZ = cDimZ;
        }
    }

    // Wrap mode is fixed to Clamp (GL_CLAMP_TO_EDGE); traces that leave the
    // domain terminate instead of wrapping to the opposite boundary.
    const std::string key = makeKey(name, isCell);
    auto it = m_entries.find(key);
    if (it != m_entries.end() && it->second.tex.has()) {
        // Structured fields know their dims cheaply, so verify them. For an
        // unstructured field the entry is authoritative: content changes are
        // handled by invalidateAll(), never by re-scanning every frame.
        const bool dimsMatch = expectedDimX <= 0 ||
            (it->second.dimX == expectedDimX && it->second.dimY == expectedDimY &&
             it->second.dimZ == expectedDimZ);
        if (dimsMatch) {
            try { touch(it->second, key); }
            catch (const std::bad_alloc&) { /* LRU is best-effort; the texture is still valid */ }
            return it->second.tex.get();
        }
    }

    return buildTexture(name, mesh, boxMin, boxMax, placement, licBoundaryMode, snap);
}

GLuint VectorTextureCache::buildTexture(const std::string& name, const RenderMesh* mesh,
                                         const glm::vec3& boxMin, const glm::vec3& boxMax,
                                         int placement, int licBoundaryMode,
                                         const postfields::ComputedSnapshot* snap) {
    (void)boxMin; (void)boxMax; (void)placement;
    const glm::vec3* data = nullptr;
    size_t count = 0;
    bool isCell = false;
    if (!resolveField(*mesh, name, placement, data, count, isCell, snap)) return 0;

    std::vector<glm::vec3> grid;
    int dimX = 0, dimY = 0, dimZ = 0;
    try {
        if (!buildGrid(*mesh, data, count, isCell, grid, dimX, dimY, dimZ)) return 0;
    } catch (const std::bad_alloc&) {
        return 0;   // out of memory: LIC degrades to the normal surface draw
    }

    int texDimX = dimX, texDimY = dimY, texDimZ = dimZ;
    const glm::vec3* texData = grid.data();
    const int wrapMode = GL_CLAMP_TO_EDGE; // fixed: Clamp; a trace that leaves the domain stops
    (void)licBoundaryMode; // deprecated — ignored, kept for API compat

    if (texDimX <= 0 || texDimY <= 0 || texDimZ <= 0) return 0;
    {
        const size_t need =
            static_cast<size_t>(texDimX) * static_cast<size_t>(texDimY) * static_cast<size_t>(texDimZ);
        if (need == 0 || need > kMaxGridTexels) return 0;
    }

    GLuint raw = 0;
    glCreateTextures(GL_TEXTURE_3D, 1, &raw);
    if (raw == 0) return 0;
    // Own the texture from here on: any throw below deletes it instead of
    // leaking the GL object.
    GlTexture pending(raw);
    glTextureStorage3D(pending, 1, GL_RGB32F, texDimX, texDimY, texDimZ);
    glTextureParameteri(pending, GL_TEXTURE_WRAP_S, wrapMode);
    glTextureParameteri(pending, GL_TEXTURE_WRAP_T, wrapMode);
    glTextureParameteri(pending, GL_TEXTURE_WRAP_R, wrapMode);
    glTextureParameteri(pending, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(pending, GL_TEXTURE_MAG_FILTER, GL_LINEAR);


    size_t bytes = static_cast<size_t>(texDimX) * texDimY * texDimZ * 3 * sizeof(float);
    if (!m_pboInitialized) {
        for (int i = 0; i < 2; ++i)
            if (!m_pbo[i].has()) glCreateBuffers(1, m_pbo[i].ptr());
        m_pboInitialized = true;
    }
    if (m_pbo[0].has() && bytes > 0) {
        GlBuffer& pbo = m_pbo[m_pboIndex];
        m_pboIndex = (m_pboIndex + 1) % 2;
        glNamedBufferData(pbo, bytes, nullptr, GL_STREAM_DRAW);
        void* ptr = glMapNamedBufferRange(pbo, 0, bytes, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        if (ptr) {
            std::memcpy(ptr, texData, bytes);
            glUnmapNamedBuffer(pbo);
            GLint prevPbo = 0;
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prevPbo);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo.get());
            glTextureSubImage3D(pending, 0, 0, 0, 0, texDimX, texDimY, texDimZ, GL_RGB, GL_FLOAT, nullptr);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(prevPbo));
        } else {
            glTextureSubImage3D(pending, 0, 0, 0, 0, texDimX, texDimY, texDimZ, GL_RGB, GL_FLOAT, texData);
        }
    } else {
        glTextureSubImage3D(pending, 0, 0, 0, 0, texDimX, texDimY, texDimZ, GL_RGB, GL_FLOAT, texData);
    }

    // Strongest vector of the sampled field: the LIC uses it to make its
    // "no direction here" cut-off relative to the field, so a flow in mm/s
    // behaves like a flow in m/s.
    float maxMag = 0.0f;
    for (const glm::vec3& v : grid) maxMag = std::max(maxMag, glm::length(v));

    const std::string key = makeKey(name, isCell);
    try {
        auto existing = m_entries.find(key);
        if (existing != m_entries.end() && existing->second.hasLru) {
            m_lru.erase(existing->second.lruIt);
            existing->second.hasLru = false;
        }
        const GLuint texId = pending.get();
        Entry& e = m_entries[key];
        e.tex = std::move(pending);   // deletes the previous texture, if any
        e.dimX = texDimX; e.dimY = texDimY; e.dimZ = texDimZ;
        e.maxMagnitude = maxMag;
        m_lru.push_front(key);
        e.lruIt = m_lru.begin();
        e.hasLru = true;
        evictIfNeeded();
        return texId;
    } catch (const std::bad_alloc&) {
        return 0;   // `pending` still owns the texture and deletes it
    }
}

bool VectorTextureCache::getTextureDims(const std::string& name, int& dimX, int& dimY, int& dimZ) const {
    auto it = m_entries.find(name);
    if (it != m_entries.end()) {
        dimX = it->second.dimX;
        dimY = it->second.dimY;
        dimZ = it->second.dimZ;
        return true;
    }
    // Scan over isCell variants (no boundary mode). Keep prefix-safe exact match.
    for (const auto& key : m_lru) {
        for (int cell = 0; cell <= 1; ++cell) {
            if (key == makeKey(name, cell != 0)) {
                auto jt = m_entries.find(key);
                if (jt != m_entries.end()) {
                    dimX = jt->second.dimX;
                    dimY = jt->second.dimY;
                    dimZ = jt->second.dimZ;
                    return true;
                }
            }
        }
    }
    return false;
}

bool VectorTextureCache::getTextureDims(const std::string& name, int /*licBoundaryMode*/, int& dimX, int& dimY, int& dimZ) const {
    // Deprecated mode param ignored — delegate to canonical.
    return getTextureDims(name, dimX, dimY, dimZ);
}

float VectorTextureCache::getMaxMagnitude(const std::string& name, int placement, const RenderMesh* mesh,
                                          const postfields::ComputedSnapshot* snap) const {
    if (mesh) {
        const glm::vec3* d = nullptr;
        size_t cnt = 0;
        bool isCell = false;
        if (resolveField(*mesh, name, placement, d, cnt, isCell, snap)) {
            const std::string key = makeKey(name, isCell);
            auto it = m_entries.find(key);
            if (it != m_entries.end()) return it->second.maxMagnitude;
        }
    }
    for (const auto& key : m_lru) {
        for (int cell = 0; cell <= 1; ++cell) {
            if (key == makeKey(name, cell != 0)) {
                auto jt = m_entries.find(key);
                if (jt != m_entries.end()) return jt->second.maxMagnitude;
            }
        }
    }
    return 0.0f;
}

bool VectorTextureCache::getTextureDims(const std::string& name, int placement, int /*licBoundaryMode*/, const RenderMesh* mesh, int& dimX, int& dimY, int& dimZ,
                                        const postfields::ComputedSnapshot* snap) const {
    if (mesh) {
        const glm::vec3* d = nullptr;
        size_t cnt = 0;
        bool isCell = false;
        if (resolveField(*mesh, name, placement, d, cnt, isCell, snap)) {
            const std::string key = makeKey(name, isCell);
            auto it = m_entries.find(key);
            if (it != m_entries.end()) {
                dimX = it->second.dimX;
                dimY = it->second.dimY;
                dimZ = it->second.dimZ;
                return true;
            }
        }
    }
    return getTextureDims(name, dimX, dimY, dimZ);
}

void VectorTextureCache::invalidate(const std::string& name) {
    for (int cell = 0; cell <= 1; ++cell) {
        const std::string k = makeKey(name, cell != 0);
        auto it = m_entries.find(k);
        if (it != m_entries.end()) {
            if (it->second.hasLru)
                m_lru.erase(it->second.lruIt);
            m_entries.erase(it);
        }
        // Drop legacy "#B*" keys if present (old cache entries)
        for (int mode = 0; mode <= 2; ++mode) {
            const std::string legacy = name + (cell ? "#C" : "#P") + "#K" + "#B" + std::to_string(mode);
            auto jt = m_entries.find(legacy);
            if (jt != m_entries.end()) {
                if (jt->second.hasLru)
                    m_lru.erase(jt->second.lruIt);
                m_entries.erase(jt);
            }
        }
    }
    // Also drop a legacy bare-name entry if present.
    auto bare = m_entries.find(name);
    if (bare != m_entries.end()) {
        if (bare->second.hasLru)
            m_lru.erase(bare->second.lruIt);
        m_entries.erase(bare);
    }
}

void VectorTextureCache::invalidateAll() {
    m_entries.clear();
    m_lru.clear();
    for (int i = 0; i < 2; ++i) if (m_pbo[i].has()) m_pbo[i].reset();
    m_pboInitialized = false;
}

void VectorTextureCache::shutdown() {
    invalidateAll();
    for (int i = 0; i < 2; ++i) if (m_pbo[i].has()) m_pbo[i].reset();
    m_pboInitialized = false;
}