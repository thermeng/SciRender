#include "core/unstructured_volume.h"

#include "core/cell_topology.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace volume {

namespace {

using dvec3 = glm::dvec3;

constexpr double kBoundsEps = 1e-12;   // relative to the longest extent
constexpr double kInsideTol = -1e-6;   // normalized barycentric tolerance

double det3(const dvec3& a, const dvec3& b, const dvec3& c) {
    return glm::dot(a, glm::cross(b, c));
}

bool finite3(const dvec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Barycentric weights of `x` with respect to the tetrahedron p0..p3. Returns
// false for a degenerate (zero-volume) tet.
bool barycentric(const dvec3 p[4], const dvec3& x, double w[4]) {
    const dvec3 e1 = p[0] - p[3];
    const dvec3 e2 = p[1] - p[3];
    const dvec3 e3 = p[2] - p[3];
    const double det = det3(e1, e2, e3);
    if (det == 0.0) return false;

    const dvec3 r = x - p[3];
    const double inv = 1.0 / det;
    w[0] = det3(r, e2, e3) * inv;
    w[1] = det3(e1, r, e3) * inv;
    w[2] = det3(e1, e2, r) * inv;
    w[3] = 1.0 - w[0] - w[1] - w[2];
    return true;
}

bool boundsOf(const RenderMesh& mesh, dvec3& lo, dvec3& hi) {
    const auto& b = mesh.bounds;
    if (b.maxX >= b.minX && b.maxY >= b.minY && b.maxZ >= b.minZ &&
        (b.maxX > b.minX || b.maxY > b.minY || b.maxZ > b.minZ)) {
        lo = dvec3(b.minX, b.minY, b.minZ);
        hi = dvec3(b.maxX, b.maxY, b.maxZ);
        return true;
    }

    // Fallback for meshes whose parser did not fill `bounds` (e.g. a test
    // fixture). Eligibility is rare enough that the O(n) scan is fine.
    if (mesh.vertices.size() < 3) return false;
    lo = dvec3(std::numeric_limits<double>::max());
    hi = dvec3(-std::numeric_limits<double>::max());
    for (std::size_t i = 0; i + 2 < mesh.vertices.size(); i += 3) {
        const dvec3 v(mesh.vertices[i], mesh.vertices[i + 1], mesh.vertices[i + 2]);
        if (!finite3(v)) continue;
        lo = glm::min(lo, v);
        hi = glm::max(hi, v);
    }
    return hi.x >= lo.x && hi.y >= lo.y && hi.z >= lo.z;
}

// True when the CSR in `mesh` is self-consistent enough to index cells.
bool topologyLooksValid(const RenderMesh& mesh) {
    if (!mesh.hasUnstructuredTopology()) return false;
    if (mesh.cellTypes.size() != mesh.cellOffsets.size()) return false;
    if (mesh.cellOffsets.empty()) return false;
    // cellOffsets is an exclusive-end offset array: monotonically increasing
    // and bounded by the connectivity size.
    std::uint32_t prev = 0;
    for (std::uint32_t end : mesh.cellOffsets) {
        if (end < prev) return false;
        if (end > mesh.cellConnectivity.size()) return false;
        prev = end;
    }
    return true;
}

} // namespace

bool eligible(const RenderMesh& mesh) {
    if (mesh.vertices.empty()) return false;
    if (!mesh.hasUnstructuredVolumeCells()) return false;
    if (!topologyLooksValid(mesh)) return false;

    dvec3 lo, hi;
    if (!boundsOf(mesh, lo, hi)) return false;

    const dvec3 extent = hi - lo;
    const double longest = std::max({extent.x, extent.y, extent.z});
    if (!(longest > 0.0) || !std::isfinite(longest)) return false;
    const double eps = longest * kBoundsEps;
    return extent.x > eps && extent.y > eps && extent.z > eps;
}

bool sourcePointValues(const RenderMesh& mesh, const std::vector<float>& values,
                       std::vector<float>& scratch,
                       const std::vector<float>*& out) {
    out = nullptr;
    const std::size_t nFinal = mesh.vertices.size() / 3;
    const std::size_t nSrc = mesh.sourcePointCount > 0
        ? static_cast<std::size_t>(mesh.sourcePointCount)
        : nFinal;
    if (nSrc == 0) return false;

    if (values.size() == nSrc) {
        out = &values;
        return true;
    }
    if (values.size() == nFinal && mesh.vertexSourceIndex.size() == nFinal) {
        scratch.assign(nSrc, 0.0f);
        for (std::size_t i = 0; i < nFinal; ++i) {
            const std::uint32_t src = mesh.vertexSourceIndex[i];
            if (src < nSrc) scratch[src] = values[i];
        }
        out = &scratch;
        return true;
    }
    if (values.size() >= nSrc) {
        out = &values;
        return true;
    }
    return false;
}

bool build(const RenderMesh& mesh, const BuildOptions& options, DerivedGrid& out) {
    out = DerivedGrid{};
    if (!eligible(mesh)) return false;

    dvec3 lo, hi;
    if (!boundsOf(mesh, lo, hi)) return false;

    const dvec3 extent = hi - lo;
    const double longest = std::max({extent.x, extent.y, extent.z});
    const int maxDim = std::max(2, options.maxDim);
    const std::size_t maxVoxels =
        std::max<std::size_t>(8, options.maxVoxels);

    // Node-centred dims: one voxel per unit of `spacing`, plus one so the last
    // voxel centre stays inside the box.
    double spacing = longest / static_cast<double>(maxDim);
    int dims[3] = {2, 2, 2};
    for (;;) {
        std::size_t total = 1;
        for (int a = 0; a < 3; ++a) {
            const double f = std::ceil(extent[a] / spacing) + 1.0;
            // Clamp in double first: a tiny spacing (huge maxDim) would
            // overflow the int cast before the clamp could apply.
            const double d = std::clamp(f, 2.0, static_cast<double>(maxDim));
            dims[a] = static_cast<int>(d);
            total *= static_cast<std::size_t>(dims[a]);
        }
        if (total <= maxVoxels) break;
        spacing *= 1.25;
        if (!(spacing > 0.0) || !std::isfinite(spacing)) return false;
    }

    const std::size_t nVoxels = static_cast<std::size_t>(dims[0])
                              * static_cast<std::size_t>(dims[1])
                              * static_cast<std::size_t>(dims[2]);

    out.dimX = dims[0];
    out.dimY = dims[1];
    out.dimZ = dims[2];
    out.origin = lo;
    out.extent = extent;
    out.cellId.assign(nVoxels, DerivedGrid::kEmpty);
    out.tet.assign(nVoxels, 0);

    const std::size_t nCells = mesh.cellOffsets.size();
    const double step[3] = {
        extent.x / static_cast<double>(dims[0]),
        extent.y / static_cast<double>(dims[1]),
        extent.z / static_cast<double>(dims[2]),
    };
    const double loArr[3] = {lo.x, lo.y, lo.z};
    const double stepArr[3] = {step[0], step[1], step[2]};
    const std::size_t strideY = static_cast<std::size_t>(dims[0]);
    const std::size_t strideZ = strideY * static_cast<std::size_t>(dims[1]);

    const float* verts = mesh.vertices.data();
    const std::size_t vertexCount = mesh.vertices.size() / 3;

    std::vector<std::array<std::uint32_t, 4>> tets;
    std::vector<std::uint32_t> cornerIds;

    for (std::size_t c = 0; c < nCells; ++c) {
        const std::uint8_t type = mesh.cellTypes[c];
        if (!cell_topology::isVolumeCell(type)) continue;

        const std::uint32_t begin = (c == 0) ? 0u : mesh.cellOffsets[c - 1];
        const std::uint32_t end = mesh.cellOffsets[c];
        const int corners = cell_topology::cornerCount(type);
        if (end <= begin) continue;
        if (static_cast<std::size_t>(end - begin) < static_cast<std::size_t>(corners)) continue;

        cornerIds.assign(mesh.cellConnectivity.begin() + begin,
                         mesh.cellConnectivity.begin() + begin + corners);

        // Skip cells with an out-of-range or non-finite corner.
        bool ok = true;
        dvec3 centre(0.0);
        for (int k = 0; k < corners; ++k) {
            const std::uint32_t id = cornerIds[static_cast<std::size_t>(k)];
            if (id >= vertexCount) { ok = false; break; }
            const dvec3 p(verts[id * 3], verts[id * 3 + 1], verts[id * 3 + 2]);
            if (!finite3(p)) { ok = false; break; }
            centre += p;
        }
        if (!ok) continue;
        centre /= static_cast<double>(corners);

        tets.clear();
        cell_topology::tets(type, std::span<const std::uint32_t>(cornerIds), tets);
        if (tets.empty()) continue;

        // Scale-relative volume floor: degenerate/sliver tets are skipped
        // rather than rasterized with an ill-conditioned barycentric solve.
        double scale3 = 1.0;
        {
            const dvec3 p0(verts[cornerIds[0] * 3], verts[cornerIds[0] * 3 + 1],
                           verts[cornerIds[0] * 3 + 2]);
            double maxDist = 0.0;
            for (int k = 1; k < corners; ++k) {
                const std::uint32_t id = cornerIds[static_cast<std::size_t>(k)];
                maxDist = std::max(maxDist, glm::length(dvec3(verts[id * 3],
                                                              verts[id * 3 + 1],
                                                              verts[id * 3 + 2]) - p0));
            }
            if (maxDist > 0.0) scale3 = maxDist * maxDist * maxDist;
        }
        const double volumeEps = scale3 * 1e-12;

        bool touched = false;

        for (std::size_t t = 0; t < tets.size(); ++t) {
            dvec3 p[4];
            for (int k = 0; k < 4; ++k) {
                const std::uint32_t slot = tets[t][static_cast<std::size_t>(k)];
                if (slot < static_cast<std::uint32_t>(corners)) {
                    const std::uint32_t id = cornerIds[static_cast<std::size_t>(slot)];
                    p[k] = dvec3(verts[id * 3], verts[id * 3 + 1], verts[id * 3 + 2]);
                } else {
                    p[k] = centre; // centroid slot
                }
            }

            const dvec3 e1 = p[0] - p[3];
            const dvec3 e2 = p[1] - p[3];
            const dvec3 e3 = p[2] - p[3];
            if (std::fabs(det3(e1, e2, e3)) <= volumeEps) continue;

            dvec3 tmin = glm::min(glm::min(p[0], p[1]), glm::min(p[2], p[3]));
            dvec3 tmax = glm::max(glm::max(p[0], p[1]), glm::max(p[2], p[3]));

            int i0[3], i1[3];
            for (int a = 0; a < 3; ++a) {
                const double f0 = (tmin[a] - loArr[a]) / stepArr[a] - 0.5;
                const double f1 = (tmax[a] - loArr[a]) / stepArr[a] - 0.5;
                i0[a] = std::clamp(static_cast<int>(std::floor(f0)), 0, dims[a] - 1);
                i1[a] = std::clamp(static_cast<int>(std::ceil(f1)), 0, dims[a] - 1);
            }

            for (int z = i0[2]; z <= i1[2]; ++z) {
                const double cz = loArr[2] + (z + 0.5) * stepArr[2];
                for (int y = i0[1]; y <= i1[1]; ++y) {
                    const double cy = loArr[1] + (y + 0.5) * stepArr[1];
                    const std::size_t rowBase = static_cast<std::size_t>(z) * strideZ
                                              + static_cast<std::size_t>(y) * strideY;
                    for (int x = i0[0]; x <= i1[0]; ++x) {
                        const dvec3 point(loArr[0] + (x + 0.5) * stepArr[0], cy, cz);
                        double w[4];
                        if (!barycentric(p, point, w)) continue;
                        if (w[0] < kInsideTol || w[1] < kInsideTol ||
                            w[2] < kInsideTol || w[3] < kInsideTol) {
                            continue;
                        }
                        const std::size_t idx = rowBase + static_cast<std::size_t>(x);
                        out.cellId[idx] = static_cast<std::uint32_t>(c);
                        out.tet[idx] = static_cast<std::uint8_t>(
                            std::min<std::size_t>(t, 255));
                        touched = true;
                    }
                }
            }
        }

        // Sub-voxel cells must not vanish: pin the cell centre to the voxel
        // that contains it (last writer wins, like shared boundaries).
        if (!touched) {
            int vx = 0, vy = 0, vz = 0;
            const double cc[3] = {centre.x, centre.y, centre.z};
            const int v[3] = {dims[0], dims[1], dims[2]};
            int idx3[3];
            for (int a = 0; a < 3; ++a) {
                idx3[a] = std::clamp(static_cast<int>((cc[a] - loArr[a]) / stepArr[a]),
                                     0, v[a] - 1);
            }
            vx = idx3[0]; vy = idx3[1]; vz = idx3[2];
            const std::size_t idx = static_cast<std::size_t>(vx)
                                  + static_cast<std::size_t>(vy) * strideY
                                  + static_cast<std::size_t>(vz) * strideZ;
            out.cellId[idx] = static_cast<std::uint32_t>(c);
            out.tet[idx] = 0;
        }
    }

    // Group voxels by cell (counting sort) so scatter can tetrahedralize a
    // cell once instead of once per voxel.
    out.cellVoxelStart.assign(nCells + 1, 0);
    for (std::uint32_t id : out.cellId) {
        if (id != DerivedGrid::kEmpty && id < nCells) ++out.cellVoxelStart[id + 1];
    }
    for (std::size_t c = 1; c <= nCells; ++c) {
        out.cellVoxelStart[c] += out.cellVoxelStart[c - 1];
    }
    out.cellVoxels.resize(out.cellVoxelStart[nCells]);
    {
        std::vector<std::uint32_t> cursor(out.cellVoxelStart.begin(),
                                          out.cellVoxelStart.end() - 1);
        for (std::size_t i = 0; i < out.cellId.size(); ++i) {
            const std::uint32_t id = out.cellId[i];
            if (id == DerivedGrid::kEmpty || id >= nCells) continue;
            out.cellVoxels[cursor[id]++] = static_cast<std::uint32_t>(i);
        }
    }

    return true;
}

void scatter(const RenderMesh& mesh, const DerivedGrid& grid,
             const std::vector<float>* values, bool cellPlacement,
             ScalarGrid& out) {
    out = ScalarGrid{};
    if (grid.empty() || values == nullptr) return;

    const std::size_t nVoxels = grid.voxelCount();
    out.value.assign(nVoxels, 0.0f);
    out.occupancy.assign(nVoxels, 0);
    for (int a = 0; a < 3; ++a) {
        out.planeMin[static_cast<std::size_t>(a)].assign(
            static_cast<std::size_t>(grid.dim(a)),
            std::numeric_limits<float>::infinity());
        out.planeMax[static_cast<std::size_t>(a)].assign(
            static_cast<std::size_t>(grid.dim(a)),
            -std::numeric_limits<float>::infinity());
    }

    if (grid.cellVoxelStart.size() < 2) return;
    const std::size_t nCells = grid.cellVoxelStart.size() - 1;

    // Resolve the value array. Point data lives in the pre-split vertex space
    // that cellConnectivity indexes; the result builder stores the post-split
    // (one entry per rendered vertex) copy, so remap it once when needed.
    std::vector<float> sourceValues;
    const std::vector<float>* cellValues = nullptr;
    const std::vector<float>* pointValues = nullptr;
    if (cellPlacement) {
        if (values->size() < nCells) return;
        cellValues = values;
    } else {
        const std::vector<float>* resolved = nullptr;
        if (!sourcePointValues(mesh, *values, sourceValues, resolved) || !resolved) return;
        pointValues = resolved;
    }

    const float* verts = mesh.vertices.data();
    const std::size_t vertexCount = mesh.vertices.size() / 3;
    const double stepArr[3] = {
        grid.extent.x / static_cast<double>(grid.dimX),
        grid.extent.y / static_cast<double>(grid.dimY),
        grid.extent.z / static_cast<double>(grid.dimZ),
    };
    const double loArr[3] = {grid.origin.x, grid.origin.y, grid.origin.z};

    std::vector<std::array<std::uint32_t, 4>> tets;

    for (std::size_t c = 0; c < nCells; ++c) {
        const std::uint32_t begin = (c == 0) ? 0u : mesh.cellOffsets[c - 1];
        const std::uint32_t end = mesh.cellOffsets[c];
        if (end <= begin) continue;

        const std::uint32_t vLo = grid.cellVoxelStart[c];
        const std::uint32_t vHi = grid.cellVoxelStart[c + 1];
        if (vLo >= vHi) continue;

        if (cellValues) {
            const float val = (*cellValues)[c];
            if (!std::isfinite(val)) continue;
            for (std::uint32_t k = vLo; k < vHi; ++k) {
                const std::uint32_t voxel = grid.cellVoxels[k];
                out.value[voxel] = val;
                out.occupancy[voxel] = 1;
            }
            continue;
        }

        const std::uint8_t type = mesh.cellTypes[c];
        if (!cell_topology::isVolumeCell(type)) continue;
        const int corners = cell_topology::cornerCount(type);
        if (static_cast<std::size_t>(end - begin) < static_cast<std::size_t>(corners)) continue;

        // 8 corners + the centroid slot.
        dvec3 cp[9];
        float cv[9];
        bool ok = true;
        for (int k = 0; k < corners; ++k) {
            const std::uint32_t id = mesh.cellConnectivity[begin + static_cast<std::size_t>(k)];
            if (id >= vertexCount || id >= pointValues->size()) { ok = false; break; }
            cp[k] = dvec3(verts[id * 3], verts[id * 3 + 1], verts[id * 3 + 2]);
            cv[k] = (*pointValues)[id];
        }
        if (!ok) continue;

        dvec3 centre(0.0);
        double centreValue = 0.0;
        for (int k = 0; k < corners; ++k) {
            centre += cp[k];
            centreValue += static_cast<double>(cv[k]);
        }
        centre /= static_cast<double>(corners);
        centreValue /= static_cast<double>(corners);
        cp[corners] = centre;
        cv[corners] = static_cast<float>(centreValue);

        tets.clear();
        cell_topology::tets(type,
            std::span<const std::uint32_t>(mesh.cellConnectivity.data() + begin,
                                           static_cast<std::size_t>(corners)),
            tets);
        if (tets.empty()) continue;

        for (std::uint32_t k = vLo; k < vHi; ++k) {
            const std::uint32_t voxel = grid.cellVoxels[k];
            std::size_t t = grid.tet[voxel];
            if (t >= tets.size()) t = 0; // pinned sub-voxel cell centre

            dvec3 p[4];
            double tv[4];
            for (int j = 0; j < 4; ++j) {
                const std::uint32_t slot = tets[t][static_cast<std::size_t>(j)];
                p[j] = cp[slot];
                tv[j] = static_cast<double>(cv[slot]);
            }

            const std::size_t x = voxel % static_cast<std::size_t>(grid.dimX);
            const std::size_t rest = voxel / static_cast<std::size_t>(grid.dimX);
            const std::size_t y = rest % static_cast<std::size_t>(grid.dimY);
            const std::size_t z = rest / static_cast<std::size_t>(grid.dimY);
            const dvec3 point(loArr[0] + (static_cast<double>(x) + 0.5) * stepArr[0],
                              loArr[1] + (static_cast<double>(y) + 0.5) * stepArr[1],
                              loArr[2] + (static_cast<double>(z) + 0.5) * stepArr[2]);

            double w[4];
            double value = 0.0;
            bool have = false;
            if (barycentric(p, point, w)) {
                double sum = 0.0;
                for (int j = 0; j < 4; ++j) {
                    if (w[j] > 0.0) sum += w[j];
                }
                if (sum > 0.0) {
                    for (int j = 0; j < 4; ++j) {
                        if (w[j] > 0.0) value += (w[j] / sum) * tv[j];
                    }
                    have = true;
                }
            }
            if (!have) {
                // Epsilon-outside or degenerate: clamp to the nearest corner.
                double best = std::numeric_limits<double>::max();
                for (int j = 0; j < 4; ++j) {
                    const dvec3 delta = p[j] - point;
                    const double d = glm::dot(delta, delta);
                    if (d < best) { best = d; value = tv[j]; }
                }
            }

            if (!std::isfinite(value)) continue;
            out.value[voxel] = static_cast<float>(value);
            out.occupancy[voxel] = 1;
        }
    }

    // Slice ranges come from the same scatter, so the slice overlay never has
    // to scan the field again.
    const std::size_t dimX = static_cast<std::size_t>(grid.dimX);
    const std::size_t dimY = static_cast<std::size_t>(grid.dimY);
    const std::size_t dimZ = static_cast<std::size_t>(grid.dimZ);
    for (std::size_t z = 0; z < dimZ; ++z) {
        for (std::size_t y = 0; y < dimY; ++y) {
            const std::size_t rowBase = z * dimX * dimY + y * dimX;
            for (std::size_t x = 0; x < dimX; ++x) {
                const std::size_t idx = rowBase + x;
                if (!out.occupancy[idx]) continue;
                const float v = out.value[idx];
                if (!std::isfinite(v)) continue;

                auto accumulate = [&](int axis, std::size_t slice) {
                    float& mn = out.planeMin[static_cast<std::size_t>(axis)][slice];
                    float& mx = out.planeMax[static_cast<std::size_t>(axis)][slice];
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                };
                accumulate(0, x);
                accumulate(1, y);
                accumulate(2, z);
            }
        }
    }
}

} // namespace volume
