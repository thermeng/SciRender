#include "fvm/Laplacian.h"
#include "fvm/MatrixAssembly.hpp"
#include "fvm/Validation.h"

#include <stdexcept>

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
    validateDirichlet(mesh_.nCells(), dirichletMask, dirichletValues);
    ensureWorkspace_(mesh_.nCells());

    assembleDiffusion<Dim>(mesh_, A, b, diagSum_, triplets_,
                           dirichletMask, dirichletValues, source, phiGrad,
                           [this](std::size_t, std::size_t, std::size_t) {
                               return k_;
                           });
}

template<std::size_t Dim>
const Mesh<Dim>& Laplacian<Dim>::mesh() const { return mesh_; }

template<std::size_t Dim>
double Laplacian<Dim>::k() const { return k_; }

template class Laplacian<2>;
template class Laplacian<3>;

} // namespace fvm
