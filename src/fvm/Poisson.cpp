#include "fvm/Poisson.h"
#include "fvm/Validation.h"
#include "fvm/SSTConstants.h"

#include <stdexcept>

namespace fvm {

template<std::size_t Dim>
Poisson<Dim>::Poisson(const Mesh<Dim>& mesh) : mesh_(mesh) {}

template<std::size_t Dim>
void Poisson<Dim>::discretize(SparseMatrix& A, Eigen::VectorXd& b,
                              const Field<Dim>& c,
                              const Field<Dim>& source,
                              const std::vector<char>& dirichletMask,
                              const std::vector<double>& dirichletValues,
                              const std::array<Field<Dim>, Dim>* phiGrad) {
    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    const bool hasMask = !dirichletMask.empty();
    const bool hasGrad = (phiGrad != nullptr);

    validateDirichlet(n, dirichletMask, dirichletValues);

    A.resize(n, n);
    b.resize(n);
    b.setZero();

    std::vector<double> diagSum(n, 0.0);
    std::vector<Triplet> triplets;
    triplets.reserve(n * (2 * Dim + 1));

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = mesh_.owner(f);
        const std::size_t N = mesh_.neighbor(f);
    if (N == std::size_t(-1)) {
        // Boundary face: Dirichlet cells are handled via identity rows below;
        // unmasked (Neumann/outlet/symmetry) cells contribute zero flux.
        // (Previously this added a diag term for unmasked cells, which
        // incorrectly imposed phi=0 at Neumann boundaries.)
        continue;
    }

        const double dist = mesh_.faceDist(f);
        const double area = mesh_.faceArea(f);
        const double w = mesh_.faceWeight(f);
        const double cFace = (1.0 - w) * c[P] + w * c[N];
        const double* fn = mesh_.faceNormalPtr(f);
        const double* xP = mesh_.cellCenterPtr(P);
        const double* xN = mesh_.cellCenterPtr(N);
        double delta[Dim];
        for (std::size_t d = 0; d < Dim; ++d) delta[d] = xN[d] - xP[d];
        double dotSfd = 0.0;
        for (std::size_t d = 0; d < Dim; ++d) dotSfd += fn[d] * delta[d];
        // Signed face coefficient, as in the Laplacian and the momentum
        // operator: it carries the sign of the cosine between the face normal
        // and the centre-to-centre vector, which is what the Green-Gauss
        // decomposition of the face gradient requires. The mesh orients every
        // internal face normal owner -> neighbour, so the coefficient is
        // positive.
        const double coeff = (dist > 1e-12) ? cFace * area * dotSfd / (dist * dist) : 0.0;

        // Non-orthogonal (cross-diffusion) correction, using the same face
        // treatment as the momentum operator so that the transport and the
        // momentum are discretised consistently on a skewed mesh.
        double correction = 0.0;
        if (hasGrad) {
            double Sf_nonorth[Dim];
            const double dot_f_dhat = dotSfd / dist;
            for (std::size_t d = 0; d < Dim; ++d) {
                Sf_nonorth[d] = area * fn[d] - area * dot_f_dhat * delta[d] / dist;
            }
            for (std::size_t d = 0; d < Dim; ++d) {
                correction += cFace * (*phiGrad)[d][P] * Sf_nonorth[d];
            }
            correction *= nonOrthCorrectionFactor;
        }

        if (!(hasMask && dirichletMask[P])) {
            if (hasMask && dirichletMask[N]) {
                b(P) += coeff * dirichletValues[N];
            } else {
                triplets.emplace_back(P, N, -coeff);
            }
            diagSum[P] += coeff;
            if (hasGrad) b(P) -= correction;
        }

        if (!(hasMask && dirichletMask[N])) {
            if (hasMask && dirichletMask[P]) {
                b(N) += coeff * dirichletValues[P];
            } else {
                triplets.emplace_back(N, P, -coeff);
            }
            diagSum[N] += coeff;
            if (hasGrad) b(N) += correction;
        }
    }

    for (std::size_t P = 0; P < n; ++P) {
        if (hasMask && dirichletMask[P]) {
            triplets.emplace_back(P, P, 1.0);
            b(P) = dirichletValues[P];
        } else {
            triplets.emplace_back(P, P, diagSum[P]);
            b(P) += source[P] * mesh_.cellVolume(P);
        }
    }

    A.setFromTriplets(triplets.begin(), triplets.end());
    A.makeCompressed();
}

template<std::size_t Dim>
const Mesh<Dim>& Poisson<Dim>::mesh() const { return mesh_; }

template class Poisson<2>;
template class Poisson<3>;

} // namespace fvm
