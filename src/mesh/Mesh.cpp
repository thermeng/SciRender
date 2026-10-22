#include "mesh/Mesh.h"

#include <algorithm>
#include <utility>

namespace fvm {

template<std::size_t Dim>
Mesh<Dim>::Mesh(std::array<std::size_t, Dim> n, std::array<double, Dim> l) {
    initUniform_(n, l);
}

template<std::size_t Dim>
Mesh<Dim> Mesh<Dim>::makeStretching(std::array<std::size_t, Dim> n,
                                    std::array<double, Dim> l,
                                    std::array<double, Dim> ratio) {
    Mesh m;
    for (std::size_t d = 0; d < Dim; ++d) {
        if (n[d] == 0) throw std::invalid_argument("Mesh dimensions must be > 0");
        if (l[d] <= 0.0) throw std::invalid_argument("Domain lengths must be > 0");
        if (ratio[d] <= 0.0)
            throw std::invalid_argument("Stretching ratio must be > 0");
    }
    m.dims_ = n;
    m.lengths_ = l;
    m.strides_[0] = 1;
    for (std::size_t d = 1; d < Dim; ++d) {
        m.strides_[d] = m.strides_[d - 1] * m.dims_[d - 1];
    }
    m.nCells_ = m.dims_[Dim - 1] * m.strides_[Dim - 1];

    for (std::size_t d = 0; d < Dim; ++d) {
        m.faceCoords_[d] = geometricStretching_(n[d], l[d], ratio[d]);
        m.spacings_[d] = l[d] / static_cast<double>(n[d]);
    }
    m.uniform_ = false;
    m.precomputeCellData_();
    m.computeFaceData_();
    return m;
}

template<std::size_t Dim>
Mesh<Dim>::Mesh(std::vector<std::array<double, Dim>> vertices,
                std::vector<std::vector<std::size_t>> cells,
                std::vector<std::vector<std::vector<std::size_t>>> cellFaces)
    : vertices_(std::move(vertices)), cells_(std::move(cells)),
      cellFaceVerts_(std::move(cellFaces)), unstructured_(true) {

    nCells_ = cells_.size();
    cellCenters_.resize(nCells_);
    cellVolumes_.resize(nCells_);
    isBoundary_.resize(nCells_, false);

    // Cell geometry first: the face normals are oriented with respect to the
    // cell centres, so the centres must be known before the face table is built.
    computeUnstructuredCellData_();

    computeUnstructuredFaceData_();
}

template<std::size_t Dim>
std::size_t Mesh<Dim>::nCells() const { return nCells_; }

template<std::size_t Dim>
const std::array<std::size_t, Dim>& Mesh<Dim>::dims() const { return dims_; }

template<std::size_t Dim>
const std::array<double, Dim>& Mesh<Dim>::lengths() const { return lengths_; }

template<std::size_t Dim>
const std::array<double, Dim>& Mesh<Dim>::spacings() const { return spacings_; }

template<std::size_t Dim>
const std::array<std::size_t, Dim>& Mesh<Dim>::strides() const { return strides_; }

template<std::size_t Dim>
bool Mesh<Dim>::isUniform() const { return uniform_; }

template<std::size_t Dim>
bool Mesh<Dim>::isUnstructured() const { return unstructured_; }

template<std::size_t Dim>
std::size_t Mesh<Dim>::n(std::size_t d) const {
    if (unstructured_) throw std::logic_error("Mesh::n(d) not valid for unstructured mesh");
    return dims_[d];
}

template<std::size_t Dim>
double Mesh<Dim>::dx(std::size_t d) const {
    if (unstructured_) throw std::logic_error("Mesh::dx(d) not valid for unstructured mesh");
    return spacings_[d];
}

template<std::size_t Dim>
double Mesh<Dim>::lx(std::size_t d) const {
    if (unstructured_) throw std::logic_error("Mesh::lx(d) not valid for unstructured mesh");
    return lengths_[d];
}

template<std::size_t Dim>
double Mesh<Dim>::dx(std::size_t d, std::size_t cellIdx) const {
    if (unstructured_) throw std::logic_error("Mesh::dx(d,cellIdx) not valid for unstructured mesh");
    if (uniform_) return spacings_[d];
    auto c = cellCoords(cellIdx);
    return faceCoords_[d][c[d] + 1] - faceCoords_[d][c[d]];
}

template<std::size_t Dim>
double Mesh<Dim>::faceCoord(std::size_t d, std::size_t faceIdx) const {
    if (unstructured_) throw std::logic_error("Mesh::faceCoord not valid for unstructured mesh");
    if (uniform_) return static_cast<double>(faceIdx) * spacings_[d];
    return faceCoords_[d][faceIdx];
}

template<std::size_t Dim>
std::array<double, Dim> Mesh<Dim>::cellCenter(std::size_t idx) const {
    return cellCenters_[idx];
}

template<std::size_t Dim>
double Mesh<Dim>::faceArea(std::size_t d, std::size_t idx) const {
    if (unstructured_) throw std::logic_error("Mesh::faceArea(d,idx) not valid for unstructured mesh");
    double area = 1.0;
    auto c = cellCoords(idx);
    for (std::size_t dd = 0; dd < Dim; ++dd) {
        if (dd != d) {
            if (uniform_) {
                area *= spacings_[dd];
            } else {
                area *= (faceCoords_[dd][c[dd] + 1] - faceCoords_[dd][c[dd]]);
            }
        }
    }
    return area;
}

template<std::size_t Dim>
double Mesh<Dim>::faceWeight(std::size_t f) const {
    return faceWeights_[f];
}

template<std::size_t Dim>
double Mesh<Dim>::cellVolume(std::size_t idx) const {
    return cellVolumes_[idx];
}

template<std::size_t Dim>
std::size_t Mesh<Dim>::cellIndex(const std::array<std::size_t, Dim>& c) const {
    if (unstructured_) throw std::logic_error("Mesh::cellIndex not valid for unstructured mesh");
    return cellIndex_(c);
}

template<std::size_t Dim>
std::array<std::size_t, Dim> Mesh<Dim>::cellCoords(std::size_t idx) const {
    if (unstructured_) throw std::logic_error("Mesh::cellCoords not valid for unstructured mesh");
    std::array<std::size_t, Dim> c{};
    for (std::size_t d = 0; d < Dim; ++d) {
        c[d] = (idx / strides_[d]) % dims_[d];
    }
    return c;
}

template<std::size_t Dim>
bool Mesh<Dim>::isBoundary(std::size_t idx) const {
    return isBoundary_[idx];
}

template<std::size_t Dim>
std::pair<std::size_t, int> Mesh<Dim>::neighborOffset(std::size_t idx, std::size_t d, int sign) const {
    if (unstructured_) throw std::logic_error("Mesh::neighborOffset not valid for unstructured mesh");
    std::array<std::size_t, Dim> c = cellCoords(idx);
    if (sign > 0 && c[d] == dims_[d] - 1) return {0, -1};
    if (sign < 0 && c[d] == 0) return {0, -1};
    c[d] += sign;
    return {cellIndex_(c), 0};
}

template<std::size_t Dim>
std::size_t Mesh<Dim>::nFaces() const { return nFaces_; }

template<std::size_t Dim>
std::size_t Mesh<Dim>::owner(std::size_t f) const { return owner_[f]; }

template<std::size_t Dim>
std::size_t Mesh<Dim>::neighbor(std::size_t f) const { return neighbor_[f]; }

template<std::size_t Dim>
double Mesh<Dim>::faceArea(std::size_t f) const { return faceArea_[f]; }

template<std::size_t Dim>
const std::array<double, Dim>& Mesh<Dim>::faceNormal(std::size_t f) const { return faceNormal_[f]; }

template<std::size_t Dim>
const std::array<double, Dim>& Mesh<Dim>::faceCenter(std::size_t f) const { return faceCenter_[f]; }

template<std::size_t Dim>
double Mesh<Dim>::faceDist(std::size_t f) const { return faceDist_[f]; }

template<std::size_t Dim>
std::size_t Mesh<Dim>::nFacesOnBoundary(std::size_t d, int sign) const {
    std::size_t idx = d * 2 + (sign < 0 ? 0 : 1);
    if (idx < nFacesOnBoundaryCache_.size() && boundaryFacesCache_.size() == 2 * Dim) {
        return nFacesOnBoundaryCache_[idx];
    }
    if (unstructured_) {
        std::size_t count = 0;
        for (std::size_t f = 0; f < nFaces_; ++f) {
            if (neighbor_[f] != std::size_t(-1)) continue;
            const auto& normal = faceNormal_[f];
            double maxComp = 0.0;
            for (std::size_t dd = 0; dd < Dim; ++dd)
                maxComp = std::max(maxComp, std::abs(normal[dd]));
            if (std::abs(normal[d]) >= 0.9 * maxComp
                && ((sign < 0 && normal[d] < -0.5) || (sign > 0 && normal[d] > 0.5))) {
                ++count;
            }
        }
        return count;
    }
    return (nFaces_ / dims_[d]);
}

template<std::size_t Dim>
const std::vector<std::size_t>& Mesh<Dim>::boundaryFaces(std::size_t d, int sign) const {
    std::size_t idx = d * 2 + (sign < 0 ? 0 : 1);
    if (idx < boundaryFacesCache_.size()) return boundaryFacesCache_[idx];
    static const std::vector<std::size_t> empty;
    return empty;
}

template<std::size_t Dim>
const std::vector<std::array<double, Dim>>& Mesh<Dim>::vertices() const {
    return vertices_;
}

template<std::size_t Dim>
const std::vector<std::vector<std::size_t>>& Mesh<Dim>::cells() const {
    return cells_;
}

template<std::size_t Dim>
void Mesh<Dim>::setBoundaryGroups(std::map<std::string, std::vector<std::size_t>> groups) {
    boundaryGroups_ = std::move(groups);
}

template<std::size_t Dim>
const std::map<std::string, std::vector<std::size_t>>& Mesh<Dim>::boundaryGroups() const {
    return boundaryGroups_;
}

template<std::size_t Dim>
const std::vector<std::size_t>* Mesh<Dim>::boundaryGroupFaces(const std::string& name) const {
    auto it = boundaryGroups_.find(name);
    return (it != boundaryGroups_.end()) ? &it->second : nullptr;
}

template<std::size_t Dim>
std::vector<std::string> Mesh<Dim>::boundaryGroupNames() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : boundaryGroups_) {
        names.push_back(name);
    }
    return names;
}

template<std::size_t Dim>
const std::vector<std::size_t>& Mesh<Dim>::cellVertices(std::size_t idx) const {
    return cells_[idx];
}

template<std::size_t Dim>
const std::vector<std::size_t>& Mesh<Dim>::faceVertices(std::size_t f) const {
    return faceVertices_[f];
}

template<std::size_t Dim>
const std::vector<std::size_t>& Mesh<Dim>::cellNeighbors(std::size_t idx) const {
    return cellNeighbors_[idx];
}

template<std::size_t Dim>
const std::vector<std::size_t>& Mesh<Dim>::cellFaces(std::size_t idx) const {
    return cellFaces_[idx];
}

// --- Private method implementations ---

template<std::size_t Dim>
std::size_t Mesh<Dim>::cellIndex_(const std::array<std::size_t, Dim>& c) const {
    std::size_t idx = 0;
    for (std::size_t d = 0; d < Dim; ++d) {
        idx += c[d] * strides_[d];
    }
    return idx;
}

template<std::size_t Dim>
void Mesh<Dim>::precomputeCellData_() {
    cellVolumes_.resize(nCells_);
    cellCenters_.resize(nCells_);
    isBoundary_.resize(nCells_, false);

    for (std::size_t idx = 0; idx < nCells_; ++idx) {
        if (uniform_) {
            double vol = 1.0;
            for (std::size_t d = 0; d < Dim; ++d) vol *= spacings_[d];
            cellVolumes_[idx] = vol;
        } else {
            auto c = cellCoords(idx);
            double vol = 1.0;
            for (std::size_t d = 0; d < Dim; ++d) {
                vol *= faceCoords_[d][c[d] + 1] - faceCoords_[d][c[d]];
            }
            cellVolumes_[idx] = vol;
        }

        // Precompute cell center for O(1) lookup
        if (uniform_) {
            std::array<double, Dim> coords{};
            for (std::size_t d = 0; d < Dim; ++d) {
                std::size_t i = (idx / strides_[d]) % dims_[d];
                coords[d] = (static_cast<double>(i) + 0.5) * spacings_[d];
            }
            cellCenters_[idx] = coords;
        } else {
            auto c = cellCoords(idx);
            std::array<double, Dim> coords{};
            for (std::size_t d = 0; d < Dim; ++d) {
                coords[d] = 0.5 * (faceCoords_[d][c[d]] + faceCoords_[d][c[d] + 1]);
            }
            cellCenters_[idx] = coords;
        }

        auto c = cellCoords(idx);
        for (std::size_t d = 0; d < Dim; ++d) {
            if (c[d] == 0 || c[d] == dims_[d] - 1) {
                isBoundary_[idx] = true;
                break;
            }
        }
    }
}

template<std::size_t Dim>
void Mesh<Dim>::initUniform_(std::array<std::size_t, Dim> n, std::array<double, Dim> l) {
    for (std::size_t d = 0; d < Dim; ++d) {
        if (n[d] == 0) throw std::invalid_argument("Mesh dimensions must be > 0");
        if (l[d] <= 0.0) throw std::invalid_argument("Domain lengths must be > 0");
    }
    dims_ = n;
    lengths_ = l;
    uniform_ = true;

    strides_[0] = 1;
    for (std::size_t d = 1; d < Dim; ++d) {
        strides_[d] = strides_[d - 1] * dims_[d - 1];
    }
    nCells_ = dims_[Dim - 1] * strides_[Dim - 1];
    for (std::size_t d = 0; d < Dim; ++d) {
        spacings_[d] = lengths_[d] / static_cast<double>(dims_[d]);
    }
    precomputeCellData_();
    computeFaceData_();
}

template<std::size_t Dim>
std::vector<double> Mesh<Dim>::geometricStretching_(std::size_t n, double L, double r) {
    std::vector<double> faces(n + 1);
    if (r == 1.0) {
        double dx = L / static_cast<double>(n);
        for (std::size_t i = 0; i <= n; ++i) {
            faces[i] = static_cast<double>(i) * dx;
        }
    } else {
        double dx0 = L * (r - 1.0) / (std::pow(r, static_cast<double>(n)) - 1.0);
        faces[0] = 0.0;
        double x = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            x += dx0 * std::pow(r, static_cast<double>(i));
            faces[i + 1] = x;
        }
        faces[n] = L;
    }
    return faces;
}

template<std::size_t Dim>
std::array<double, Dim> Mesh<Dim>::computeCellCenter_(
    const std::vector<std::size_t>& cell) const {
    std::array<double, Dim> center{};
    double nVerts = static_cast<double>(cell.size());
    for (std::size_t d = 0; d < Dim; ++d) {
        for (std::size_t v = 0; v < cell.size(); ++v) {
            center[d] += vertices_[cell[v]][d];
        }
        center[d] /= nVerts;
    }
    return center;
}

template<std::size_t Dim>
std::vector<std::vector<std::size_t>> Mesh<Dim>::cellFaceLoops_(std::size_t ci) const {
    if (!cellFaceVerts_.empty() && ci < cellFaceVerts_.size()) {
        return cellFaceVerts_[ci];
    }
    const auto& cell = cells_[ci];
    const std::size_t nv = cell.size();
    std::vector<std::vector<std::size_t>> loops;
    if constexpr (Dim == 2) {
        loops.reserve(nv);
        for (std::size_t i = 0; i < nv; ++i) {
            loops.push_back({cell[i], cell[(i + 1) % nv]});
        }
    } else if constexpr (Dim == 3) {
        if (nv == 4) {
            // Tetrahedron: the four triangular faces.
            loops = {{cell[0], cell[1], cell[2]},
                     {cell[0], cell[1], cell[3]},
                     {cell[0], cell[2], cell[3]},
                     {cell[1], cell[2], cell[3]}};
        } else if (nv == 8) {
            // Hexahedron in VTK/Gmsh element order: 0-3 bottom, 4-7 top.
            loops = {{cell[0], cell[1], cell[2], cell[3]},
                     {cell[4], cell[5], cell[6], cell[7]},
                     {cell[0], cell[1], cell[5], cell[4]},
                     {cell[2], cell[3], cell[7], cell[6]},
                     {cell[0], cell[3], cell[7], cell[4]},
                     {cell[1], cell[2], cell[6], cell[5]}};
        } else {
            // Fallback: a triangle fan from vertex 0. Only valid for convex
            // cells whose faces are triangles. Any other polyhedral cell must
            // be supplied with an explicit face list.
            for (std::size_t i = 0; i + 1 < nv; ++i) {
                loops.push_back({cell[0], cell[i], cell[i + 1]});
            }
        }
    }
    return loops;
}

template<std::size_t Dim>
void Mesh<Dim>::polygonGeometry3D_(const std::vector<std::size_t>& loop,
                                    double& area,
                                    std::array<double, 3>& normal,
                                    std::array<double, 3>& centroid) const {
    area = 0.0;
    normal = {0.0, 0.0, 0.0};
    centroid = {0.0, 0.0, 0.0};
    if constexpr (Dim != 3) {
        return;  // only meaningful in 3D
    }
    const std::size_t n = loop.size();
    if (n == 0) return;
    if (n < 3) {
        // Degenerate "face" (a point or an edge): no area, no normal.
        for (std::size_t d = 0; d < 3; ++d) {
            double s = 0.0;
            for (std::size_t v = 0; v < n; ++v) s += vertices_[loop[v]][d];
            centroid[d] = s / static_cast<double>(n);
        }
        return;
    }

    // Newell normal: the sum of the fan triangles' area vectors, so it is the
    // area-weighted average normal of a warped polygon and the exact normal of
    // a planar one.
    double nw[3] = {0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < n; ++i) {
        const auto& p = vertices_[loop[i]];
        const auto& q = vertices_[loop[(i + 1) % n]];
        nw[0] += p[1] * q[2] - p[2] * q[1];
        nw[1] += p[2] * q[0] - p[0] * q[2];
        nw[2] += p[0] * q[1] - p[1] * q[0];
    }
    const double mag = std::sqrt(nw[0] * nw[0] + nw[1] * nw[1] + nw[2] * nw[2]);

    // Fan triangulation: the true surface area of the (possibly warped) face.
    double acc = 0.0;
    double c[3] = {0.0, 0.0, 0.0};
    for (std::size_t i = 1; i + 1 < n; ++i) {
        const auto& p0 = vertices_[loop[0]];
        const auto& p1 = vertices_[loop[i]];
        const auto& p2 = vertices_[loop[i + 1]];
        const double e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const double e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const double cr[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                              e1[2] * e2[0] - e1[0] * e2[2],
                              e1[0] * e2[1] - e1[1] * e2[0]};
        const double triArea = 0.5 * std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
        if (triArea <= 0.0) continue;
        area += triArea;
        for (std::size_t d = 0; d < 3; ++d) {
            c[d] += triArea * (p0[d] + p1[d] + p2[d]) / 3.0;
        }
    }
    if (area > 0.0) {
        for (std::size_t d = 0; d < 3; ++d) centroid[d] = c[d] / area;
    } else {
        for (std::size_t d = 0; d < 3; ++d) {
            centroid[d] = 0.0;
            for (std::size_t v = 0; v < n; ++v) centroid[d] += vertices_[loop[v]][d];
            centroid[d] /= static_cast<double>(n);
        }
    }
    if (mag > 0.0) {
        normal[0] = nw[0] / mag;
        normal[1] = nw[1] / mag;
        normal[2] = nw[2] / mag;
    }
}

template<std::size_t Dim>
void Mesh<Dim>::computeUnstructuredCellData_() {
    cellVolumes_.assign(nCells_, 0.0);
    cellCenters_.assign(nCells_, std::array<double, Dim>{});

    for (std::size_t ci = 0; ci < nCells_; ++ci) {
        if constexpr (Dim == 2) {
            // Shoelace area and the matching polygon centroid. The centroid is
            // the true area centroid, not the average of the vertices: the
            // latter is only equal to it for triangles and it feeds the face
            // distances, the interpolation weights and every gradient
            // reconstruction, so the error would propagate through the whole
            // discretisation.
            const auto& cell = cells_[ci];
            const std::size_t nv = cell.size();
            double a2 = 0.0;      // twice the signed area
            double cx = 0.0, cy = 0.0;
            for (std::size_t i = 0; i < nv; ++i) {
                const std::size_t j = (i + 1) % nv;
                const auto& pi = vertices_[cell[i]];
                const auto& pj = vertices_[cell[j]];
                const double cr = pi[0] * pj[1] - pj[0] * pi[1];
                a2 += cr;
                cx += (pi[0] + pj[0]) * cr;
                cy += (pi[1] + pj[1]) * cr;
            }
            const double area = std::abs(a2) / 2.0;
            cellVolumes_[ci] = area;
            if (area > 1e-30) {
                cellCenters_[ci][0] = cx / (3.0 * a2);
                cellCenters_[ci][1] = cy / (3.0 * a2);
            } else {
                cellCenters_[ci] = computeCellCenter_(cell);
            }
        } else {
            // Divergence theorem over the cell's own faces:
            //   V      = (1/3) sum_f A_f (x_fc . n_out)
            //   V * c  = (1/4) sum_f A_f (x_fc . n_out) x_fc
            // This is exact for any convex (or star shaped) polyhedron and
            // needs no assumption about the element type, so a hexahedron read
            // in a non-VTK vertex order, a prism or a general polyhedron all
            // get the right volume and centroid.
            const auto loops = cellFaceLoops_(ci);
            const std::array<double, Dim> guess = computeCellCenter_(cells_[ci]);
            double vol = 0.0;
            double mom[3] = {0.0, 0.0, 0.0};
            for (const auto& loop : loops) {
                double area = 0.0;
                std::array<double, 3> nrm{};
                std::array<double, 3> fc{};
                polygonGeometry3D_(loop, area, nrm, fc);
                if (area <= 0.0) continue;
                // Orient the face normal away from the cell (outward).
                double s = 0.0;
                for (std::size_t d = 0; d < 3; ++d) s += (fc[d] - guess[d]) * nrm[d];
                if (s < 0.0) {
                    for (std::size_t d = 0; d < 3; ++d) nrm[d] = -nrm[d];
                    s = -s;
                }
                double triple = 0.0;
                for (std::size_t d = 0; d < 3; ++d) triple += fc[d] * nrm[d];
                vol += area * triple / 3.0;
                for (std::size_t d = 0; d < 3; ++d) mom[d] += area * triple * fc[d] / 4.0;
            }
            cellVolumes_[ci] = std::abs(vol);
            if (std::abs(vol) > 1e-30) {
                // The outward orientation makes both the volume and the first
                // moment flip together, so the ratio is sign independent.
                for (std::size_t d = 0; d < 3; ++d) cellCenters_[ci][d] = mom[d] / vol;
            } else {
                cellCenters_[ci] = guess;
            }
        }
    }
}

template<std::size_t Dim>
void Mesh<Dim>::computeUnstructuredFaceData_() {
    using FaceKey = std::vector<std::size_t>;

    // A face is identified by the SET of its vertices (sorted), so the two
    // cells sharing it agree on the key whatever the orientation of their
    // vertex loops. The loop of the owner cell is kept as well: the face
    // geometry (area, normal, centroid) is computed from the ordered loop, not
    // from the sorted key, because a sorted key destroys the polygon order and
    // with it the area of a warped quadrilateral.
    struct FaceRecord {
        FaceKey key;
        std::size_t owner = 0;
        std::size_t neighbor = std::size_t(-1);
        std::vector<std::size_t> loop;
    };
    std::vector<FaceRecord> faces;
    faces.reserve(nCells_ * 6);
    std::unordered_map<FaceKey, std::size_t, FaceKeyHash> faceIndex;
    faceIndex.reserve(nCells_ * 6);

    for (std::size_t ci = 0; ci < nCells_; ++ci) {
        for (const auto& loop : cellFaceLoops_(ci)) {
            if (loop.size() < (Dim == 2 ? 2u : 3u)) continue;
            FaceKey key = loop;
            std::sort(key.begin(), key.end());
            key.erase(std::unique(key.begin(), key.end()), key.end());
            if (key.size() < (Dim == 2 ? 2u : 3u)) continue;
            auto it = faceIndex.find(key);
            if (it == faceIndex.end()) {
                faceIndex.emplace(key, faces.size());
                faces.push_back(FaceRecord{std::move(key), ci, std::size_t(-1), loop});
            } else if (faces[it->second].neighbor == std::size_t(-1)) {
                faces[it->second].neighbor = ci;
            }
            // A face shared by more than two cells is non-manifold: the first
            // neighbour is kept and the extra one ignored.
        }
    }

    // Deterministic face numbering: sorting by the key makes the face indices
    // independent of the hash order of the lookup table, so a mesh rebuilt from
    // the same input always produces the same numbering.
    std::sort(faces.begin(), faces.end(),
              [](const FaceRecord& a, const FaceRecord& b) { return a.key < b.key; });

    nFaces_ = faces.size();
    owner_.resize(nFaces_);
    neighbor_.resize(nFaces_);
    faceArea_.resize(nFaces_);
    faceNormal_.resize(nFaces_);
    faceCenter_.resize(nFaces_);
    faceDist_.resize(nFaces_, 0.0);
    faceWeights_.resize(nFaces_, 0.0);
    faceVertices_.resize(nFaces_);
    isBoundary_.assign(nCells_, false);

    for (std::size_t f = 0; f < nFaces_; ++f) {
        const FaceRecord& rec = faces[f];
        owner_[f] = rec.owner;
        neighbor_[f] = rec.neighbor;
        faceVertices_[f] = rec.key;

        if constexpr (Dim == 3) {
            double area = 0.0;
            std::array<double, 3> n{};
            std::array<double, 3> fc{};
            polygonGeometry3D_(rec.loop, area, n, fc);
            faceArea_[f] = area;
            for (std::size_t d = 0; d < 3; ++d) {
                faceCenter_[f][d] = fc[d];
                faceNormal_[f][d] = n[d];
            }
        } else {
            const auto& p0 = vertices_[rec.loop[0]];
            const auto& p1 = vertices_[rec.loop[1]];
            const double dx = p1[0] - p0[0];
            const double dy = p1[1] - p0[1];
            const double mag = std::sqrt(dx * dx + dy * dy);
            faceArea_[f] = mag;
            for (std::size_t d = 0; d < 2; ++d) {
                faceCenter_[f][d] = 0.5 * (p0[d] + p1[d]);
            }
            // Right-hand normal of the loop direction; the orientation is fixed
            // below (owner -> neighbour, or outward for a boundary face).
            faceNormal_[f][0] = mag > 0.0 ? dy / mag : 0.0;
            faceNormal_[f][1] = mag > 0.0 ? -dx / mag : 0.0;
        }

        // Orient the face normal: owner -> neighbour for an internal face,
        // outward from the owner for a boundary face.
        {
            const auto& oc = cellCenters_[owner_[f]];
            double delta[Dim];
            if (neighbor_[f] != std::size_t(-1)) {
                const auto& nc = cellCenters_[neighbor_[f]];
                for (std::size_t dd = 0; dd < Dim; ++dd) delta[dd] = nc[dd] - oc[dd];
            } else {
                for (std::size_t dd = 0; dd < Dim; ++dd) delta[dd] = faceCenter_[f][dd] - oc[dd];
            }
            double dot = 0.0;
            for (std::size_t dd = 0; dd < Dim; ++dd) dot += faceNormal_[f][dd] * delta[dd];
            if (dot < 0.0) {
                for (std::size_t dd = 0; dd < Dim; ++dd) faceNormal_[f][dd] = -faceNormal_[f][dd];
            }
        }

        if (neighbor_[f] != std::size_t(-1)) {
            const auto& oc = cellCenters_[owner_[f]];
            const auto& nc = cellCenters_[neighbor_[f]];
            double dist2 = 0.0;
            for (std::size_t d = 0; d < Dim; ++d) {
                double diff = nc[d] - oc[d];
                dist2 += diff * diff;
            }
            faceDist_[f] = std::sqrt(dist2);
            // Interpolation weight: the parameter of the orthogonal projection
            // of the face centre onto the owner->neighbour segment. This is the
            // exact linear-interpolation coefficient between the two cell
            // centres; the distance ratio |xF-xP|/|xN-xP| overestimates it on
            // a skewed mesh (the face centre is then off the centre-to-centre
            // line) and can even exceed 1, which turns the interpolation into
            // an extrapolation.
            double num = 0.0;
            double den = 0.0;
            for (std::size_t d = 0; d < Dim; ++d) {
                const double pf = faceCenter_[f][d] - oc[d];
                const double pn = nc[d] - oc[d];
                num += pf * pn;
                den += pn * pn;
            }
            double w = (den > 1e-24) ? num / den : 0.5;
            faceWeights_[f] = std::min(1.0, std::max(0.0, w));
        } else {
            const auto& oc = cellCenters_[owner_[f]];
            double dist2 = 0.0;
            for (std::size_t d = 0; d < Dim; ++d) {
                double diff = faceCenter_[f][d] - oc[d];
                dist2 += diff * diff;
            }
            faceDist_[f] = std::sqrt(dist2);
            faceWeights_[f] = 0.0;
            isBoundary_[owner_[f]] = true;
        }
    }
    buildAdjacencyAndBoundaryCache_();
}

template<std::size_t Dim>
void Mesh<Dim>::computeFaceData_() {
    nFaces_ = 0;
    for (std::size_t d = 0; d < Dim; ++d) {
        std::size_t nFacesD = dims_[d] + 1;
        for (std::size_t dd = 0; dd < Dim; ++dd) {
            if (dd != d) nFacesD *= dims_[dd];
        }
        nFaces_ += nFacesD;
    }

    owner_.resize(nFaces_);
    neighbor_.resize(nFaces_, std::size_t(-1));
    faceArea_.resize(nFaces_);
    faceNormal_.resize(nFaces_);
    faceCenter_.resize(nFaces_);
    faceDist_.resize(nFaces_, 0.0);
    faceWeights_.resize(nFaces_, 0.0);

    std::size_t faceOffset = 0;

    for (std::size_t d = 0; d < Dim; ++d) {
        std::size_t crossCells = nCells_ / dims_[d];

        std::array<std::size_t, Dim> crossStrides{};
        {
            std::size_t cs = 1;
            for (std::size_t dd = 0; dd < Dim; ++dd) {
                if (dd != d) {
                    crossStrides[dd] = cs;
                    cs *= dims_[dd];
                }
            }
        }

        for (std::size_t fi = 0; fi <= dims_[d]; ++fi) {
            for (std::size_t ci = 0; ci < crossCells; ++ci) {
                std::size_t f = faceOffset + fi * crossCells + ci;

                std::array<std::size_t, Dim> baseCoords{};
                for (std::size_t dd = 0; dd < Dim; ++dd) {
                    if (dd != d) {
                        baseCoords[dd] = (ci / crossStrides[dd]) % dims_[dd];
                    }
                }

                if (fi == 0) {
                    std::array<std::size_t, Dim> oc = baseCoords;
                    oc[d] = 0;
                    owner_[f] = cellIndex_(oc);
                    neighbor_[f] = std::size_t(-1);
                } else {
                    std::array<std::size_t, Dim> oc = baseCoords;
                    oc[d] = fi - 1;
                    owner_[f] = cellIndex_(oc);
                    if (fi == dims_[d]) {
                        neighbor_[f] = std::size_t(-1);
                    } else {
                        std::array<std::size_t, Dim> nc = baseCoords;
                        nc[d] = fi;
                        neighbor_[f] = cellIndex_(nc);
                    }
                }

                double area = 1.0;
                for (std::size_t dd = 0; dd < Dim; ++dd) {
                    if (dd != d) {
                        if (uniform_) {
                            area *= spacings_[dd];
                        } else {
                            area *= (faceCoords_[dd][baseCoords[dd] + 1] - faceCoords_[dd][baseCoords[dd]]);
                        }
                    }
                }
                faceArea_[f] = area;

                std::array<double, Dim> normal{};
                normal[d] = (fi == 0) ? -1.0 : 1.0;
                faceNormal_[f] = normal;

                std::array<double, Dim> center{};
                for (std::size_t dd = 0; dd < Dim; ++dd) {
                    if (dd == d) {
                        if (uniform_) {
                            center[dd] = static_cast<double>(fi) * spacings_[dd];
                        } else {
                            center[dd] = faceCoords_[dd][fi];
                        }
                    } else {
                        if (uniform_) {
                            center[dd] = (static_cast<double>(baseCoords[dd]) + 0.5) * spacings_[dd];
                        } else {
                            center[dd] = 0.5 * (faceCoords_[dd][baseCoords[dd]]
                                              + faceCoords_[dd][baseCoords[dd] + 1]);
                        }
                    }
                }
                faceCenter_[f] = center;

                if (neighbor_[f] != std::size_t(-1)) {
                    auto oc = cellCenter(owner_[f]);
                    auto nc = cellCenter(neighbor_[f]);
                    double dist2 = 0.0;
                    for (std::size_t dd = 0; dd < Dim; ++dd) {
                        double diff = nc[dd] - oc[dd];
                        dist2 += diff * diff;
                    }
                    faceDist_[f] = std::sqrt(dist2);
                    // w = the parameter of the orthogonal projection of the face
                    // centre onto the owner->neighbour segment (the exact
                    // linear-interpolation coefficient between the two cell
                    // centres). On a uniform grid it is exactly 0.5.
                    double num = 0.0;
                    double den = 0.0;
                    for (std::size_t dd = 0; dd < Dim; ++dd) {
                        const double pf = faceCenter_[f][dd] - oc[dd];
                        const double pn = nc[dd] - oc[dd];
                        num += pf * pn;
                        den += pn * pn;
                    }
                    double w = (den > 1e-24) ? num / den : 0.5;
                    faceWeights_[f] = std::min(1.0, std::max(0.0, w));
                } else {
                    auto oc = cellCenter(owner_[f]);
                    double dist2 = 0.0;
                    for (std::size_t dd = 0; dd < Dim; ++dd) {
                        double diff = faceCenter_[f][dd] - oc[dd];
                        dist2 += diff * diff;
                    }
                    faceDist_[f] = std::sqrt(dist2);
                    faceWeights_[f] = 0.0;
                }
            }
        }
        faceOffset += (dims_[d] + 1) * crossCells;
    }
    buildAdjacencyAndBoundaryCache_();
}

template<std::size_t Dim>
void Mesh<Dim>::buildAdjacencyAndBoundaryCache_() {
    cellNeighbors_.assign(nCells_, {});
    cellFaces_.assign(nCells_, {});
    for (std::size_t c = 0; c < nCells_; ++c) {
        cellNeighbors_[c].reserve(6);
        cellFaces_[c].reserve(6);
    }
    for (std::size_t f = 0; f < nFaces_; ++f) {
        std::size_t P = owner_[f];
        std::size_t N = neighbor_[f];
        cellFaces_[P].push_back(f);
        if (N != std::size_t(-1)) {
            cellFaces_[N].push_back(f);
            cellNeighbors_[P].push_back(N);
            cellNeighbors_[N].push_back(P);
        }
    }
    // Boundary cache: 2*Dim directions
    boundaryFacesCache_.assign(2 * Dim, {});
    nFacesOnBoundaryCache_.assign(2 * Dim, 0);
    for (std::size_t f = 0; f < nFaces_; ++f) {
        if (neighbor_[f] != std::size_t(-1)) continue;
        const auto& n = faceNormal_[f];
        // Find dominant direction
        std::size_t maxD = 0;
        double maxA = 0.0;
        for (std::size_t d = 0; d < Dim; ++d) {
            double a = std::abs(n[d]);
            if (a > maxA) { maxA = a; maxD = d; }
        }
        int sign = (n[maxD] < 0) ? -1 : 1;
        std::size_t idx = maxD * 2 + (sign < 0 ? 0 : 1);
        if (idx < boundaryFacesCache_.size()) {
            boundaryFacesCache_[idx].push_back(f);
            nFacesOnBoundaryCache_[idx]++;
        }
    }
}

template class Mesh<2>;
template class Mesh<3>;

} // namespace fvm
