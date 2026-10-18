#include "mesh/Mesh.h"

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
                std::vector<std::vector<std::size_t>> cells)
    : vertices_(std::move(vertices)), cells_(std::move(cells)), unstructured_(true) {

    nCells_ = cells_.size();
    cellCenters_.resize(nCells_);
    cellVolumes_.resize(nCells_);
    isBoundary_.resize(nCells_, false);

    for (std::size_t i = 0; i < nCells_; ++i) {
        cellCenters_[i] = computeCellCenter_(cells_[i]);
        cellVolumes_[i] = computeCellVolume_(cells_[i]);
    }

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
double Mesh<Dim>::computeCellVolume_(
    const std::vector<std::size_t>& cell) const {
    if constexpr (Dim == 2) {
        double area = 0.0;
        std::size_t n = cell.size();
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t j = (i + 1) % n;
            const auto& pi = vertices_[cell[i]];
            const auto& pj = vertices_[cell[j]];
            area += pi[0] * pj[1] - pj[0] * pi[1];
        }
        return std::abs(area) / 2.0;
    } else if constexpr (Dim == 3) {
        std::size_t nv = cell.size();
        if (nv == 4) {
            const auto& v0 = vertices_[cell[0]];
            const auto& v1 = vertices_[cell[1]];
            const auto& v2 = vertices_[cell[2]];
            const auto& v3 = vertices_[cell[3]];
            double ax = v1[0]-v0[0], ay = v1[1]-v0[1], az = v1[2]-v0[2];
            double bx = v2[0]-v0[0], by = v2[1]-v0[1], bz = v2[2]-v0[2];
            double cx = v3[0]-v0[0], cy = v3[1]-v0[1], cz = v3[2]-v0[2];
            double det = ax*(by*cz - bz*cy) - ay*(bx*cz - bz*cx) + az*(bx*cy - by*cx);
            return std::abs(det) / 6.0;
        } else if (nv == 8) {
            auto tetVol = [&](std::size_t i0, std::size_t i1, std::size_t i2, std::size_t i3) -> double {
                const auto& p0 = vertices_[cell[i0]];
                const auto& p1 = vertices_[cell[i1]];
                const auto& p2 = vertices_[cell[i2]];
                const auto& p3 = vertices_[cell[i3]];
                double ax = p1[0]-p0[0], ay = p1[1]-p0[1], az = p1[2]-p0[2];
                double bx = p2[0]-p0[0], by = p2[1]-p0[1], bz = p2[2]-p0[2];
                double cx = p3[0]-p0[0], cy = p3[1]-p0[1], cz = p3[2]-p0[2];
                return std::abs(ax*(by*cz - bz*cy) - ay*(bx*cz - bz*cx) + az*(bx*cy - by*cx)) / 6.0;
            };
            double vol = 0.0;
            vol += tetVol(0,1,3,4);
            vol += tetVol(1,2,3,6);
            vol += tetVol(1,4,5,6);
            vol += tetVol(3,4,6,7);
            vol += tetVol(1,3,4,6);
            return vol;
        } else {
            double cx = 0, cy = 0, cz = 0;
            for (std::size_t i = 0; i < nv; ++i) {
                cx += vertices_[cell[i]][0];
                cy += vertices_[cell[i]][1];
                cz += vertices_[cell[i]][2];
            }
            cx /= nv; cy /= nv; cz /= nv;
            double vol = 0.0;
            for (std::size_t i = 1; i + 1 < nv; ++i) {
                const auto& p0 = vertices_[cell[0]];
                const auto& p1 = vertices_[cell[i]];
                const auto& p2 = vertices_[cell[i+1]];
                double ax = p1[0]-p0[0], ay = p1[1]-p0[1], az = p1[2]-p0[2];
                double bx = p2[0]-p0[0], by = p2[1]-p0[1], bz = p2[2]-p0[2];
                double nx = ay*bz - az*by, ny = az*bx - ax*bz, nz = ax*by - ay*bx;
                double d[3] = {cx-p0[0], cy-p0[1], cz-p0[2]};
                double dot = nx*d[0] + ny*d[1] + nz*d[2];
                if (dot > 0) { nx = -nx; ny = -ny; nz = -nz; }
                double mag = std::sqrt(nx*nx + ny*ny + nz*nz);
                if (mag > 0) {
                    double area = mag / 2.0;
                    double fcx = (p0[0]+p1[0]+p2[0])/3.0;
                    double fcy = (p0[1]+p1[1]+p2[1])/3.0;
                    double fcz = (p0[2]+p1[2]+p2[2])/3.0;
                    vol += std::abs((fcx*nx/mag + fcy*ny/mag + fcz*nz/mag) * area) / 3.0;
                }
            }
            return vol;
        }
    }
    return 0.0;
}

template<std::size_t Dim>
void Mesh<Dim>::computeUnstructuredFaceData_() {
    using FaceKey = std::vector<std::size_t>;
    std::unordered_map<FaceKey, std::pair<std::size_t, std::size_t>, FaceKeyHash> faceMap;
    faceMap.reserve(nCells_ * 6);

    for (std::size_t ci = 0; ci < nCells_; ++ci) {
        const auto& cell = cells_[ci];
        std::size_t nv = cell.size();

        if constexpr (Dim == 2) {
            for (std::size_t i = 0; i < nv; ++i) {
                std::size_t j = (i + 1) % nv;
                FaceKey fk = {cell[i], cell[j]};
                if (fk[0] > fk[1]) std::swap(fk[0], fk[1]);
                auto it = faceMap.find(fk);
                if (it == faceMap.end()) {
                    faceMap.emplace(fk, std::pair<std::size_t,std::size_t>{ci, std::size_t(-1)});
                } else {
                    it->second.second = ci;
                }
            }
        } else if constexpr (Dim == 3) {
            auto addFace = [&](FaceKey fk) {
                std::sort(fk.begin(), fk.end());
                auto it = faceMap.find(fk);
                if (it == faceMap.end()) faceMap.emplace(fk, std::pair<std::size_t,std::size_t>{ci, std::size_t(-1)});
                else it->second.second = ci;
            };
            if (nv == 4) {
                addFace({cell[0], cell[1], cell[2]});
                addFace({cell[0], cell[1], cell[3]});
                addFace({cell[0], cell[2], cell[3]});
                addFace({cell[1], cell[2], cell[3]});
            } else if (nv == 8) {
                addFace({cell[0], cell[1], cell[2], cell[3]});
                addFace({cell[4], cell[5], cell[6], cell[7]});
                addFace({cell[0], cell[1], cell[5], cell[4]});
                addFace({cell[2], cell[3], cell[7], cell[6]});
                addFace({cell[0], cell[3], cell[7], cell[4]});
                addFace({cell[1], cell[2], cell[6], cell[5]});
            } else {
                for (std::size_t i = 0; i + 1 < nv; ++i) {
                    FaceKey fk = {cell[0], cell[i], cell[i+1]};
                    std::sort(fk.begin(), fk.end());
                    addFace(fk);
                }
            }
        }
    }

    nFaces_ = faceMap.size();
    owner_.resize(nFaces_);
    neighbor_.resize(nFaces_);
    faceArea_.resize(nFaces_);
    faceNormal_.resize(nFaces_);
    faceCenter_.resize(nFaces_);
    faceDist_.resize(nFaces_, 0.0);
    faceWeights_.resize(nFaces_, 0.0);
    faceVertices_.resize(nFaces_);
    isBoundary_.assign(nCells_, false);

    std::size_t f = 0;
    for (const auto& [key, pr] : faceMap) {
        owner_[f] = pr.first;
        neighbor_[f] = pr.second;
        faceVertices_[f] = key;

        std::size_t nFaceVerts = key.size();
        for (std::size_t d = 0; d < Dim; ++d) {
            faceCenter_[f][d] = 0.0;
            for (std::size_t v = 0; v < nFaceVerts; ++v) {
                faceCenter_[f][d] += vertices_[key[v]][d];
            }
            faceCenter_[f][d] /= static_cast<double>(nFaceVerts);
        }

        if constexpr (Dim == 3) {
            double n[3] = {0, 0, 0};
            double mag = 0.0;
            if (nFaceVerts == 3) {
                const auto& p0 = vertices_[key[0]];
                const auto& p1 = vertices_[key[1]];
                const auto& p2 = vertices_[key[2]];
                double e1[3] = {p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2]};
                double e2[3] = {p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2]};
                n[0] = e1[1]*e2[2] - e1[2]*e2[1];
                n[1] = e1[2]*e2[0] - e1[0]*e2[2];
                n[2] = e1[0]*e2[1] - e1[1]*e2[0];
                mag = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
                faceArea_[f] = 0.5 * mag;
            } else if (nFaceVerts == 4) {
                const auto& p0 = vertices_[key[0]];
                const auto& p1 = vertices_[key[1]];
                const auto& p2 = vertices_[key[2]];
                const auto& p3 = vertices_[key[3]];
                double d1[3] = {p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2]};
                double d2[3] = {p3[0]-p1[0], p3[1]-p1[1], p3[2]-p1[2]};
                n[0] = d1[1]*d2[2] - d1[2]*d2[1];
                n[1] = d1[2]*d2[0] - d1[0]*d2[2];
                n[2] = d1[0]*d2[1] - d1[1]*d2[0];
                mag = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
                faceArea_[f] = 0.5 * mag;
            }

            if (neighbor_[f] != std::size_t(-1)) {
                const auto& oc = cellCenters_[owner_[f]];
                const auto& nc = cellCenters_[neighbor_[f]];
                double d[3] = {nc[0]-oc[0], nc[1]-oc[1], nc[2]-oc[2]};
                if (n[0]*d[0] + n[1]*d[1] + n[2]*d[2] < 0) {
                    n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
                }
            } else {
                const auto& oc = cellCenters_[owner_[f]];
                double d[3] = {faceCenter_[f][0]-oc[0], faceCenter_[f][1]-oc[1], faceCenter_[f][2]-oc[2]};
                if (n[0]*d[0] + n[1]*d[1] + n[2]*d[2] < 0) {
                    n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
                }
            }
            if (mag > 0) {
                faceNormal_[f][0] = n[0] / mag;
                faceNormal_[f][1] = n[1] / mag;
                faceNormal_[f][2] = n[2] / mag;
            } else {
                faceNormal_[f] = {};
            }
        } else if constexpr (Dim == 2) {
            const auto& p0 = vertices_[key[0]];
            const auto& p1 = vertices_[key[1]];
            double dx = p1[0] - p0[0], dy = p1[1] - p0[1];
            double mag = std::sqrt(dx*dx + dy*dy);
            faceArea_[f] = mag;

            double nx = dy, ny = -dx;
            if (neighbor_[f] != std::size_t(-1)) {
                const auto& oc = cellCenters_[owner_[f]];
                const auto& nc = cellCenters_[neighbor_[f]];
                double d[2] = {nc[0]-oc[0], nc[1]-oc[1]};
                if (nx*d[0] + ny*d[1] < 0) { nx = -nx; ny = -ny; }
            } else {
                const auto& oc = cellCenters_[owner_[f]];
                double d[2] = {faceCenter_[f][0]-oc[0], faceCenter_[f][1]-oc[1]};
                if (nx*d[0] + ny*d[1] < 0) { nx = -nx; ny = -ny; }
            }
            if (mag > 0) {
                faceNormal_[f][0] = nx / mag;
                faceNormal_[f][1] = ny / mag;
            } else {
                faceNormal_[f] = {};
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
            // weight = |xF - xP| / |xN - xP|
            double dPF2 = 0.0;
            for (std::size_t d = 0; d < Dim; ++d) {
                double diff = faceCenter_[f][d] - oc[d];
                dPF2 += diff * diff;
            }
            double distPF = std::sqrt(dPF2);
            double distPN = faceDist_[f];
            faceWeights_[f] = (distPN > 1e-12) ? distPF / distPN : 0.5;
        } else {
            const auto& oc = cellCenters_[owner_[f]];
            double dist2 = 0.0;
            for (std::size_t d = 0; d < Dim; ++d) {
                double diff = faceCenter_[f][d] - oc[d];
                dist2 += diff * diff;
            }
            faceDist_[f] = std::sqrt(dist2);
            faceWeights_[f] = 0.0;
        }

        if (neighbor_[f] == std::size_t(-1)) {
            isBoundary_[owner_[f]] = true;
        }

        f++;
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
                    // w = |xF - xP| / |xN - xP|
                    double dPF2 = 0.0;
                    for (std::size_t dd = 0; dd < Dim; ++dd) {
                        double diff = faceCenter_[f][dd] - oc[dd];
                        dPF2 += diff * diff;
                    }
                    double dPF = std::sqrt(dPF2);
                    double dPN = faceDist_[f];
                    faceWeights_[f] = (dPN > 1e-12) ? dPF / dPN : 0.5;
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
