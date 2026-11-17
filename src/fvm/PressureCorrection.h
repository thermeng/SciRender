#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/Types.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/SIMPLEBoundaryConditions.h"
#include "fvm/Gradient.h"
#include "fvm/LinearSolverDispatcher.h"
#include <Eigen/Sparse>
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
    bool useAMGPreconditioner = true;
};

    struct PressureSolverStats {
        std::size_t solves = 0;
        std::size_t amgSolves = 0;
        std::size_t amgBuilds = 0;
        std::size_t amgRejected = 0;
        std::size_t krylovIters = 0;
        std::size_t amgKrylovIters = 0;
        double amgSeconds = 0.0;
        double buildSeconds = 0.0;
    };

    PressureCorrection(const Mesh<Dim>& mesh, const Config& config);
    PressureCorrection(const Mesh<Dim>& mesh);
    void setConfig(const Config& cfg) {
        config_ = cfg;
        linearSolver_.setConfig(solverConfig_(cfg));
    }
    const std::vector<char>& pressureMask() const { return pMask_; }
    const std::array<Field<Dim>, Dim>& pressureGradient() const { return pressureGrad_; }

    // Recompute the least-squares pressure gradient. The compact Rhie-Chow
    // face flux used by the residual diagnostics needs the gradient of the
    // CURRENT pressure; the one left behind by the last solve() is the
    // gradient of the pre-correction pressure.
    void refreshPressureGradient(const Field<Dim>& pressure);

    // Inject a caller-owned Gradient (the same operator SIMPLE and
    // correctVelocity use). When set, refreshPressureGradient() uses it
    // instead of the internally-owned gradientCalc_.
    void setPressureGradient(Gradient<Dim>* g) { pressureGradient_ = g; }

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
    int amgLevels() const { return linearSolver_.amgLevels(0); }
    const std::vector<int>& amgLevelSizes() const { return linearSolver_.amgLevelSizes(0); }
    // Per-cycle relative residuals from the last AMG build/solve.
    const std::vector<double>& amgLastResidualHistory() const { return linearSolver_.amgLastResidualHistory(0); }

    void solve(
        const VectorField<Dim>& uStar,
        const std::array<std::vector<double>, Dim>& dCoeff,
        const Field<Dim>& pressure,
        Field<Dim>& pPrime,
        const BCDiscretization& bc,
        const std::atomic<bool>* cancel = nullptr);

private:
    // Map this component's config onto the shared linear-solver dispatcher.
    static typename LinearSolverDispatcher<Dim>::Config solverConfig_(const Config& cfg);

    const Mesh<Dim>& mesh_;
    Config config_;
    // The least-squares gradient of the pressure field. In compact Rhie-Chow
    // mode it supplies the face-interpolated gradient that the Rhie-Chow
    // correction subtracts; in legacy mode it is also the operator whose
    // divergence the pressure matrix is assembled from. It is the same
    // operator the velocity correction applies.
    std::unique_ptr<Gradient<Dim>> gradientCalc_;
    // Non-owning pointer to a caller-injected Gradient (e.g. SIMPLE's
    // pressureGradientCalc_). When set, refreshPressureGradient() uses it
    // instead of gradientCalc_, ensuring all consumers share the same operator.
    Gradient<Dim>* pressureGradient_ = nullptr;
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
    bool isolatedCellsReported_ = false;
    // Diagnostics of the last linear solve.
    std::size_t lastSolveIterations_ = 0;
    double lastSolveResidual_ = 0.0;
    bool lastSolveProducedNonFinite_ = false;
    // Pressure-outlet faces with inward normal velocity during the last solve.
    std::size_t lastReverseFlowFaces_ = 0;
    mutable PressureSolverStats pressureStats_{};

    // Unified linear-solver dispatch (replaces inline AMG + Krylov).
    // Component 0 is used for the pressure solve.
    LinearSolverDispatcher<Dim> linearSolver_;
    bool amgBuilt_ = false;

    mutable SparseMatrix A_;
    mutable bool aPatternBuilt_ = false;
    mutable std::size_t aPatternHash_ = 0;
    // O(nnz) value update — avoids O(nnz log nnz) sort + O(nnz) hash per iter
    mutable std::vector<int> aTripletToValuePos_;
};

extern template class PressureCorrection<2>;
extern template class PressureCorrection<3>;

} // namespace fvm
