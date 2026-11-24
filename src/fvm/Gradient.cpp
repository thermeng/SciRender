#include "fvm/Gradient.h"

#include "fvm/FaceState.h"
#include "fvm/SlopeLimiter.h"

#include <cmath>
#include <stdexcept>
#include <vector>

namespace fvm {

template<std::size_t Dim>
Gradient<Dim>::Gradient(const Mesh<Dim>& mesh, GradientScheme scheme)
    : mesh_(mesh), scheme_(scheme) {}

template<std::size_t Dim>
void Gradient<Dim>::setScheme(GradientScheme scheme) { scheme_ = scheme; }

template<std::size_t Dim>
GradientScheme Gradient<Dim>::scheme() const { return scheme_; }

template<std::size_t Dim>
double Gradient<Dim>::cellValue_(std::size_t cell, const Field<Dim>& phi,
                                 const std::vector<char>& dirichletMask,
                                 const std::vector<double>& dirichletValues) const {
    // A prescribed value is a CELL value: it replaces the value of the cell it
    // belongs to. It is never used as the value at a face. The value buffer is
    // checked independently of the mask: callers may pass a mask that is
    // longer than the value list, and a masked index must never read past it.
    if (!dirichletMask.empty() && cell < dirichletMask.size() && dirichletMask[cell] &&
        cell < dirichletValues.size())
        return dirichletValues[cell];
    return phi.ptr()[cell];
}

template<std::size_t Dim>
void Gradient<Dim>::compute(const Field<Dim>& phi,
                            std::array<Field<Dim>, Dim>& grad,
                            const std::vector<char>& dirichletMask,
                            const std::vector<double>& dirichletValues) const {
    for (auto& g : grad) g.assign(0.0);

    switch (scheme_) {
    case GradientScheme::GreenGaussCellBased:
        computeGreenGaussCellBased_(phi, grad, dirichletMask, dirichletValues);
        break;
    case GradientScheme::GreenGaussNodeBased:
        computeGreenGaussNodeBased_(phi, grad, dirichletMask, dirichletValues);
        break;
    case GradientScheme::LeastSquares:
    default:
        computeLeastSquares_(phi, grad, dirichletMask, dirichletValues);
        break;
    }

    if (limitGradients_) limitGradient_(phi, grad, dirichletMask, dirichletValues);
}

template<std::size_t Dim>
void Gradient<Dim>::limitGradient_(const Field<Dim>& phi,
                                   std::array<Field<Dim>, Dim>& grad,
                                   const std::vector<char>& dirichletMask,
                                   const std::vector<double>& dirichletValues) const {
    // Venkatakrishnan slope limiter (cell-wide aggregation): the per-cell alpha
    // is the minimum over the cell's faces, so scaling the whole gradient by it
    // keeps every face reconstruction inside the local bound set.
    SlopeLimiter<Dim> limiter(mesh_);
    limiter.configure(limiterConfig_);  // the solver-armed variant
    const std::vector<char>* mask =
        dirichletMask.empty() ? nullptr : &dirichletMask;
    const std::vector<double>* values =
        dirichletValues.empty() ? nullptr : &dirichletValues;
    limiter.prepare(phi, grad, mask, values);
    const std::size_t n = mesh_.nCells();
    for (std::size_t P = 0; P < n; ++P) {
        const double alpha = limiter.alphaCell(P);
        if (alpha >= 1.0) continue;
        for (std::size_t d = 0; d < Dim; ++d) grad[d][P] *= alpha;
    }
}

// ---------------------------------------------------------------------------
// Green-Gauss
// ---------------------------------------------------------------------------

template<std::size_t Dim>
void Gradient<Dim>::accumulateGreenGauss_(const std::vector<double>& phiFace,
                                          std::array<Field<Dim>, Dim>& grad) const {
    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    const std::size_t* ownerPtr = mesh_.ownerPtr();
    const std::size_t* neighborPtr = mesh_.neighborPtr();
    const double* areaPtr = mesh_.faceAreaPtr();
    const double* volPtr = mesh_.cellVolumePtr();

    std::vector<double> invVol(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double v = volPtr[i];
        invVol[i] = (v > 1e-30) ? 1.0 / v : 0.0;
    }

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        const double* fn = mesh_.faceNormalPtr(f);
        const double contribScale = phiFace[f] * areaPtr[f];
        for (std::size_t d = 0; d < Dim; ++d) {
            const double contrib = contribScale * fn[d];
            grad[d][P] += contrib * invVol[P];
            if (N != std::size_t(-1))
                grad[d][N] -= contrib * invVol[N];
        }
    }
}

template<std::size_t Dim>
void Gradient<Dim>::computeGreenGaussCellBased_(const Field<Dim>& phi,
                                                 std::array<Field<Dim>, Dim>& grad,
                                                 const std::vector<char>& dirichletMask,
                                                 const std::vector<double>& dirichletValues) const {
    const std::size_t nF = mesh_.nFaces();
    const std::size_t* ownerPtr = mesh_.ownerPtr();
    const std::size_t* neighborPtr = mesh_.neighborPtr();

    std::vector<double> phiFace(nF, 0.0);
    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        if (N == std::size_t(-1)) {
            // A wall: the face belongs to the cell that owns it, so the face
            // value is that cell's value - the prescribed one for a cell with a
            // prescribed value, the cell value itself otherwise (a
            // zero-gradient wall).
            phiFace[f] = cellValue_(P, phi, dirichletMask, dirichletValues);
        } else {
            // The exact linear interpolation between the two cell centres: the
            // mesh's faceWeight is the parameter of the face centre on the
            // owner->neighbour segment. A plain 0.5 average is only correct on
            // a uniform grid and drops to first order on a skewed mesh.
            phiFace[f] = faceValueFromCentres(cellValue_(P, phi, dirichletMask, dirichletValues),
                                              cellValue_(N, phi, dirichletMask, dirichletValues),
                                              mesh_.faceWeight(f));
        }
    }

    accumulateGreenGauss_(phiFace, grad);
}

template<std::size_t Dim>
void Gradient<Dim>::computeGreenGaussNodeBased_(const Field<Dim>& phi,
                                                 std::array<Field<Dim>, Dim>& grad,
                                                 const std::vector<char>& dirichletMask,
                                                 const std::vector<double>& dirichletValues) const {
    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    const std::size_t* ownerPtr = mesh_.ownerPtr();

    std::vector<double> phiFace(nF, 0.0);

    if (mesh_.isUnstructured()) {
        // The nodal values: the average of the cells around each node.
        const auto& verts = mesh_.vertices();
        std::vector<double> nodeVal(verts.size(), 0.0);
        std::vector<int> nodeCount(verts.size(), 0);
        for (std::size_t c = 0; c < n; ++c) {
            const double val = cellValue_(c, phi, dirichletMask, dirichletValues);
            for (std::size_t vid : mesh_.cellVertices(c)) {
                if (vid < nodeVal.size()) {
                    nodeVal[vid] += val;
                    nodeCount[vid]++;
                }
            }
        }
        for (std::size_t v = 0; v < nodeVal.size(); ++v)
            if (nodeCount[v] > 0) nodeVal[v] /= static_cast<double>(nodeCount[v]);

        for (std::size_t f = 0; f < nF; ++f) {
            double sum = 0.0;
            int cnt = 0;
            for (std::size_t vid : mesh_.faceVertices(f)) {
                if (vid < nodeVal.size()) {
                    sum += nodeVal[vid];
                    ++cnt;
                }
            }
            phiFace[f] = (cnt > 0) ? sum / cnt
                                    : cellValue_(ownerPtr[f], phi, dirichletMask, dirichletValues);
        }
    } else {
        // A structured mesh carries the same information in its grid: the nodes
        // are the grid points. They are reconstructed here instead of reading
        // the vertex arrays of the mesh, which a structured mesh does not have
        // (reading them was an out-of-bounds access).
        const auto& dims = mesh_.dims();
        std::array<std::size_t, Dim> nodeDim{};
        std::size_t nNodes = 1;
        for (std::size_t d = 0; d < Dim; ++d) {
            nodeDim[d] = dims[d] + 1;
            nNodes *= nodeDim[d];
        }
        auto nodeIndex = [&](const std::array<std::size_t, Dim>& g) {
            std::size_t idx = 0;
            for (std::size_t d = 0; d < Dim; ++d) idx = idx * nodeDim[d] + g[d];
            return idx;
        };

        // The nodal values: the average of the cells around each node. The cell
        // (i, j) owns the corners (i|i+1, j|j+1).
        std::vector<double> nodeVal(nNodes, 0.0);
        std::vector<int> nodeCount(nNodes, 0);
        for (std::size_t c = 0; c < n; ++c) {
            const auto cc = mesh_.cellCoords(c);
            const double val = cellValue_(c, phi, dirichletMask, dirichletValues);
            for (std::size_t corner = 0; corner < (std::size_t(1) << Dim); ++corner) {
                std::array<std::size_t, Dim> g{};
                for (std::size_t d = 0; d < Dim; ++d)
                    g[d] = cc[d] + ((corner >> d) & std::size_t(1));
                const std::size_t node = nodeIndex(g);
                nodeVal[node] += val;
                nodeCount[node]++;
            }
        }
        for (std::size_t v = 0; v < nNodes; ++v)
            if (nodeCount[v] > 0) nodeVal[v] /= static_cast<double>(nodeCount[v]);

        for (std::size_t f = 0; f < nF; ++f) {
            const std::size_t P = ownerPtr[f];
            const double* fn = mesh_.faceNormalPtr(f);
            // The face's direction: the dominant component of its normal.
            std::size_t d = 0;
            for (std::size_t dd = 1; dd < Dim; ++dd)
                if (std::abs(fn[dd]) > std::abs(fn[d])) d = dd;
            const auto cc = mesh_.cellCoords(P);
            // The face sits at the owner's high side when the normal points
            // along the axis and at its low side otherwise; the transverse
            // coordinates are the owner's.
            std::array<std::size_t, Dim> g{};
            g[d] = cc[d] + (fn[d] > 0.0 ? std::size_t(1) : std::size_t(0));
            for (std::size_t dd = 0; dd < Dim; ++dd)
                if (dd != d) g[dd] = cc[dd];
            // The face spans the transverse directions.
            double sum = 0.0;
            int cnt = 0;
            const std::size_t nTrans = Dim - 1;
            for (std::size_t subset = 0; subset < (std::size_t(1) << nTrans); ++subset) {
                std::array<std::size_t, Dim> ng = g;
                std::size_t k = 0;
                for (std::size_t dd = 0; dd < Dim; ++dd) {
                    if (dd == d) continue;
                    ng[dd] += (subset >> k) & std::size_t(1);
                    ++k;
                }
                sum += nodeVal[nodeIndex(ng)];
                ++cnt;
            }
            phiFace[f] = (cnt > 0) ? sum / cnt
                                    : cellValue_(P, phi, dirichletMask, dirichletValues);
        }
    }

    accumulateGreenGauss_(phiFace, grad);
}

// ---------------------------------------------------------------------------
// Least squares
// ---------------------------------------------------------------------------

template<std::size_t Dim>
void Gradient<Dim>::buildNormalMatrix_() const {
    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    M_all_.assign(n, Eigen::Matrix<double, Dim, Dim>::Zero());
    stencil_.assign(n, {});

    // Interior neighbours only. Boundary faces are not part of the fit: a
    // prescribed value is a CELL value, and a wall face has no known face
    // value, so adding it as a mirror ghost with zero difference would halve
    // the one-sided gradient of every wall cell.
    std::vector<std::vector<std::size_t>> immediate(n);
    const std::size_t* ownerPtr = mesh_.ownerPtr();
    const std::size_t* neighborPtr = mesh_.neighborPtr();
    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        if (N == std::size_t(-1)) continue;
        immediate[P].push_back(N);
        immediate[N].push_back(P);
    }

    auto addEntry = [&](std::size_t P, std::size_t Q) {
        const double* xP = mesh_.cellCenterPtr(P);
        const double* xQ = mesh_.cellCenterPtr(Q);
        StencilEntry e;
        e.cell = Q;
        for (std::size_t dd = 0; dd < Dim; ++dd) e.d[dd] = xQ[dd] - xP[dd];
        stencil_[P].push_back(e);
        for (std::size_t i = 0; i < Dim; ++i)
            for (std::size_t j = 0; j < Dim; ++j)
                M_all_[P](i, j) += e.d[i] * e.d[j];
    };
    auto fullRank = [&](std::size_t P) {
        Eigen::CompleteOrthogonalDecomposition<Eigen::Matrix<double, Dim, Dim>> cod(M_all_[P]);
        cod.setThreshold(1e-12);
        return cod.rank() == Dim;
    };

    for (std::size_t P = 0; P < n; ++P) {
        for (std::size_t N : immediate[P]) addEntry(P, N);
        if (fullRank(P)) continue;
        // Only a cell whose interior neighbours cannot span Dim directions
        // needs boundary information: a corner/edge tet with one or two
        // interior neighbours. Add the wall faces as mirrored ghost cells with
        // a zero value difference (zero-normal-gradient constraint). A
        // structured wall cell already has full rank and keeps its one-sided
        // interior fit, so its prescribed-cell gradient is not diluted.
        for (std::size_t f = 0; f < nF; ++f) {
            if (ownerPtr[f] != P || neighborPtr[f] != std::size_t(-1)) continue;
            const double* xP = mesh_.cellCenterPtr(P);
            const double* xF = mesh_.faceCenterPtr(f);
            StencilEntry e;
            e.cell = P; // phi_P - phi_P = 0: no right-hand side contribution
            for (std::size_t dd = 0; dd < Dim; ++dd) e.d[dd] = 2.0 * (xF[dd] - xP[dd]);
            stencil_[P].push_back(e);
            for (std::size_t i = 0; i < Dim; ++i)
                for (std::size_t j = 0; j < Dim; ++j)
                    M_all_[P](i, j) += e.d[i] * e.d[j];
            if (fullRank(P)) break;
        }
    }

    // Rank-truncated pseudo-inverse of each normal matrix. With the guarded
    // ghost constraints above this only affects truly isolated cells.
    Mpinv_all_.assign(n, Eigen::Matrix<double, Dim, Dim>::Zero());
    for (std::size_t P = 0; P < n; ++P) {
        Eigen::CompleteOrthogonalDecomposition<Eigen::Matrix<double, Dim, Dim>> cod(
            M_all_[P]);
        cod.setThreshold(1e-12);
        Mpinv_all_[P] = cod.pseudoInverse();
    }

    normalMatrixBuilt_ = true;
}

template<std::size_t Dim>
void Gradient<Dim>::computeLeastSquares_(const Field<Dim>& phi,
                                          std::array<Field<Dim>, Dim>& grad,
                                          const std::vector<char>& dirichletMask,
                                          const std::vector<double>& dirichletValues) const {
    const std::size_t n = mesh_.nCells();

    // The normal-equation matrix depends only on the cell-centre geometry, so it
    // is built once per Gradient; only the phi-dependent right-hand side is
    // refilled on each call.
    if (!normalMatrixBuilt_ || M_all_.size() != n) buildNormalMatrix_();
    if (rhs_all_.size() != n) rhs_all_.assign(n, Eigen::Matrix<double, Dim, 1>::Zero());
    else for (auto& r : rhs_all_) r.setZero();
    auto& rhs_all = rhs_all_;

    const double* phiPtr = phi.ptr();
    const bool hasMask = !dirichletMask.empty();
    auto value = [&](std::size_t c) -> double {
        if (hasMask && c < dirichletMask.size() && dirichletMask[c]) return dirichletValues[c];
        return phiPtr[c];
    };

    // The right-hand side mirrors the stencil built above: one equation per
    // neighbour, (grad phi) . d = phi_neighbour - phi_P.
    for (std::size_t P = 0; P < n; ++P) {
        auto& rp = rhs_all[P];
        for (const auto& e : stencil_[P]) {
            const double delta = value(e.cell) - value(P);
            for (std::size_t i = 0; i < Dim; ++i) rp(i) += e.d[i] * delta;
        }
    }

    for (std::size_t P = 0; P < n; ++P) {
        const Eigen::Matrix<double, Dim, 1> g = Mpinv_all_[P] * rhs_all[P];
        for (std::size_t d = 0; d < Dim; ++d)
            grad[d][P] = g(static_cast<Eigen::Index>(d));
    }
}


template<std::size_t Dim>
const Mesh<Dim>& Gradient<Dim>::mesh() const { return mesh_; }

template class Gradient<2>;
template class Gradient<3>;

} // namespace fvm
