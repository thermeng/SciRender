#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "fvm/Types.h"
#include "fvm/Laplacian.h"
#include "fvm/Convection.h"
#include "fvm/Gradient.h"
#include "fvm/BoundaryConditions.h"

#include <Eigen/Sparse>
#include <Eigen/IterativeLinearSolvers>
#include <array>
#include <atomic>
#include <cstddef>
#include <string>
#include <memory>

namespace fvm {

namespace detail {
template<std::size_t Dim>
std::array<Field<Dim>, Dim> makeVelocities(const Mesh<Dim>& mesh);
}

template<std::size_t Dim>
class AdvectionDiffusionSolver {
public:
    AdvectionDiffusionSolver(const Mesh<Dim>& mesh, double k);

    void setScheme(ConvectionScheme scheme);
    void setUniformVelocity(double u, double v);
    void setUniformVelocity(double u, double v, double w);
    void setSource(const Field<Dim>& source);

    BoundaryConditions<Dim>& bc();
    const BoundaryConditions<Dim>& bc() const;

    Convection<Dim>& convection();
    const Convection<Dim>& convection() const;

    Eigen::VectorXd solveSteady();

    const Field<Dim>& solution() const;
    int lastIterations() const;
    double lastResidual() const;

    std::array<Field<Dim>, Dim>& velocities();
    const std::array<Field<Dim>, Dim>& velocities() const;

    const Mesh<Dim>& mesh() const;
    double k() const;

    void requestCancel();
    void resetCancel();
    bool isCancelRequested() const;

private:
    const Mesh<Dim>& mesh_;
    double k_;
    ConvectionScheme scheme_ = ConvectionScheme::Upwind;

    BoundaryConditions<Dim> bcs_;
    Laplacian<Dim> laplacian_;
    Convection<Dim> convection_;

    std::array<Field<Dim>, Dim> velocities_;
    Field<Dim> source_;
    Field<Dim> solution_;

    mutable std::unique_ptr<Gradient<Dim>> gradCalc_;

    int lastIterations_ = 0;
    double lastResidual_ = 0.0;
    std::atomic<bool> cancelRequested_{false};
};

extern template class AdvectionDiffusionSolver<2>;
extern template class AdvectionDiffusionSolver<3>;

namespace detail {
extern template std::array<Field<2>, 2> makeVelocities<2>(const Mesh<2>&);
extern template std::array<Field<3>, 3> makeVelocities<3>(const Mesh<3>&);
}

} // namespace fvm
