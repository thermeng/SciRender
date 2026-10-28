#pragma once

#include "fvm/Types.h"
#include "fvm/SSTConstants.h"
#include "mesh/Mesh.h"
#include "field/Field.h"
#include "core/AlignedBuffer.hpp"

#include <Eigen/Sparse>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace fvm {

// Assembles
//
//     div(Gamma grad(phi)) = source
//
// over the mesh, with an identity row for every Dirichlet cell and a zero-flux
// (no matrix contribution) boundary face otherwise.
//
// `faceDiffusivity(f, owner, neighbour)` supplies Gamma on face `f`: a constant
// for the Laplacian, an interpolated cell field for Poisson. Keeping the whole
// face loop here means the sign convention, the Dirichlet row handling and the
// non-orthogonal correction are written once, so the two operators cannot
// drift apart.
//
// The face coefficient is Gamma * |S_f| * (d . n) / |d|^2. It carries the sign
// of the cosine between the face normal and the centre-to-centre vector: the
// mesh orients every internal normal owner -> neighbour, so the coefficient is
// positive on a valid mesh. Faces with a degenerate centre-to-centre distance
// are skipped rather than divided through, which previously produced an
// infinite coefficient and an undefined correction.
template<std::size_t Dim, class FaceDiffusivity>
void assembleDiffusion(const Mesh<Dim>& mesh,
                       SparseMatrix& A,
                       Eigen::VectorXd& b,
                       AlignedBuffer<double>& diagSum,
                       std::vector<Triplet>& triplets,
                       const std::vector<char>& dirichletMask,
                       const std::vector<double>& dirichletValues,
                       std::span<const double> source,
                       const std::array<Field<Dim>, Dim>* phiGrad,
                       FaceDiffusivity&& faceDiffusivity) {
    const std::size_t n = mesh.nCells();
    const std::size_t nF = mesh.nFaces();
    const bool hasMask = !dirichletMask.empty();
    const bool hasSource = !source.empty();
    const bool hasGrad = (phiGrad != nullptr);

    diagSum.fill(0.0);
    triplets.clear();
    triplets.reserve(n * (2 * Dim + 1));

    A.resize(n, n);
    b.resize(n);
    b.setZero();

    const double* areaPtr = mesh.faceAreaPtr();
    const double* distPtr = mesh.faceDistPtr();
    const std::size_t* ownerPtr = mesh.ownerPtr();
    const std::size_t* neighborPtr = mesh.neighborPtr();
    const double* volPtr = mesh.cellVolumePtr();

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        if (N == std::size_t(-1)) {
            // Boundary face: Dirichlet cells get an identity row below; every
            // other boundary type is homogeneous Neumann (zero flux).
            continue;
        }

        const double dist = distPtr[f];
        if (!(dist > 1e-12)) continue;

        const double area = areaPtr[f];
        const double* fn = mesh.faceNormalPtr(f);
        const double* xP = mesh.cellCenterPtr(P);
        const double* xN = mesh.cellCenterPtr(N);

        double d[Dim];
        for (std::size_t k = 0; k < Dim; ++k) d[k] = xN[k] - xP[k];

        double dotSfd = 0.0;
        for (std::size_t k = 0; k < Dim; ++k) dotSfd += fn[k] * d[k];

        const double gamma = faceDiffusivity(f, P, N);
        const double coeff = gamma * area * dotSfd / (dist * dist);

        double correction = 0.0;
        if (hasGrad) {
            // Cross-diffusion part of the face area vector:
            //   S_f - (d . S_f) d / |d|^2
            const double dot_f_dhat = dotSfd / dist;
            double Sf_nonorth[Dim];
            for (std::size_t k = 0; k < Dim; ++k)
                Sf_nonorth[k] = area * fn[k] - area * dot_f_dhat * d[k] / dist;
            for (std::size_t k = 0; k < Dim; ++k)
                correction += gamma * (*phiGrad)[k][P] * Sf_nonorth[k];
            correction *= nonOrthCorrectionFactor;
        }

        if (!(hasMask && dirichletMask[P])) {
            if (hasMask && dirichletMask[N]) {
                b(P) += coeff * dirichletValues[N];
            } else {
                triplets.emplace_back(P, N, -coeff);
            }
            diagSum[P] += coeff;
            if (hasGrad) b(P) -= correction;
        }

        if (!(hasMask && dirichletMask[N])) {
            if (hasMask && dirichletMask[P]) {
                b(N) += coeff * dirichletValues[P];
            } else {
                triplets.emplace_back(N, P, -coeff);
            }
            diagSum[N] += coeff;
            if (hasGrad) b(N) += correction;
        }
    }

    for (std::size_t P = 0; P < n; ++P) {
        if (hasMask && dirichletMask[P]) {
            triplets.emplace_back(P, P, 1.0);
            b(P) = dirichletValues[P];
        } else {
            triplets.emplace_back(P, P, diagSum[P]);
            if (hasSource) b(P) += source[P] * volPtr[P];
        }
    }

    A.setFromTriplets(triplets.begin(), triplets.end());
    A.makeCompressed();
}

} // namespace fvm
