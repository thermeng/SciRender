#pragma once

#include <stdexcept>
#include <vector>
#include <cstddef>

namespace fvm {

// A Dirichlet mask/value pair must cover every cell: otherwise the prescribed
// values are read out of bounds while the operator is assembled.
inline void validateDirichlet(std::size_t nCells, const std::vector<char>& mask,
                              const std::vector<double>& values) {
    if (mask.empty()) return;
    if (mask.size() != nCells)
        throw std::invalid_argument("operator::discretize: the Dirichlet mask needs one entry per cell");
    if (values.size() != nCells)
        throw std::invalid_argument("operator::discretize: the Dirichlet values need one entry per cell");
}

} // namespace fvm