#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "fvm/Types.h"
#include <vector>
#include <cstddef>

namespace fvm {

template<std::size_t Dim>
class Poisson {
public:
    Poisson(const Mesh<Dim>& mesh);

    void discretize(SparseMatrix& A, Eigen::VectorXd& b,
                    const Field<Dim>& c,
                    const Field<Dim>& source,
                    const std::vector<char>& dirichletMask = {},
                    const std::vector<double>& dirichletValues = {});

    const Mesh<Dim>& mesh() const;

private:
    const Mesh<Dim>& mesh_;
};

extern template class Poisson<2>;
extern template class Poisson<3>;

} // namespace fvm
