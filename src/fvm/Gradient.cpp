#include "fvm/Gradient.h"

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
            const double w = mesh_.faceWeight(f);
            phiFace[f] = (1.0 - w) * cellValue_(P, phi, dirichletMask, dirichletValues)
                       + w * cellValue_(N, phi, dirichletMask, dirichletValues);
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
void Gradient<Dim>::computeLeastSquares_(const Field<Dim>& phi,
                                          std::array<Field<Dim>, Dim>& grad,
                                          const std::vector<char>& dirichletMask,
                                          const std::vector<double>& dirichletValues) const {
    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();

    if (M_all_.size() != n) {
        M_all_.assign(n, Eigen::Matrix<double, Dim, Dim>::Zero());
        rhs_all_.assign(n, Eigen::Matrix<double, Dim, 1>::Zero());
    } else {
        for (auto& m : M_all_) m.setZero();
        for (auto& r : rhs_all_) r.setZero();
    }
    auto& M_all = M_all_;
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
        const double* xP = mesh_.cellCenterPtr(P);
        if (N == std::size_t(-1)) {
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
            continue;
        } else {
            const double* xN = mesh_.cellCenterPtr(N);
            double dPN[Dim];
            for (std::size_t dd = 0; dd < Dim; ++dd) dPN[dd] = xN[dd] - xP[dd];
            const double delta = value(N) - value(P);
            auto& Mp = M_all[P];
            auto& Mn = M_all[N];
            auto& rp = rhs_all[P];
            auto& rn = rhs_all[N];
            for (std::size_t i = 0; i < Dim; ++i) {
                for (std::size_t j = 0; j < Dim; ++j) {
                    double contrib = dPN[i] * dPN[j];
                    Mp(i, j) += contrib;
                    Mn(i, j) += contrib;
                }
                rp(i) += dPN[i] * delta;
                rn(i) += dPN[i] * delta;
            }
        }
    }

    for (std::size_t P = 0; P < n; ++P) {
        if constexpr (Dim == 2) {
            double m00 = M_all[P](0, 0), m01 = M_all[P](0, 1);
            double m10 = M_all[P](1, 0), m11 = M_all[P](1, 1);
            double det = m00 * m11 - m01 * m10;
            if (std::abs(det) > 1e-30) {
                double invDet = 1.0 / det;
                grad[0][P] = (rhs_all[P](0) * m11 - m01 * rhs_all[P](1)) * invDet;
                grad[1][P] = (m00 * rhs_all[P](1) - rhs_all[P](0) * m10) * invDet;
            } else {
                // Singular (e.g., 3x1 mesh has no y variation) - fallback to 1D LSQ per direction
                if (std::abs(m00) > 1e-30) grad[0][P] = rhs_all[P](0) / m00;
                else grad[0][P] = 0.0;
                if (std::abs(m11) > 1e-30) grad[1][P] = rhs_all[P](1) / m11;
                else grad[1][P] = 0.0;
            }
        } else {
            double m00 = M_all[P](0, 0), m01 = M_all[P](0, 1), m02 = M_all[P](0, 2);
            double m10 = M_all[P](1, 0), m11 = M_all[P](1, 1), m12 = M_all[P](1, 2);
            double m20 = M_all[P](2, 0), m21 = M_all[P](2, 1), m22 = M_all[P](2, 2);
            double det = m00 * (m11 * m22 - m12 * m21)
                       - m01 * (m10 * m22 - m12 * m20)
                       + m02 * (m10 * m21 - m11 * m20);
            if (std::abs(det) > 1e-30) {
                double invDet = 1.0 / det;
                double r0 = rhs_all[P](0), r1 = rhs_all[P](1), r2 = rhs_all[P](2);
                grad[0][P] = ((m11 * m22 - m12 * m21) * r0
                            + (m02 * m21 - m01 * m22) * r1
                            + (m01 * m12 - m02 * m11) * r2) * invDet;
                grad[1][P] = ((m12 * m20 - m10 * m22) * r0
                            + (m00 * m22 - m02 * m20) * r1
                            + (m02 * m10 - m00 * m12) * r2) * invDet;
                grad[2][P] = ((m10 * m21 - m11 * m20) * r0
                            + (m01 * m20 - m00 * m21) * r1
                            + (m00 * m11 - m01 * m10) * r2) * invDet;
            } else {
                // Singular (e.g., 2D mesh in 3D) - fallback to diagonal
                if (std::abs(m00) > 1e-30) grad[0][P] = rhs_all[P](0) / m00; else grad[0][P] = 0.0;
                if (std::abs(m11) > 1e-30) grad[1][P] = rhs_all[P](1) / m11; else grad[1][P] = 0.0;
                if (std::abs(m22) > 1e-30) grad[2][P] = rhs_all[P](2) / m22; else grad[2][P] = 0.0;
            }
        }
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

    // The normal matrix M of the per-cell fit, exactly as computeLeastSquares_
    // builds it: an interior face's direction enters squared, which makes the
    // accumulation independent of which side this cell is on.
    std::vector<Eigen::Matrix<double, Dim, Dim>> M(n, Eigen::Matrix<double, Dim, Dim>::Zero());
    const std::size_t nF = mesh_.nFaces();
    const std::size_t* ownerPtr = mesh_.ownerPtr();
    const std::size_t* neighborPtr = mesh_.neighborPtr();

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        if (N == std::size_t(-1))
            continue; // a wall face contributes nothing to the fit
        const double* xP = mesh_.cellCenterPtr(P);
        const double* xN = mesh_.cellCenterPtr(N);
        for (std::size_t i = 0; i < Dim; ++i) {
            for (std::size_t j = 0; j < Dim; ++j) {
                const double contrib = (xN[i] - xP[i]) * (xN[j] - xP[j]);
                M[P](i, j) += contrib;
                M[N](i, j) += contrib;
            }
        }
    }

    for (std::size_t C = 0; C < n; ++C) {
        const auto& Mc = M[C];

        // The fit g = M^-1 * sum_f d_f (phi_other - phi_C) is linear in the
        // field: the coefficient of phi_other is M^-1 d, and the coefficient
        // of phi_C is its negation. The branch structure (invertible / the
        // per-direction fallback) mirrors computeLeastSquares_ so both give
        // the same gradient on every mesh.
        auto addCoefficients = [&](std::size_t other, const double* dOther) {
            for (std::size_t d = 0; d < Dim; ++d) {
                double self = 0.0;
                bool known = false;
                double k = 0.0;
                if constexpr (Dim == 2) {
                    const double m00 = Mc(0, 0), m01 = Mc(0, 1), m10 = Mc(1, 0), m11 = Mc(1, 1);
                    const double det = m00 * m11 - m01 * m10;
                    if (std::abs(det) > 1e-30) {
                        const double invDet = 1.0 / det;
                        // Rows of the 2x2 inverse: row 0 = {m11,-m01} * invDet, row 1 = {-m10, m00} * invDet.
                        const double r0 = (d == 0 ? m11 : -m10) * invDet;
                        const double r1 = (d == 0 ? -m01 : m00) * invDet;
                        k = r0 * dOther[0] + r1 * dOther[1];
                        known = true;
                    } else if (std::abs(Mc(d, d)) > 1e-30) {
                        k = dOther[d] / Mc(d, d); // per-direction fallback
                        known = true;
                    }
                } else {
                    const double m00 = Mc(0, 0), m01 = Mc(0, 1), m02 = Mc(0, 2);
                    const double m10 = Mc(1, 0), m11 = Mc(1, 1), m12 = Mc(1, 2);
                    const double m20 = Mc(2, 0), m21 = Mc(2, 1), m22 = Mc(2, 2);
                    const double det = m00 * (m11 * m22 - m12 * m21)
                                     - m01 * (m10 * m22 - m12 * m20)
                                     + m02 * (m10 * m21 - m11 * m20);
                    if (std::abs(det) > 1e-30) {
                        const double invDet = 1.0 / det;
                        const double rows[3][3] = {
                            {m11 * m22 - m12 * m21, m02 * m21 - m01 * m22, m01 * m12 - m02 * m11},
                            {m12 * m20 - m10 * m22, m00 * m22 - m02 * m20, m02 * m10 - m00 * m12},
                            {m10 * m21 - m11 * m20, m01 * m20 - m00 * m21, m00 * m11 - m01 * m10}};
                        k = (rows[d][0] * dOther[0] + rows[d][1] * dOther[1] + rows[d][2] * dOther[2]) * invDet;
                        known = true;
                    } else if (std::abs(Mc(d, d)) > 1e-30) {
                        k = dOther[d] / Mc(d, d);
                        known = true;
                    }
                }
                if (known) {
                    out[d][C].emplace_back(other, k);
                    self = -k;
                }
                if (self != 0.0)
                    out[d][C].emplace_back(C, self);
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
