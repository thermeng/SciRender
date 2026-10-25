#include "ui/controllers/cfd_controller.h"

#include "cfd/amg_settings_bridge.h"
#include "cfd/cfd_mesh_adapter.h"
#include "cfd/cfd_result_builder.h"
#include "cfd/cfd_types.h"
#include "cfd/case_codec.h"
#include "cfd/transient_settings_bridge.h"
#include "cfd/solver_config_bridge.h"

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/SIMPLE.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/SSTModel.h"
#include "io/VTKXMLWriter.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QDir>
#include <QFileInfo>
#include <QDataStream>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>


#include <memory>
#include <mutex>
#include <fstream>

#include <omp.h>
#include <Eigen/Core>

using namespace cfd;

namespace {

constexpr qint64 kMaxMeshBytes = 1024LL * 1024 * 1024; // 1 GiB
constexpr qint64 kMaxSettingsBytes = 16LL * 1024 * 1024;
constexpr qint32 kScfdVersion = 5;

struct PvdEntry {
    std::string time;
    std::string file;
};

// Resolve a possibly-relative PVD path against the case (or mesh) directory so
// output lands next to the case instead of in the process CWD or %TEMP%.
QString resolvePvdPath(const QString& raw, const QString& baseDir) {
    if (raw.isEmpty()) return QDir(baseDir).filePath("transient.pvd");
    QFileInfo fi(raw);
    if (fi.isAbsolute()) return raw;
    return QDir(baseDir).filePath(raw);
}

// The embedded mesh is written to a temp file whose extension selects the
// parser. A display name is not guaranteed to carry a usable suffix (e.g. a
// legacy file saved without one), so fall back to sniffing the payload instead
// of feeding the loader a random extension.
QString meshExtension(const QString& displayName, const QByteArray& bytes) {
    static const QStringList kKnown{"msh", "vtk", "vtu", "vts", "vtr", "vti",
                                    "vtm", "stl", "obj"};
    const QString suffix = QFileInfo(displayName).suffix().toLower();
    if (!suffix.isEmpty() && kKnown.contains(suffix)) return suffix;

    const QByteArray head = bytes.left(512);
    if (head.startsWith("<?xml")) {
        if (head.contains("type=\"RectilinearGrid\"")) return "vtr";
        if (head.contains("type=\"ImageData\"")) return "vti";
        if (head.contains("type=\"StructuredGrid\"")) return "vts";
        return "vtu";
    }
    if (head.startsWith("# vtk DataFile")) return "vtk";
    if (head.startsWith("$MeshFormat")) return "msh";
    if (head.startsWith("solid ") || head.startsWith("facet")) return "stl";
    return "msh"; // ANSYS / Gmsh binary / anything else the loader can sniff
}

std::vector<PvdEntry> readExistingPvd(const QString& path) {
    std::vector<PvdEntry> entries;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return entries;
    const QString text = QString::fromUtf8(f.readAll());
    QRegularExpression re(R"re(<DataSet\s+timestep="([^"]*)"\s+file="([^"]*)")re");
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        entries.push_back({m.captured(1).toStdString(), m.captured(2).toStdString()});
    }
    return entries;
}

void writePvd(const std::string& path, const std::vector<PvdEntry>& entries) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    out << "<?xml version=\"1.0\"?>\n";
    out << "<VTKFile type=\"Collection\" version=\"0.1\">\n";
    out << "  <Collection>\n";
    for (const auto& e : entries) {
        out << "    <DataSet timestep=\"" << e.time << "\" file=\"" << e.file << "\"/>\n";
    }
    out << "  </Collection>\n";
    out << "</VTKFile>\n";
}

} // namespace

CfdController::CfdController(QObject* parent) : QObject(parent) {}

CfdController::~CfdController() {
    m_cancel.store(true);
    requestSolverCancel();
    // A QtConcurrent solve lambda captures `this`; it must be finished before
    // the members it reads go away.
    if (m_solveWatcher) {
        m_solveWatcher->disconnect(this);
        if (m_solveWatcher->isRunning()) m_solveWatcher->waitForFinished();
        m_solveWatcher = nullptr;
    }
    stopWorkers();
}

// --- Lifecycle helpers ----------------------------------------------------

void CfdController::stopWorkers() {
    m_renderPrepRunning.store(false);
    m_diskWriterRunning.store(false);
    if (m_renderPrepThread.joinable()) m_renderPrepThread.join();
    if (m_diskWriterThread.joinable()) m_diskWriterThread.join();
}

void CfdController::requestSolverCancel() {
    std::shared_ptr<fvm::SIMPLE<2>> solver;
    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        solver = m_solver;
    }
    if (solver) solver->requestCancel();
}

void CfdController::resetSolutionState() {
    // Bump the generation first: any in-flight worker that still holds the old
    // generator must not publish into the new state.
    ++m_gen;
    stopWorkers();
    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        m_solver.reset();
        m_solverMesh.reset();
    }
    m_velocity.reset();
    m_pressure.reset();
    m_turbulence.reset();
    m_transient = TransientSnapshot{};
    m_hasSolution = false;
    m_lastTime.store(0.0);
    m_latestStep.store(nullptr);
    m_latestFrameSlot.store(nullptr);
}

bool CfdController::solverMatchesCurrentMesh() const {
    std::lock_guard<std::mutex> lk(m_solverMutex);
    return m_solver && m_meshPtr && m_solverMesh == m_meshPtr;
}

void CfdController::setError(const QString& msg) {
    m_lastError = msg;
    emit failed(msg);
}

void CfdController::captureLiveState() {
    std::shared_ptr<fvm::SIMPLE<2>> solver;
    std::shared_ptr<const fvm::Mesh<2>> solverMesh;
    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        solver = m_solver;
        solverMesh = m_solverMesh;
    }
    if (!solver || !m_meshPtr || solverMesh != m_meshPtr) return;

    const std::size_t n = m_meshPtr->nCells();

    // Current turbulence is independent of the time mode: a steady run needs
    // it just as much for a lossless Save.
    const fvm::SSTModel<2>* turb = solver->turbulence();
    if (turb) {
        m_transient.hasTurbulence = true;
        m_transient.k.resize(n);
        m_transient.omega.resize(n);
        m_transient.nut.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            m_transient.k[i] = turb->k()[i];
            m_transient.omega[i] = turb->omega()[i];
            m_transient.nut[i] = turb->nut()[i];
        }
    } else {
        m_transient.hasTurbulence = false;
        m_transient.k.clear();
        m_transient.omega.clear();
        m_transient.nut.clear();
    }

    if (!m_settings.transient.enabled) {
        m_transient.valid = false;
        return;
    }

    m_transient.valid = solver->hasTransientState();
    m_transient.time = solver->currentTime();
    m_transient.dt = solver->currentDt();
    m_transient.dtPrev = solver->currentDtPrev();
    m_transient.step = solver->currentStep();

    const auto& vnm1 = solver->velocityNM1();
    const bool haveV = vnm1.size() == n &&
                       vnm1[0].size() == n && vnm1[1].size() == n;
    m_transient.hasVelocityHistory = haveV;
    if (haveV) {
        for (std::size_t d = 0; d < 2; ++d) {
            m_transient.velocityNM1[d].resize(n);
            for (std::size_t i = 0; i < n; ++i) m_transient.velocityNM1[d][i] = vnm1[d][i];
        }
    } else {
        m_transient.velocityNM1[0].clear();
        m_transient.velocityNM1[1].clear();
    }

    if (turb) {
        const auto& kn = solver->kNM1();
        const auto& on = solver->omegaNM1();
        m_transient.hasTurbulenceHistory = kn.size() == n && on.size() == n;
        if (m_transient.hasTurbulenceHistory) {
            m_transient.kNM1.resize(n);
            m_transient.omegaNM1.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                m_transient.kNM1[i] = kn[i];
                m_transient.omegaNM1[i] = on[i];
            }
        }
    } else {
        m_transient.hasTurbulenceHistory = false;
        m_transient.kNM1.clear();
        m_transient.omegaNM1.clear();
    }
    m_lastTime.store(m_transient.time);
}

bool CfdController::loadMesh(const QString& path) {
    m_lastError.clear();
    if (m_running.load()) {
        setError("Stop the current run before loading a mesh.");
        return false;
    }
    auto res = cfd::loadMesh(path.toStdString());
    if (!res.error.empty() || !res.mesh2) {
        setError(QString::fromStdString(res.error.empty() ? "Unsupported mesh" : res.error));
        return false;
    }

    resetSolutionState();
    m_mesh = std::move(res);
    m_meshPtr = m_mesh.mesh2;
    m_meshPath = path;
    m_meshName = QFileInfo(path).fileName();
    m_embeddedMesh.clear();
    m_casePath.clear();
    m_caseDir = QFileInfo(path).absolutePath();
    m_tempDir.reset();

    m_patchBCs.clear();
    for (const auto& patch : m_mesh.patches) {
        cfd::PatchBC pbc;
        pbc.patchName = patch.name;
        pbc.type = cfd::BCType::WallNoSlip;
        m_patchBCs.push_back(pbc);
    }

    emit meshLoaded();
    return true;
}

bool CfdController::run() {
    m_lastError.clear();
    if (m_running.load()) {
        setError("A solve is already running.");
        return false;
    }
    if (!m_meshPtr || !m_mesh.renderMesh) {
        setError("No mesh loaded.");
        return false;
    }

    // Join the previous run's helper threads, then keep its final state (the
    // fields are bound by reference and already hold it) before replacing the
    // solver. Capture happens before the generation bump so the snapshot is
    // still attributed to the completed run.
    stopWorkers();
    captureLiveState();

    m_cancel.store(false);
    m_latestStep.store(nullptr);
    m_latestFrameSlot.store(nullptr);

    const uint64_t gen = ++m_gen;

    auto meshPtr = m_meshPtr;
    auto settings = m_settings;
    auto base = m_mesh.renderMesh;
    auto patchBCs = m_patchBCs;
    const std::size_t nCells = meshPtr->nCells();

    // Honor the user's thread-count choice (0 = OpenMP/Eigen default).
    const int requestedThreads = settings.numThreads;
    const int threadCount = requestedThreads > 0 ? requestedThreads : omp_get_max_threads();
    if (threadCount > 0) {
        omp_set_num_threads(threadCount);
        Eigen::setNbThreads(threadCount);
    }

    // Resolve a relative PVD path against the case/mesh directory.
    const QString baseDir = !m_caseDir.isEmpty()
        ? m_caseDir
        : QFileInfo(m_meshPath).absolutePath();
    if (!settings.transient.pvdFile.empty()) {
        settings.transient.pvdFile =
            resolvePvdPath(QString::fromStdString(settings.transient.pvdFile), baseDir)
                .toStdString();
    }

    // Continue from the current solution when it belongs to this mesh;
    // otherwise (fresh mesh / first run) start from zero. Initialize is the
    // explicit reset.
    const bool fieldsMatch = m_velocity && m_pressure &&
        m_velocity->size() == nCells &&
        m_velocity->operator[](0).size() == nCells &&
        m_velocity->operator[](1).size() == nCells &&
        m_pressure->size() == nCells;
    if (!fieldsMatch) {
        m_velocity = std::make_shared<fvm::VectorField<2>>(*meshPtr, 0.0);
        m_pressure = std::make_shared<fvm::Field<2>>(*meshPtr, 0.0);
        m_hasSolution = false;
        m_transient = TransientSnapshot{};
    }

    if (settings.transient.enabled) {
        if (!(settings.transient.tEnd > 0.0)) {
            setError("End time must be greater than zero.");
            return false;
        }
        if (m_transient.valid && m_transient.time >= settings.transient.tEnd - 1e-12) {
            setError(QString("The saved state is already at t = %1 s (end time %2 s). "
                             "Raise the end time to continue the run.")
                         .arg(m_transient.time, 0, 'g', 8)
                         .arg(settings.transient.tEnd, 0, 'g', 8));
            return false;
        }
    } else {
        // A steady run has no transient clock; do not persist a stale one.
        m_transient.valid = false;
    }

    m_lastTime.store(settings.transient.enabled && m_transient.valid ? m_transient.time : 0.0);

    // True when the fields are a previous result (loaded case or earlier run)
    // rather than the zero initial condition of a fresh start. The solver only
    // needs a restart payload in the former case.
    const bool hadSolution = m_hasSolution;

    // From here on the run is accepted, so the fields are a solution: they are
    // either the loaded/previous result or the zero initial condition the solver
    // is about to advance.
    m_hasSolution = true;

    fvm::SIMPLE<2>::Config sc = cfd::toFvmConfig<2>(settings);
    if (settings.turbulence) {
        for (const auto& pbc : patchBCs) {
            if (pbc.type != cfd::BCType::VelocityInlet) continue;
            fvm::SIMPLE<2>::InletTurbulence it;
            it.patch = pbc.patchName;
            it.k = pbc.kInlet;
            it.omega = pbc.omegaInlet;
            sc.inletTurbulence.push_back(it);
        }
    }
    sc.iterationCallback = [this, gen](int iter, double cont, double mom) {
        if (gen != m_gen) return;
        if (m_cancel.load()) return;
        emit progress(iter, cont, mom);
    };
    // Frame bus (Phase 2): solver stores an immutable step snapshot each
    // iteration; render-prep thread builds the mesh off the UI thread.
    if (settings.transient.enabled) {
        sc.renderCallback = [this, gen](int step, double time,
            const fvm::VectorField<2>& velocity, const fvm::Field<2>& pressure) {
            if (gen != m_gen) return;
            if (m_cancel.load()) return;
            m_lastTime.store(time);
            auto v = std::make_shared<fvm::VectorField<2>>(velocity);
            auto p = std::make_shared<fvm::Field<2>>(pressure);
            auto ts = std::make_shared<TransientStep>();
            ts->step = step;
            ts->time = time;
            ts->velocity = v;
            ts->pressure = p;
            ts->gen = gen;
            m_latestStep.store(std::shared_ptr<const TransientStep>(ts));
        };
    }

    auto solver = std::make_shared<fvm::SIMPLE<2>>(*meshPtr, settings.rho, settings.nu, sc);
    if (settings.turbulence) {
        solver->enableTurbulence(settings.kInf, settings.omegaInf, settings.alphaK, settings.alphaOmega);
    }
    solver->bindVelocity(*m_velocity);
    solver->bindPressure(*m_pressure);

    // Apply per-patch BCs
    for (const auto& pbc : patchBCs) {
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

    // Queue the restart payload. initializeSolve_ applies it after the
    // turbulence model exists, so restored k/omega/nut survive.
    if (hadSolution || m_transient.hasTurbulence || m_transient.valid) {
        fvm::SIMPLE<2>::RestartState rs;
        if (hadSolution) {
            rs.hasVelocity = true;
            for (std::size_t d = 0; d < 2; ++d) {
                rs.velocity[d].resize(nCells);
                for (std::size_t i = 0; i < nCells; ++i)
                    rs.velocity[d][i] = m_velocity->operator[](d)[i];
            }
            rs.pressure.resize(nCells);
            for (std::size_t i = 0; i < nCells; ++i)
                rs.pressure[i] = m_pressure->operator[](i);
        }
        if (m_transient.hasTurbulence) {
            rs.hasTurbulence = true;
            rs.k = m_transient.k;
            rs.omega = m_transient.omega;
            rs.nut = m_transient.nut;
        }
        if (settings.transient.enabled && m_transient.valid) {
            rs.hasTransient = true;
            rs.time = m_transient.time;
            rs.dt = m_transient.dt;
            rs.dtPrev = m_transient.dtPrev;
            rs.step = m_transient.step;
            if (m_transient.hasVelocityHistory) {
                rs.hasVelocityHistory = true;
                for (std::size_t d = 0; d < 2; ++d) rs.velocityNM1[d] = m_transient.velocityNM1[d];
            }
            if (m_transient.hasTurbulenceHistory) {
                rs.hasTurbulenceHistory = true;
                rs.kNM1 = m_transient.kNM1;
                rs.omegaNM1 = m_transient.omegaNM1;
            }
        }
        solver->setRestartState(std::move(rs));
    }

    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        m_solver = solver;
        m_solverMesh = meshPtr;
    }

    if (settings.transient.enabled) {
        m_renderPrepRunning.store(true);
        auto baseMesh = base;
        auto meshP = meshPtr;
        auto cfg = settings;
        auto bcs = patchBCs;
        m_renderPrepThread = std::thread([this, gen, baseMesh, meshP, cfg, bcs]() {
            std::shared_ptr<const TransientStep> last;
            while (m_renderPrepRunning.load() && gen == m_gen && !m_cancel.load()) {
                auto step = m_latestStep.load();
                if (!step || step == last) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (m_cancel.load() || gen != m_gen) break;
                const fvm::SSTModel<2>* turb = nullptr;
                {
                    std::lock_guard<std::mutex> lk(m_solverMutex);
                    if (m_solver) turb = m_solver->turbulence();
                }
                auto rm = buildResultMesh(baseMesh, *meshP, *step->velocity, *step->pressure, turb, cfg, bcs);
                if (m_cancel.load() || gen != m_gen) break;
                if (rm) {
                    // Publish mesh + post-field snapshot + status as ONE immutable
                    // slot: the GUI then always sees a consistent time step, and
                    // nothing here touches RenderSettings.
                    auto slot = std::make_shared<CfdFrameSlot>();
                    slot->mesh = rm;
                    slot->step = step->step;
                    slot->time = step->time;
                    slot->gen = gen;
                    if (m_postExtractor) {
                        auto res = m_postExtractor(*rm);
                        if (res.keepExisting) {
                            auto prev = m_latestFrameSlot.load();
                            slot->computed = prev ? prev->computed : nullptr;
                        } else {
                            slot->computed = res.ok ? res.snapshot : nullptr;
                            slot->postStatus = std::move(res.status);
                            slot->postExtracted = true;
                        }
                    }
                    m_latestFrameSlot.store(std::move(slot));
                }
                last = step;
            }
        });
    }

    if (settings.transient.enabled && settings.transient.writeInterval > 0) {
        m_diskWriterRunning.store(true);
        auto meshP = meshPtr;

        // One PVD path for the whole run; default next to the case/mesh. Carry
        // forward the entries of an existing .pvd so a continued run appends
        // instead of overwriting the earlier frames.
        const QString pvdPath = resolvePvdPath(
            QString::fromStdString(settings.transient.pvdFile), baseDir);
        QString baseName = pvdPath;
        if (baseName.endsWith(".pvd", Qt::CaseInsensitive))
            baseName.chop(4);
        const std::vector<PvdEntry> existing = readExistingPvd(pvdPath);
        const int wi = settings.transient.writeInterval;

        m_diskWriterThread = std::thread([this, gen, meshP, baseName, pvdPath, existing, wi]() {
            std::vector<PvdEntry> entries = existing;
            int lastSeen = -1;
            const std::string ext = meshP->isUnstructured()
                ? ".vtu" : (meshP->isUniform() ? ".vti" : ".vtr");
            while (m_diskWriterRunning.load() && gen == m_gen && !m_cancel.load()) {
                auto stepPtr = m_latestStep.load();
                if (!stepPtr || stepPtr->gen != gen) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                const int step = stepPtr->step;
                if (step == lastSeen) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                lastSeen = step;
                if (step % wi != 0) continue;

                const std::string fname =
                    (baseName + "_" + QString::number(step)).toStdString() + ext;
                std::array<const fvm::Field<2>*, 2> velPtrs{};
                for (std::size_t d = 0; d < 2; ++d) velPtrs[d] = &stepPtr->velocity->operator[](d);
                fvm::VTKXMLWriter<2>::writeWithVelocity(fname, *meshP, *stepPtr->pressure, velPtrs, "Pressure");
                entries.push_back({std::to_string(stepPtr->time), fname});
                writePvd(pvdPath.toStdString(), entries);
                std::cout << "  [transient] VTK written to: " << fname << " (pvd updated)\n";

                const fvm::SSTModel<2>* turb = nullptr;
                {
                    std::lock_guard<std::mutex> lk(m_solverMutex);
                    if (m_solver) turb = m_solver->turbulence();
                }
                if (turb) {
                    std::string turbFile = baseName.toStdString() + "_turb_"
                        + std::to_string(step) + ext;
                    fvm::VTKXMLWriter<2>::writeTurbulence(turbFile, *meshP, turb->k(), turb->omega(), turb->nut());
                }
            }
        });
    }

    auto* w = new QFutureWatcher<void>(this);
    m_solveWatcher = w;
    connect(w, &QFutureWatcher<void>::finished, this, [this, w, gen, solver, settings]() {
        if (m_solveWatcher == w) m_solveWatcher = nullptr;
        w->deleteLater();
        m_renderPrepRunning.store(false);
        if (m_renderPrepThread.joinable()) m_renderPrepThread.join();
        m_diskWriterRunning.store(false);
        if (m_diskWriterThread.joinable()) m_diskWriterThread.join();
        m_running.store(false);
        if (gen != m_gen) return;

        // The solve is no longer writing: snapshot the final state so Save and
        // a subsequent Run both see it.
        captureLiveState();
        m_lastTime.store(solver->currentTime());

        const fvm::SIMPLE<2>::Status st = solver->status();
        if (st == fvm::SIMPLE<2>::Status::Cancelled) {
            emit finished("Cancelled");
            return;
        }
        if (st == fvm::SIMPLE<2>::Status::Aborted) {
            emit finished("Solve aborted: the residual diverged or went non-finite");
            return;
        }
        if (settings.transient.enabled) {
            if (st == fvm::SIMPLE<2>::Status::Completed) {
                emit finished(QString("Transient run complete (t = %1 s, step %2)")
                                  .arg(solver->currentTime(), 0, 'g', 8)
                                  .arg(solver->currentStep()));
            } else {
                emit finished(QString("Transient run stopped before the end time (t = %1 s)")
                                  .arg(solver->currentTime(), 0, 'g', 8));
            }
            return;
        }
        // Steady: report the VERDICT, not just "the run ended".
        const double cont = solver->finalContinuityResidual();
        const int iters = solver->lastIterations();
        switch (st) {
        case fvm::SIMPLE<2>::Status::Converged:
            emit finished(QString("Converged in %1 iterations (continuity %2)")
                              .arg(iters).arg(cont, 0, 'g', 4));
            break;
        case fvm::SIMPLE<2>::Status::NotConverged:
            emit finished(QString("NOT converged: iteration limit (%1) reached, continuity %2")
                              .arg(iters).arg(cont, 0, 'g', 4));
            break;
        default:
            emit finished("Solve finished");
            break;
        }
    });

    auto velocity = m_velocity;
    auto pressure = m_pressure;
    auto fut = QtConcurrent::run([this, gen, solver, settings, velocity, pressure]() {
        if (gen != m_gen) return;
        if (settings.transient.enabled)
            solver->solveTransient(*velocity, *pressure);
        else
            solver->solve(*velocity, *pressure);
    });
    m_running.store(true);
    w->setFuture(fut);
    return true;
}

std::shared_ptr<const RenderMesh> CfdController::initialize(double ux, double uy, double p,
                                                            double k, double omega) {
    if (m_running.load()) {
        setError("Stop the current run before initializing.");
        return nullptr;
    }
    stopWorkers();
    if (!m_meshPtr || !m_mesh.renderMesh) {
        setError("No mesh loaded.");
        return nullptr;
    }

    resetSolutionState();
    m_velocity = std::make_shared<fvm::VectorField<2>>(*m_meshPtr, 0.0);
    for (double& val : (*m_velocity)[0].values()) val = ux;
    for (double& val : (*m_velocity)[1].values()) val = uy;
    m_pressure = std::make_shared<fvm::Field<2>>(*m_meshPtr, p);
    m_hasSolution = true;

    const fvm::SSTModel<2>* turb = nullptr;
    if (m_settings.turbulence) {
        fvm::SSTModel<2>::Config sstCfg{};
        sstCfg.kInf = k;
        sstCfg.omegaInf = omega;
        sstCfg.alphaK = m_settings.alphaK;
        sstCfg.alphaOmega = m_settings.alphaOmega;
        auto model = std::make_shared<fvm::SSTModel<2>>(*m_meshPtr, sstCfg);
        model->initialize(m_settings.nu * m_settings.rho, m_settings.rho);
        const std::size_t n = m_meshPtr->nCells();
        m_transient.hasTurbulence = true;
        m_transient.k.resize(n);
        m_transient.omega.resize(n);
        m_transient.nut.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            m_transient.k[i] = model->k()[i];
            m_transient.omega[i] = model->omega()[i];
            m_transient.nut[i] = model->nut()[i];
        }
        m_turbulence = std::move(model);
        turb = m_turbulence.get();
    }

    return cfd::buildResultMesh(m_mesh.renderMesh, *m_meshPtr, *m_velocity, *m_pressure,
                                turb, m_settings, m_patchBCs);
}

void CfdController::cancel() {
    // Runs on the GUI thread: only signal. Joining here would freeze the UI for
    // however long the current frame build / VTK write takes; the solve-future
    // continuation (and run()/initialize()/the destructor) own the joins.
    m_cancel.store(true);
    m_renderPrepRunning.store(false);
    m_diskWriterRunning.store(false);
    requestSolverCancel();
}

void CfdController::pause() {
    std::shared_ptr<fvm::SIMPLE<2>> solver;
    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        solver = m_solver;
    }
    if (solver) solver->requestPause();
}

void CfdController::resume() {
    std::shared_ptr<fvm::SIMPLE<2>> solver;
    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        solver = m_solver;
    }
    if (solver) solver->requestResume();
}

void CfdController::step() {
    std::shared_ptr<fvm::SIMPLE<2>> solver;
    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        solver = m_solver;
    }
    if (solver) solver->requestStep();
}

bool CfdController::isPaused() const {
    std::shared_ptr<fvm::SIMPLE<2>> solver;
    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        solver = m_solver;
    }
    return solver && solver->isPaused();
}

bool CfdController::isPausedSettled() const {
    std::shared_ptr<fvm::SIMPLE<2>> solver;
    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        solver = m_solver;
    }
    return solver && solver->isPauseSettled();
}

int CfdController::currentStep() const {
    if (m_running.load()) {
        if (isPausedSettled()) {
            std::shared_ptr<fvm::SIMPLE<2>> solver;
            {
                std::lock_guard<std::mutex> lk(m_solverMutex);
                solver = m_solver;
            }
            if (solver) return solver->currentStep();
        }
        return latestStep();
    }
    return m_transient.valid ? m_transient.step : 0;
}

double CfdController::currentTime() const {
    if (m_running.load()) {
        if (isPausedSettled()) {
            std::shared_ptr<fvm::SIMPLE<2>> solver;
            {
                std::lock_guard<std::mutex> lk(m_solverMutex);
                solver = m_solver;
            }
            if (solver) return solver->currentTime();
        }
        return latestTime();
    }
    return m_transient.valid ? m_transient.time : 0.0;
}

void CfdController::exportVtu(const QString& path) {
    m_lastError.clear();
    if (!m_hasSolution || !m_meshPtr || !m_velocity || !m_pressure) {
        setError("No solution to export. Run the solver first.");
        return;
    }
    if (m_running.load() && !(isPaused() && isPausedSettled())) {
        setError("Pause the run before exporting.");
        return;
    }
    std::array<const fvm::Field<2>*, 2> vel{&m_velocity->operator[](0), &m_velocity->operator[](1)};
    const fvm::SSTModel<2>* turb = nullptr;
    if (solverMatchesCurrentMesh()) {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        if (m_solver) turb = m_solver->turbulence();
    }
    if (!turb && m_turbulence) turb = m_turbulence.get();
    const fvm::Field<2>* k = turb ? &turb->k() : nullptr;
    const fvm::Field<2>* om = turb ? &turb->omega() : nullptr;
    const fvm::Field<2>* nt = turb ? &turb->nut() : nullptr;
    fvm::VTKXMLWriter<2>::writeWithVelocity(path.toStdString(), *m_meshPtr, *m_pressure, vel, "pressure", k, om, nt);
}

// --- Case I/O -------------------------------------------------------------

bool CfdController::saveCase(const QString& path) {
    m_lastError.clear();
    if (!m_meshPtr || !m_mesh.renderMesh) {
        setError("No mesh loaded.");
        return false;
    }
    if (m_running.load() && !(isPaused() && isPausedSettled())) {
        setError("Pause the run before saving. A Save is allowed once the solver has "
                 "stopped at a step boundary.");
        return false;
    }

    // A save while paused must capture the solver's current state, otherwise
    // the file would contain the state from the start of the run.
    captureLiveState();

    QByteArray meshBytes = m_embeddedMesh;
    QString meshName = m_meshName;
    if (meshBytes.isEmpty()) {
        if (m_meshPath.isEmpty()) {
            setError("The mesh has no source file to embed.");
            return false;
        }
        QFile meshFile(m_meshPath);
        if (!meshFile.open(QIODevice::ReadOnly)) {
            setError("Cannot read the mesh file.");
            return false;
        }
        meshBytes = meshFile.readAll();
        if (meshName.isEmpty()) meshName = QFileInfo(m_meshPath).fileName();
    }
    if (meshName.isEmpty()) meshName = "mesh";

    const QByteArray settingsBytes = cfd::serializeCaseSettings(m_settings, m_patchBCs);

    const std::size_t n = m_meshPtr->nCells();
    const bool fieldsMatch = m_velocity && m_pressure &&
                             m_velocity->operator[](0).size() == n &&
                             m_velocity->operator[](1).size() == n &&
                             m_pressure->size() == n;
    const bool hasResult = m_hasSolution && fieldsMatch;
    const bool hasTurb = hasResult && m_settings.turbulence && m_transient.hasTurbulence &&
                         m_transient.k.size() == n && m_transient.omega.size() == n &&
                         m_transient.nut.size() == n;

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        setError("Cannot open the case file for writing.");
        return false;
    }
    QDataStream ds(&out);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << QString("SCFD");
    ds << kScfdVersion;
    ds << meshName;
    ds << (qint64)meshBytes.size();
    ds.writeRawData(meshBytes.constData(), meshBytes.size());
    ds << (qint64)settingsBytes.size();
    ds.writeRawData(settingsBytes.constData(), settingsBytes.size());
    ds << hasResult;
    if (hasResult) {
        ds << (qint64)n;
        for (std::size_t d = 0; d < 2; ++d)
            for (std::size_t i = 0; i < n; ++i) ds << m_velocity->operator[](d)[i];
        for (std::size_t i = 0; i < n; ++i) ds << m_pressure->operator[](i);
        ds << hasTurb;
        if (hasTurb) {
            for (std::size_t i = 0; i < n; ++i) ds << m_transient.k[i];
            for (std::size_t i = 0; i < n; ++i) ds << m_transient.omega[i];
            for (std::size_t i = 0; i < n; ++i) ds << m_transient.nut[i];
        }
    }

    // Only a transient case carries a transient clock. If the user switched the
    // mode after a transient run, do not persist a clock the settings deny.
    const bool hasTransientState = m_settings.transient.enabled && m_transient.valid;
    ds << hasTransientState;
    if (hasTransientState) {
        ds << m_transient.time;
        ds << m_transient.dt;
        ds << m_transient.dtPrev;
        ds << (qint32)m_transient.step;
        const bool hasTurbHistory = m_transient.hasTurbulenceHistory &&
                                    m_transient.kNM1.size() == n &&
                                    m_transient.omegaNM1.size() == n;
        ds << hasTurbHistory;
        if (hasTurbHistory) {
            for (std::size_t i = 0; i < n; ++i) ds << m_transient.kNM1[i];
            for (std::size_t i = 0; i < n; ++i) ds << m_transient.omegaNM1[i];
        }
        const bool hasVelocityHistory = m_transient.hasVelocityHistory &&
                                        m_transient.velocityNM1[0].size() == n &&
                                        m_transient.velocityNM1[1].size() == n;
        ds << hasVelocityHistory;
        if (hasVelocityHistory) {
            for (std::size_t d = 0; d < 2; ++d)
                for (std::size_t i = 0; i < n; ++i) ds << m_transient.velocityNM1[d][i];
        }
    }

    if (ds.status() != QDataStream::Ok || !out.commit()) {
        setError("Failed to write the case file.");
        return false;
    }

    // Remember the embedded source so a later save does not lose it (and does
    // not fall back to a temporary extraction path).
    m_embeddedMesh = meshBytes;
    m_meshName = meshName;
    m_casePath = path;
    m_caseDir = QFileInfo(path).absolutePath();
    return true;
}

bool CfdController::loadCase(const QString& path) {
    m_lastError.clear();
    if (m_running.load()) {
        setError("Stop the current run before loading a case.");
        return false;
    }

    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        setError("Cannot open the case file.");
        return false;
    }
    QDataStream ds(&in);
    ds.setByteOrder(QDataStream::LittleEndian);

    QString magic;
    qint32 version = 0;
    ds >> magic >> version;
    if (ds.status() != QDataStream::Ok || magic != "SCFD") {
        setError("Not a SciRender CFD case file.");
        return false;
    }
    if (version != kScfdVersion) {
        setError(QString("Unsupported case format version %1 (this build reads version %2). "
                         "Load the mesh and re-create the case.")
                     .arg(version).arg(kScfdVersion));
        return false;
    }

    QString meshName;
    ds >> meshName;
    qint64 meshSz = 0;
    ds >> meshSz;
    if (ds.status() != QDataStream::Ok || meshSz <= 0 || meshSz > kMaxMeshBytes) {
        setError("Corrupt case file (invalid mesh payload).");
        return false;
    }
    QByteArray meshBytes;
    meshBytes.resize((int)meshSz);
    if (ds.readRawData(meshBytes.data(), meshSz) != meshSz) {
        setError("Corrupt case file (truncated mesh payload).");
        return false;
    }

    qint64 settingsSz = 0;
    ds >> settingsSz;
    if (ds.status() != QDataStream::Ok || settingsSz < 0 || settingsSz > kMaxSettingsBytes) {
        setError("Corrupt case file (invalid settings payload).");
        return false;
    }
    QByteArray settingsBytes;
    settingsBytes.resize((int)settingsSz);
    if (ds.readRawData(settingsBytes.data(), settingsSz) != settingsSz) {
        setError("Corrupt case file (truncated settings payload).");
        return false;
    }

    cfd::CaseSettings newSettings;
    std::vector<cfd::PatchBC> newBCs;
    if (!cfd::deserializeCaseSettings(settingsBytes, newSettings, newBCs)) {
        setError("Corrupt case file (invalid settings).");
        return false;
    }

    // Materialise the embedded mesh under a controller-owned temporary
    // directory. The .scfd bytes stay in m_embeddedMesh, so the display name is
    // never replaced by this random file name.
    auto tempDir = std::make_unique<QTemporaryDir>();
    if (!tempDir->isValid()) {
        setError("Cannot create a temporary directory for the embedded mesh.");
        return false;
    }
    QString ext = meshExtension(meshName, meshBytes);
    const QString tempPath = tempDir->filePath("mesh." + ext);
    {
        QFile mf(tempPath);
        if (!mf.open(QIODevice::WriteOnly) || mf.write(meshBytes) != meshBytes.size()) {
            setError("Cannot write the embedded mesh.");
            return false;
        }
    }
    cfd::MeshLoadResult newMesh = cfd::loadMesh(tempPath.toStdString());
    if (!newMesh.error.empty() || !newMesh.mesh2 || !newMesh.renderMesh) {
        setError(QString::fromStdString(newMesh.error.empty()
                     ? "The embedded mesh could not be loaded." : newMesh.error));
        return false;
    }

    const std::size_t nCells = newMesh.mesh2->nCells();

    // Result fields.
    auto vel = std::make_shared<fvm::VectorField<2>>(*newMesh.mesh2, 0.0);
    auto pres = std::make_shared<fvm::Field<2>>(*newMesh.mesh2, 0.0);
    std::shared_ptr<fvm::SSTModel<2>> newTurbulence;
    bool hasResult = false;
    ds >> hasResult;
    if (ds.status() != QDataStream::Ok) {
        setError("Corrupt case file (missing result block).");
        return false;
    }
    if (hasResult) {
        qint64 n = 0;
        ds >> n;
        if (ds.status() != QDataStream::Ok || n != (qint64)nCells) {
            setError(QString("The case fields do not match the mesh (%1 cells in the file, "
                             "%2 in the mesh).").arg(n).arg(nCells));
            return false;
        }
        for (std::size_t d = 0; d < 2; ++d)
            for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; (*vel)[d][i] = v; }
        for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; (*pres)[i] = v; }

        bool hasTurb = false;
        ds >> hasTurb;
        if (ds.status() != QDataStream::Ok) {
            setError("Corrupt case file (truncated result block).");
            return false;
        }
        if (hasTurb) {
            auto model = std::make_shared<fvm::SSTModel<2>>(*newMesh.mesh2, fvm::SSTModel<2>::Config{});
            model->initialize(newSettings.nu * newSettings.rho, newSettings.rho);
            for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; model->k()[i] = v; }
            for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; model->omega()[i] = v; }
            for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; model->nut()[i] = v; }
            if (ds.status() != QDataStream::Ok) {
                setError("Corrupt case file (truncated turbulence block).");
                return false;
            }
            newTurbulence = std::move(model);
        }
    }

    // Transient state.
    TransientSnapshot snap;
    bool hasTransientState = false;
    ds >> hasTransientState;
    if (ds.status() != QDataStream::Ok) {
        setError("Corrupt case file (missing transient block).");
        return false;
    }
    if (hasTransientState) {
        qint32 step = 0;
        double t = 0.0, dt = 0.0, dtPrev = 0.0;
        ds >> t >> dt >> dtPrev >> step;
        bool hasTurbHistory = false;
        ds >> hasTurbHistory;
        if (hasTurbHistory) {
            snap.kNM1.resize(nCells);
            snap.omegaNM1.resize(nCells);
            for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; snap.kNM1[i] = v; }
            for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; snap.omegaNM1[i] = v; }
            snap.hasTurbulenceHistory = true;
        }
        bool hasVelocityHistory = false;
        ds >> hasVelocityHistory;
        if (hasVelocityHistory) {
            for (std::size_t d = 0; d < 2; ++d) snap.velocityNM1[d].resize(nCells);
            for (std::size_t d = 0; d < 2; ++d)
                for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; snap.velocityNM1[d][i] = v; }
            snap.hasVelocityHistory = true;
        }
        if (ds.status() != QDataStream::Ok) {
            setError("Corrupt case file (truncated transient state).");
            return false;
        }
        snap.valid = hasResult;
        snap.time = t;
        snap.dt = dt;
        snap.dtPrev = dtPrev;
        snap.step = step;
    }

    // --- Commit (nothing above mutated the controller) --------------------
    stopWorkers();
    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        m_solver.reset();
        m_solverMesh.reset();
    }
    ++m_gen;

    m_mesh = std::move(newMesh);
    m_meshPtr = m_mesh.mesh2;
    m_meshPath = tempPath;
    m_meshName = meshName.isEmpty() ? QFileInfo(path).fileName() : meshName;
    m_embeddedMesh = meshBytes;
    m_casePath = path;
    m_caseDir = QFileInfo(path).absolutePath();
    m_tempDir = std::move(tempDir);

    m_settings = std::move(newSettings);
    m_patchBCs = std::move(newBCs);
    if (m_patchBCs.empty()) {
        for (const auto& patch : m_mesh.patches) {
            cfd::PatchBC pbc;
            pbc.patchName = patch.name;
            pbc.type = cfd::BCType::WallNoSlip;
            m_patchBCs.push_back(pbc);
        }
    }

    m_velocity = std::move(vel);
    m_pressure = std::move(pres);
    // A settings-only case carries zeros, not a solution: do not pretend the
    // zero fields are a result (Export would otherwise write a bogus field).
    m_hasSolution = hasResult;

    m_turbulence = std::move(newTurbulence);
    snap.hasTurbulence = (m_turbulence && m_settings.turbulence);
    if (snap.hasTurbulence) {
        snap.k.resize(nCells);
        snap.omega.resize(nCells);
        snap.nut.resize(nCells);
        for (std::size_t i = 0; i < nCells; ++i) {
            snap.k[i] = m_turbulence->k()[i];
            snap.omega[i] = m_turbulence->omega()[i];
            snap.nut[i] = m_turbulence->nut()[i];
        }
    } else {
        m_turbulence.reset();
        snap.k.clear();
        snap.omega.clear();
        snap.nut.clear();
    }
    m_transient = std::move(snap);

    m_lastTime.store(m_transient.valid ? m_transient.time : 0.0);
    m_latestStep.store(nullptr);
    m_latestFrameSlot.store(nullptr);
    m_cancel.store(false);
    m_running.store(false);

    emit caseLoaded();
    return true;
}

std::shared_ptr<const RenderMesh> CfdController::resultMesh() const {
    if (!m_meshPtr || !m_hasSolution || !m_velocity || !m_pressure || !m_mesh.renderMesh)
        return nullptr;
    const fvm::SSTModel<2>* turb = nullptr;
    if (solverMatchesCurrentMesh()) {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        if (m_solver) turb = m_solver->turbulence();
    }
    if (!turb && m_turbulence) turb = m_turbulence.get();
    return buildResultMesh(m_mesh.renderMesh, *m_meshPtr, *m_velocity, *m_pressure,
                           turb, m_settings, m_patchBCs);
}
