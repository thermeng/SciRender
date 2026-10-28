#include "fvm/Poisson.h"
#include "fvm/MatrixAssembly.hpp"
#include "fvm/Validation.h"

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
    validateDirichlet(n, dirichletMask, dirichletValues);

    AlignedBuffer<double> diagSum(n);
    std::vector<Triplet> triplets;

    // Face diffusivity is the distance-weighted interpolation of c between the
    // two cell centres, matching the momentum operator's face interpolation.
    assembleDiffusion<Dim>(mesh_, A, b, diagSum, triplets,
                           dirichletMask, dirichletValues, source.values(),
                           phiGrad,
                           [this, &c](std::size_t f, std::size_t P, std::size_t N) {
                               const double w = mesh_.faceWeight(f);
                               return (1.0 - w) * c[P] + w * c[N];
                           });
}

template<std::size_t Dim>
const Mesh<Dim>& Poisson<Dim>::mesh() const { return mesh_; }

template class Poisson<2>;
template class Poisson<3>;

} // namespace fvm
