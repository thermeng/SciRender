#include "ui/controllers/cfd_controller.h"

#include "cfd/amg_settings_bridge.h"
#include "cfd/cfd_mesh_adapter.h"
#include "cfd/cfd_result_builder.h"
#include "cfd/cfd_types.h"
#include "cfd/transient_settings_bridge.h"

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/SIMPLE.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/SSTModel.h"
#include "io/VTKXMLWriter.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QFileInfo>

#include <memory>
#include <mutex>

#include <omp.h>
#include <Eigen/Core>

using namespace cfd;

CfdController::CfdController(QObject* parent) : QObject(parent) {}

CfdController::~CfdController() = default;

bool CfdController::loadMesh(const QString& path) {
    m_mesh = cfd::loadMesh(path.toStdString());
    if (!m_mesh.error.empty()) {
        emit failed(QString::fromStdString(m_mesh.error));
        return false;
    }
    m_meshPtr = m_mesh.mesh2;
    m_meshPath = path;
    emit meshLoaded();
    return true;
}

void CfdController::run() {
    if (!m_meshPtr) { emit failed("No mesh loaded"); return; }
    m_cancel.store(false);
    m_lastTime.store(0.0);
    uint64_t gen = ++m_gen;

    auto meshPtr = m_meshPtr;
    auto settings = m_settings;
    auto base = m_mesh.renderMesh;
    auto patchBCs = m_patchBCs;

    // Honor the user's thread-count choice (0 = OpenMP/Eigen default).
    const int requestedThreads = settings.numThreads;
    const int threadCount = requestedThreads > 0 ? requestedThreads : omp_get_max_threads();
    if (threadCount > 0) {
        omp_set_num_threads(threadCount);
        Eigen::setNbThreads(threadCount);
    }
    // Resolve a relative PVD path against the mesh's directory so the .vtu
    // frames and the .pvd land next to the case instead of in the process CWD.
    if (!settings.transient.pvdFile.empty()) {
        std::string& pvd = settings.transient.pvdFile;
        if (pvd.find_first_of("/\\\\") == std::string::npos) {
            QString dir = QFileInfo(m_meshPath).absolutePath();
            pvd = (dir + "/" + QString::fromStdString(pvd)).toStdString();
        }
    }

    m_velocity = std::make_shared<fvm::VectorField<2>>(*meshPtr, 0.0);
    m_pressure = std::make_shared<fvm::Field<2>>(*meshPtr, 0.0);

    fvm::SIMPLE<2>::Config sc;
    sc.maxIterations = settings.maxIterations;
    sc.tolerance = settings.tolerance;
    sc.runFixedIterations = settings.runFixedIterations;
    sc.convergenceHoldIterations = settings.convergenceHoldIterations;
    sc.alphaU = settings.alphaU;
    sc.alphaP = settings.alphaP;
    sc.simplec = settings.simplec;
    sc.rhieChow = settings.rhieChow;
    sc.convectionScheme = settings.scheme == 1 ? fvm::ConvectionScheme::Central
                          : (settings.scheme == 2 ? fvm::ConvectionScheme::SecondOrderUpwind
                             : (settings.scheme == 3 ? fvm::ConvectionScheme::QUICK : fvm::ConvectionScheme::Upwind));
    sc.gradientScheme = settings.gradientScheme == 1 ? fvm::GradientScheme::GreenGaussCellBased
                       : (settings.gradientScheme == 2 ? fvm::GradientScheme::GreenGaussNodeBased : fvm::GradientScheme::LeastSquares);
    sc.printFrequency = settings.printFrequency;
    sc.residualHistoryWindow = settings.residualHistoryWindow;
    sc.pressureCorrectors = settings.pressureCorrectors;
    sc.limitPressureCorrection = settings.limitPressureCorrection;
    sc.pressureCorrectionLimit = settings.pressureCorrectionLimit;
    sc.pressureRefCell = settings.pressureRefCell == 0 ? SIZE_MAX : settings.pressureRefCell;
    sc.pressureUseAMG = settings.pressureUseAMG;
    sc.pressureUseAMGPreconditioner = settings.pressureUseAMGPreconditioner;
    sc.pressureAmgThreshold = settings.pressureAmgThreshold;
    sc.pressureAmgConfig = toFvmAmg(settings.pressureAmg);
    sc.useAMGMomentum = settings.useAMGMomentum;
    sc.momentumAmgThreshold = settings.momentumAmgThreshold;
    sc.momentumAmgCycles = settings.momentumAmgCycles;
    sc.momentumAmgMaxResidual = settings.momentumAmgMaxResidual;
    sc.momentumSolverStats = settings.momentumSolverStats;
    sc.momentumAmgConfig = toFvmAmg(settings.momentumAmg);
    sc.solverType = toFvmSolverType(settings.transient);
    sc.transient = toFvmTransient<2>(settings.transient);
    sc.iterationCallback = [this, gen](int iter, double cont, double mom) {
        if (gen != m_gen) return;
        if (m_cancel.load()) return;
        emit progress(iter, cont, mom);
    };
    // Live viewport streaming for transient runs. One frame per time step,
    // independent of the on-disk write cadence. The callback runs on the
    // solver's worker thread between steps (nothing is mutating the fields at
    // that point), so building the RenderMesh here is safe.
    if (settings.transient.enabled) {
        sc.transientCallback = [this, gen, meshPtr, base, settings, patchBCs](int step, double time,
            const fvm::VectorField<2>& velocity, const fvm::Field<2>& pressure) {
            if (gen != m_gen) return;
            if (m_cancel.load()) return;
            m_lastTime.store(time);
            const fvm::SSTModel<2>* turb = nullptr;
            {
                std::lock_guard<std::mutex> lk(m_solverMutex);
                if (m_solver) turb = m_solver->turbulence();
            }
            auto rm = buildResultMesh(base, *meshPtr, velocity, pressure, turb, settings, patchBCs);
            if (m_cancel.load()) return;
            if (rm) emit frameReady(rm, step, time);
        };
    }

    auto solver = std::make_shared<fvm::SIMPLE<2>>(*meshPtr, settings.rho, settings.nu, sc);
    if (settings.turbulence) solver->enableTurbulence(settings.kInf, settings.omegaInf, settings.alphaK, settings.alphaOmega);
    solver->bindVelocity(*m_velocity);
    solver->bindPressure(*m_pressure);

    // Apply per-patch BCs
    for (const auto& pbc : m_patchBCs) {
        auto it = meshPtr->boundaryGroups().find(pbc.patchName);
        if (it == meshPtr->boundaryGroups().end()) continue;
        const std::vector<std::size_t>& faces = it->second;
        std::string typeStr;
        switch (pbc.type) {
            case cfd::BCType::VelocityInlet: typeStr = "inlet"; break;
            case cfd::BCType::PressureOutlet: typeStr = "pressure_outlet"; break;
            case cfd::BCType::Outlet: typeStr = "outlet"; break;
            case cfd::BCType::WallNoSlip: typeStr = "wall"; break;
            case cfd::BCType::SlipWall: typeStr = "slip_wall"; break;
            case cfd::BCType::Symmetry: typeStr = "symmetry"; break;
            case cfd::BCType::MovingWall: typeStr = "moving_wall"; break;
        }
        if (pbc.type == cfd::BCType::PressureOutlet) {
            solver->setPressureOutletOnFacesWithBackflow(faces, pbc.pressure, pbc.suppressBackflow,
                pbc.backflowU, pbc.backflowV, pbc.backflowW, pbc.backflowK, pbc.backflowOmega);
        } else {
            bool slip = (pbc.type == cfd::BCType::SlipWall || pbc.type == cfd::BCType::Symmetry);
            solver->setVelocityBCOnFacesWithSlip(faces, typeStr, pbc.u, pbc.v, pbc.w, slip, pbc.pressure);
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        m_solver = solver;
    }

    QFutureWatcher<void>* w = new QFutureWatcher<void>(this);
    connect(w, &QFutureWatcher<void>::finished, this, [this, w, gen]() {
        w->deleteLater();
        if (gen != m_gen) return;
        if (m_cancel.load()) { emit finished("Cancelled"); return; }
        if (m_settings.transient.enabled) {
            emit finished(QString("Transient run complete (t = %1 s)").arg(m_lastTime.load(), 0, 'g', 6));
            return;
        }
        // Report the VERDICT, not just "the run ended". A run that hit the
        // iteration limit without meeting the tolerance is not a steady state,
        // and the user has to be able to tell the two apart before they trust
        // the field they are looking at.
        fvm::SIMPLE<2>::Status st = fvm::SIMPLE<2>::Status::NotConverged;
        double cont = 0.0;
        int iters = 0;
        {
            std::lock_guard<std::mutex> lk(m_solverMutex);
            if (m_solver) {
                st = m_solver->status();
                cont = m_solver->finalContinuityResidual();
                iters = m_solver->lastIterations();
            }
        }
        switch (st) {
        case fvm::SIMPLE<2>::Status::Converged:
            emit finished(QString("Converged in %1 iterations (continuity %2)")
                              .arg(iters).arg(cont, 0, 'g', 4));
            break;
        case fvm::SIMPLE<2>::Status::Aborted:
            emit finished("Solve aborted: the residual diverged or went non-finite");
            break;
        case fvm::SIMPLE<2>::Status::Cancelled:
            emit finished("Cancelled");
            break;
        case fvm::SIMPLE<2>::Status::NotConverged:
            emit finished(QString("NOT converged: iteration limit (%1) reached, continuity %2")
                              .arg(iters).arg(cont, 0, 'g', 4));
            break;
        }
    });

    // The fields are captured BY VALUE: run() replaces m_velocity/m_pressure
    // with fresh objects, and a worker that dereferenced the members would
    // then write into the NEW fields while the new solver is using them (and
    // the shared_ptr assignment itself would race with the worker's read).
    auto velocity = m_velocity;
    auto pressure = m_pressure;
    auto fut = QtConcurrent::run([this, gen, solver, settings, velocity, pressure]() {
        if (gen != m_gen) return;
        if (settings.transient.enabled)
            solver->solveTransient(*velocity, *pressure);
        else
            solver->solve(*velocity, *pressure);
    });
    w->setFuture(fut);
}

std::shared_ptr<const RenderMesh> CfdController::initialize() {
    if (!m_meshPtr || !m_mesh.renderMesh) return nullptr;
    // Invalidate any in-flight solve generation so its result cannot be applied
    // after this reset, then start from a uniform zero field.
    ++m_gen;
    m_velocity = std::make_shared<fvm::VectorField<2>>(*m_meshPtr, 0.0);
    m_pressure = std::make_shared<fvm::Field<2>>(*m_meshPtr, 0.0);
    return resultMesh();
}

void CfdController::cancel() {
    m_cancel.store(true);
    std::shared_ptr<fvm::SIMPLE<2>> solver;
    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        solver = m_solver;
    }
    if (solver) solver->requestCancel();
}

void CfdController::exportVtu(const QString& path) {
    if (!m_meshPtr || !m_velocity || !m_pressure) return;
    std::array<const fvm::Field<2>*, 2> vel{&m_velocity->operator[](0), &m_velocity->operator[](1)};
    const fvm::SSTModel<2>* turb = m_solver ? m_solver->turbulence() : nullptr;
    const fvm::Field<2>* k = turb ? &turb->k() : nullptr;
    const fvm::Field<2>* om = turb ? &turb->omega() : nullptr;
    const fvm::Field<2>* nt = turb ? &turb->nut() : nullptr;
    fvm::VTKXMLWriter<2>::writeWithVelocity(path.toStdString(), *m_meshPtr, *m_pressure, vel, "pressure", k, om, nt);
}

std::shared_ptr<const RenderMesh> CfdController::resultMesh() const {
    if (!m_meshPtr || !m_velocity || !m_pressure || !m_mesh.renderMesh) return nullptr;
    const fvm::SSTModel<2>* turb = m_solver ? m_solver->turbulence() : nullptr;
    return buildResultMesh(m_mesh.renderMesh, *m_meshPtr, *m_velocity, *m_pressure, turb, m_settings, m_patchBCs);
}
