#pragma once

#include <string>
#include <cstddef>

namespace fvm {

enum class SolverType { SIMPLE, PISO, PIMPLE };

inline SolverType solverTypeFromString(const std::string& s) {
    std::string t;
    t.reserve(s.size());
    for (char c : s) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "piso") return SolverType::PISO;
    if (t == "pimple") return SolverType::PIMPLE;
    return SolverType::SIMPLE;
}

inline std::string solverTypeToString(SolverType t) {
    switch (t) {
        case SolverType::PISO: return "PISO";
        case SolverType::PIMPLE: return "PIMPLE";
        default: return "SIMPLE";
    }
}

// Velocity boundary condition types — dimension-independent.
// Extracted from SIMPLE<Dim>::VelocityBCType to break the god-file cycle.
// SIMPLE<Dim> re-exports these as nested aliases for backward compatibility.
enum class VelocityBCType { NoSlip, Inlet, Outlet, PressureOutlet, Symmetry, SlipWall, MovingWall };

struct VelocityBC {
    VelocityBCType type = VelocityBCType::NoSlip;
    double u = 0.0;
    double v = 0.0;
    double w = 0.0;
    double pressureValue = 0.0;
};

struct VelocityPatch {
    std::string name;
    std::size_t wallDim = 0;
    int wallSign = 0;
    VelocityBCType type = VelocityBCType::NoSlip;
    double u = 0.0;
    double v = 0.0;
    double w = 0.0;
};

struct FaceVelocityBC {
    VelocityBCType type = VelocityBCType::NoSlip;
    double u = 0.0;
    double v = 0.0;
    double w = 0.0;
    double pressureValue = 0.0;
    // Fluent-like pressure outlet backflow handling
    bool suppressBackflow = false; // if true, clamp U·n >=0 (prevent inflow through outlet)
    double backflowU = 0.0;
    double backflowV = 0.0;
    double backflowW = 0.0;
    double backflowK = 0.0;
    double backflowOmega = 0.0;
};

struct PressureOutletConfig {
    double p0 = 0.0; // gauge static pressure at outlet
    double operatingPressure = 0.0; // operating pressure offset (Fluent)
    bool radialEquilibrium = false;
    bool suppressBackflow = false;
    double backflowU = 0.0;
    double backflowV = 0.0;
    double backflowW = 0.0;
    double backflowK = 0.0;
    double backflowOmega = 0.0;
};

} // namespace fvm
