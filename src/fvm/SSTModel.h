#pragma once

#include "fvm/SSTConstants.h"
#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/Types.h"
#include "fvm/Poisson.h"
#include "fvm/Convection.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/LinearSolverDispatcher.h"
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
        double alphaK     = 0.8;
        double alphaOmega = 0.8;
        double kMin       = 1e-14;
        double omegaMin   = 1e-14;
        double kMax       = 1e3;
        double omegaMax   = 1e6;
        // Wall treatment mode:
        //   LowRe    - resolve viscous sublayer everywhere (y+ ~ 1 required)
        //   HighRe   - use wall functions everywhere (y+ > 30 required)
        //   AllYPlus - per-cell: low-Re where y+ < threshold, wall function where y+ >= threshold
        enum class WallTreatment { LowRe, HighRe, AllYPlus };
        WallTreatment wallTreatment = WallTreatment::LowRe;
        // y+ threshold for AllYPlus switching (standard: 11.225, edge of viscous sublayer).
        double yPlusThreshold = 11.225;
        bool wallFunction = false; // Deprecated: use wallTreatment. true => AllYPlus.
        // Cap on nut/nu. A numerical guard, not a physical model constant.
        double nutMaxFactor = 1e5;

        // Turbulence linear-solver multigrid configuration, forwarded to the
        // LinearSolverDispatcher for both the k and omega transport equations.
        // Same defaults as momentum (standalone AMG, acceleration None): the
        // transport equations are nonsymmetric, so a Krylov acceleration and
        // proportional scaling are not appropriate.
        cfd::AmgSettings turbulenceAmgConfig = cfd::AmgSettings::momentumDefaults();

        int turbulenceAmgCycles = 1;
        double turbulenceAmgMaxResidual = 0.1;
        fvm::SlopeLimiterConfig limiter;
    };

    SSTModel(const Mesh<Dim>& mesh, Config config);
    void initialize(double mu, double rho, double U_ref = 1.0);
    void setConfig(const Config& config) {
        config_ = config;
        kDispatcher_.setConfig(dispatcherConfig_(config_, "turbulence[k]"));
        omegaDispatcher_.setConfig(dispatcherConfig_(config_, "turbulence[omega]"));
    }
    const Config& config() const { return config_; }
    void setBoundaryConditions(const std::vector<FaceVelocityBC>& faceBCs);
    void computeWallDistance(const std::vector<FaceVelocityBC>& faceBCs);
    void setInletTurbulence(const std::vector<std::size_t>& faces, double k, double omega);
    void clampFields();
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

    // Relative linear-solve residuals |b - A x| / |b| of the last k/omega
    // transport solve, so they are comparable across meshes and source scales.
    double finalKResid() const { return kResidual_; }
    double finalOmegaResid() const { return omegaResidual_; }

    // Turbulence linear-solver timing (cumulative): AMG setup vs cycle time
    // across both the k and omega transport solves. Lets the caller attribute
    // the turbulence phase to setup versus smoothing.
    double turbulenceBuildSeconds() const {
        return kDispatcher_.buildSeconds() + omegaDispatcher_.buildSeconds();
    }
    double turbulenceSolveSeconds() const {
        return kDispatcher_.amgSeconds() + omegaDispatcher_.amgSeconds();
    }
    std::size_t turbulenceBuilds() const {
        return kDispatcher_.amgBuilds() + omegaDispatcher_.amgBuilds();
    }

    // Largest y+ over the wall-adjacent cells. The model is a low-Re
    // formulation: y+ should be O(1). A large value means the first cell is
    // inside the log layer and the wall shear is not resolved (unless wall
    // functions are used), so the caller should warn.
    double maxYPlus() const { return maxYPlus_; }
    double maxWallDistance() const { return maxWallDistance_; }

    // Per-cell y+ at wall-adjacent cells (zero elsewhere). Populated by
    // computeWallDiagnostics(). Useful for all-y+ treatment and mesh-quality
    // reporting.
    const std::vector<double>& cellYPlus() const { return cellYPlus_; }
    // Per-cell wall treatment: 1 = wall function (high-Re), 0 = low-Re.
    // Populated by computeWallDiagnostics() when wallTreatment == AllYPlus.
    const std::vector<char>& cellIsHighRe() const { return cellIsHighRe_; }

    // Active Dirichlet masks and values, exposed for diagnostics and tests.
    const std::vector<char>& kDirichletMask() const { return kBCMask_; }
    const std::vector<double>& kDirichletValues() const { return kBCVals_; }
    const std::vector<char>& omegaDirichletMask() const { return omegaBCMask_; }
    const std::vector<double>& omegaDirichletValues() const { return omegaBCVals_; }

private:
    static bool isWallType(VelocityBCType t) {
        return t == VelocityBCType::NoSlip || t == VelocityBCType::MovingWall;
    }
    static bool isInletType(VelocityBCType t) {
        return t == VelocityBCType::Inlet;
    }

    // Which faces the caller declared. `faceBCs` may be empty, in which case
    // every boundary face keeps the historical no-slip fallback.
    VelocityBCType faceBCType_(const std::vector<FaceVelocityBC>& faceBCs,
                               std::size_t f) const;

    // Fills `out` with the gradient of `phi`; writing straight into the
    // caller's array avoids a Dim-field copy per call.
    void computeScalarGradients(
        std::array<Field<Dim>, Dim>& out,
        const Field<Dim>& phi,
        const std::vector<char>& mask,
        const std::vector<double>& vals) const;

    void computeStrainRate(const VectorField<Dim>& velocity);
    void computeWallDiagnostics(const VectorField<Dim>& velocity);
    // Update per-cell wall boundary conditions based on current velocity and
    // local y+. Called each iteration before the k/omega transport solves so
    // that wall-adjacent cells use low-Re or wall-function BCs according to
    // wallTreatment and the current local y+.
    void updateWallBCs_(const VectorField<Dim>& velocity);
    // Rebuild the active masks from the fixed (wall/inlet) ones and add a
    // Dirichlet k/omega condition on outlet faces that are actually ingesting
    // fluid. Without this the backflow k/omega UI controls were dead and a
    // reversed outlet fed the domain with no turbulence at all.
    void updateBackflowBCs_(const VectorField<Dim>& velocity);
    void computeBlendingFunctions();
    void computeEddyViscosity();

    // Shared by the steady and transient paths so the two formulations cannot
    // drift apart again (the transient path used to lack the source caps).
    void computeKDiffusionSource_(Field<Dim>& diff, Field<Dim>& src) const;
    void computeOmegaDiffusionSource_(Field<Dim>& diff, Field<Dim>& src) const;

    void applyUnderRelaxation_(SparseMatrix& A, Eigen::VectorXd& b,
                               const Field<Dim>& phi, double alpha,
                               const std::vector<char>& mask) const;

    // Boundary cells are assigned their prescribed value; interior cells are
    // clamped into [lo, hi].
    void limitField_(Field<Dim>& phi, double lo, double hi,
                     const std::vector<char>& mask,
                     const std::vector<double>& vals) const;

    void solveKTransport(const VectorField<Dim>& velocity,
                          ConvectionScheme scheme);
    void solveOmegaTransport(const VectorField<Dim>& velocity,
                            ConvectionScheme scheme);
    void solveKTransportImpl(const VectorField<Dim>& velocity,
                            ConvectionScheme scheme,
                            double dt,
                            double dtPrev,
                            TimeScheme timeScheme,
                            const Field<Dim>* kOld,
                            const Field<Dim>* kOldPrev);
    void solveOmegaTransportImpl(const VectorField<Dim>& velocity,
                                ConvectionScheme scheme,
                                double dt,
                                double dtPrev,
                                TimeScheme timeScheme,
                                const Field<Dim>* omegaOld,
                                const Field<Dim>* omegaOldPrev);

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
    void sanitizeSolution_(Eigen::VectorXd& x) const;

    const Mesh<Dim>& mesh_;
    mutable std::vector<double> invVol_;
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
    // Reusable strain-rate buffer to avoid per-iteration Field allocations (Dim x Dim fields)
    mutable std::array<std::array<Field<Dim>, Dim>, Dim> dUdxBuffer_{};
    mutable bool dUdxInitialized_ = false;

    std::vector<char>   kBCMask_;
    std::vector<double> kBCVals_;
    std::vector<char>   omegaBCMask_;
    std::vector<double> omegaBCVals_;

    // Per-cell y+ at wall-adjacent cells (zero elsewhere). Populated by
    // computeWallDiagnostics().
    std::vector<double> cellYPlus_;
    // Per-cell wall treatment: 1 = wall function (high-Re), 0 = low-Re.
    // Populated by computeWallDiagnostics() when wallTreatment == AllYPlus.
    std::vector<char>   cellIsHighRe_;

    // Fixed wall/inlet conditions. The active masks above start from these
    // every iteration and add the current outlet-backflow cells.
    std::vector<char>   kFixedMask_;
    std::vector<double> kFixedVals_;
    std::vector<char>   omegaFixedMask_;
    std::vector<double> omegaFixedVals_;
    // Per-face outlet data (pressure outlet and plain outlet).
    std::vector<char>   faceIsOutlet_;
    std::vector<double> outletBackflowK_;
    std::vector<double> outletBackflowOmega_;

    // Per-face classification, used by the strain rate and the gradients so
    // boundary faces use the prescribed velocity instead of the owner value.
    std::vector<char> faceIsWall_;
    std::vector<char> faceIsInlet_;
    std::vector<std::array<double, Dim>> faceVelocity_;
    // A cell may own both a wall face and an inlet face (a channel corner).
    // Wall conditions win there, independently of the face iteration order.
    std::vector<char> cellIsWall_;

    double kResidual_ = 0.0;
    double omegaResidual_ = 0.0;
    double maxYPlus_ = 0.0;
    double maxWallDistance_ = 0.0;

    Poisson<Dim>    poisson_;
    Convection<Dim> convection_;

    // Map the turbulence config onto the shared linear-solver dispatcher.
    // The k and omega transport equations each own one dispatcher; `label`
    // distinguishes them in the solver log.
    static typename LinearSolverDispatcher<Dim>::Config
    dispatcherConfig_(const Config& config, const char* label) {
        typename LinearSolverDispatcher<Dim>::Config c;
        c.amgConfig = config.turbulenceAmgConfig;
        c.amgCycles = config.turbulenceAmgCycles;
        c.amgMaxResidual = config.turbulenceAmgMaxResidual;
        c.labels[0] = label;
        return c;
    }

    // AMG-preconditioned Krylov solvers for the k and omega transport equations,
    // mirroring the momentum setup in SIMPLE::linearSolverDispatcher_.
    LinearSolverDispatcher<Dim> kDispatcher_;
    LinearSolverDispatcher<Dim> omegaDispatcher_;
};

extern template class SSTModel<2>;
extern template class SSTModel<3>;

} // namespace fvm
