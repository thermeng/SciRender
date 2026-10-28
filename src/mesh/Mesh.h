#pragma once

#include <array>
#include <cstddef>
#include <stdexcept>
#include <vector>
#include <cmath>
#include <map>
#include <string>
#include <algorithm>
#include <unordered_map>

namespace fvm {

template<std::size_t Dim>
class Mesh {
public:
    Mesh() = default;

    template<typename... Args>
     requires (sizeof...(Args) == 2 * Dim - 1)
    Mesh(std::size_t first, Args... rest) {
        std::array<std::size_t, Dim> n{};
        std::array<double, Dim> l{};
        std::size_t arr[] = {first, static_cast<std::size_t>(rest)...};
        double lengthsArr[] = {static_cast<double>(rest)...};
        for (std::size_t d = 0; d < Dim; ++d) {
            n[d] = arr[d];
            l[d] = lengthsArr[Dim - 1 + d];
        }
        initUniform_(n, l);
    }

    Mesh(std::array<std::size_t, Dim> n, std::array<double, Dim> l);

    static Mesh makeStretching(std::array<std::size_t, Dim> n,
                               std::array<double, Dim> l,
                               std::array<double, Dim> ratio);

    // Unstructured mesh.
    //
    // `cells` holds the vertex indices of every cell. `cellFaces` is optional:
    // when it is provided it holds, for every cell, the ordered vertex loop of
    // each of its faces. Supplying it is the only way to describe a cell whose
    // topology is not implied by its vertex count (a prism, a pyramid, a general
    // polyhedron) or whose vertices are not in the VTK/Gmsh element order
    // (hexahedra read from other formats). When it is empty the topology is
    // inferred: edges for 2D polygons, the four triangles of a tetrahedron and
    // the six VTK-ordered faces of a hexahedron.
    Mesh(std::vector<std::array<double, Dim>> vertices,
         std::vector<std::vector<std::size_t>> cells,
         std::vector<std::vector<std::vector<std::size_t>>> cellFaces = {});

    std::size_t nCells() const;
    const std::array<std::size_t, Dim>& dims() const;
    const std::array<double, Dim>& lengths() const;
    const std::array<double, Dim>& spacings() const;
    const std::array<std::size_t, Dim>& strides() const;
    bool isUniform() const;
    bool isUnstructured() const;

    std::size_t n(std::size_t d) const;
    double dx(std::size_t d) const;
    double lx(std::size_t d) const;
    double dx(std::size_t d, std::size_t cellIdx) const;
    double faceCoord(std::size_t d, std::size_t faceIdx) const;
    std::array<double, Dim> cellCenter(std::size_t idx) const;
    double faceArea(std::size_t d, std::size_t idx) const;
    double cellVolume(std::size_t idx) const;
    // Interpolation weight for face f: phi_f = (1-w)*phi_P + w*phi_N.
    // w is the parameter of the orthogonal projection of the face centre onto
    // the owner->neighbour segment, so it is the exact linear-interpolation
    // coefficient between the two cell centres. It is 0.5 on a uniform mesh.
    double faceWeight(std::size_t f) const;
    // Distance-weighted face value: (1-w)*phi_P + w*phi_N, or phi_P for boundary
    template<typename T>
    T faceValue(std::size_t f, const T& phiP, const T& phiN) const {
        if (neighbor_[f] == std::size_t(-1)) return phiP;
        double w = faceWeights_[f];
        return (1.0 - w) * phiP + w * phiN;
    }

    template<typename... Coord>
     requires (sizeof...(Coord) == Dim)
    std::size_t cellIndex(Coord... coords) const {
        std::array<std::size_t, Dim> c = {static_cast<std::size_t>(coords)...};
        return cellIndex_(c);
    }

    std::size_t cellIndex(const std::array<std::size_t, Dim>& c) const;
    std::array<std::size_t, Dim> cellCoords(std::size_t idx) const;
    bool isBoundary(std::size_t idx) const;

    template<typename... Coord>
     requires (sizeof...(Coord) == Dim)
    bool isBoundary(Coord... coords) const {
        std::array<std::size_t, Dim> c = {static_cast<std::size_t>(coords)...};
        for (std::size_t d = 0; d < Dim; ++d) {
            if (c[d] == 0 || c[d] == dims_[d] - 1) return true;
        }
        return false;
    }

    std::pair<std::size_t, int> neighborOffset(std::size_t idx, std::size_t d, int sign) const;

    std::size_t nFaces() const;
    std::size_t owner(std::size_t f) const;
    std::size_t neighbor(std::size_t f) const;
    double faceArea(std::size_t f) const;
    const std::array<double, Dim>& faceNormal(std::size_t f) const;
    const std::array<double, Dim>& faceCenter(std::size_t f) const;
    double faceDist(std::size_t f) const;
    std::size_t nFacesOnBoundary(std::size_t d, int sign) const;
    const std::vector<std::size_t>& boundaryFaces(std::size_t d, int sign) const;

    const std::vector<std::array<double, Dim>>& vertices() const;
    const std::vector<std::vector<std::size_t>>& cells() const;

    void setBoundaryGroups(std::map<std::string, std::vector<std::size_t>> groups);
    const std::map<std::string, std::vector<std::size_t>>& boundaryGroups() const;
    const std::vector<std::size_t>* boundaryGroupFaces(const std::string& name) const;
    std::vector<std::string> boundaryGroupNames() const;

    const std::vector<std::size_t>& cellVertices(std::size_t idx) const;
    const std::vector<std::size_t>& faceVertices(std::size_t f) const;
    const std::vector<std::size_t>& cellNeighbors(std::size_t idx) const;
    const std::vector<std::size_t>& cellFaces(std::size_t idx) const;

    // Hot-path optimized accessors for inner loops
    const std::size_t* ownerPtr() const noexcept { return owner_.data(); }
    const std::size_t* neighborPtr() const noexcept { return neighbor_.data(); }
    const double* faceAreaPtr() const noexcept { return faceArea_.data(); }
    const double* faceDistPtr() const noexcept { return faceDist_.data(); }
    const double* faceWeightPtr() const noexcept { return faceWeights_.data(); }
    const double* cellVolumePtr() const noexcept { return cellVolumes_.data(); }
    const double* cellCenterPtr(std::size_t idx) const noexcept { return cellCenters_[idx].data(); }
    const double* faceNormalPtr(std::size_t f) const noexcept { return faceNormal_[f].data(); }
    const double* faceCenterPtr(std::size_t f) const noexcept { return faceCenter_[f].data(); }

    std::array<std::size_t, Dim> dims_;
    std::array<double, Dim> lengths_{};
    std::array<double, Dim> spacings_{};
    std::array<std::size_t, Dim> strides_{};
    std::size_t nCells_ = 0;
    bool uniform_ = true;
    std::array<std::vector<double>, Dim> faceCoords_;

    std::size_t nFaces_ = 0;
    std::vector<std::size_t> owner_;
    std::vector<std::size_t> neighbor_;
    std::vector<double> faceArea_;
    std::vector<std::array<double, Dim>> faceNormal_;
    std::vector<std::array<double, Dim>> faceCenter_;
    std::vector<double> faceDist_;
    std::vector<double> faceWeights_;

    std::vector<std::array<double, Dim>> vertices_;
    std::vector<std::vector<std::size_t>> cells_;
    // Optional explicit face topology: cellFaceVerts_[cell][face] = ordered
    // vertex loop of that face. Empty => the topology is inferred from the
    // cell's vertex list.
    std::vector<std::vector<std::vector<std::size_t>>> cellFaceVerts_;
    std::vector<std::array<double, Dim>> cellCenters_;
    std::vector<double> cellVolumes_;
    std::vector<std::vector<std::size_t>> faceVertices_;
    std::vector<char> isBoundary_;
    bool unstructured_ = false;
    std::map<std::string, std::vector<std::size_t>> boundaryGroups_;
    // Optimized adjacency (O(1) neighbor lookup)
    std::vector<std::vector<std::size_t>> cellNeighbors_;
    std::vector<std::vector<std::size_t>> cellFaces_;
    std::vector<std::vector<std::size_t>> boundaryFacesCache_;
    std::vector<std::size_t> nFacesOnBoundaryCache_;

private:
    std::size_t cellIndex_(const std::array<std::size_t, Dim>& c) const;
    void precomputeCellData_();
    void initUniform_(std::array<std::size_t, Dim> n, std::array<double, Dim> l);
    static std::vector<double> geometricStretching_(std::size_t n, double L, double r);
    std::array<double, Dim> computeCellCenter_(const std::vector<std::size_t>& cell) const;
    // Ordered vertex loops of the faces of cell `ci` (explicit topology when
    // supplied, otherwise inferred from the cell's vertex list).
    std::vector<std::vector<std::size_t>> cellFaceLoops_(std::size_t ci) const;
    // Area, unit normal and centroid of a 3D polygon given as an ordered
    // vertex loop. The normal follows the loop orientation, the area is the
    // true surface area (fan triangulation) so warped quads are handled.
    void polygonGeometry3D_(const std::vector<std::size_t>& loop,
                            double& area,
                            std::array<double, 3>& normal,
                            std::array<double, 3>& centroid) const;
    // Volume and centroid of every unstructured cell.
    void computeUnstructuredCellData_();
    void computeUnstructuredFaceData_();
    void computeFaceData_();
    void buildAdjacencyAndBoundaryCache_();
    struct FaceKeyHash {
        std::size_t operator()(const std::vector<std::size_t>& k) const noexcept {
            std::size_t h = k.size();
            for (auto v : k) h ^= std::hash<std::size_t>{}(v + 0x9e3779b97f4a7c15ULL + (h<<6) + (h>>2));
            return h;
        }
    };
};

extern template class Mesh<2>;
extern template class Mesh<3>;

} // namespace fvm
