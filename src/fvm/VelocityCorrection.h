#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/Gradient.h"
#include "fvm/SIMPLETypes.h"
#include <Eigen/Dense>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace fvm {

// u = u* - d * grad(p').
//
// The gradient here IS the operator whose divergence the pressure matrix is
// assembled from (the per-cell least-squares gradient, see Gradient.h): the
// pressure equation solves div(d grad p') = div(u*), so the correction below
// cancels the divergence it was built for EXACTLY - on every cell, not only
// in total. A different gradient would make the two operators disagree where
// p' has a local feature, which is where the mass balance then leaks.
template<std::size_t Dim>
inline void correctVelocity(
    const Mesh<Dim>& mesh,
    const VectorField<Dim>& uStar,
    const Field<Dim>& pPrime,
    const std::array<std::vector<double>, Dim>& dCoeff,
    const std::vector<std::vector<char>>& dirichletMask,
    const std::vector<std::vector<double>>& dirichletVals,
    std::array<Field<Dim>, Dim>& dpdx,
    VectorField<Dim>& velocity,
    const std::vector<FaceVelocityBC>* faceBC = nullptr,
    const std::vector<char>* faceHasBC = nullptr) {

    Gradient<Dim> gradient(mesh, GradientScheme::LeastSquares);
    std::array<Field<Dim>, Dim> lsqGrad{};
    for (auto& g : lsqGrad) g = Field<Dim>(mesh, 0.0);
    gradient.compute(pPrime, lsqGrad);

    for (std::size_t d = 0; d < Dim; ++d) dpdx[d] = lsqGrad[d];

    for (std::size_t d = 0; d < Dim; ++d) {
        #pragma omp parallel for schedule(static)
        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            if (dirichletMask[d][i]) velocity[d][i] = dirichletVals[d][i];
            else velocity[d][i] = uStar[d][i] - dCoeff[d][i] * dpdx[d][i];
        }
    }
    if (faceBC && faceHasBC) {
        for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
            if (mesh.neighbor(f) != std::size_t(-1)) continue;
            if (f >= faceBC->size() || f >= faceHasBC->size()) continue;
            if (!(*faceHasBC)[f]) continue;
            auto t = (*faceBC)[f].type;
            if (t == VelocityBCType::Symmetry || t == VelocityBCType::SlipWall) {
                std::size_t P = mesh.owner(f);
                const auto& n = mesh.faceNormal(f);
                double un = 0.0;
                for (std::size_t d = 0; d < Dim; ++d) un += velocity[d][P] * n[d];
                for (std::size_t d = 0; d < Dim; ++d) velocity[d][P] -= un * n[d];
            }
        }
    }
}

} // namespace fvm
