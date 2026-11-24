#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "fvm/SlopeLimiter.h"
#include <array>
#include <cstddef>
#include <utility>
#include <vector>
#include <Eigen/Dense>

namespace fvm {

enum class GradientScheme { LeastSquares, GreenGaussCellBased, GreenGaussNodeBased };

// Cell-centred gradient of a scalar field.
//
// The three schemes differ only in how the value AT A FACE is obtained:
//   LeastSquares        - a least-squares fit over the neighbouring cell
//                          centres;
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

    // Clamp the reconstructed gradient to the local field variation
    // (|phi_N - phi_P| / |x_N - x_P|). Prevents a rank-deficient least-squares
    // fit or a skewed face from producing an O(1/eps) gradient.
    void setLimitGradients(bool limit) { limitGradients_ = limit; }
    bool limitGradients() const { return limitGradients_; }

    // The slope-limiter variant applied to the reconstructed gradients. The
    // solver arms it from the case configuration so the selectable FVM limiter
    // (MinMod / Venkatakrishnan / Cubic, per-face or cell-wide) reaches the
    // gradient operators instead of the hardcoded default.
    void setLimiter(const SlopeLimiterConfig& config) { limiterConfig_ = config; }
    const SlopeLimiterConfig& limiterConfig() const { return limiterConfig_; }

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

    void limitGradient_(const Field<Dim>& phi,
                        std::array<Field<Dim>, Dim>& grad,
                        const std::vector<char>& dirichletMask,
                        const std::vector<double>& dirichletValues) const;

    // Geometry-only normal matrix of the least-squares fit, together with its
    // rank-truncated pseudo-inverse. Built once.
    void buildNormalMatrix_() const;

    const Mesh<Dim>& mesh_;
    GradientScheme scheme_ = GradientScheme::LeastSquares;
    mutable std::vector<Eigen::Matrix<double, Dim, Dim>> M_all_;
    // Pseudo-inverse of M_all_, with eigenvalues below a relative threshold
    // dropped. A cell on a wall edge has only Dim-1 interior neighbours, so its
    // normal matrix is rank deficient; a plain inverse divides by a
    // roundoff-sized determinant and produces an O(1/eps) gradient that
    // destroys the run.
    mutable std::vector<Eigen::Matrix<double, Dim, Dim>> Mpinv_all_;
    mutable std::vector<Eigen::Matrix<double, Dim, 1>> rhs_all_;
    // Least-squares stencil: interior neighbours first, extended with
    // neighbours-of-neighbours only where that set cannot span Dim directions.
    struct StencilEntry {
        std::size_t cell;
        std::array<double, Dim> d; // x_other - x_cell
    };
    mutable std::vector<std::vector<StencilEntry>> stencil_;
    mutable bool normalMatrixBuilt_ = false;
    bool limitGradients_ = false;
    SlopeLimiterConfig limiterConfig_;
};

extern template class Gradient<2>;
extern template class Gradient<3>;

} // namespace fvm
