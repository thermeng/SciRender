#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "fvm/Types.h"
#include <array>
#include <vector>
#include <cstddef>

namespace fvm {

template<std::size_t Dim>
class Poisson {
public:
    Poisson(const Mesh<Dim>& mesh);

    // Discretises  div(c grad(phi)) = source.
    //
    // `phiGrad` is the cell gradient of the transported quantity. When it is
    // supplied the non-orthogonal (cross-diffusion) correction is added, so
    // this operator uses the same face treatment as the momentum and Laplacian
    // operators; without it the transport is discretised orthogonally only.
    void discretize(SparseMatrix& A, Eigen::VectorXd& b,
                    const Field<Dim>& c,
                    const Field<Dim>& source,
                    const std::vector<char>& dirichletMask = {},
                    const std::vector<double>& dirichletValues = {},
                    const std::array<Field<Dim>, Dim>* phiGrad = nullptr);

    const Mesh<Dim>& mesh() const;

private:
    const Mesh<Dim>& mesh_;
};

extern template class Poisson<2>;
extern template class Poisson<3>;

} // namespace fvm
