#pragma once

#include "fvm/SSTConstants.h"
#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/Types.h"
#include "fvm/Poisson.h"
#include "fvm/Convection.h"
#include "fvm/Gradient.h"
#include "mesh/BoundaryUtils.h"
#include <Eigen/IterativeLinearSolvers>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <queue>
#include <utility>
#include <vector>
#include <type_traits>

namespace fvm {

template<std::size_t Dim>
class SSTModel {
public:
    struct Config {
        SSTConstants constants;
        double kInf       = 1e-3;
        double omegaInf   = 1.0;
        double alphaK     = 0.7;
        double alphaOmega = 0.7;
        double kMin       = 1e-14;
        double omegaMin   = 1e-14;
    };

    SSTModel(const Mesh<Dim>& mesh, Config config);

    void initialize(double mu, double rho);
    void computeWallDistance();

    template<typename VelocityBCType>
    void computeWallDistance(const std::array<VelocityBCType, 2 * Dim>& velBCs) {
        const std::size_t n = mesh_.nCells();
        for (std::size_t i = 0; i < n; ++i)
            wallDist_[i] = std::numeric_limits<double>::max();

        // Seed propagation only from no-slip wall boundary faces
        for (std::size_t d = 0; d < Dim; ++d) {
            for (int sign : {-1, +1}) {
                std::size_t idx = d * 2 + (sign < 0 ? 0 : 1);
                auto bcType = velBCs[idx];
                if (!isWallBC(bcType)) continue;

                forEachBoundaryFace(mesh_, d, sign, [&](std::size_t f, std::size_t cell) {
                    auto center = mesh_.cellCenter(cell);
                    double dist2 = 0.0;
                    for (std::size_t dd = 0; dd < Dim; ++dd) {
                        double diff = mesh_.faceCenter(f)[dd] - center[dd];
                        dist2 += diff * diff;
                    }
                    double d0 = std::sqrt(dist2);
                    if (d0 < wallDist_[cell]) wallDist_[cell] = d0;
                });
            }
        }

        std::vector<bool> visited(n, false);
        using Entry = std::pair<double, std::size_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pq;

        for (std::size_t i = 0; i < n; ++i) {
            if (wallDist_[i] < std::numeric_limits<double>::max())
                pq.push({wallDist_[i], i});
        }

        while (!pq.empty()) {
            auto [dist, cell] = pq.top();
            pq.pop();
            if (visited[cell]) continue;
            visited[cell] = true;

            for (std::size_t neighbor : mesh_.cellNeighbors(cell)) {
                if (visited[neighbor]) continue;
                auto cP = mesh_.cellCenter(cell);
                auto cN = mesh_.cellCenter(neighbor);
                double d2 = 0.0;
                for (std::size_t dd = 0; dd < Dim; ++dd) {
                    double diff = cN[dd] - cP[dd];
                    d2 += diff * diff;
                }
                double newDist = dist + std::sqrt(d2);
                if (newDist < wallDist_[neighbor]) {
                    wallDist_[neighbor] = newDist;
                    pq.push({newDist, neighbor});
                }
            }
        }

        for (std::size_t i = 0; i < n; ++i) {
            if (wallDist_[i] == std::numeric_limits<double>::max())
                wallDist_[i] = 0.0;
        }
    }

    template<typename VelocityBCType>
    void setBoundaryConditions(
        const std::array<VelocityBCType, 2 * Dim>& velBCs) {

        const std::size_t n = mesh_.nCells();
        kBCMask_.assign(n, 0);
        kBCVals_.assign(n, 0.0);
        omegaBCMask_.assign(n, 0);
        omegaBCVals_.assign(n, 0.0);

        for (std::size_t d = 0; d < Dim; ++d) {
            for (int sign : {-1, +1}) {
                std::size_t idx = d * 2 + (sign < 0 ? 0 : 1);
                auto bcType = velBCs[idx];

                if (isWallBC(bcType)) {
                    forEachBoundaryFace(mesh_, d, sign, [&](std::size_t f, std::size_t cell) {
                        kBCMask_[cell] = 1;
                        kBCVals_[cell] = 0.0;
                        omegaBCMask_[cell] = 1;
                        double y1 = wallDist_[cell];
                        double om = 6.0 * mu_
                                             / (rho_ * config_.constants.beta1 * y1 * y1 + 1e-30);
                        if (om > 1e6) om = 1e6;
                        omegaBCVals_[cell] = om;
                    });
                 } else if (isInletBC(bcType)) {
                    forEachBoundaryFace(mesh_, d, sign, [&](std::size_t f, std::size_t cell) {
                        kBCMask_[cell] = 1;
                        kBCVals_[cell] = config_.kInf;
                        omegaBCMask_[cell] = 1;
                        omegaBCVals_[cell] = config_.omegaInf;
                    });
                } else {
                    // Outlet, Symmetry, SlipWall, PressureOutlet: zero-gradient (natural BC)
                    // No explicit BC needed — transport equation handles it
                }
            }
        }
    }

    void solveIteration(const VectorField<Dim>& velocity,
                        ConvectionScheme scheme);
    void solveIterationTransient(const VectorField<Dim>& velocity,
                                 ConvectionScheme scheme,
                                 double dt,
                                 double dtPrev,
                                 TimeScheme timeScheme,
                                 const Field<Dim>& kOld,
                                 const Field<Dim>& omegaOld,
                                 const Field<Dim>* kOldPrev = nullptr,
                                 const Field<Dim>* omegaOldPrev = nullptr);

    const Field<Dim>& k() const;
    const Field<Dim>& omega() const;
    const Field<Dim>& nut() const;
    Field<Dim>& nut();
    const Field<Dim>& wallDistance() const;
    Field<Dim>& k();
    Field<Dim>& omega();

    double finalKResid() const { return kResidual_; }
    double finalOmegaResid() const { return omegaResidual_; }

private:
    static bool isWallBC(auto bcType) {
        using BC = std::decay_t<decltype(bcType)>;
        return bcType == BC::NoSlip || bcType == BC::MovingWall;
    }

    static bool isInletBC(auto bcType) {
        using BC = std::decay_t<decltype(bcType)>;
        return bcType == BC::Inlet;
    }

    static bool isOutletBC(auto bcType) {
        using BC = std::decay_t<decltype(bcType)>;
        return bcType == BC::Outlet || bcType == BC::PressureOutlet;
    }

    std::array<Field<Dim>, Dim> computeScalarGradients(const Field<Dim>& phi) const;

    void computeStrainRate(const VectorField<Dim>& velocity);
    void computeBlendingFunctions();
    void computeEddyViscosity();
    void solveKTransport(const VectorField<Dim>& velocity,
                         ConvectionScheme scheme);
    void solveOmegaTransport(const VectorField<Dim>& velocity,
                             ConvectionScheme scheme);
    void solveKTransportTransient(const VectorField<Dim>& velocity,
                                  ConvectionScheme scheme,
                                  double dt,
                                  double dtPrev,
                                  TimeScheme timeScheme,
                                  const Field<Dim>& kOld,
                                  const Field<Dim>* kOldPrev);
    void solveOmegaTransportTransient(const VectorField<Dim>& velocity,
                                      ConvectionScheme scheme,
                                      double dt,
                                      double dtPrev,
                                      TimeScheme timeScheme,
                                      const Field<Dim>& omegaOld,
                                      const Field<Dim>* omegaOldPrev);

    Eigen::VectorXd solveKSystem_(const SparseMatrix& A, const Eigen::VectorXd& b);
    Eigen::VectorXd solveOmegaSystem_(const SparseMatrix& A, const Eigen::VectorXd& b);
    Eigen::VectorXd biCGSTABSolve(const SparseMatrix& A,
                                   const Eigen::VectorXd& b);

    bool kPatternAnalyzed_ = false;
    bool omegaPatternAnalyzed_ = false;
    bool patternAnalyzed_ = false;

    const Mesh<Dim>& mesh_;
    Config config_;
    double mu_ = 1e-5;
    double rho_ = 1.0;

    Field<Dim> k_;
    Field<Dim> omega_;
    Field<Dim> nut_;
    Field<Dim> wallDist_;
    Field<Dim> S_;
    Field<Dim> F1_;
    Field<Dim> F2_;
    std::array<Field<Dim>, Dim> gradK_{};
    std::array<Field<Dim>, Dim> gradOmega_{};
    mutable std::array<Field<Dim>, Dim> scalarGradBuffer_{};
    // Reusable strain-rate buffer to avoid per-iteration Field allocations (Dim x Dim fields)
    mutable std::array<std::array<Field<Dim>, Dim>, Dim> dUdxBuffer_{};
    mutable bool dUdxInitialized_ = false;

    std::vector<char>   kBCMask_;
    std::vector<double> kBCVals_;
    std::vector<char>   omegaBCMask_;
    std::vector<double> omegaBCVals_;

    double kResidual_ = 0.0;
    double omegaResidual_ = 0.0;

    Poisson<Dim>    poisson_;
    Convection<Dim> convection_;
    Gradient<Dim>   gradient_;

    mutable Eigen::BiCGSTAB<SparseMatrix, Eigen::IncompleteLUT<double>> kSolver_;
    mutable Eigen::BiCGSTAB<SparseMatrix, Eigen::IncompleteLUT<double>> omegaSolver_;
};

extern template class SSTModel<2>;
extern template class SSTModel<3>;

} // namespace fvm
