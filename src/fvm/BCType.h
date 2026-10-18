#pragma once

#include <cstddef>
#include <string>

namespace fvm {

enum class BCType { Dirichlet, Neumann };

struct BoundaryCondition {
    BCType type;
    double value;
};

struct Patch {
    std::string name;
    std::size_t wallDim;
    int wallSign;
    BCType type;
    double value;
};

} // namespace fvm
