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

// The mask must cover every cell and every component, otherwise the
// prescribed values would be read out of bounds.
inline void validateVelocityMask(std::size_t dim, std::size_t nCells,
                                 const std::vector<std::vector<char>>& mask,
                                 const std::vector<std::vector<double>>& values) {
    if (mask.empty()) return;
    if (mask.size() < dim)
        throw std::invalid_argument("computeDivergenceInto: the Dirichlet mask needs one entry per component");
    if (!values.empty() && values.size() < dim)
        throw std::invalid_argument("computeDivergenceInto: the Dirichlet values need one entry per component");
    for (std::size_t d = 0; d < dim; ++d) {
        if (mask[d].size() != nCells)
            throw std::invalid_argument("computeDivergenceInto: the Dirichlet mask needs one entry per cell");
        if (!values.empty() && values[d].size() != nCells)
            throw std::invalid_argument("computeDivergenceInto: the Dirichlet values need one entry per cell");
    }
}

} // namespace fvm