#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/FaceState.h"

#include <cstddef>
#include <cmath>
#include <vector>

namespace fvm {

// Single owner of face geometry + the divergence assembly. The face *velocity*
// policy lives in fvm/FaceState.h, which this uses, so that the divergence, the
// momentum convection, the pressure correction and the continuity residual are
// all built from the same face value.
template<std::size_t Dim>
class FaceOperator {
public:
    explicit FaceOperator(const Mesh<Dim>& mesh) : mesh_(mesh) {}

    const Mesh<Dim>& mesh() const { return mesh_; }

    void divergence(const VectorField<Dim>& velocity, Field<Dim>& div,
                    const std::vector<std::vector<char>>& dirichletMask,
                    const std::vector<std::vector<double>>& dirichletValues,
                    double* sumFlux = nullptr) const;

private:
    const Mesh<Dim>& mesh_;
};

template<std::size_t Dim>
void FaceOperator<Dim>::divergence(const VectorField<Dim>& velocity, Field<Dim>& div,
                                   const std::vector<std::vector<char>>& dirichletMask,
                                   const std::vector<std::vector<double>>& dirichletValues,
                                   double* sumFlux) const {
    const std::size_t n = mesh_.nCells();
    for (std::size_t i = 0; i < n; ++i) div[i] = 0.0;
    if (sumFlux) *sumFlux = 0.0;

    const std::size_t nF = mesh_.nFaces();
    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = mesh_.owner(f);
        const std::size_t N = mesh_.neighbor(f);
        const bool isBoundary = (N == kNoNeighbor);

        const auto& nrm = mesh_.faceNormal(f);
        const double area = mesh_.faceArea(f);
        const double w = isBoundary ? 0.0 : mesh_.faceWeight(f);

        const std::array<double, Dim> face = interpolateFaceVelocity(
            velocity, P, N, w, &dirichletMask, &dirichletValues);
        const double un = dotNormal(face, nrm.data());

        const double flux = un * area;
        div[P] += flux / mesh_.cellVolume(P);
        if (!isBoundary)
            div[N] -= flux / mesh_.cellVolume(N);
        if (sumFlux) *sumFlux += std::abs(flux);
    }
}

} // namespace fvm