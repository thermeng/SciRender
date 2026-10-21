#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include <array>
#include <cstddef>
#include <vector>
#include <Eigen/Dense>

namespace fvm {

enum class GradientScheme { LeastSquares, GreenGaussCellBased, GreenGaussNodeBased };

// Cell-centred gradient of a scalar field.
//
// The three schemes differ only in how the value AT A FACE is obtained:
//   LeastSquares        - a weighted least-squares fit over the neighbouring
//                          cell centres, with a mirrored ghost cell at a wall;
//   GreenGaussCellBased - the exact linear interpolation between the two cell
//                          centres of the face;
//   GreenGaussNodeBased - the average of the nodal values on the face.
//
// A prescribed (Dirichlet) value is a CELL value: it replaces the value of the
// cell it belongs to and is never used as the value at a face.
template<std::size_t Dim>
class Gradient {
public:
    explicit Gradient(const Mesh<Dim>& mesh, GradientScheme scheme = GradientScheme::LeastSquares);

    void setScheme(GradientScheme scheme);

    GradientScheme scheme() const;

    void compute(const Field<Dim>& phi,
                 std::array<Field<Dim>, Dim>& grad,
                 const std::vector<char>& dirichletMask = {},
                 const std::vector<double>& dirichletValues = {}) const;

    const Mesh<Dim>& mesh() const;

private:
    // The value of the field in a cell. A prescribed value IS the cell value.
    double cellValue_(std::size_t cell, const Field<Dim>& phi,
                      const std::vector<char>& dirichletMask,
                      const std::vector<double>& dirichletValues) const;

    // The Green-Gauss sum: grad = (1/V) * sum_faces phi_f * A * n, with the
    // flux entering the owner cell and leaving the neighbour.
    void accumulateGreenGauss_(const std::vector<double>& phiFace,
                               std::array<Field<Dim>, Dim>& grad) const;

    void computeGreenGaussCellBased_(const Field<Dim>& phi, std::array<Field<Dim>, Dim>& grad,
                                     const std::vector<char>& dirichletMask,
                                     const std::vector<double>& dirichletValues) const;

    void computeGreenGaussNodeBased_(const Field<Dim>& phi, std::array<Field<Dim>, Dim>& grad,
                                     const std::vector<char>& dirichletMask,
                                     const std::vector<double>& dirichletValues) const;

    void computeLeastSquares_(const Field<Dim>& phi, std::array<Field<Dim>, Dim>& grad,
                              const std::vector<char>& dirichletMask,
                              const std::vector<double>& dirichletValues) const;

    const Mesh<Dim>& mesh_;
    GradientScheme scheme_ = GradientScheme::LeastSquares;
    mutable std::vector<Eigen::Matrix<double, Dim, Dim>> M_all_;
    mutable std::vector<Eigen::Matrix<double, Dim, 1>> rhs_all_;
};

extern template class Gradient<2>;
extern template class Gradient<3>;

} // namespace fvm
