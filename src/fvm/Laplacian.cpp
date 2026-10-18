#include "fvm/Laplacian.h"
#include "fvm/SSTConstants.h"

namespace fvm {

template<std::size_t Dim>
Laplacian<Dim>::Laplacian(const Mesh<Dim>& mesh, double k)
    : mesh_(mesh), k_(k) {}

template<std::size_t Dim>
void Laplacian<Dim>::ensureWorkspace_(std::size_t n) const {
    if (!workspaceInitialized_ || diagSum_.size() != n) {
        diagSum_.resize(n);
        triplets_.reserve(n * (2 * Dim + 1));
        workspaceInitialized_ = true;
    }
}

template<std::size_t Dim>
void Laplacian<Dim>::discretize(SparseMatrix& A, Eigen::VectorXd& b,
                                const std::vector<char>& dirichletMask,
                                const std::vector<double>& dirichletValues,
                                std::span<const double> source,
                                const std::array<Field<Dim>, Dim>* phiGrad) {
    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    const bool hasMask = !dirichletMask.empty();
    const bool hasSource = !source.empty();
    const bool hasGrad = (phiGrad != nullptr);

    ensureWorkspace_(n);
    diagSum_.fill(0.0);
    triplets_.clear();

    A.resize(n, n);
    b.resize(n);
    b.setZero();

    const double* areaPtr = mesh_.faceAreaPtr();
    const double* distPtr = mesh_.faceDistPtr();
    const std::size_t* ownerPtr = mesh_.ownerPtr();
    const std::size_t* neighborPtr = mesh_.neighborPtr();
    const double* volPtr = mesh_.cellVolumePtr();

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        if (N == std::size_t(-1)) {
            // Neumann zero-flux: no contribution to matrix (was incorrectly adding diag for no-mask case)
            // Dirichlet cells are handled via mask -> identity row, so skip all boundary faces here
            continue;
        }

        const double dist = distPtr[f];
        const double area = areaPtr[f];
        const double* fn = mesh_.faceNormalPtr(f);
        const double* xP = mesh_.cellCenterPtr(P);
        const double* xN = mesh_.cellCenterPtr(N);

        double d[Dim];
        for (std::size_t d2 = 0; d2 < Dim; ++d2)
            d[d2] = xN[d2] - xP[d2];

        double dotSfd = 0.0;
        for (std::size_t d2 = 0; d2 < Dim; ++d2)
            dotSfd += fn[d2] * d[d2];

        bool negativeDiffusion = (dotSfd < 0.0);
        double absDotSfd = negativeDiffusion ? -dotSfd : dotSfd;
        const double coeff = k_ * area * absDotSfd / (dist * dist);

        double correction = 0.0;
        if (hasGrad) {
            double Sf_nonorth[Dim];
            double dot_f_dhat = absDotSfd / dist;
            for (std::size_t d2 = 0; d2 < Dim; ++d2) {
                Sf_nonorth[d2] = area * fn[d2] - area * dot_f_dhat * d[d2] / dist;
            }
            for (std::size_t d2 = 0; d2 < Dim; ++d2)
                correction += k_ * (*phiGrad)[d2][P] * Sf_nonorth[d2];
            correction *= nonOrthCorrectionFactor;
        }

        if (negativeDiffusion) {
            if (!(hasMask && dirichletMask[P])) {
                b(P) += coeff * (hasMask && dirichletMask[N] ? dirichletValues[N] : 0.0);
                b(P) -= correction;
            }
            if (!(hasMask && dirichletMask[N])) {
                b(N) += coeff * (hasMask && dirichletMask[P] ? dirichletValues[P] : 0.0);
                b(N) += correction;
            }
        } else {
            if (!(hasMask && dirichletMask[P])) {
                if (hasMask && dirichletMask[N]) {
                    b(P) += coeff * dirichletValues[N];
                } else {
                    triplets_.emplace_back(P, N, -coeff);
                }
                diagSum_[P] += coeff;
                if (hasGrad) b(P) -= correction;
            }

            if (!(hasMask && dirichletMask[N])) {
                if (hasMask && dirichletMask[P]) {
                    b(N) += coeff * dirichletValues[P];
                } else {
                    triplets_.emplace_back(N, P, -coeff);
                }
                diagSum_[N] += coeff;
                if (hasGrad) b(N) += correction;
            }
        }
    }

    for (std::size_t P = 0; P < n; ++P) {
        if (hasMask && dirichletMask[P]) {
            triplets_.emplace_back(P, P, 1.0);
            b(P) = dirichletValues[P];
        } else {
            triplets_.emplace_back(P, P, diagSum_[P]);
            if (hasSource) {
                b(P) += source[P] * volPtr[P];
            }
        }
    }

    A.setFromTriplets(triplets_.begin(), triplets_.end());
    A.makeCompressed();
}

template<std::size_t Dim>
const Mesh<Dim>& Laplacian<Dim>::mesh() const { return mesh_; }

template<std::size_t Dim>
double Laplacian<Dim>::k() const { return k_; }

template class Laplacian<2>;
template class Laplacian<3>;

} // namespace fvm
