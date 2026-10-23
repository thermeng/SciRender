#include "fvm/Solver.h"
#include <iostream>
#include <cmath>
#include <memory>

namespace fvm {

namespace detail {

template<std::size_t Dim>
std::array<Field<Dim>, Dim> makeVelocities(const Mesh<Dim>& mesh) {
    std::array<Field<Dim>, Dim> vels{};
    for (auto& v : vels) v = Field<Dim>(mesh, 0.0);
    return vels;
}

template std::array<Field<2>, 2> makeVelocities<2>(const Mesh<2>&);
template std::array<Field<3>, 3> makeVelocities<3>(const Mesh<3>&);

} // namespace detail

template<std::size_t Dim>
AdvectionDiffusionSolver<Dim>::AdvectionDiffusionSolver(const Mesh<Dim>& mesh, double k)
    : mesh_(mesh),
      k_(k),
      bcs_(mesh, k),
      laplacian_(mesh, k),
      convection_(mesh),
      velocities_(detail::makeVelocities(mesh)),
      source_(mesh, 0.0),
      solution_(mesh, 0.0),
      gradCalc_(std::make_unique<Gradient<Dim>>(mesh)) {}

template<std::size_t Dim>
void AdvectionDiffusionSolver<Dim>::setScheme(ConvectionScheme scheme) {
    scheme_ = scheme;
    convection_.setScheme(scheme);
}

template<std::size_t Dim>
void AdvectionDiffusionSolver<Dim>::setUniformVelocity(double u, double v) {
    if constexpr (Dim == 2) {
        velocities_[0].assign(u);
        velocities_[1].assign(v);
    }
}

template<std::size_t Dim>
void AdvectionDiffusionSolver<Dim>::setUniformVelocity(double u, double v, double w) {
    if constexpr (Dim == 3) {
        velocities_[0].assign(u);
        velocities_[1].assign(v);
        velocities_[2].assign(w);
    }
}

template<std::size_t Dim>
void AdvectionDiffusionSolver<Dim>::setSource(const Field<Dim>& source) {
    const auto src = source.values();
    auto dst = source_.values();
    std::copy(src.begin(), src.end(), dst.begin());
}

template<std::size_t Dim>
BoundaryConditions<Dim>& AdvectionDiffusionSolver<Dim>::bc() { return bcs_; }

template<std::size_t Dim>
const BoundaryConditions<Dim>& AdvectionDiffusionSolver<Dim>::bc() const { return bcs_; }

template<std::size_t Dim>
Convection<Dim>& AdvectionDiffusionSolver<Dim>::convection() { return convection_; }

template<std::size_t Dim>
const Convection<Dim>& AdvectionDiffusionSolver<Dim>::convection() const { return convection_; }

template<std::size_t Dim>
Eigen::VectorXd AdvectionDiffusionSolver<Dim>::solveSteady() {
    if (cancelRequested_.load(std::memory_order_acquire)) return Eigen::VectorXd();

    bcs_.buildDirichletMask();

    const bool isSOU = isSecondOrderUpwind(scheme_);
    const bool nonlinear = isSOU;
    const int maxOuterIters = nonlinear ? 50 : 1;
    const double outerTol = 1e-6;

    for (int outer = 0; outer < maxOuterIters; ++outer) {
        if (cancelRequested_.load(std::memory_order_acquire)) break;

        SparseMatrix A;
        Eigen::VectorXd b;
        laplacian_.discretize(A, b, bcs_.dirichletMask(),
                              bcs_.dirichletValues(), source_.values());

        std::array<const Field<Dim>*, Dim> velPtrs{};
        for (std::size_t d = 0; d < Dim; ++d) {
            velPtrs[d] = &velocities_[d];
        }

        std::array<Field<Dim>, Dim> solGrad;
        for (auto& g : solGrad) g = Field<Dim>(mesh_, 0.0);
        if (nonlinear) {
            gradCalc_->compute(solution_, solGrad);
        }

        convection_.discretize(A, b, solution_, velPtrs,
                                ConvectionBCs{bcs_.dirichletMask(), bcs_.dirichletValues(), {}, {}},
                                nonlinear ? &solGrad : nullptr);

        bcs_.addNeumannContributions(b);

        Eigen::BiCGSTAB<SparseMatrix, Eigen::IncompleteLUT<double>> solver;
        solver.setMaxIterations(50000);
        solver.setTolerance(1e-10);
        solver.compute(A);
        Eigen::VectorXd x = solver.solve(b);
        lastIterations_ = static_cast<int>(solver.iterations());
        lastResidual_  = solver.error();

        if (solver.info() != Eigen::Success) {
            std::cerr << "  WARNING: advection-diffusion BiCGSTAB failed at outer iter "
                      << outer << " (iters=" << solver.iterations()
                      << ", resid=" << solver.error() << ")\n";
        }

        Field<Dim> oldSolution = solution_;
        bool hasNaN = false;
        for (std::size_t i = 0; i < solution_.size(); ++i) {
            double val = x(static_cast<Eigen::Index>(i));
            if (std::isnan(val) || std::isinf(val)) {
                hasNaN = true;
                break;
            }
            solution_[i] = val;
        }

        if (hasNaN) {
            std::cerr << "  WARNING: solution contains NaN/Inf at outer iter "
                      << outer << ", keeping previous solution\n";
            break;
        }

        if (!nonlinear) break;

        double maxChange = 0.0;
        for (std::size_t i = 0; i < solution_.size(); ++i) {
            maxChange = std::max(maxChange, std::abs(solution_[i] - oldSolution[i]));
        }
        double scale = 0.0;
        for (std::size_t i = 0; i < solution_.size(); ++i) {
            scale = std::max(scale, std::abs(solution_[i]));
        }
        scale = std::max(scale, 1e-30);

        if (maxChange / scale < outerTol) break;
    }

    Eigen::VectorXd x(solution_.size());
    for (std::size_t i = 0; i < solution_.size(); ++i) {
        x(static_cast<Eigen::Index>(i)) = solution_[i];
    }
    return x;
}

template<std::size_t Dim>
const Field<Dim>& AdvectionDiffusionSolver<Dim>::solution() const {
    return solution_;
}

template<std::size_t Dim>
int AdvectionDiffusionSolver<Dim>::lastIterations() const {
    return lastIterations_;
}

template<std::size_t Dim>
double AdvectionDiffusionSolver<Dim>::lastResidual() const {
    return lastResidual_;
}

template<std::size_t Dim>
std::array<Field<Dim>, Dim>& AdvectionDiffusionSolver<Dim>::velocities() {
    return velocities_;
}

template<std::size_t Dim>
const std::array<Field<Dim>, Dim>& AdvectionDiffusionSolver<Dim>::velocities() const {
    return velocities_;
}

template<std::size_t Dim>
const Mesh<Dim>& AdvectionDiffusionSolver<Dim>::mesh() const { return mesh_; }

template<std::size_t Dim>
double AdvectionDiffusionSolver<Dim>::k() const { return k_; }

template class AdvectionDiffusionSolver<2>;
template class AdvectionDiffusionSolver<3>;

} // namespace fvm
