// End-to-end tests for the .scfd v5 case format.
//
// These use the real CfdController (Qt), so they only build with the app build
// (SCIRENDER_BUILD_APP): the Qt-free `tests` preset has no Qt at all. The
// executables land next to the deployed Qt DLLs, so they run without a
// windeployqt step.
#include "doctest/doctest.h"

#include "ui/controllers/cfd_controller.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>

namespace {

// One QCoreApplication for the whole binary: the controller's QFutureWatcher
// needs an event loop, and Qt allows only one application object.
QCoreApplication& app() {
    static int argc = 1;
    static char name[] = "scfd_tests";
    static char* argv[] = {name, nullptr};
    static QCoreApplication instance(argc, argv);
    return instance;
}

QString sampleMeshPath() {
    return QStringLiteral(SCIRENDER_SAMPLES_DIR "/cfd/CavityQuad_40x40.msh");
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

// Starts the run and returns once the controller reports finished/failed.
// Returns false on refusal or timeout.
bool runToCompletion(CfdController& ctrl, int timeoutMs = 120000) {
    QEventLoop loop;
    QObject::connect(&ctrl, &CfdController::finished, &loop, &QEventLoop::quit);
    QObject::connect(&ctrl, &CfdController::failed, &loop, &QEventLoop::quit);

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

// Starts the run, cancels it once `ready()` becomes true, and returns when the
// controller reports the run stopped.
bool runThenCancelWhen(CfdController& ctrl, const std::function<bool()>& ready,
                       int timeoutMs = 120000) {
    QEventLoop loop;
    QObject::connect(&ctrl, &CfdController::finished, &loop, &QEventLoop::quit);
    QObject::connect(&ctrl, &CfdController::failed, &loop, &QEventLoop::quit);

    if (!ctrl.run()) return false;

    bool timedOut = false;
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (ctrl.isRunning() && ready()) {
            if (!ctrl.isCancelled()) ctrl.cancel();
            poll.stop();
        }
    });
    poll.start();

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

} // namespace

TEST_SUITE("ui") {

TEST_CASE("ui.scfd_v5_roundtrip_preserves_case_and_mesh_name") {
    app();

    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    CfdController ctrl;
    REQUIRE(ctrl.loadMesh(sampleMeshPath()));
    const QString meshName = ctrl.meshName();
    REQUIRE(meshName == "CavityQuad_40x40.msh");

    cfd::CaseSettings s = ctrl.settings();
    s.transient.enabled = true;
    s.transient.dt = 0.005;
    s.transient.tEnd = 2.0;
    s.turbulence = false;
    ctrl.setSettings(s);

    REQUIRE(ctrl.initialize(0.0, 0.0, 0.0) != nullptr);

    const QString fileA = dir.filePath("a.scfd");
    REQUIRE(ctrl.saveCase(fileA));
    CHECK(QFile::exists(fileA));

    // Loading must keep the embedded display name, not the random extraction
    // file name (the old reader exposed the temporary path in the UI).
    CfdController loaded;
    REQUIRE(loaded.loadCase(fileA));
    CHECK(loaded.meshName() == meshName);
    CHECK(loaded.settings().transient.enabled);
    CHECK(loaded.settings().transient.dt == doctest::Approx(0.005));
    CHECK(loaded.currentStep() == 0);
    CHECK(loaded.currentTime() == doctest::Approx(0.0));
    CHECK(loaded.resultMesh() != nullptr);

    // Load -> save must be lossless: the mesh bytes and the display name are
    // retained, so the second file is byte-for-byte identical to the first.
    const QString fileB = dir.filePath("b.scfd");
    REQUIRE(loaded.saveCase(fileB));

    QFile fa(fileA);
    QFile fb(fileB);
    REQUIRE(fa.open(QIODevice::ReadOnly));
    REQUIRE(fb.open(QIODevice::ReadOnly));
    CHECK(fa.readAll() == fb.readAll());
}

TEST_CASE("ui.scfd_v5_transient_continuation_and_end_time_guard") {
    app();

    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    // --- Run to t = 0.02, then save ---------------------------------------
    CfdController ctrl;
    REQUIRE(ctrl.loadMesh(sampleMeshPath()));
    configureTransient(ctrl, 0.02);
    REQUIRE(runToCompletion(ctrl));
    INFO("last error: " << ctrl.lastError().toStdString());
    REQUIRE_FALSE(ctrl.isRunning());
    REQUIRE(ctrl.currentStep() == 2);
    CHECK(ctrl.currentTime() == doctest::Approx(0.02).epsilon(1e-9));

    const QString file = dir.filePath("run.scfd");
    REQUIRE(ctrl.saveCase(file));

    // --- Load: the clock and the fields come back --------------------------
    CfdController loaded;
    REQUIRE(loaded.loadCase(file));
    CHECK(loaded.currentStep() == 2);
    CHECK(loaded.currentTime() == doctest::Approx(0.02).epsilon(1e-9));

    // --- End time already reached: refuse, do not silently restart ---------
    configureTransient(loaded, 0.01);
    CHECK_FALSE(loaded.run());
    CHECK(loaded.lastError().contains("Raise the end time"));

    // --- Raise it: the run continues from step 2 to step 3 -----------------
    configureTransient(loaded, 0.03);
    REQUIRE(runToCompletion(loaded));
    INFO("last error: " << loaded.lastError().toStdString());
    CHECK(loaded.currentStep() == 3);
    CHECK(loaded.currentTime() == doctest::Approx(0.03).epsilon(1e-9));
}

TEST_CASE("ui.scfd_v5_cancel_leaves_a_restartable_state") {
    app();

    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    CfdController ctrl;
    REQUIRE(ctrl.loadMesh(sampleMeshPath()));
    configureTransient(ctrl, 10.0); // long enough that the clock stops first

    REQUIRE(runThenCancelWhen(ctrl, [&] { return ctrl.currentStep() >= 2; }));
    INFO("last error: " << ctrl.lastError().toStdString());
    REQUIRE_FALSE(ctrl.isRunning());
    REQUIRE(ctrl.isCancelled());

    const int cancelledStep = ctrl.currentStep();
    const double cancelledTime = ctrl.currentTime();
    REQUIRE(cancelledStep >= 2);
    // Cancel is honoured at a time-step boundary: the saved clock is a whole
    // number of steps, so it can be resumed without replaying a partial step.
    CHECK(cancelledTime ==
          doctest::Approx(cancelledStep * 0.01).epsilon(1e-9));

    const QString file = dir.filePath("cancelled.scfd");
    REQUIRE(ctrl.saveCase(file));

    CfdController loaded;
    REQUIRE(loaded.loadCase(file));
    CHECK(loaded.currentStep() == cancelledStep);
    CHECK(loaded.currentTime() == doctest::Approx(cancelledTime).epsilon(1e-9));

    // A saved cancelled run is a valid restart point: continue for 5 more steps.
    configureTransient(loaded, cancelledTime + 0.05);
    REQUIRE(runToCompletion(loaded));
    INFO("last error: " << loaded.lastError().toStdString());
    CHECK(loaded.currentStep() == cancelledStep + 5);
    CHECK(loaded.currentTime() == doctest::Approx(cancelledTime + 0.05).epsilon(1e-9));
}

} // TEST_SUITE
