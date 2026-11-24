#include "app/cli_runner.h"

#include "ui/controllers/cfd_controller.h"

#include <QEventLoop>
#include <QObject>
#include <QString>

#include <climits>
#include <iostream>
#include <string>

namespace cli {

namespace {

void applyAmgVerbosity(cfd::CaseSettings& settings,
                       cfd::AmgSettings::Verbosity verbosity) {
    settings.pressureAmg.verbosity = verbosity;
    settings.momentumAmg.verbosity = verbosity;
    settings.turbulenceAmg.verbosity = verbosity;
}

} // namespace

int runHeadlessCase(const Options& options) {
    CfdController controller;

    const QString casePath = QString::fromLocal8Bit(options.casePath.string().c_str());
    if (!controller.loadCase(casePath)) {
        std::cerr << "Error: " << controller.lastError().toStdString() << '\n';
        return 1;
    }

    cfd::CaseSettings settings = controller.settings();
    const bool transient = settings.transient.enabled;

    if (options.iterations) {
        if (transient) {
            settings.stopping.transient.maxTimeSteps = *options.iterations;
        } else {
            settings.stopping.steady.fixedIterations = *options.iterations;
        }
    }
    if (options.verbosity) applyAmgVerbosity(settings, *options.verbosity);
    if (options.smoothedAggregation) {
        settings.pressureAmg.smoothedProlongation = true;
        settings.momentumAmg.smoothedProlongation = true;
        settings.turbulenceAmg.smoothedProlongation = true;
    }

    // Logs only: no result files, no live frame builds. The solver's own
    // iteration table goes straight to stdout because the GUI's output sink is
    // never installed in this path.
    settings.printFrequency = 1;
    settings.transient.writeInterval = 0;
    settings.resultUpdateMode = cfd::CaseSettings::ResultUpdateMode::EveryNSteps;
    settings.visualizationInterval = INT_MAX;
    controller.setSettings(settings);

    const int startStep = controller.currentStep();

    QEventLoop loop;
    int exitCode = 0;
    QObject::connect(&controller, &CfdController::finished, &loop,
                     [&](const QString& summary) {
        const std::string text = summary.toStdString();
        std::cout << text << '\n';
        if (summary.startsWith("Solve aborted")) exitCode = 1;
        loop.quit();
    });
    QObject::connect(&controller, &CfdController::failed, &loop,
                     [&](const QString& error) {
        std::cerr << "Error: " << error.toStdString() << '\n';
        exitCode = 1;
        loop.quit();
    });

    std::cout << "Solving " << controller.meshName().toStdString();
    if (transient) std::cout << " (transient)";
    std::cout << std::endl;

    if (!controller.run()) {
        if (!controller.lastError().isEmpty())
            std::cerr << "Error: " << controller.lastError().toStdString() << '\n';
        return 1;
    }

    loop.exec();

    if (exitCode == 0 && transient && options.iterations) {
        const int completed = controller.currentStep() - startStep;
        if (completed == static_cast<int>(*options.iterations)) {
            std::cout << "Step budget " << *options.iterations << " reached.\n";
        }
    }
    return exitCode;
}

} // namespace cli
