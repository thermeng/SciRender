#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/SIMPLETypes.h"
#include <array>
#include <cstddef>
#include <vector>

namespace fvm {

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

    const std::size_t n = mesh.nCells();
    const std::size_t nF = mesh.nFaces();

    // Unstructured meshes carry boundary conditions on faces, not cells. A cell
    // that owns a Dirichlet (wall/inlet/moving-wall) face must not have its
    // velocity overwritten by the pressure correction, otherwise the predictor's
    // face-based boundary treatment is undone every outer iteration.
    // Unstructured meshes carry boundary conditions on faces, not cells. A cell
    // that owns a Dirichlet (wall/inlet/moving-wall) face must not have its
    // velocity overwritten by the pressure correction, otherwise the predictor's
    // face-based boundary treatment is undone every outer iteration.
    std::vector<char> fixedCell(n, 0);
    if (faceBC && faceHasBC) {
        for (std::size_t f = 0; f < nF; ++f) {
            if (mesh.neighbor(f) != std::size_t(-1)) continue;
            if (f >= faceBC->size() || f >= faceHasBC->size()) continue;
            if (!(*faceHasBC)[f]) continue;
            auto t = (*faceBC)[f].type;
            if (t == VelocityBCType::NoSlip ||
                t == VelocityBCType::MovingWall ||
                t == VelocityBCType::Inlet) {
                fixedCell[mesh.owner(f)] = 1;
            }
        }
    }

    for (auto& g : dpdx) g.assign(0.0);

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = mesh.owner(f);
        const std::size_t N = mesh.neighbor(f);

        double pFace;
        if (N == std::size_t(-1)) {
            pFace = pPrime[P];
        } else {
            double w = mesh.faceWeight(f);
            pFace = (1.0 - w) * pPrime[P] + w * pPrime[N];
        }

        for (std::size_t d = 0; d < Dim; ++d) {
            double contrib = pFace * mesh.faceNormal(f)[d] * mesh.faceArea(f);
            dpdx[d][P] += contrib / mesh.cellVolume(P);
            if (N != std::size_t(-1)) {
                dpdx[d][N] -= contrib / mesh.cellVolume(N);
            }
        }
    }

    // clampDelta <= 0 means "no clamp". Previously this collapsed to a bound of
    // zero, which zeroed the entire velocity field, and a negative factor was
    // plain undefined behaviour.

    for (std::size_t d = 0; d < Dim; ++d) {
        #pragma omp parallel for schedule(static)
        for (std::size_t i = 0; i < n; ++i) {
            if (fixedCell[i]) {
                velocity[d][i] = uStar[d][i];
                continue;
            }
            if (dirichletMask[d][i]) {
                velocity[d][i] = dirichletVals[d][i];
            } else {
                velocity[d][i] = uStar[d][i] - dCoeff[d][i] * dpdx[d][i];
            }
        }
    }
    // Face-based exact normal enforcement for Symmetry/SlipWall (fixes oblique maxD leak)
    if (faceBC && faceHasBC) {
        for (std::size_t f = 0; f < nF; ++f) {
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
