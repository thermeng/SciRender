#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/Types.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/SIMPLEBoundaryConditions.h"
#include "fvm/Gradient.h"
#include "fvm/RhieChow.h"
#include "fvm/AMG.h"
#include <Eigen/Sparse>
#include <Eigen/IterativeLinearSolvers>
#include <atomic>
#include <array>
#include <vector>
#include <memory>
#include <cstddef>
#include <utility>

namespace fvm {

template<std::size_t Dim>
class PressureCorrection {
public:
    struct Config : public cfd::CaseSettings {
    bool compactRhieChow = false;
    bool useAMG = true;
    bool useAMGPreconditioner = true;
    // Number of cells below which the solve falls back to CG+IC (or
    // BiCGSTAB+ILU). 0 means "always use AMG".
    std::size_t pressureAmgThreshold = 0;
    // How many times the pressure matrix may change its VALUES before the
    // AMG hierarchy is rebuilt. The coarse levels depend on the matrix
    // through the Galerkin product, so a value change makes the hierarchy
    // stale - but rebuilding it on every outer iteration costs more than
    // the solve it accelerates. At high Reynolds numbers the matrix changes
    // quickly, so a value of 1 (rebuild every iteration) keeps the
    // preconditioner effective.
    std::size_t amgRebuildInterval = 1;
};

    struct PressureSolverStats {
        std::size_t solves = 0;
        std::size_t amgSolves = 0;
        std::size_t iluSolves = 0;
        std::size_t amgBuilds = 0;
        std::size_t amgRejected = 0;
        std::size_t krylovIters = 0;
        std::size_t amgKrylovIters = 0;
        double iluSeconds = 0.0;
        double amgSeconds = 0.0;
        double buildSeconds = 0.0;
    };

    PressureCorrection(const Mesh<Dim>& mesh, const Config& config);
    PressureCorrection(const Mesh<Dim>& mesh);
    void setConfig(const Config& cfg) { config_ = cfg; }
    const std::vector<char>& pressureMask() const { return pMask_; }
    const std::array<Field<Dim>, Dim>& pressureGradient() const { return pressureGrad_; }

    // Recompute the least-squares pressure gradient. The compact Rhie-Chow
    // face flux used by the residual diagnostics needs the gradient of the
    // CURRENT pressure; the one left behind by the last solve() is the
    // gradient of the pre-correction pressure.
    void refreshPressureGradient(const Field<Dim>& pressure);

    // True when the assembled pressure operator contains the Rhie-Chow face
    // flux. Residual diagnostics must add the same term only in that mode:
    // the legacy composed operator never sees it, so reporting it would measure
    // a mass imbalance the pressure solve neither created nor can remove.
    bool usesCompactRhieChow() const { return config_.compactRhieChow; }

    // The number of cells that have no pressure equation at all (no interior
    // face: a detached cell). Such a cell is excluded from the pressure
    // unknowns and its mass balance can never be corrected, so the count is
    // reported - otherwise the run silently never converges.
    std::size_t isolatedCellCount() const { return isolatedCellCount_; }

    // Diagnostics of the last linear solve: the iteration count and the
    // residual the solver ended with.
    std::size_t lastSolveIterations() const { return lastSolveIterations_; }
    double lastSolveResidual() const { return lastSolveResidual_; }
    // True when the last solve produced a non-finite p'. Such a solve is a
    // hard failure, not a slightly inaccurate one: the entries are zeroed to
    // keep the field usable, but the caller must abort rather than continue
    // from a correction that was never computed.
    bool lastSolveProducedNonFinite() const { return lastSolveProducedNonFinite_; }
    // Pressure-outlet faces whose normal velocity is inward during the last
    // solve. The diagnostic is a property of the flow state, not of any single
    // corrector, so the caller reports it once per time step rather than letting
    // every corrector repeat it. 0 means "no reverse flow at the outlets".
    std::size_t lastReverseFlowFaces() const { return lastReverseFlowFaces_; }
    const PressureSolverStats& pressureStats() const { return pressureStats_; }
    void resetPressureStats() { pressureStats_ = PressureSolverStats{}; }
    // AMG hierarchy diagnostics (levels actually built and their sizes), for
    // comparing against a  "Level / Size" table.
    bool amgBuilt() const { return amgBuilt_; }
    int amgLevels() const { return amg_.numLevels(); }
    const std::vector<int>& amgLevelSizes() const { return amg_.levelSizes(); }
    // Per-cycle relative residuals from the last AMG build/solve.
    const std::vector<double>& amgLastResidualHistory() const { return amg_.lastResidualHistory(); }
    // The assembled system of the last solve (diagnostics).
    const SparseMatrix& lastMatrix() const { return A_; }
    const Eigen::VectorXd& lastRhs() const { return bVec_; }

    void solve(
        const VectorField<Dim>& uStar,
        const std::array<std::vector<double>, Dim>& dCoeff,
        const Field<Dim>& pressure,
        Field<Dim>& pPrime,
        const BCDiscretization& bc,
        const std::atomic<bool>* cancel = nullptr);

private:
    const Mesh<Dim>& mesh_;
    Config config_;
    // The least-squares gradient of the pressure field. In compact Rhie-Chow
    // mode it supplies the face-interpolated gradient that the Rhie-Chow
    // correction subtracts; in legacy mode it is also the operator whose
    // divergence the pressure matrix is assembled from. It is the same
    // operator the velocity correction applies.
    std::unique_ptr<Gradient<Dim>> gradientCalc_;
    std::array<Field<Dim>, Dim> pressureGrad_;
    // The per-cell least-squares gradient as row coefficients
    // (gradientRows_[d][cell] = (column, coefficient) pairs), built once per
    // PressureCorrection object: the pressure matrix is assembled from them.
    std::array<std::vector<std::vector<std::pair<std::size_t, double>>>, Dim> gradientRows_;
    bool gradientRowsBuilt_ = false;

    // Reusable buffers to avoid per-iteration allocation
    mutable std::vector<Triplet> triplets_;
    mutable Eigen::VectorXd bVec_;
    mutable std::vector<char> pMask_;
    mutable std::vector<double> pVals_;
    mutable std::vector<char> isPressureOutlet_;
    mutable Field<Dim> divU_;
    mutable std::vector<std::vector<std::pair<std::size_t, double>>> rowAcc_;

    // The number of cells that have no pressure equation at all (no interior
    // face: a detached cell). Such a cell is excluded from the pressure
    // unknowns and its mass balance can never be corrected, so the count is
    // reported - otherwise the run silently never converges.
    std::size_t isolatedCellCount_ = 0;
    // Diagnostics of the last linear solve.
    std::size_t lastSolveIterations_ = 0;
    double lastSolveResidual_ = 0.0;
    bool lastSolveProducedNonFinite_ = false;
    // Pressure-outlet faces with inward normal velocity during the last solve.
    std::size_t lastReverseFlowFaces_ = 0;
    mutable PressureSolverStats pressureStats_{};

    Eigen::ConjugateGradient<SparseMatrix, Eigen::Lower|Eigen::Upper,
                             Eigen::IncompleteCholesky<double>> cgSolver_;
    //  solves the pressure equation with PCG preconditioned by AMG
    // ("Preconditioned Conjugate-Gradient solve of SPD-system"). That is valid
    // for the compact Rhie-Chow operator, which is symmetric positive definite.
    Eigen::ConjugateGradient<SparseMatrix, Eigen::Lower|Eigen::Upper,
                             AMGPreconditioner> cgAmgSolver_;
    Eigen::BiCGSTAB<SparseMatrix, AMGPreconditioner> bicgSolver_;
    // GMRES is not part of Eigen's supported iterative solvers; the AMG
    // acceleration "GMRES" uses fvm::gmresRestarted (fvm/Gmres.h) instead of a
    // member here.
    Eigen::BiCGSTAB<SparseMatrix, Eigen::IncompleteLUT<double>> bicgFallbackSolver_;
    bool pPatternAnalyzed_ = false;
    // The fallback ILU/CG solvers are analysed separately from the AMG Krylov
    // solvers: an AMG solve can succeed without ever touching them, and a
    // later fallback must still call analyzePattern() before factorize().
    bool fallbackPatternAnalyzed_ = false;
    std::size_t fallbackPatternHash_ = 0;
    mutable std::vector<char> prevPMask_;
    mutable std::size_t prevNonZeroPattern_ = 0;
    // The compact and legacy operators have different stencils. A transition
    // between them (e.g. when PISO forces Rhie-Chow after construction) must
    // re-analyse the preconditioner, not just overwrite values.
    bool prevCompact_ = false;

    AMGHierarchy amg_;
    AMGPreconditioner amgPrec_;
    bool amgBuilt_ = false;
    std::size_t amgPatternHash_ = 0;
    // Number of value changes of the pressure matrix since the hierarchy was
    // last built (see Config::amgRebuildInterval).
    std::size_t amgValueUpdates_ = 0;
    mutable bool amgPathLogged_ = false;

    mutable SparseMatrix A_;
    mutable bool aPatternBuilt_ = false;
    mutable std::size_t aPatternHash_ = 0;
    // O(nnz) value update — avoids O(nnz log nnz) sort + O(nnz) hash per iter
    mutable std::vector<int> aTripletToValuePos_;
    mutable bool aNeedPosRebuild_ = false;
};

extern template class PressureCorrection<2>;
extern template class PressureCorrection<3>;

} // namespace fvm
