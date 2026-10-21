#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/Types.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/Gradient.h"
#include "fvm/RhieChow.h"
#include "fvm/AMG.h"
#include <Eigen/Sparse>
#include <Eigen/IterativeLinearSolvers>
#include <array>
#include <vector>
#include <memory>
#include <cstddef>

namespace fvm {

template<std::size_t Dim>
class PressureCorrection {
public:
    struct Config {
        bool rhieChow = true;
        std::size_t pressureRefCell = SIZE_MAX;
        bool useAMG = true;
        bool useAMGPreconditioner = true;
        // Number of cells below which the AMG setup cost is not amortised and
        // the solve falls back to CG+IC (or BiCGSTAB+ILU). Pure-Neumann meshes
        // use twice this, because AMG handles a pinned reference cell poorly.
        std::size_t amgThreshold = 10000;
        AMGHierarchy::Config amgConfig = []{
            AMGHierarchy::Config c;
            c.interpolationType = AMGHierarchy::InterpolationType::Extended;
            c.coarseningType = AMGHierarchy::CoarseningType::RS;
            c.cycleType = AMGHierarchy::CycleType::V;
            c.strengthThreshold = 0.25;
            c.truncThreshold = 0.2;
            c.preSweeps = 2;
            c.postSweeps = 2;
            c.maxIterations = 100;
            c.tolerance = 1e-8;
            return c;
        }();
    };

    PressureCorrection(const Mesh<Dim>& mesh, const Config& config);
    PressureCorrection(const Mesh<Dim>& mesh);
    void setConfig(const Config& cfg) { config_ = cfg; }
    const std::vector<char>& pressureMask() const { return pMask_; }
    const std::array<Field<Dim>, Dim>& pressureGradient() const { return pressureGrad_; }

    // The number of cells that have no pressure equation at all (no interior
    // face: a detached cell). Such a cell is excluded from the pressure
    // unknowns and its mass balance can never be corrected, so the count is
    // reported - otherwise the run silently never converges.
    std::size_t isolatedCellCount() const { return isolatedCellCount_; }

    // Diagnostics of the last linear solve: the iteration count and the
    // residual the solver ended with.
    std::size_t lastSolveIterations() const { return lastSolveIterations_; }
    double lastSolveResidual() const { return lastSolveResidual_; }
    // The assembled system of the last solve (diagnostics).
    const SparseMatrix& lastMatrix() const { return A_; }
    const Eigen::VectorXd& lastRhs() const { return bVec_; }

    void solve(
        const VectorField<Dim>& uStar,
        const std::array<std::vector<double>, Dim>& dCoeff,
        const Field<Dim>& pressure,
        Field<Dim>& pPrime,
        const std::vector<std::pair<std::size_t, double>>& pressureOutletCells,
        const std::vector<char>& isPressureNeumann,
        const std::vector<char>* isPressureDirichletFace = nullptr,
        const std::vector<std::pair<std::size_t, double>>* pressureOutletFaces = nullptr,
        const std::vector<FaceVelocityBC>* faceBC = nullptr,
        const std::vector<char>* faceHasBC = nullptr,
        // Velocity Dirichlet data (cell-level). When a cell carries a prescribed
        // velocity the momentum assembly uses that prescribed value for the WHOLE
        // face; the divergence here must use the very same face velocity, or the
        // pressure correction balances a mass imbalance that the momentum
        // equation never had.
        const std::vector<std::vector<char>>* velDirichletMask = nullptr,
        const std::vector<std::vector<double>>* velDirichletVals = nullptr);

private:
    const Mesh<Dim>& mesh_;
    Config config_;
    std::unique_ptr<Gradient<Dim>> gradientCalc_;
    std::array<Field<Dim>, Dim> pressureGrad_;
    std::array<Field<Dim>, Dim> pressureGradPrev_;
    bool hasPrevGrad_ = false;

    // Reusable buffers to avoid per-iteration allocation
    mutable std::vector<Triplet> triplets_;
    mutable Eigen::VectorXd bVec_;
    mutable std::vector<double> diagSum_;
    mutable std::vector<char> pMask_;
    mutable std::vector<double> pVals_;
    mutable std::vector<char> isPressureOutlet_;
    mutable Field<Dim> divU_;
    // Cells without a pressure equation (see isolatedCellCount()).
    std::size_t isolatedCellCount_ = 0;
    // Diagnostics of the last linear solve.
    std::size_t lastSolveIterations_ = 0;
    double lastSolveResidual_ = 0.0;

    Eigen::ConjugateGradient<SparseMatrix, Eigen::Lower|Eigen::Upper,
                             Eigen::IncompleteCholesky<double>> cgSolver_;
    Eigen::BiCGSTAB<SparseMatrix, AMGPreconditioner> bicgSolver_;
    Eigen::BiCGSTAB<SparseMatrix, Eigen::IncompleteLUT<double>> bicgFallbackSolver_;
    bool pPatternAnalyzed_ = false;
    mutable std::vector<char> prevPMask_;
    mutable std::size_t prevNonZeroPattern_ = 0;

    AMGHierarchy amg_;
    AMGPreconditioner amgPrec_;
    bool amgBuilt_ = false;
    std::size_t amgPatternHash_ = 0;

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
