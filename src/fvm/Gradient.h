#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include <array>
#include <cstddef>
#include <Eigen/Dense>

namespace fvm {

template<std::size_t Dim>
class Gradient {
public:
    explicit Gradient(const Mesh<Dim>& mesh);

    void compute(const Field<Dim>& phi,
                 std::array<Field<Dim>, Dim>& grad) const;

    const Mesh<Dim>& mesh() const;

private:
    const Mesh<Dim>& mesh_;
    mutable std::vector<Eigen::Matrix<double, Dim, Dim>> M_all_;
    mutable std::vector<Eigen::Matrix<double, Dim, 1>> rhs_all_;
};

extern template class Gradient<2>;
extern template class Gradient<3>;

} // namespace fvm
