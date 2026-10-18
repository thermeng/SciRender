#include "fvm/Gradient.h"

namespace fvm {

template<std::size_t Dim>
Gradient<Dim>::Gradient(const Mesh<Dim>& mesh) : mesh_(mesh) {}

template<std::size_t Dim>
void Gradient<Dim>::compute(const Field<Dim>& phi,
                            std::array<Field<Dim>, Dim>& grad) const {
    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();

    for (auto& g : grad) g.assign(0.0);

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
    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        const double* xP = mesh_.cellCenterPtr(P);
        if (N == std::size_t(-1)) {
            // Skip boundary faces - they bias LSQ without rhs contribution (was adding d*d^T with no delta)
            continue;
        } else {
            const double* xN = mesh_.cellCenterPtr(N);
            double dPN[Dim];
            for (std::size_t dd = 0; dd < Dim; ++dd) dPN[dd] = xN[dd] - xP[dd];
            double delta = phiPtr[N] - phiPtr[P];
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
const Mesh<Dim>& Gradient<Dim>::mesh() const { return mesh_; }

template class Gradient<2>;
template class Gradient<3>;

} // namespace fvm
