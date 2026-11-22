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
    limiter.configure(SlopeLimiterConfig{});  // Venkatakrishnan, CellWide, FaceCentre
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

    const std::size_t* ownerPtr = mesh_.ownerPtr();
    const std::size_t* neighborPtr = mesh_.neighborPtr();
    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        // Wall faces are excluded: the fit uses interior neighbours only
        // (see the face loop for why a reflected ghost cell was removed).
        if (N == std::size_t(-1)) continue;
        const double* xP = mesh_.cellCenterPtr(P);
        const double* xN = mesh_.cellCenterPtr(N);
        double dPN[Dim];
        for (std::size_t dd = 0; dd < Dim; ++dd) dPN[dd] = xN[dd] - xP[dd];
        auto& Mp = M_all_[P];
        auto& Mn = M_all_[N];
        for (std::size_t i = 0; i < Dim; ++i) {
            for (std::size_t j = 0; j < Dim; ++j) {
                const double contrib = dPN[i] * dPN[j];
                Mp(i, j) += contrib;
                Mn(i, j) += contrib;
            }
        }
    }

    // Rank-truncated pseudo-inverse of each normal matrix.
    //
    // A cell on a wall edge has only Dim-1 interior neighbours (walls are
    // excluded from the fit), so M is rank deficient. Its determinant is not
    // exactly zero - it is roundoff times the matrix scale - so a determinant
    // threshold compared against an absolute 1e-30 accepts it and inverts it,
    // producing gradients of order 1/eps. That is not an accuracy loss: it is a
    // divergent source term (|grad p| ~ 1e13 in a 3D tet run, which the
    // momentum equation then turns into |u| ~ 1e29).
    //
    // The complete orthogonal decomposition is rank revealing with a threshold
    // relative to the largest pivot, and its pseudo-inverse is the minimum-norm
    // least-squares solution: exact in the well-conditioned directions, zero in
    // the underdetermined one. The pressure matrix is assembled from this same
    // pseudo-inverse (see buildMatrices), so the correction still cancels the
    // divergence it was built for.
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
    const std::size_t nF = mesh_.nFaces();

    // The normal-equation matrix depends only on the cell-centre geometry, so it
    // is built once per Gradient; only the phi-dependent right-hand side is
    // refilled on each call.
    if (!normalMatrixBuilt_ || M_all_.size() != n) buildNormalMatrix_();
    if (rhs_all_.size() != n) rhs_all_.assign(n, Eigen::Matrix<double, Dim, 1>::Zero());
    else for (auto& r : rhs_all_) r.setZero();
    auto& rhs_all = rhs_all_;

    // Use raw pointers to avoid copying std::array per face (hot loop)
    const std::size_t* ownerPtr = mesh_.ownerPtr();
    const std::size_t* neighborPtr = mesh_.neighborPtr();
    const double* phiPtr = phi.ptr();
    const bool hasMask = !dirichletMask.empty();
    auto value = [&](std::size_t c) -> double {
        if (hasMask && c < dirichletMask.size() && dirichletMask[c]) return dirichletValues[c];
        return phiPtr[c];
    };

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        // Wall-adjacent cell: the gradient is fitted over the INTERIOR
        // neighbours only.
        //
        // A reflected ghost cell used to be added here, carrying a
        // zero-gradient (Neumann) condition: it contributed its direction
        // to the normal matrix and nothing to the right-hand side, so it
        // dragged the gradient of the wall cell towards zero in the
        // wall-normal direction.
        //
        // That is inconsistent with the pressure equation, whose wall face
        // contributes NOTHING at all (the flux through a wall is
        // prescribed, so it does not change with the pressure correction).
        // The two operators have to be the same one: where they are not,
        // the pressure correction cannot cancel the divergence it was
        // built for, and the error is largest exactly in the wall layer.
        //
        // The cell-centre gradient of a wall cell is an extrapolation
        // from the interior, which is the standard treatment.
        if (N == std::size_t(-1)) continue;
        const double* xP = mesh_.cellCenterPtr(P);
        const double* xN = mesh_.cellCenterPtr(N);
        double dPN[Dim];
        for (std::size_t dd = 0; dd < Dim; ++dd) dPN[dd] = xN[dd] - xP[dd];
        const double delta = value(N) - value(P);
        auto& rp = rhs_all[P];
        auto& rn = rhs_all[N];
        for (std::size_t i = 0; i < Dim; ++i) {
            rp(i) += dPN[i] * delta;
            rn(i) += dPN[i] * delta;
        }
    }

    for (std::size_t P = 0; P < n; ++P) {
        const Eigen::Matrix<double, Dim, 1> g = Mpinv_all_[P] * rhs_all[P];
        for (std::size_t d = 0; d < Dim; ++d)
            grad[d][P] = g(static_cast<Eigen::Index>(d));
    }
}

template<std::size_t Dim>
void Gradient<Dim>::buildMatrices(
    std::array<std::vector<std::vector<std::pair<std::size_t, double>>>, Dim>& out) const {
    if (scheme_ != GradientScheme::LeastSquares) {
        throw std::invalid_argument("buildMatrices is only defined for the least-squares gradient");
    }

    const std::size_t n = mesh_.nCells();
    for (auto& perDim : out) {
        perDim.clear();
        perDim.resize(n);
    }

    // Exactly the (rank-truncated) pseudo-inverse computeLeastSquares_ applies,
    // so the pressure equation assembled from these coefficients is the precise
    // composition of the divergence with the gradient the velocity correction
    // uses.
    if (!normalMatrixBuilt_ || M_all_.size() != n) buildNormalMatrix_();

    for (std::size_t C = 0; C < n; ++C) {
        const auto& Minv = Mpinv_all_[C];

        // The fit is linear in the field: the coefficient of phi_other is
        // (row d of M^-1) . d_other, and the coefficient of phi_C is its
        // negation.
        auto addCoefficients = [&](std::size_t other, const double* dOther) {
            for (std::size_t d = 0; d < Dim; ++d) {
                double k = 0.0;
                for (std::size_t i = 0; i < Dim; ++i)
                    k += Minv(static_cast<Eigen::Index>(d),
                              static_cast<Eigen::Index>(i)) * dOther[i];
                if (k == 0.0) continue;
                out[d][C].emplace_back(other, k);
                out[d][C].emplace_back(C, -k);
            }
        };

        for (std::size_t f : mesh_.cellFaces(C)) {
            const std::size_t Nf = mesh_.neighbor(f);
            if (Nf == std::size_t(-1))
                continue; // wall faces contribute nothing to the fit
            // C can be the face's OWNER (other cell = neighbour) or the face's
            // NEIGHBOUR (other cell = owner): the direction to the other cell
            // decides, and the same value difference times that direction
            // enters the fit either way - the accumulation below is
            // orientation independent (see computeLeastSquares_).
            const std::size_t O = (Nf == C) ? mesh_.owner(f) : Nf;
            const double* xC = mesh_.cellCenterPtr(C);
            const double* xO = mesh_.cellCenterPtr(O);
            double dOther[Dim];
            for (std::size_t i = 0; i < Dim; ++i)
                dOther[i] = xO[i] - xC[i];
            addCoefficients(O, dOther);
        }
    }
}

template<std::size_t Dim>
const Mesh<Dim>& Gradient<Dim>::mesh() const { return mesh_; }

template class Gradient<2>;
template class Gradient<3>;

} // namespace fvm
