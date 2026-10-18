#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include <array>
#include <cmath>
#include <cstddef>

namespace fvm {

template<std::size_t Dim>
struct FaceGeometry {
    std::size_t P = 0;
    std::size_t N = 0;
    bool isBoundary = false;
    double area = 0.0;
    double dist = 0.0;
    std::array<double, Dim> normal{};
    std::array<double, Dim> delta{};
    double dotSfd = 0.0;
    double absDotSfd = 0.0;
};

template<std::size_t Dim>
inline FaceGeometry<Dim> computeFaceGeometry(const Mesh<Dim>& mesh, std::size_t f) {
    FaceGeometry<Dim> geo;
    geo.P = mesh.owner(f);
    geo.N = mesh.neighbor(f);
    geo.isBoundary = (geo.N == std::size_t(-1));
    geo.area = mesh.faceArea(f);
    geo.dist = mesh.faceDist(f);
    geo.normal = mesh.faceNormal(f);
    const double* xP = mesh.cellCenterPtr(geo.P);
    if (!geo.isBoundary) {
        const double* xN = mesh.cellCenterPtr(geo.N);
        for (std::size_t d = 0; d < Dim; ++d) {
            geo.delta[d] = xN[d] - xP[d];
            geo.dotSfd += geo.normal[d] * geo.delta[d];
        }
    } else {
        const double* xF = mesh.faceCenterPtr(f);
        for (std::size_t d = 0; d < Dim; ++d) {
            geo.delta[d] = xF[d] - xP[d];
            geo.dotSfd += geo.normal[d] * geo.delta[d];
        }
    }
    geo.absDotSfd = std::abs(geo.dotSfd);
    return geo;
}

template<std::size_t Dim>
inline std::array<double, Dim> computeNonOrthogonalCorrection(
    const Mesh<Dim>& mesh,
    std::size_t f,
    const FaceGeometry<Dim>& geo,
    const std::array<Field<Dim>, Dim>& grad) {
    std::array<double, Dim> Sf_nonorth{};
    double dot_f_dhat = geo.absDotSfd / geo.dist;
    for (std::size_t d = 0; d < Dim; ++d) {
        Sf_nonorth[d] = geo.area * geo.normal[d] - geo.area * dot_f_dhat * geo.delta[d] / geo.dist;
    }
    std::array<double, Dim> corr{};
    for (std::size_t d = 0; d < Dim; ++d) {
        corr[d] = grad[d][geo.P] * Sf_nonorth[d];
    }
    return corr;
}

template<std::size_t Dim>
inline void addDirichletContribution(
    Eigen::VectorXd& b,
    std::size_t P,
    std::size_t N,
    const std::vector<char>& dirichletMask,
    const std::vector<double>& dirichletValues,
    double coeff,
    bool negativeDiffusion,
    const std::array<double, Dim>& correctionP,
    const std::array<double, Dim>& correctionN) {
    if (negativeDiffusion) {
        if (!(dirichletMask.size() > P && dirichletMask[P])) {
            b(P) += coeff * (dirichletMask.size() > N && dirichletMask[N] ? dirichletValues[N] : 0.0);
            for (std::size_t d = 0; d < Dim; ++d) b(P) -= correctionP[d];
        }
        if (!(dirichletMask.size() > N && dirichletMask[N])) {
            b(N) += coeff * (dirichletMask.size() > P && dirichletMask[P] ? dirichletValues[P] : 0.0);
            for (std::size_t d = 0; d < Dim; ++d) b(N) += correctionN[d];
        }
    } else {
        if (!(dirichletMask.size() > P && dirichletMask[P])) {
            if (dirichletMask.size() > N && dirichletMask[N]) {
                b(P) += coeff * dirichletValues[N];
            }
            for (std::size_t d = 0; d < Dim; ++d) b(P) -= correctionP[d];
        }
        if (!(dirichletMask.size() > N && dirichletMask[N])) {
            if (dirichletMask.size() > P && dirichletMask[P]) {
                b(N) += coeff * dirichletValues[P];
            }
            for (std::size_t d = 0; d < Dim; ++d) b(N) += correctionN[d];
        }
    }
}

template<std::size_t Dim>
inline void addDiffusionTriplets(
    std::vector<Eigen::Triplet<double>>& triplets,
    std::vector<double>& diagSum,
    const FaceGeometry<Dim>& geo,
    double coeff,
    const std::vector<char>& dirichletMask,
    const std::vector<double>& dirichletValues) {
    bool maskP = dirichletMask.size() > geo.P && dirichletMask[geo.P];
    bool maskN = dirichletMask.size() > geo.N && dirichletMask[geo.N];
    if (geo.dotSfd < 0.0) {
        if (!maskP) {
            diagSum[geo.P] += coeff;
        }
        if (!maskN) {
            diagSum[geo.N] += coeff;
        }
    } else {
        if (!maskP) {
            if (maskN) {
                // handled in b
            } else {
                triplets.emplace_back(static_cast<Eigen::Index>(geo.P), static_cast<Eigen::Index>(geo.N), -coeff);
            }
            diagSum[geo.P] += coeff;
        }
        if (!maskN) {
            if (maskP) {
                // handled in b
            } else {
                triplets.emplace_back(static_cast<Eigen::Index>(geo.N), static_cast<Eigen::Index>(geo.P), -coeff);
            }
            diagSum[geo.N] += coeff;
        }
    }
}

} // namespace fvm
