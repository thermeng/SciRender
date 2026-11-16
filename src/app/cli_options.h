#pragma once

#include "cfd/cfd_types.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace cli {

// Command-line invocation of the headless solver. Built before any Qt
// application object exists, so it must stay free of Qt types.
struct Options {
    std::filesystem::path casePath;
    // Steady: exact iteration budget. Transient: time steps from the restart
    // point. Absent means "use the case's stopping criteria".
    std::optional<std::size_t> iterations;
    // Overrides pressure/momentum/turbulence AMG verbosity when set.
    std::optional<cfd::AmgSettings::Verbosity> verbosity;
    // Diagnostic switch: force the cached-aggregation AMG backend
    // instead of BoomerAMG (used to measure the cheaper setup path).
    bool cachedAggregation = false;
    // Force Smoothed Aggregation prolongation (cached-aggregation backend):
    // tentative piecewise-constant prolongation is Jacobi-smoothed so the
    // coarse grid can represent smooth error modes.
    bool smoothedAggregation = false;
    bool help = false;
};

// True when argv contains a known CLI option, i.e. the process must run the
// headless path instead of opening the GUI.
bool wantsCli(int argc, char** argv);

// Parses argv[1..]. Returns nullopt and fills `error` on invalid input.
std::optional<Options> parse(int argc, char** argv, std::string& error);

std::string usage();

} // namespace cli
