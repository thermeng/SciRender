#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"

#include <cstddef>
#include <cmath>
#include <vector>

namespace fvm {

namespace detail {
inline bool prescribedValue(const std::vector<std::vector<char>>& mask,
                            const std::vector<std::vector<double>>& values,
                            std::size_t d, std::size_t cell, double& out) {
    if (d >= mask.size() || mask[d].empty() || cell >= mask[d].size()) return false;
    if (!mask[d][cell]) return false;
    if (d >= values.size() || cell >= values[d].size()) return false;
    out = values[d][cell];
    return true;
}
} // namespace detail

// Single owner of face geometry + the divergence assembly. Future consumers
// (Momentum, PressureCorrection, VelocityCorrection) will call these helpers
// instead of re-implementing the face loops.
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
        const bool isBoundary = (N == std::size_t(-1));

        const auto& nrm = mesh_.faceNormal(f);
        const double area = mesh_.faceArea(f);
        const double w = isBoundary ? 0.0 : mesh_.faceWeight(f);

        double un = 0.0;
        for (std::size_t d = 0; d < Dim; ++d) {
            double prescribed = 0.0;
            double uFace;
            if (isBoundary) {
                uFace = detail::prescribedValue(dirichletMask, dirichletValues, d, P, prescribed)
                            ? prescribed
                            : velocity[d][P];
            } else if (detail::prescribedValue(dirichletMask, dirichletValues, d, N, prescribed)) {
                uFace = prescribed;
            } else if (detail::prescribedValue(dirichletMask, dirichletValues, d, P, prescribed)) {
                uFace = prescribed;
            } else {
                uFace = (1.0 - w) * velocity[d][P] + w * velocity[d][N];
            }
            un += uFace * nrm[d];
        }

        const double flux = un * area;
        div[P] += flux / mesh_.cellVolume(P);
        if (!isBoundary)
            div[N] -= flux / mesh_.cellVolume(N);
        if (sumFlux) *sumFlux += std::abs(flux);
    }
}

} // namespace fvm