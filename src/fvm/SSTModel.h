#pragma once

#include "fvm/SSTConstants.h"
#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/Types.h"
#include "fvm/Poisson.h"
#include "fvm/Convection.h"
#include "fvm/SIMPLETypes.h"
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
        double kMax       = 1e3;
        double omegaMax   = 1e6;
        bool wallFunction = false;   // enable wall-function (high-Re) treatment
        // Cap on nut/nu. A numerical guard, not a physical model constant.
        double nutMaxFactor = 1e5;
    };

    SSTModel(const Mesh<Dim>& mesh, Config config);

    // Reset k/omega/nut to the freestream state and set the fluid properties.
    void initialize(double mu, double rho);

    // Adopt a new configuration without touching the fields. Used when the
    // caller reconfigures turbulence after the model already exists (the model
    // instance must stay alive across solves because callers cache its pointer).
    void setConfig(const Config& config) { config_ = config; }
    const Config& config() const { return config_; }

    // Classify every boundary face from the momentum face BCs, rebuild the
    // k/omega Dirichlet masks and recompute the wall distance from the faces
    // that are actually walls. `faceBCs` is indexed by mesh face; interior
    // entries are ignored. A boundary face without an entry is treated as a
    // no-slip wall, matching the momentum discretization's own fallback.
    void setBoundaryConditions(const std::vector<FaceVelocityBC>& faceBCs);

    // Wall distance seeded only from faces classified as walls. Inlets,
    // outlets and symmetry planes are not wall sources.
    void computeWallDistance(const std::vector<FaceVelocityBC>& faceBCs);

    // Override the k/omega inlet values on explicit faces. Applied after
    // setBoundaryConditions(), so a named inlet patch wins over kInf/omegaInf.
    void setInletTurbulence(const std::vector<std::size_t>& faces, double k, double omega);

    // Clamp interior cells into the configured bounds. Boundary cells keep
    // their prescribed value: a cap must never silently rewrite a BC.
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

    // Largest y+ over the wall-adjacent cells. The model is a low-Re
    // formulation: y+ should be O(1). A large value means the first cell is
    // inside the log layer and the wall shear is not resolved (there is no
    // wall function), so the caller should warn.
    double maxYPlus() const { return maxYPlus_; }
    double maxWallDistance() const { return maxWallDistance_; }

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

    std::array<Field<Dim>, Dim> computeScalarGradients(
        const Field<Dim>& phi,
        const std::vector<char>& mask,
        const std::vector<double>& vals) const;

    void computeStrainRate(const VectorField<Dim>& velocity);
    void computeWallDiagnostics(const VectorField<Dim>& velocity);
    void applyWallFunctions(const VectorField<Dim>& velocity);
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

    mutable Eigen::BiCGSTAB<SparseMatrix, Eigen::IncompleteLUT<double>> kSolver_;
    mutable Eigen::BiCGSTAB<SparseMatrix, Eigen::IncompleteLUT<double>> omegaSolver_;
};

extern template class SSTModel<2>;
extern template class SSTModel<3>;

} // namespace fvm
