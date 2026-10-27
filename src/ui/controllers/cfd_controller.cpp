#include "ui/controllers/cfd_controller.h"

#include "cfd/case_codec.h"
#include "cfd/cfd_mesh_adapter.h"
#include "cfd/cfd_session.h"
#include "cfd/solution_state.h"
#include "cfd/transient_frame.h"

#include "mesh/Mesh.h"

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

#include <memory>
#include <fstream>
#include <iostream>

#include <omp.h>
#include <Eigen/Core>

using namespace cfd;

namespace {

constexpr qint64 kMaxMeshBytes = 1024LL * 1024 * 1024; // 1 GiB
constexpr qint64 kMaxSettingsBytes = 16LL * 1024 * 1024;
constexpr qint32 kScfdVersion = 7;
constexpr qint32 kScfdVersionReadMin = 5; // v5 files are read through a shim

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
    if (m_session) m_session->requestCancel();
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

void CfdController::resetSolutionState() {
    // Bump the generation first: any in-flight worker that still holds the old
    // generator must not publish into the new state.
    ++m_gen;
    stopWorkers();
    if (m_session) m_session->clear();
    m_state.clear();
    m_lastTime.store(0.0);
    m_latestStep.store(nullptr);
    m_latestFrameSlot.store(nullptr);
}

void CfdController::setError(const QString& msg) {
    m_lastError = msg;
    emit failed(msg);
}

void CfdController::captureLiveState() {
    if (!m_session) return;
    cfd::SolutionSnapshot live = m_session->capture();
    if (live.hasFields()) m_state = std::move(live);
    m_lastTime.store(m_state.transientValid ? m_state.time : 0.0);
}

bool CfdController::loadMesh(const QString& path) {
    m_lastError.clear();
    if (m_running.load()) {
        setError("Stop the current run before loading a mesh.");
        return false;
    }
    auto res = cfd::loadMesh(path.toStdString());
    if (!res.error.empty() || (!res.mesh2 && !res.mesh3)) {
        setError(QString::fromStdString(res.error.empty() ? "Unsupported mesh" : res.error));
        return false;
    }

    resetSolutionState();
    m_mesh = std::move(res);
    m_session = cfd::makeSession(m_mesh);
    if (!m_session) {
        m_mesh = cfd::MeshLoadResult{};
        setError("Unsupported mesh.");
        return false;
    }
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
    m_monitorHistory.clear();
    m_lastError.clear();
    if (m_running.load()) {
        setError("A solve is already running.");
        return false;
    }
    if (!m_session || !m_mesh.renderMesh) {
        setError("No mesh loaded.");
        return false;
    }

    // Join the previous run's helper threads, then keep its final state (the
    // session still owns it) before starting a new solver.
    stopWorkers();
    captureLiveState();

    m_cancel.store(false);
    m_latestStep.store(nullptr);
    m_latestFrameSlot.store(nullptr);

    const uint64_t gen = ++m_gen;

    auto settings = m_settings;
    auto base = m_mesh.renderMesh;
    auto patchBCs = m_patchBCs;
    auto* session = m_session.get();

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

    cfd::RunHooks hooks;
    hooks.iteration = [this, gen](int iter, double cont, double mom) {
        if (gen != m_gen) return;
        if (m_cancel.load()) return;
        emit progress(iter, cont, mom);
        if (m_session) {
            auto moms = m_session->finalMomentumResiduals();
            int dim = m_session->dim();
            bool hasTurb = m_session->hasTurbulence();
            double k = m_session->finalKResidual();
            double omega = m_session->finalOmegaResidual();
            {
                std::lock_guard<std::mutex> lock(m_monitorMutex);
                MonitorPoint pt;
                pt.iteration = iter;
                pt.time = currentTime();
                pt.continuity = cont;
                pt.momU = moms[0]; pt.momV = moms[1]; pt.momW = moms[2];
                pt.k = k; pt.omega = omega;
                pt.hasTurbulence = hasTurb;
                pt.dim = dim;
                m_monitorHistory.push_back(pt);
            }
            emit residualsUpdated(iter, dim, cont,
                                 moms[0], moms[1], moms[2],
                                 k, omega, hasTurb);
        }
    };
    // Frame bus (Phase 2): the solver publishes a self-contained snapshot each
    // time step; the render-prep thread builds the mesh off the UI thread.
    if (settings.transient.enabled) {
        hooks.frame = [this, gen](const cfd::SolutionSnapshot& snapshot) {
            if (gen != m_gen) return;
            if (m_cancel.load()) return;
            m_lastTime.store(snapshot.time);
            auto step = std::make_shared<TransientStep>();
            step->step = snapshot.step;
            step->time = snapshot.time;
            step->state = std::make_shared<const cfd::SolutionSnapshot>(snapshot);
            step->gen = gen;
            m_latestStep.store(std::shared_ptr<const TransientStep>(step));
        };
    }

    std::string startError;
    if (!session->startRun(settings, patchBCs, m_state, hooks, startError)) {
        setError(QString::fromStdString(startError.empty()
                     ? "The run could not be started." : startError));
        return false;
    }

    m_lastTime.store(settings.transient.enabled && m_state.transientValid
                         ? m_state.time : 0.0);

    if (settings.transient.enabled) {
        m_renderPrepRunning.store(true);
        m_renderPrepThread = std::thread([this, gen, session, base, settings, patchBCs]() {
            std::shared_ptr<const TransientStep> last;
            while (m_renderPrepRunning.load() && gen == m_gen && !m_cancel.load()) {
                auto step = m_latestStep.load();
                if (!step || step == last || step->gen != gen) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (m_cancel.load() || gen != m_gen) break;
                auto rm = session->buildResult(base, *step->state, settings, patchBCs);
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

        m_diskWriterThread = std::thread([this, gen, session, baseName, pvdPath,
                                          existing, wi]() {
            std::vector<PvdEntry> entries = existing;
            int lastSeen = -1;
            const std::string ext = session->outputExtension();
            while (m_diskWriterRunning.load() && gen == m_gen && !m_cancel.load()) {
                auto step = m_latestStep.load();
                if (!step || step->gen != gen) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                const int stepIndex = step->step;
                if (stepIndex == lastSeen) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                lastSeen = stepIndex;
                if (stepIndex % wi != 0) continue;

                const std::string fname =
                    (baseName + "_" + QString::number(stepIndex)).toStdString() + ext;
                session->exportVtu(fname, *step->state);
                entries.push_back({std::to_string(step->time), fname});
                writePvd(pvdPath.toStdString(), entries);
                std::cout << "  [transient] VTK written to: " << fname << " (pvd updated)\n";

                if (step->state->hasTurbulence()) {
                    std::string turbFile = baseName.toStdString() + "_turb_"
                        + std::to_string(stepIndex) + ext;
                    session->exportTurbulence(turbFile, *step->state);
                }
            }
        });
    }

    auto* w = new QFutureWatcher<void>(this);
    m_solveWatcher = w;
    connect(w, &QFutureWatcher<void>::finished, this,
            [this, w, gen, session, settings]() {
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
        m_lastTime.store(session->currentTime());

        const cfd::SessionStatus st = session->status();
        if (st == cfd::SessionStatus::Cancelled) {
            emit finished("Cancelled");
            return;
        }
        if (st == cfd::SessionStatus::Aborted) {
            emit finished("Solve aborted: the residual diverged or went non-finite");
            return;
        }
        if (settings.transient.enabled) {
            if (st == cfd::SessionStatus::Completed) {
                emit finished(QString("Transient run complete (t = %1 s, step %2)")
                                  .arg(session->currentTime(), 0, 'g', 8)
                                  .arg(session->currentStep()));
            } else {
                emit finished(QString("Transient run stopped before the end time (t = %1 s)")
                                  .arg(session->currentTime(), 0, 'g', 8));
            }
            return;
        }
        // Steady: report the VERDICT, not just "the run ended".
        const double cont = session->finalContinuityResidual();
        const int iters = session->lastIterations();
        switch (st) {
        case cfd::SessionStatus::Converged:
            emit finished(QString("Converged in %1 iterations (continuity %2)")
                              .arg(iters).arg(cont, 0, 'g', 4));
            break;
        case cfd::SessionStatus::NotConverged:
            emit finished(QString("NOT converged: iteration limit (%1) reached, continuity %2")
                              .arg(iters).arg(cont, 0, 'g', 4));
            break;
        default:
            emit finished("Solve finished");
            break;
        }
    });

    auto fut = QtConcurrent::run([this, gen, session]() {
        if (gen != m_gen) return;
        session->solve();
    });
    m_running.store(true);
    w->setFuture(fut);
    return true;
}

std::shared_ptr<const RenderMesh> CfdController::initialize(double ux, double uy, double p,
                                                           double k, double omega) {
    m_monitorHistory.clear();
    cfd::InitConditions ic;
    ic.ux = ux;
    ic.uy = uy;
    ic.uz = 0.0;
    ic.pressure = p;
    ic.k = k;
    ic.omega = omega;
    return initialize(ic);
}

std::shared_ptr<const RenderMesh> CfdController::initialize(const cfd::InitConditions& ic) {
    m_monitorHistory.clear();
    if (m_running.load()) {
        setError("Stop the current run before initializing.");
        return nullptr;
    }
    if (!m_session || !m_mesh.renderMesh) {
        setError("No mesh loaded.");
        return nullptr;
    }
    stopWorkers();

    resetSolutionState();
    m_state = m_session->initialize(ic, m_settings);
    return m_session->buildResult(m_mesh.renderMesh, m_state, m_settings, m_patchBCs);
}

void CfdController::cancel() {
    // Runs on the GUI thread: only signal. Joining here would freeze the UI for
    // however long the current frame build / VTK write takes; the solve-future
    // continuation (and run()/initialize()/the destructor) own the joins.
    m_cancel.store(true);
    m_renderPrepRunning.store(false);
    m_diskWriterRunning.store(false);
    if (m_session) m_session->requestCancel();
}

void CfdController::pause() {
    if (m_session) m_session->requestPause();
    emit pauseRequested();
}

void CfdController::resume() {
    if (m_session) m_session->requestResume();
    emit resumed();
}

void CfdController::step() {
    if (m_session) m_session->requestStep();
    emit stepRequested();
}

bool CfdController::isPaused() const {
    return m_session && m_session->isPaused();
}

bool CfdController::isPausedSettled() const {
    return m_session && m_session->isPausedSettled();
}

int CfdController::currentStep() const {
    if (m_running.load() && !isPausedSettled()) return latestStep();
    if (m_session) return m_session->currentStep();
    return m_state.transientValid ? m_state.step : 0;
}

double CfdController::currentTime() const {
    if (m_running.load() && !isPausedSettled()) return latestTime();
    if (m_session) return m_session->currentTime();
    return m_state.transientValid ? m_state.time : 0.0;
}

void CfdController::exportVtu(const QString& path) {
    m_lastError.clear();
    if (!m_session || !m_mesh.renderMesh) {
        setError("No mesh loaded.");
        return;
    }
    if (m_running.load() && !(isPaused() && isPausedSettled())) {
        setError("Pause the run before exporting.");
        return;
    }
    captureLiveState();
    if (!m_state.hasFields()) {
        setError("No solution to export. Run the solver first.");
        return;
    }
    try {
        m_session->exportVtu(path.toStdString(), m_state);
    } catch (const std::exception& e) {
        setError(QString::fromStdString(e.what()));
    }
}

// --- Case I/O -------------------------------------------------------------

bool CfdController::saveCase(const QString& path) {
    m_lastError.clear();
    if (!m_session || !m_mesh.renderMesh) {
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

    const int dim = m_session->dim();
    const std::size_t n = m_session->cells();
    const bool hasResult = m_state.hasFields() && m_state.dim == dim && m_state.cells == n;
    const bool hasTurb = hasResult && m_settings.turbulence && m_state.hasTurbulence();

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
    ds << (qint32)dim;
    ds << (qint64)meshBytes.size();
    ds.writeRawData(meshBytes.constData(), meshBytes.size());
    ds << (qint64)settingsBytes.size();
    ds.writeRawData(settingsBytes.constData(), settingsBytes.size());
    ds << hasResult;
    if (hasResult) {
        ds << (qint64)n;
        for (int d = 0; d < dim; ++d)
            for (std::size_t i = 0; i < n; ++i) ds << m_state.velocity[d][i];
        for (std::size_t i = 0; i < n; ++i) ds << m_state.pressure[i];
        ds << hasTurb;
        if (hasTurb) {
            for (std::size_t i = 0; i < n; ++i) ds << m_state.turbulence.k[i];
            for (std::size_t i = 0; i < n; ++i) ds << m_state.turbulence.omega[i];
            for (std::size_t i = 0; i < n; ++i) ds << m_state.turbulence.nut[i];
        }
    }

    // Only a transient case carries a transient clock. If the user switched the
    // mode after a transient run, do not persist a clock the settings deny.
    const bool hasTransientState = m_settings.transient.enabled && m_state.transientValid;
    ds << hasTransientState;
    if (hasTransientState) {
        ds << m_state.time;
        ds << m_state.dt;
        ds << m_state.dtPrev;
        ds << (qint32)m_state.step;
        const bool hasTurbHistory = m_state.hasTurbulenceHistory();
        ds << hasTurbHistory;
        if (hasTurbHistory) {
            for (std::size_t i = 0; i < n; ++i) ds << m_state.turbulence.kNM1[i];
            for (std::size_t i = 0; i < n; ++i) ds << m_state.turbulence.omegaNM1[i];
        }
        const bool hasVelocityHistory = m_state.hasVelocityHistorySized();
        ds << hasVelocityHistory;
        if (hasVelocityHistory) {
            for (int d = 0; d < dim; ++d)
                for (std::size_t i = 0; i < n; ++i) ds << m_state.velocityNM1[d][i];
        }
    }

    // Monitor/residual history.
    {
        std::lock_guard<std::mutex> lock(m_monitorMutex);
        const bool hasMon = !m_monitorHistory.empty();
        ds << hasMon;
        if (hasMon) {
            ds << (qint64)m_monitorHistory.size();
            ds << (qint32)m_monitorHistory.front().dim;
            for (const auto& pt : m_monitorHistory) {
                ds << (qint32)pt.iteration;
                ds << pt.time;
                ds << pt.continuity;
                ds << pt.momU; ds << pt.momV; ds << pt.momW;
                ds << pt.k; ds << pt.omega;
                ds << pt.hasTurbulence;
            }
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
    if (version < kScfdVersionReadMin || version > kScfdVersion) {
        setError(QString("Unsupported case format version %1 (this build reads versions %2 to %3). "
                         "Load the mesh and re-create the case.")
                     .arg(version).arg(kScfdVersionReadMin).arg(kScfdVersion));
        return false;
    }

    QString meshName;
    ds >> meshName;

    // v6 adds an explicit dimension. A v5 file predates 3D support, so it is
    // read as a 2D case; the mesh check below rejects a v5 file whose embedded
    // mesh turns out to be 3D.
    int fileDim = 2;
    if (version >= 6) {
        qint32 dimValue = 0;
        ds >> dimValue;
        if (ds.status() != QDataStream::Ok || (dimValue != 2 && dimValue != 3)) {
            setError("Corrupt case file (invalid dimension).");
            return false;
        }
        fileDim = dimValue;
    }

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
    if (!newMesh.error.empty() || (!newMesh.mesh2 && !newMesh.mesh3) || !newMesh.renderMesh) {
        setError(QString::fromStdString(newMesh.error.empty()
                     ? "The embedded mesh could not be loaded." : newMesh.error));
        return false;
    }

    // The case dimension must match the embedded mesh. This also rejects a v5
    // file whose embedded mesh is 3D (v5 implied 2D).
    const int meshDim = newMesh.mesh3 ? 3 : 2;
    if (fileDim != meshDim) {
        setError(QString("The case is %1D but its embedded mesh is %2D.")
                     .arg(fileDim).arg(meshDim));
        return false;
    }
    const std::size_t nCells = newMesh.mesh3 ? newMesh.mesh3->nCells()
                                             : newMesh.mesh2->nCells();

    cfd::SolutionSnapshot state;
    state.dim = fileDim;
    state.cells = nCells;

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
        state.velocity.assign(fileDim, std::vector<double>(nCells, 0.0));
        state.pressure.assign(nCells, 0.0);
        for (int d = 0; d < fileDim; ++d)
            for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; state.velocity[d][i] = v; }
        for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; state.pressure[i] = v; }

        bool hasTurb = false;
        ds >> hasTurb;
        if (ds.status() != QDataStream::Ok) {
            setError("Corrupt case file (truncated result block).");
            return false;
        }
        if (hasTurb) {
            state.turbulence.k.resize(nCells);
            state.turbulence.omega.resize(nCells);
            state.turbulence.nut.resize(nCells);
            for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; state.turbulence.k[i] = v; }
            for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; state.turbulence.omega[i] = v; }
            for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; state.turbulence.nut[i] = v; }
            if (ds.status() != QDataStream::Ok) {
                setError("Corrupt case file (truncated turbulence block).");
                return false;
            }
            state.turbulence.valid = newSettings.turbulence;
            if (!state.turbulence.valid) state.turbulence.clear();
        }
    }

    // Transient state.
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
            state.turbulence.kNM1.resize(nCells);
            state.turbulence.omegaNM1.resize(nCells);
            for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; state.turbulence.kNM1[i] = v; }
            for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; state.turbulence.omegaNM1[i] = v; }
            state.turbulence.hasHistory = true;
        }
        bool hasVelocityHistory = false;
        ds >> hasVelocityHistory;
        if (hasVelocityHistory) {
            state.velocityNM1.assign(fileDim, std::vector<double>(nCells, 0.0));
            for (int d = 0; d < fileDim; ++d)
                for (std::size_t i = 0; i < nCells; ++i) { double v; ds >> v; state.velocityNM1[d][i] = v; }
            state.hasVelocityHistory = true;
        }
        if (ds.status() != QDataStream::Ok) {
            setError("Corrupt case file (truncated transient state).");
            return false;
        }
        state.transientValid = hasResult;
        state.time = t;
        state.dt = dt;
        state.dtPrev = dtPrev;
        state.step = step;
    }
    std::vector<MonitorPoint> loadedMonitorHistory;
    if (version >= 7) {
        bool hasMonitorHistory = false;
        ds >> hasMonitorHistory;
        if (hasMonitorHistory) {
            qint64 nmon = 0;
            ds >> nmon;
            qint32 monDim = 0;
            ds >> monDim;
            if (ds.status() != QDataStream::Ok || nmon < 0) {
                setError("Corrupt case file (invalid monitor history).");
                return false;
            }
            loadedMonitorHistory.reserve((std::size_t)nmon);
            for (qint64 i = 0; i < nmon; ++i) {
                MonitorPoint pt;
                qint32 iter = 0;
                ds >> iter;
                pt.iteration = iter;
                ds >> pt.time >> pt.continuity >> pt.momU >> pt.momV >> pt.momW >> pt.k >> pt.omega;
                ds >> pt.hasTurbulence;
                pt.dim = monDim;
                loadedMonitorHistory.push_back(pt);
                if (ds.status() != QDataStream::Ok) {
                    setError("Corrupt case file (truncated monitor history).");
                    return false;
                }
            }
        }
    }

    if (!state.hasTurbulence()) {
        state.turbulence.clear();
    }

    // --- Commit (nothing above mutated the controller) --------------------
    stopWorkers();
    ++m_gen;

    m_mesh = std::move(newMesh);
    m_session = cfd::makeSession(m_mesh);
    if (!m_session) {
        setError("The embedded mesh could not be loaded.");
        return false;
    }
    std::string stateError;
    if (!m_session->setState(state, stateError)) {
        setError(QString::fromStdString(stateError));
        return false;
    }
    m_state = state.hasFields() ? state : cfd::SolutionSnapshot{};

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

    m_lastTime.store(m_state.transientValid ? m_state.time : 0.0);
    m_latestStep.store(nullptr);
    m_latestFrameSlot.store(nullptr);
    m_cancel.store(false);
    m_running.store(false);

    m_monitorHistory = std::move(loadedMonitorHistory);

    emit caseLoaded();
    return true;
}

std::shared_ptr<const RenderMesh> CfdController::resultMesh() const {
    if (!m_session || !m_mesh.renderMesh) return nullptr;
    cfd::SolutionSnapshot state = m_session->capture();
    if (!state.hasFields()) return nullptr;
    return m_session->buildResult(m_mesh.renderMesh, state, m_settings, m_patchBCs);
}
