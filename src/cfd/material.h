#pragma once

#include <array>
#include <string>

namespace cfd {

// A fluid material defined by density and DYNAMIC viscosity. The solver
// works with kinematic viscosity nu = mu / rho; nu() derives it so the two
// representations never drift apart.
struct Material {
    std::string name;
    double rho = 1.0;  // kg/m^3
    double mu = 1e-3;  // Pa*s (dynamic viscosity)

    double nu() const { return mu / rho; }
};

namespace materials {

// Built-in reference materials at 20 degrees C, 1 atm.
inline Material air() { return Material{"Air", 1.204, 1.825e-5}; }
inline Material water() { return Material{"Water", 998.2, 1.002e-3}; }

inline std::array<Material, 2> builtIns() { return {air(), water()}; }

// Combo entry for hand-typed properties (not a library entry). Cases saved
// with this name (or with no material name at all) load as custom.
inline const char* customName() { return "Custom"; }

} // namespace materials
} // namespace cfd
