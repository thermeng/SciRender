#include "fvm/Divergence.h"
#include <algorithm>

namespace fvm {

template<std::size_t Dim>
Field<Dim> computeDivergence(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity) {
    Field<Dim> div(mesh, 0.0);
    computeDivergenceInto(mesh, velocity, div);
    return div;
}

template<std::size_t Dim>
void computeDivergenceInto(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity, Field<Dim>& div) {
    // WARNING: This overload uses interior cell velocity at boundary faces.
    // It is INCORRECT for meshes with Dirichlet BCs (walls, inlets, etc.).
    // Use the 5-arg overload with dirichletMask/dirichletValues for correct results.
    for (std::size_t i = 0; i < mesh.nCells(); ++i) div[i] = 0.0;

    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        const std::size_t P = mesh.owner(f);
        const std::size_t N = mesh.neighbor(f);
        const bool isBoundary = (N == std::size_t(-1));

        // Pre-cache face normal and area for branch-friendly access
        const auto& nrm = mesh.faceNormal(f);
        const auto  area = mesh.faceArea(f);

        double w = isBoundary ? 0.0 : mesh.faceWeight(f);
        double un = 0.0;
        for (std::size_t d = 0; d < Dim; ++d) {
            double uFace = isBoundary ? velocity[d][P]
                                      : (1.0 - w) * velocity[d][P] + w * velocity[d][N];
            un += uFace * nrm[d];
        }

        double flux = un * area;
        div[P] += flux / mesh.cellVolume(P);
        if (!isBoundary) {
            div[N] -= flux / mesh.cellVolume(N);
        }
    }
}

template<std::size_t Dim>
double maxDivergence(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity) {
    Field<Dim> div(mesh, 0.0);
    computeDivergenceInto(mesh, velocity, div);
    double maxDiv = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i)) continue;
        maxDiv = std::max(maxDiv, std::abs(div[i]));
    }
    return maxDiv;
}

template<std::size_t Dim>
void computeDivergenceInto(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity, Field<Dim>& div,
                           const std::vector<std::vector<char>>& dirichletMask,
                           const std::vector<std::vector<double>>& dirichletValues) {
    for (std::size_t i = 0; i < mesh.nCells(); ++i) div[i] = 0.0;

    const bool hasMask = !dirichletMask.empty() && !dirichletMask[0].empty();

    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        const std::size_t P = mesh.owner(f);
        const std::size_t N = mesh.neighbor(f);
        const bool isBoundary = (N == std::size_t(-1));

        const auto& nrm = mesh.faceNormal(f);
        const auto  area = mesh.faceArea(f);

        double w = isBoundary ? 0.0 : mesh.faceWeight(f);
        double un = 0.0;
        for (std::size_t d = 0; d < Dim; ++d) {
            double uFace;
            if (isBoundary) {
                uFace = (hasMask && dirichletMask[d][P])
                            ? dirichletValues[d][P]
                            : velocity[d][P];
            } else {
                uFace = (1.0 - w) * velocity[d][P] + w * velocity[d][N];
            }
            un += uFace * nrm[d];
        }

        double flux = un * area;
        div[P] += flux / mesh.cellVolume(P);
        if (!isBoundary) {
            div[N] -= flux / mesh.cellVolume(N);
        }
    }
}

template<std::size_t Dim>
void computeDivergenceInto(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity, Field<Dim>& div,
                           double& sumFlux) {
    const std::size_t n = mesh.nCells();
    for (std::size_t i = 0; i < n; ++i) div[i] = 0.0;

    const std::size_t nF = mesh.nFaces();
    sumFlux = 0.0;

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = mesh.owner(f);
        const std::size_t N = mesh.neighbor(f);
        const bool isBoundary = (N == std::size_t(-1));

        const auto& nrm = mesh.faceNormal(f);
        const auto  area = mesh.faceArea(f);

        double w = isBoundary ? 0.0 : mesh.faceWeight(f);
        double un = 0.0;
        for (std::size_t d = 0; d < Dim; ++d) {
            double uFace = isBoundary ? velocity[d][P]
                                      : (1.0 - w) * velocity[d][P] + w * velocity[d][N];
            un += uFace * nrm[d];
        }

        double flux = un * area;
        div[P] += flux / mesh.cellVolume(P);
        if (!isBoundary) {
            div[N] -= flux / mesh.cellVolume(N);
        }
        sumFlux += std::abs(flux);
    }
}

template Field<2> computeDivergence<2>(const Mesh<2>&, const VectorField<2>&);
template Field<3> computeDivergence<3>(const Mesh<3>&, const VectorField<3>&);

template void computeDivergenceInto<2>(const Mesh<2>&, const VectorField<2>&, Field<2>&);
template void computeDivergenceInto<3>(const Mesh<3>&, const VectorField<3>&, Field<3>&);

template void computeDivergenceInto<2>(const Mesh<2>&, const VectorField<2>&, Field<2>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&);
template void computeDivergenceInto<3>(const Mesh<3>&, const VectorField<3>&, Field<3>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&);

template void computeDivergenceInto<2>(const Mesh<2>&, const VectorField<2>&, Field<2>&, double&);
template void computeDivergenceInto<3>(const Mesh<3>&, const VectorField<3>&, Field<3>&, double&);

template double maxDivergence<2>(const Mesh<2>&, const VectorField<2>&);
template double maxDivergence<3>(const Mesh<3>&, const VectorField<3>&);

} // namespace fvm
