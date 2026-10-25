// The CFD page must accept a 3D mesh and drive the 3D solver behind the same
// controller API as the 2D path.
//
// This is the controller-level counterpart of cfd.mesh_adapter and
// fvm.channel3d: it proves the mesh reaches the viewport, Initialize produces
// real 3D result geometry, a run is dispatched to the 3D session rather than
// failing with "file dimension 3 does not match expected 2", and a v6 .scfd
// file round-trips the third velocity component and the transient clock.

#include "doctest/doctest.h"

#include "ui/controllers/cfd_controller.h"

#include "mesh/Mesh.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>

namespace {

QCoreApplication& app() {
    static int argc = 1;
    static char name[] = "cfd_3d_mesh_tests";
    static char* argv[] = {name, nullptr};
    static QCoreApplication instance(argc, argv);
    return instance;
}

QString samplePath(const char* relative) {
    return QStringLiteral(SCIRENDER_SAMPLES_DIR) + "/" + relative;
}

// Starts the run and returns once the controller reports finished/failed.
bool runToCompletion(CfdController& ctrl, bool& failed, int timeoutMs = 180000) {
    QEventLoop loop;
    failed = false;
    QObject::connect(&ctrl, &CfdController::finished, &loop, &QEventLoop::quit);
    QObject::connect(&ctrl, &CfdController::failed, &loop, [&] {
        failed = true;
        loop.quit();
    });

    if (!ctrl.run()) return false;

    bool timedOut = false;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        loop.quit();
    });
    timeout.start(timeoutMs);
    loop.exec();
    return !timedOut;
}

void configureTransient(CfdController& ctrl, double tEnd) {
    cfd::CaseSettings s = ctrl.settings();
    s.transient.enabled = true;
    s.transient.dt = 0.01;
    s.transient.tEnd = tEnd;
    s.transient.adaptiveCfl = false;
    s.transient.maxOuterCorrectors = 1;
    s.transient.timeScheme = cfd::TransientSettings::TimeScheme::BackwardEuler;
    s.transient.writeInterval = 0; // no VTU/PVD side effects in the test
    s.tolerance = 1e-4;
    s.numThreads = 1;
    ctrl.setSettings(s);
}

void configureSteady(CfdController& ctrl) {
    cfd::CaseSettings s = ctrl.settings();
    s.transient.enabled = false;
    s.maxIterations = 1;
    s.runFixedIterations = true;
    s.numThreads = 1;
    ctrl.setSettings(s);
}

} // namespace

TEST_SUITE("ui") {

TEST_CASE("ui.cfd_loads_and_solves_3d_mesh") {
    app();

    CfdController ctrl;
    REQUIRE(ctrl.loadMesh(samplePath("cfd/pipe_3d.msh")));
    INFO("last error: " << ctrl.lastError().toStdString());

    const cfd::MeshLoadResult& res = ctrl.meshResult();
    CHECK(res.dim == 3);
    REQUIRE(res.mesh3);
    CHECK_FALSE(res.mesh2);
    REQUIRE(res.renderMesh);
    CHECK_FALSE(res.renderMesh->indices.empty());
    CHECK_FALSE(res.patches.empty());
    CHECK(ctrl.meshName() == "pipe_3d.msh");

    // Initialize goes through the 3D session and returns real 3D geometry.
    auto initialized = ctrl.initialize(1.0, 0.0, 0.0);
    INFO("last error: " << ctrl.lastError().toStdString());
    REQUIRE(initialized);
    CHECK_FALSE(initialized->vertices.empty());
    CHECK(initialized->cellVectorCount == res.mesh3->nCells());
    CHECK(ctrl.resultMesh() != nullptr);

    // The renderer shades per vertex: the extrapolated point field must be
    // sized to the final (flat-shaded) vertex buffer, and the point vectors to
    // the source point count they were built from.
    const std::size_t nVerts = initialized->vertices.size() / 3;
    CHECK(initialized->normals.size() == initialized->vertices.size());
    REQUIRE(initialized->attributes);
    const auto pressure = initialized->attributes->pointScalars.find("pressure");
    REQUIRE(pressure != initialized->attributes->pointScalars.end());
    CHECK(pressure->second.size() == nVerts);
    CHECK(initialized->pointVectorCount ==
          static_cast<std::size_t>(initialized->sourcePointCount));

    // A steady run with a one-iteration budget must be accepted and finish.
    configureSteady(ctrl);

    bool failed = false;
    REQUIRE(runToCompletion(ctrl, failed));
    INFO("last error: " << ctrl.lastError().toStdString());
    CHECK_FALSE(failed);
    CHECK_FALSE(ctrl.isRunning());
    CHECK(ctrl.resultMesh() != nullptr);

    // v6 carries the dimension and all three velocity components.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    REQUIRE(ctrl.saveCase(dir.filePath("pipe.scfd")));
}

TEST_CASE("ui.cfd_saves_and_restores_3d_solution") {
    app();

    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    CfdController ctrl;
    REQUIRE(ctrl.loadMesh(samplePath("cfd/pipe_3d.msh")));
    configureSteady(ctrl);

    // A non-zero uz must survive to the result mesh, the file and back.
    cfd::InitConditions ic;
    ic.ux = 1.0;
    ic.uy = 0.1;
    ic.uz = 0.25;
    ic.pressure = 0.0;
    const std::size_t nCells = ctrl.meshResult().mesh3->nCells();
    auto initialized = ctrl.initialize(ic);
    REQUIRE(initialized);
    REQUIRE(initialized->cellVectorsData.size() == nCells);
    for (const auto& v : initialized->cellVectorsData) {
        CHECK(v.z == doctest::Approx(0.25f).epsilon(1e-6));
    }

    bool failed = false;
    REQUIRE(runToCompletion(ctrl, failed));
    INFO("last error: " << ctrl.lastError().toStdString());
    REQUIRE_FALSE(failed);

    auto saved = ctrl.resultMesh();
    REQUIRE(saved);
    REQUIRE(saved->cellVectorsData.size() == nCells);

    const QString file = dir.filePath("pipe3d.scfd");
    REQUIRE(ctrl.saveCase(file));

    CfdController loaded;
    REQUIRE(loaded.loadCase(file));
    INFO("last error: " << loaded.lastError().toStdString());
    CHECK(loaded.meshResult().dim == 3);
    REQUIRE(loaded.meshResult().mesh3);
    CHECK(loaded.currentStep() == 0);

    auto restored = loaded.resultMesh();
    REQUIRE(restored);
    REQUIRE(restored->cellVectorsData.size() == saved->cellVectorsData.size());
    for (std::size_t i = 0; i < saved->cellVectorsData.size(); ++i) {
        const auto& a = saved->cellVectorsData[i];
        const auto& b = restored->cellVectorsData[i];
        CHECK(b.x == doctest::Approx(a.x).epsilon(1e-6));
        CHECK(b.y == doctest::Approx(a.y).epsilon(1e-6));
        CHECK(b.z == doctest::Approx(a.z).epsilon(1e-6));
    }

    // Load -> save must be lossless for the 3D payload too.
    const QString fileB = dir.filePath("pipe3d_b.scfd");
    REQUIRE(loaded.saveCase(fileB));
    QFile fa(file);
    QFile fb(fileB);
    REQUIRE(fa.open(QIODevice::ReadOnly));
    REQUIRE(fb.open(QIODevice::ReadOnly));
    CHECK(fa.readAll() == fb.readAll());
}

TEST_CASE("ui.cfd_3d_transient_clock_roundtrip") {
    app();

    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    CfdController ctrl;
    REQUIRE(ctrl.loadMesh(samplePath("cfd/pipe_3d.msh")));
    configureTransient(ctrl, 0.02);

    bool failed = false;
    REQUIRE(runToCompletion(ctrl, failed));
    INFO("last error: " << ctrl.lastError().toStdString());
    REQUIRE_FALSE(failed);
    REQUIRE(ctrl.currentStep() == 2);
    CHECK(ctrl.currentTime() == doctest::Approx(0.02).epsilon(1e-9));

    const QString file = dir.filePath("pipe3d_transient.scfd");
    REQUIRE(ctrl.saveCase(file));

    CfdController loaded;
    REQUIRE(loaded.loadCase(file));
    INFO("last error: " << loaded.lastError().toStdString());
    CHECK(loaded.meshResult().dim == 3);
    CHECK(loaded.currentStep() == 2);
    CHECK(loaded.currentTime() == doctest::Approx(0.02).epsilon(1e-9));

    // The third velocity history row must have been written and read back:
    // continuing produces the next step, not a restart from zero.
    configureTransient(loaded, 0.03);
    REQUIRE(runToCompletion(loaded, failed));
    INFO("last error: " << loaded.lastError().toStdString());
    CHECK_FALSE(failed);
    CHECK(loaded.currentStep() == 3);
    CHECK(loaded.currentTime() == doctest::Approx(0.03).epsilon(1e-9));
}

TEST_CASE("ui.cfd_exports_3d_solution_to_vtu") {
    app();

    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    CfdController ctrl;
    REQUIRE(ctrl.loadMesh(samplePath("cfd/pipe_3d.msh")));

    // Export the freshly initialized field: uz is exactly 0.25, so the file's
    // third velocity component is easy to verify.
    cfd::InitConditions ic;
    ic.ux = 1.0;
    ic.uy = 0.1;
    ic.uz = 0.25;
    REQUIRE(ctrl.initialize(ic));

    const QString file = dir.filePath("pipe3d.vtu");
    ctrl.exportVtu(file);
    INFO("last error: " << ctrl.lastError().toStdString());
    REQUIRE(ctrl.lastError().isEmpty());

    QFile f(file);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QByteArray data = f.readAll();

    // A 3D CFD mesh is unstructured: the writer must emit a VTU grid with all
    // three velocity components.
    CHECK(data.contains("<UnstructuredGrid"));
    CHECK(data.contains("Name=\"Velocity\""));
    CHECK(data.contains("NumberOfComponents=\"3\""));

    // Extract the Velocity array and check every z component.
    const int at = data.indexOf("Name=\"Velocity\"");
    REQUIRE(at >= 0);
    const int open = data.indexOf('>', at);
    const int close = data.indexOf("</DataArray>", open);
    REQUIRE(open >= 0);
    REQUIRE(close > open);
    const QByteArray body = data.mid(open + 1, close - open - 1);
    const QList<QByteArray> tokens =
        body.simplified().split(' ');
    REQUIRE(tokens.size() % 3 == 0);
    CHECK(tokens.size() / 3 ==
          static_cast<int>(ctrl.meshResult().mesh3->nCells()));
    for (int i = 0; i < tokens.size(); i += 3) {
        CHECK(tokens[i + 2].toDouble() == doctest::Approx(0.25).epsilon(1e-9));
    }
}

TEST_CASE("ui.cfd_3d_transient_writes_vtu_frames_and_pvd") {
    app();

    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    CfdController ctrl;
    REQUIRE(ctrl.loadMesh(samplePath("cfd/pipe_3d.msh")));
    configureTransient(ctrl, 0.02);
    cfd::CaseSettings s = ctrl.settings();
    s.transient.writeInterval = 1;
    s.transient.pvdFile = dir.filePath("frames.pvd").toStdString();
    ctrl.setSettings(s);

    bool failed = false;
    REQUIRE(runToCompletion(ctrl, failed));
    INFO("last error: " << ctrl.lastError().toStdString());
    REQUIRE_FALSE(failed);

    const QString pvd = dir.filePath("frames.pvd");
    REQUIRE(QFile::exists(pvd));
    QFile p(pvd);
    REQUIRE(p.open(QIODevice::ReadOnly));
    const QByteArray xml = p.readAll();
    CHECK(xml.contains("frames_1.vtu"));
    CHECK(xml.contains("frames_2.vtu"));
    CHECK(QFile::exists(dir.filePath("frames_1.vtu")));
    CHECK(QFile::exists(dir.filePath("frames_2.vtu")));

    QFile frame(dir.filePath("frames_2.vtu"));
    REQUIRE(frame.open(QIODevice::ReadOnly));
    const QByteArray data = frame.readAll();
    CHECK(data.contains("<UnstructuredGrid"));
    CHECK(data.contains("NumberOfComponents=\"3\""));
}

} // TEST_SUITE
