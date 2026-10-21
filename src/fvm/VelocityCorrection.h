#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/SIMPLETypes.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace fvm {

// Applies the pressure correction to the predictor.
//
// The correction has to cancel the divergence of the predictor, so it must be
// built with the same face operator the pressure equation assembles: the
// pressure matrix's row is
//     sum_faces coeff * (p_P - p_N),   coeff = d * A * (S . d) / |d|^2,
// and the gradient whose divergence reproduces that row is the one built from
// the same face differences.
//
// The Green-Gauss face-value gradient (the average of the two cell values) is a
// DIFFERENT operator: its divergence is a wide stencil that does not match the
// matrix, and the correction then removes the divergence only in total, not
// face by face - which leaves a residual that grows with the mesh refinement.
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

    // A cell whose velocity is prescribed AT THE CELL LEVEL (Dirichlet mask) is
    // not corrected - that is handled component-wise below.
    //
    // A cell that merely OWNS a face carrying a Dirichlet BC (no-slip wall,
    // moving wall, inlet) is NOT frozen. On a face-based (ghost-cell) boundary
    // treatment the cell-centre velocity of such a cell is a free unknown: the
    // wall condition lives on the face, not on the cell. Freezing the cell at
    // the momentum predictor removed the only degree of freedom the pressure
    // correction has for balancing that cell's mass balance, so the wall layer
    // kept a persistent continuity error and the pressure field that drives the
    // recirculation was biased exactly at the boundary (the corner cells, where
    // the secondary vortices of a lid-driven cavity are generated).
    //
    // The predictor's face-based treatment is not undone by correcting the cell:
    // the face condition is re-imposed every time the momentum equation is
    // assembled, independently of the cell velocity.
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
