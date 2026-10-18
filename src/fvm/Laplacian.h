#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "fvm/Types.h"
#include "core/AlignedBuffer.hpp"
#include <vector>
#include <cstddef>

namespace fvm {

template<std::size_t Dim>
class Laplacian {
public:
    Laplacian(const Mesh<Dim>& mesh, double k);

    void discretize(SparseMatrix& A, Eigen::VectorXd& b,
                     const std::vector<char>& dirichletMask = {},
                     const std::vector<double>& dirichletValues = {},
                     std::span<const double> source = {},
                     const std::array<Field<Dim>, Dim>* phiGrad = nullptr);

    const Mesh<Dim>& mesh() const;
    double k() const;

private:
    const Mesh<Dim>& mesh_;
    double k_;

    mutable AlignedBuffer<double> diagSum_;
    mutable std::vector<Triplet> triplets_;
    mutable bool workspaceInitialized_ = false;

    void ensureWorkspace_(std::size_t n) const;
};

extern template class Laplacian<2>;
extern template class Laplacian<3>;

} // namespace fvm
