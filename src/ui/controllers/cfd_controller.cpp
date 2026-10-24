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
#include <QFileInfo>
#include <QDataStream>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTemporaryFile>


#include <memory>
#include <mutex>

#include <omp.h>
#include <Eigen/Core>

using namespace cfd;

CfdController::CfdController(QObject* parent) : QObject(parent) {}

CfdController::~CfdController() {
    m_renderPrepRunning.store(false);
    if (m_renderPrepThread.joinable()) m_renderPrepThread.join();
    m_diskWriterRunning.store(false);
    if (m_diskWriterThread.joinable()) m_diskWriterThread.join();
}

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
    // Stop any previous render-prep thread.
    m_renderPrepRunning.store(false);
    if (m_renderPrepThread.joinable()) m_renderPrepThread.join();
    m_cancel.store(false);
    m_lastTime.store(0.0);
    m_latestStep.store(nullptr);
    m_latestFrameSlot.store(nullptr);
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

    fvm::SIMPLE<2>::Config sc = cfd::toFvmConfig<2>(settings);
    if (settings.turbulence) {
        for (const auto& pbc : m_patchBCs) {
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
        auto cfg = settings;
        // base name
        std::string baseName = cfg.transient.pvdFile;
        if (baseName.empty()) baseName = "transient";
        if (baseName.size() > 4 && baseName.substr(baseName.size()-4) == ".pvd")
            baseName = baseName.substr(0, baseName.size()-4);
        int wi = cfg.transient.writeInterval;
        m_diskWriterThread = std::thread([this, gen, meshP, cfg, baseName, wi]() {
            std::vector<std::string> entries;
            int lastSeen = -1;
            while (m_diskWriterRunning.load() && gen == m_gen && !m_cancel.load()) {
                auto stepPtr = m_latestStep.load();
                if (!stepPtr || stepPtr->gen != gen) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                int step = stepPtr->step;
                if (step == lastSeen) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                lastSeen = step;
                if (step % wi != 0) continue;
                // write VTK
                std::string ext = meshP->isUnstructured() ? ".vtu" : (meshP->isUniform() ? ".vti" : ".vtr");
                std::string fname = baseName + "_" + std::to_string(step) + ext;
                std::array<const fvm::Field<2>*, 2> velPtrs{};
                for (std::size_t d = 0; d < 2; ++d) velPtrs[d] = &stepPtr->velocity->operator[](d);
                fvm::VTKXMLWriter<2>::writeWithVelocity(fname, *meshP, *stepPtr->pressure, velPtrs, "Pressure");
                std::string entry = std::to_string(stepPtr->time) + "|" + fname;
                entries.push_back(entry);
                // update PVD file
                // derive dir
                std::string dir;
                const std::size_t slash = fname.find_last_of("/\\");
                if (slash != std::string::npos) dir = fname.substr(0, slash + 1);
                std::string pvdFile = dir + baseName + ".pvd";
                if (!cfg.transient.pvdFile.empty()) {
                    const std::string& want = cfg.transient.pvdFile;
                    const std::size_t wslash = want.find_last_of("/\\");
                    if (wslash != std::string::npos) pvdFile = want;
                    else pvdFile = dir + want;
                }
                std::ofstream out(pvdFile);
                if (out) {
                    out << "<?xml version=\"1.0\"?>\n";
                    out << "<VTKFile type=\"Collection\" version=\"0.1\">\n";
                    out << "  <Collection>\n";
                    for (auto& e : entries) {
                        auto pos = e.find('|');
                        std::string t = (pos == std::string::npos) ? "0" : e.substr(0, pos);
                        std::string f = (pos == std::string::npos) ? e : e.substr(pos+1);
                        out << "    <DataSet timestep=\"" << t << "\" file=\"" << f << "\"/>\n";
                    }
                    out << "  </Collection>\n";
                    out << "</VTKFile>\n";
                }
                std::cout << "  [transient] VTK written to: " << fname << " (pvd updated)\n";
                // turbulence
                const fvm::SSTModel<2>* turb = nullptr;
                {
                    std::lock_guard<std::mutex> lk(m_solverMutex);
                    if (m_solver) turb = m_solver->turbulence();
                }
                if (turb) {
                    std::string turbFile = baseName + "_turb_" + std::to_string(step) + ext;
                    fvm::VTKXMLWriter<2>::writeTurbulence(turbFile, *meshP, turb->k(), turb->omega(), turb->nut());
                }
            }
        });
    }

    QFutureWatcher<void>* w = new QFutureWatcher<void>(this);
    connect(w, &QFutureWatcher<void>::finished, this, [this, w, gen]() {
        w->deleteLater();
        m_renderPrepRunning.store(false);
        if (m_renderPrepThread.joinable()) m_renderPrepThread.join();
        m_diskWriterRunning.store(false);
        if (m_diskWriterThread.joinable()) m_diskWriterThread.join();
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
    m_renderPrepRunning.store(false);
    if (m_renderPrepThread.joinable()) m_renderPrepThread.join();
    m_diskWriterRunning.store(false);
    if (m_diskWriterThread.joinable()) m_diskWriterThread.join();
    if (!m_meshPtr || !m_mesh.renderMesh) return nullptr;
    // Invalidate any in-flight solve generation so its result cannot be applied
    // after this reset, then start from a uniform zero field.
    ++m_gen;
    m_velocity = std::make_shared<fvm::VectorField<2>>(*m_meshPtr, 0.0);
    m_pressure = std::make_shared<fvm::Field<2>>(*m_meshPtr, 0.0);
    return resultMesh();
}

void CfdController::cancel() {
    // Runs on the GUI thread: only signal. Joining here would freeze the UI for
    // however long the current frame build / VTK write takes; the solve-future
    // continuation (and run()/initialize()/the destructor) own the joins.
    m_cancel.store(true);
    m_renderPrepRunning.store(false);
    m_diskWriterRunning.store(false);
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
    const fvm::SSTModel<2>* turb = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        if (m_solver) turb = m_solver->turbulence();
    }
    const fvm::Field<2>* k = turb ? &turb->k() : nullptr;
    const fvm::Field<2>* om = turb ? &turb->omega() : nullptr;
    const fvm::Field<2>* nt = turb ? &turb->nut() : nullptr;
    fvm::VTKXMLWriter<2>::writeWithVelocity(path.toStdString(), *m_meshPtr, *m_pressure, vel, "pressure", k, om, nt);
}

// --- Case I/O -------------------------------------------------------------

bool CfdController::saveCase(const QString& path) {
    if (!m_meshPtr) return false;
    if (m_meshPath.isEmpty()) return false;
    QFile meshFile(m_meshPath);
    if (!meshFile.open(QIODevice::ReadOnly)) return false;
    const QByteArray meshBytes = meshFile.readAll();
    meshFile.close();
    const QString meshName = QFileInfo(m_meshPath).fileName();

    const QByteArray settings = cfd::serializeCaseSettings(m_settings, m_patchBCs);

    const bool hasResult = (m_velocity && m_pressure);
    const std::size_t n = hasResult ? m_meshPtr->nCells() : 0;
    const fvm::SSTModel<2>* turb = m_solver ? m_solver->turbulence() : nullptr;
    const bool hasTurb = hasResult && (turb != nullptr);

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) return false;
    QDataStream ds(&out);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << QString("SCFD");
    ds << (qint32)2;
    ds << meshName;
    ds << (qint64)meshBytes.size();
    ds.writeRawData(meshBytes.constData(), meshBytes.size());
    ds << (qint64)settings.size();
    ds.writeRawData(settings.constData(), settings.size());
    ds << hasResult;
    if (hasResult) {
        ds << (qint64)n;
        for (std::size_t d = 0; d < 2; ++d)
            for (std::size_t i = 0; i < n; ++i) ds << m_velocity->operator[](d)[i];
        for (std::size_t i = 0; i < n; ++i) ds << m_pressure->operator[](i);
        ds << hasTurb;
        if (hasTurb) {
            for (std::size_t i = 0; i < n; ++i) ds << turb->k()[i];
            for (std::size_t i = 0; i < n; ++i) ds << turb->omega()[i];
            for (std::size_t i = 0; i < n; ++i) ds << turb->nut()[i];
        }
    }
    out.close();
    return true;
}

bool CfdController::loadCase(const QString& path) {
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) return false;
    QDataStream ds(&in);
    ds.setByteOrder(QDataStream::LittleEndian);
    QString magic;
    qint32 version;
    ds >> magic >> version;
    if (magic != "SCFD" || version != 2) return false;

    QString meshName;
    ds >> meshName;
    qint64 meshSz;
    ds >> meshSz;
    if (meshSz < 0) return false;
    QByteArray meshBytes((int)meshSz, '\0');
    if (ds.readRawData(meshBytes.data(), (int)meshSz) != meshSz) return false;

    qint64 sz;
    ds >> sz;
    if (sz < 0) return false;
    QByteArray settings((int)sz, '\0');
    if (ds.readRawData(settings.data(), (int)sz) != sz) return false;

    bool hasResult;
    ds >> hasResult;

    QTemporaryFile tmp;
    if (!tmp.open()) return false;
    tmp.close();
    const QString ext = QFileInfo(meshName).suffix();
    QString tempPath = tmp.fileName();
    if (!ext.isEmpty()) tempPath += "." + ext;
    QFile mf(tempPath);
    if (!mf.open(QIODevice::WriteOnly)) return false;
    mf.write(meshBytes);
    mf.close();

    if (!loadMesh(tempPath)) return false;

    if (hasResult) {
        qint64 n;
        ds >> n;
        if (n < 0) return false;
        const std::size_t N = (std::size_t)n;
        m_velocity = std::make_shared<fvm::VectorField<2>>(*m_meshPtr, 0.0);
        m_pressure = std::make_shared<fvm::Field<2>>(*m_meshPtr, 0.0);
        for (std::size_t d = 0; d < 2; ++d)
            for (std::size_t i = 0; i < N && i < m_meshPtr->nCells(); ++i) {
                double v; ds >> v;
                m_velocity->operator[](d)[i] = v;
            }
        for (std::size_t i = 0; i < N && i < m_meshPtr->nCells(); ++i) {
            double v; ds >> v;
            m_pressure->operator[](i) = v;
        }
        bool hasTurb;
        ds >> hasTurb;
        if (hasTurb) {
            double dummy;
            for (std::size_t i = 0; i < N; ++i) ds >> dummy;
            for (std::size_t i = 0; i < N; ++i) ds >> dummy;
            for (std::size_t i = 0; i < N; ++i) ds >> dummy;
        }
    } else {
        m_velocity = std::make_shared<fvm::VectorField<2>>(*m_meshPtr, 0.0);
        m_pressure = std::make_shared<fvm::Field<2>>(*m_meshPtr, 0.0);
    }

    if (!cfd::deserializeCaseSettings(settings, m_settings, m_patchBCs)) return false;

    if (m_patchBCs.empty()) {
        for (const auto& patch : m_mesh.patches) {
            cfd::PatchBC pbc;
            pbc.patchName = patch.name;
            pbc.type = cfd::BCType::WallNoSlip;
            m_patchBCs.push_back(pbc);
        }
    }

    in.close();
    emit meshLoaded();
    emit caseLoaded();
    return true;
}

std::shared_ptr<const RenderMesh> CfdController::resultMesh() const {
    if (!m_meshPtr || !m_velocity || !m_pressure || !m_mesh.renderMesh) return nullptr;
    const fvm::SSTModel<2>* turb = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        if (m_solver) turb = m_solver->turbulence();
    }
    return buildResultMesh(m_mesh.renderMesh, *m_meshPtr, *m_velocity, *m_pressure, turb, m_settings, m_patchBCs);
}
