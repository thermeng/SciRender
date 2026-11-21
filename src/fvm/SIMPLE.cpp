#include "fvm/SIMPLE.h"

#include "fvm/PressureCorrection.h"
#include "fvm/Residual.h"
#include "fvm/VelocityCorrection.h"
#include "fvm/SSTConstants.h"
#include "io/VTKXMLWriter.h"
#include "core/pvd_writer.h"

#include <iomanip>
#include <cmath>
#include <limits>
#include <cctype>
#include <fstream>
#include <utility>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace fvm {

using cfd::ResolvedStoppingPolicy;
using cfd::ResidualEquation;

// Resident set size of this process in MiB -- Task Manager's Memory column,
// i.e. the private working set. The whole working set also counts pages shared
// with system DLLs, which is why it reads ~100 MiB higher than Task Manager.
// Printed once per printed solver row (steady iteration or transient step) so a
// long run can be watched for memory drift without an external sampler.
#ifdef _WIN32
// ABI defined by the Windows SDK (Windows 10); the MinGW headers ship an older
// psapi.h without it. GetProcessMemoryInfo fills it when cb is its size.
struct ProcessMemoryCountersEx2 {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivateUsage;
    SIZE_T PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
};
#endif

double residentSetMiB() {
#ifdef _WIN32
    ProcessMemoryCountersEx2 pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                              sizeof(pmc))) return 0.0;
    return static_cast<double>(pmc.PrivateWorkingSetSize) / (1024.0 * 1024.0);
#else
    std::ifstream statm("/proc/self/statm");
    long totalPages = 0, residentPages = 0;
    if (!(statm >> totalPages >> residentPages)) return 0.0;
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) return 0.0;
    return static_cast<double>(residentPages) * pageSize / (1024.0 * 1024.0);
#endif
}

template<std::size_t Dim>
SIMPLE<Dim>::SIMPLE(const Mesh<Dim>& mesh, double rho, double nu, const typename CFDSolver<Dim>::Config& config)
    : CFDSolver<Dim>(mesh, rho, nu, config) {
    // The transient driver runs the transient path (time loop, PISO, BDF2,
    // adaptive dt) on this solver's shared context.
    this->transientDriver_ = std::make_unique<TransientDriver<Dim>>(*this);
    this->applyPressureSolverConfig_();
    // Slope limiter for the SOU reconstruction (momentum + turbulence).
    this->momentum_.setLimiter(fvm::SlopeLimiterConfig{
        static_cast<fvm::LimiterKind>(this->config_.limiterKind),
        static_cast<fvm::LimiterAggregation>(this->config_.limiterAggregation),
        static_cast<fvm::LimiterSpan>(this->config_.limiterSpan),
        this->config_.limiterK,
        this->config_.cubicTransition});
    for (auto& dc : this->dCoeff_) dc.assign(mesh.nCells(), 0.0);
    for (auto& g : this->velGrad_) g = Field<Dim>(mesh, 0.0);
    for (auto& g : this->pressureGrad_) g = Field<Dim>(mesh, 0.0);
    this->pressureGradientCalc_ = std::make_unique<Gradient<Dim>>(
        mesh, GradientScheme::LeastSquares);
}














template<std::size_t Dim>
void SIMPLE<Dim>::setVelocityBC(std::size_t d, int sign, VelocityBCType type,
                                 double val1, double val2, double val3) {
    this->bc_.setVelocityBC(d, sign, type, val1, val2, val3);
}

template<std::size_t Dim>
void SIMPLE<Dim>::setMin(std::size_t d, VelocityBCType type,
                         double u, double v, double w) {
    this->bc_.setVelocityBC(d, -1, type, u, v, w);
}

template<std::size_t Dim>
void SIMPLE<Dim>::setMax(std::size_t d, VelocityBCType type,
                         double u, double v, double w) {
    this->bc_.setVelocityBC(d, +1, type, u, v, w);
}

template<std::size_t Dim>
void SIMPLE<Dim>::addVelocityPatch(const std::string& name, std::size_t d, int sign,
                                   VelocityBCType type, double u, double v, double w) {
    this->bc_.addVelocityPatch(name, d, sign, type, u, v, w);
}

template<std::size_t Dim>
const typename SIMPLE<Dim>::VelocityPatch* SIMPLE<Dim>::findVelocityPatch(
    const std::string& name) const {
    return this->bc_.findVelocityPatch(name);
}

template<std::size_t Dim>
const std::vector<typename SIMPLE<Dim>::VelocityPatch>&
SIMPLE<Dim>::velocityPatches() const {
    return this->bc_.velocityPatches();
}

template<std::size_t Dim>
void SIMPLE<Dim>::setVelocityBCOnFaces(const std::vector<std::size_t>& faceIndices,
                                       VelocityBCType type,
                                       double u, double v, double w) {
    this->bc_.setVelocityBCOnFaces(faceIndices, type, u, v, w);
}

template<std::size_t Dim>
void SIMPLE<Dim>::setVelocityInletNormalOnFaces(
        const std::vector<std::size_t>& faceIndices, double magnitude) {
    this->bc_.setVelocityInletNormalOnFaces(faceIndices, magnitude);
}

template<std::size_t Dim>
void SIMPLE<Dim>::setVelocityBCOnFacesWithSlip(const std::vector<std::size_t>& faceIndices,
                                               const std::string& typeStr,
                                               double u, double v, double w,
                                               bool slip,
                                               double pressure) {
    this->bc_.setVelocityBCOnFacesWithSlip(faceIndices, typeStr, u, v, w, slip, pressure);
}

template<std::size_t Dim>
void SIMPLE<Dim>::setPressureOutlet(std::size_t d, int sign, double pressure) {
    this->bc_.setPressureOutlet(d, sign, pressure);
}

template<std::size_t Dim>
void SIMPLE<Dim>::setPressureOutletOnFaces(const std::vector<std::size_t>& faceIndices,
                                           double pressure) {
    this->bc_.setPressureOutletOnFaces(faceIndices, pressure);
}

template<std::size_t Dim>
void SIMPLE<Dim>::setPressureOutletOnFacesWithBackflow(const std::vector<std::size_t>& faceIndices,
                                                       double pressure,
                                                       bool suppressBackflow,
                                                       double u, double v, double w,
                                                       double k, double omega) {
    this->bc_.setPressureOutletOnFacesWithBackflow(faceIndices, pressure, suppressBackflow, u, v, w, k, omega);
}

template<std::size_t Dim>
void SIMPLE<Dim>::enableTurbulence(double kInf, double omegaInf,
                                    double alphaK, double alphaOmega,
                                    bool wallFunction) {
    // Reject the values that used to produce a silently wrong first momentum
    // solve: kInf=0 makes the model laminar, omegaInf=0 makes nut infinite.
    if (!(kInf > 0.0) || !std::isfinite(kInf)) kInf = 1e-3;
    if (!(omegaInf > 0.0) || !std::isfinite(omegaInf)) omegaInf = 1.0;

    this->config_.turbulence = true;
    this->config_.kInf = kInf;
    this->config_.omegaInf = omegaInf;
    this->config_.alphaK = std::clamp(alphaK, 1e-3, 1.0);
    this->config_.alphaOmega = std::clamp(alphaOmega, 1e-3, 1.0);
    this->config_.wallFunction = wallFunction;

    // Create the model HERE, not in this->initializeSolve_. The application caches
    // turbulence() before solve() starts (cfd_session hands the pointer to the
    // render callback), so it must already be valid. this->initializeSolve_ reuses
    // this instance so that pointer stays stable for the whole run.
    if (!this->turbulence_) {
        typename SSTModel<Dim>::Config sstCfg;
        sstCfg.kInf       = this->config_.kInf;
        sstCfg.omegaInf   = this->config_.omegaInf;
        sstCfg.alphaK     = this->config_.alphaK;
        sstCfg.alphaOmega = this->config_.alphaOmega;
        sstCfg.wallFunction = this->config_.wallFunction;
        // Map deprecated wallFunction bool to wallTreatment enum.
        using WallTreatment = typename SSTModel<Dim>::Config::WallTreatment;
        if (wallFunction) {
            sstCfg.wallTreatment = WallTreatment::AllYPlus;
        } else {
            sstCfg.wallTreatment = WallTreatment::LowRe;
        }
        sstCfg.limiter = fvm::SlopeLimiterConfig{
            static_cast<fvm::LimiterKind>(this->config_.limiterKind),
            static_cast<fvm::LimiterAggregation>(this->config_.limiterAggregation),
            static_cast<fvm::LimiterSpan>(this->config_.limiterSpan),
            this->config_.limiterK,
            this->config_.cubicTransition};
        sstCfg.turbulenceAmgConfig = this->config_.turbulenceAmg;
        sstCfg.turbulenceAmgCycles = this->config_.turbulenceAmgCycles;
        sstCfg.turbulenceAmgMaxResidual = this->config_.turbulenceAmgMaxResidual;
        this->turbulence_ = std::make_unique<SSTModel<Dim>>(this->mesh_, sstCfg);
        this->turbulence_->initialize(this->nu_ * this->rho_, this->rho_, this->maxInletSpeed_);
    }
}













template<std::size_t Dim>
void SIMPLE<Dim>::solve(VectorField<Dim>& velocity, Field<Dim>& pressure) {
    this->initializeSolve_(velocity, pressure);

    this->applyStoppingPolicy_();

    this->status_ = Status::NotConverged;

    this->convergenceMonitor_ = std::make_unique<cfd::ConvergenceMonitor>(
        this->config_.stoppingPolicy.steady);

    const auto steadyTotalStart = std::chrono::steady_clock::now();

    for (std::size_t iter = this->startIteration_; iter < this->config_.stoppingPolicy.steady.iterationLimit; ++iter) {
        this->m_iterStart = std::chrono::steady_clock::now();
        // Pause/step boundary. Placed before the iteration so a pause issued
        // after the previous iteration is observed before doing more work, and
        // the fields the UI published are always a completed iteration.
        if (!this->waitIfPaused()) {
            this->status_ = Status::Cancelled;
            break;
        }

        this->performIteration_(velocity, pressure, iter);

        auto tResid = std::chrono::steady_clock::now();
        this->updateResiduals_();
        this->steadyResidualSeconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - tResid).count();

        this->lastIterations_ = static_cast<int>(iter + 1);

        if ((iter + 1) % this->config_.printFrequency == 0 ||
            iter + 1 == this->config_.stoppingPolicy.steady.iterationLimit) {
            this->printIterationRow_(false, static_cast<int>(iter + 1), 0.0, 0.0);
        }

        if (this->config_.iterationCallback) {
            this->config_.iterationCallback(static_cast<int>(iter + 1), this->lastContResid_, this->lastMomResid_);
        }
        // Publish a render frame each iteration for steady runs (and each time
        // step for transient runs). The caller decides how often to refresh the
        // UI from these; see RunHooks.frame / visualizationInterval.
        if (this->config_.renderCallback) {
            this->config_.renderCallback(static_cast<int>(iter + 1), 0.0, velocity, pressure);
        }

        this->saveCheckpoint_(iter);

        if (this->checkConvergence_(iter)) {
            // Distinguish the three reasons the loop can end: the tolerance was
            // met, the run blew up, or the user cancelled it. Reporting "done"
            // for all three is what made an unconverged result look converged.
            if (this->aborted_) {
                this->status_ = Status::Aborted;
            } else if (this->cancelRequested_.load(std::memory_order_acquire)) {
                this->status_ = Status::Cancelled;
            } else {
                this->status_ = Status::Converged;
            }
            break;
        }
    }

    this->linearSolverDispatcher_.printStats();
    if (this->config_.pressureSolverStats) this->printPressureSolverStats_();
    this->steadyTotalSeconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - steadyTotalStart).count();
    this->printFinalStats_(/*isTransient=*/false);
}











// (cancel control now inherited from Cancellable)

// --- Private method implementations ---



template<std::size_t Dim>
void SIMPLE<Dim>::solveTransient(VectorField<Dim>& velocity, Field<Dim>& pressure) {
    // The transient path (time loop, PISO, BDF2, adaptive dt) lives in
    // TransientDriver now; see its run().
    this->transientDriver_->run(velocity, pressure);
}

template class SIMPLE<2>;
template class SIMPLE<3>;

} // namespace fvm
