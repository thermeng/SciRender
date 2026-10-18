#include "fvm/Poisson.h"
#include "fvm/SSTConstants.h"

namespace fvm {

template<std::size_t Dim>
Poisson<Dim>::Poisson(const Mesh<Dim>& mesh) : mesh_(mesh) {}

template<std::size_t Dim>
void Poisson<Dim>::discretize(SparseMatrix& A, Eigen::VectorXd& b,
                              const Field<Dim>& c,
                              const Field<Dim>& source,
                              const std::vector<char>& dirichletMask,
                              const std::vector<double>& dirichletValues) {
    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    const bool hasMask = !dirichletMask.empty();

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
        double w = mesh_.faceWeight(f);
        const double cFace = (1.0 - w) * c[P] + w * c[N];
        // Include non-orthogonal correction factor absDotSfd/dist similar to Laplacian
        const double* fn = mesh_.faceNormalPtr(f);
        const double* xP = mesh_.cellCenterPtr(P);
        const double* xN = mesh_.cellCenterPtr(N);
        double dotSfd = 0.0;
        for (std::size_t d = 0; d < Dim; ++d) dotSfd += fn[d] * (xN[d] - xP[d]);
        double absDotSfd = std::abs(dotSfd);
        double coeff = (dist > 1e-12 && absDotSfd > 1e-12) ? cFace * area * absDotSfd / (dist * dist) : cFace * area / dist;

        if (!(hasMask && dirichletMask[P])) {
            if (hasMask && dirichletMask[N]) {
                b(P) += coeff * dirichletValues[N];
            } else {
                triplets.emplace_back(P, N, -coeff);
            }
            diagSum[P] += coeff;
        }

        if (!(hasMask && dirichletMask[N])) {
            if (hasMask && dirichletMask[P]) {
                b(N) += coeff * dirichletValues[P];
            } else {
                triplets.emplace_back(N, P, -coeff);
            }
            diagSum[N] += coeff;
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
