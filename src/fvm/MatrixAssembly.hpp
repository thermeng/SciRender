#pragma once

#include "fvm/Types.h"
#include "core/AlignedBuffer.hpp"
#include <Eigen/Sparse>
#include <cstddef>
#include <vector>

namespace fvm {

template<std::size_t Dim>
inline void initAssemblyWorkspace(
    std::size_t n,
    AlignedBuffer<double>& diagSum,
    std::vector<Triplet>& triplets,
    Eigen::SparseMatrix<double>& A,
    Eigen::VectorXd& b) {
    diagSum.fill(0.0);
    triplets.clear();
    triplets.reserve(n * (2 * Dim + 1));
    A.resize(n, n);
    b.resize(n);
    b.setZero();
}

inline void applyDirichletRow(
    Eigen::VectorXd& b,
    std::vector<Triplet>& triplets,
    std::size_t P,
    const std::vector<char>& dirichletMask,
    const std::vector<double>& dirichletValues,
    double diagSumP,
    std::span<const double> source,
    const double* volPtr) {
    if (dirichletMask.size() > P && dirichletMask[P]) {
        triplets.emplace_back(static_cast<Eigen::Index>(P), static_cast<Eigen::Index>(P), 1.0);
        b(P) = dirichletValues[P];
    } else {
        triplets.emplace_back(static_cast<Eigen::Index>(P), static_cast<Eigen::Index>(P), diagSumP);
        if (!source.empty() && volPtr) {
            b(P) += source[P] * volPtr[P];
        }
    }
}

inline void finalizeAssembly(
    Eigen::SparseMatrix<double>& A,
    const std::vector<Triplet>& triplets) {
    A.setFromTriplets(triplets.begin(), triplets.end());
    A.makeCompressed();
}

} // namespace fvm
