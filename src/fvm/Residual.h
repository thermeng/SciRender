#pragma once

#include "mesh/Mesh.h"
#include "mesh/BoundaryUtils.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/Divergence.h"
#include "fvm/FaceState.h"
#include "fvm/RhieChow.h"
#include "fvm/SIMPLETypes.h"
#include <array>
#include <cstddef>
#include <vector>
#include <cmath>
#include <algorithm>
#include <utility>
#include <cstdlib>

namespace fvm {

struct ResidualResult {
    double continuity;
    double momU;
    double momV;
    double momW;
};

template<std::size_t Dim>
inline ResidualResult computeResiduals(
    const Mesh<Dim>& mesh,
    const VectorField<Dim>& velocity,
    const VectorField<Dim>& velocityOld,
    const std::array<Field<Dim>, Dim>& diag,
    Field<Dim>& divBuffer,
    const std::vector<std::vector<char>>& dirichletMask,
    const std::vector<std::vector<double>>& dirichletVals,
    const std::vector<FaceVelocityBC>* faceBC = nullptr,
    const std::vector<char>* faceHasBC = nullptr,
    const std::array<std::vector<double>, Dim>* dCoeff = nullptr,
    const Field<Dim>* pressure = nullptr,
    const std::array<Field<Dim>, Dim>* pressureGrad = nullptr,
    const std::vector<char>* pMask = nullptr,
    bool rhieChow = false) {

    // Compute divergence with faceBC-aware, distance-weighted face values
    // This mirrors PressureCorrection's divergence for consistency
    const std::size_t nF = mesh.nFaces();
    const std::size_t n = mesh.nCells();
    std::vector<char> emptyMask;
    const std::vector<char>& maskRef = pMask ? *pMask : emptyMask;
    for (std::size_t i = 0; i < n; ++i) divBuffer[i] = 0.0;
    double sumFlux = 0.0;
    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = mesh.owner(f);
        const std::size_t N = mesh.neighbor(f);
        const double* fn = mesh.faceNormalPtr(f);
        double area = mesh.faceArea(f);
        const bool hasBC = faceBC && faceHasBC &&
                           f < faceBC->size() && f < faceHasBC->size() && (*faceHasBC)[f];
        double un;
        if (N == kNoNeighbor && hasBC) {
            // The face BC owns the boundary value; the shared resolver applies
            // the no-penetration, prescribed and backflow rules in one place.
            un = boundaryFaceState<Dim>(velocity, P, fn, (*faceBC)[f]).normalVelocity;
        } else {
            // Interior, or a boundary without a face BC. A prescribed velocity
            // is the value of its CELL and the neighbour takes priority; this
            // is the same face value the momentum assembly and the pressure
            // correction use, so the continuity residual measures exactly the
            // imbalance the pressure correction drives to zero.
            const double w = (N == kNoNeighbor) ? 0.0 : mesh.faceWeight(f);
            un = dotNormal(interpolateFaceVelocity(velocity, P, N, w,
                                                   &dirichletMask, &dirichletVals), fn);
        }
        if (rhieChow && N != std::size_t(-1) && dCoeff && pressure && pressureGrad) {
            un += rhieChowCorrection(mesh, f, *dCoeff, *pressure, *pressureGrad, maskRef);
        }

        double flux = un * area;
        sumFlux += std::abs(flux);
        divBuffer[P] += flux / mesh.cellVolume(P);
        if (N != std::size_t(-1)) {
            divBuffer[N] -= flux / mesh.cellVolume(N);
        }
    }

    double maxDiv = 0.0;
    double sumAbsDivVol = 0.0;
    double totalVol = 0.0;
    #pragma omp parallel for reduction(max:maxDiv) reduction(+:sumAbsDivVol,totalVol) schedule(static)
    for (std::size_t i = 0; i < n; ++i) {
        if (mesh.isBoundary(i)) continue;
        double ad = std::abs(divBuffer[i]);
        if (ad > maxDiv) maxDiv = ad;
        sumAbsDivVol += ad * mesh.cellVolume(i);
        totalVol += mesh.cellVolume(i);
    }
    // Primary residual: sumAbsDivVol normalized by sumFlux (mass imbalance / total flux)
    // Fallback to maxDiv * L / Uchar if sumFlux is tiny (e.g., all walls)
    double contResid = 0.0;
    if (sumFlux > 1e-30) {
        contResid = sumAbsDivVol / sumFlux;
    } else {
        double maxUmag = 0.0;
        for (std::size_t i = 0; i < n; ++i) if (!mesh.isBoundary(i)) {
            double um2 = 0.0;
            for (std::size_t d = 0; d < Dim; ++d) um2 += velocity[d][i]*velocity[d][i];
            double um = std::sqrt(um2);
            if (um > maxUmag) maxUmag = um;
        }
        if (maxUmag < 1e-30) maxUmag = 1.0;
        double charLen = (totalVol>0)? std::pow(totalVol, 1.0/static_cast<double>(Dim)):1.0;
        contResid = maxDiv * charLen / maxUmag;
    }

    double maxUChange = 0.0, maxVChange = 0.0, maxWChange = 0.0;
    double maxU = 0.0, maxV = 0.0, maxW = 0.0;
    #pragma omp parallel for reduction(max:maxUChange,maxVChange,maxWChange,maxU,maxV,maxW) schedule(static)
    for (std::size_t i = 0; i < n; ++i) {
        if (mesh.isBoundary(i)) continue;
        double du = std::abs(velocity[0][i] - velocityOld[0][i]);
        if (du > maxUChange) maxUChange = du;
        double dv = std::abs(velocity[1][i] - velocityOld[1][i]);
        if (dv > maxVChange) maxVChange = dv;
        double au = std::abs(velocity[0][i]);
        if (au > maxU) maxU = au;
        double av = std::abs(velocity[1][i]);
        if (av > maxV) maxV = av;
        if constexpr (Dim == 3) {
            double dw = std::abs(velocity[2][i] - velocityOld[2][i]);
            if (dw > maxWChange) maxWChange = dw;
            double aw = std::abs(velocity[2][i]);
            if (aw > maxW) maxW = aw;
        }
    }
    double momScale = maxU;
    if (maxV > momScale) momScale = maxV;
    if constexpr (Dim==3) if (maxW > momScale) momScale = maxW;
    if (momScale < 1e-30) momScale = 1e-30;

    ResidualResult res;
    res.continuity = contResid;
    // Relative per-outer-iteration change. This is a useful diagnostic but NOT
    // a linear residual: it is proportional to (1-alphaU) times the correction,
    // so it measures iteration progress rather than how well the momentum
    // equation is satisfied. Callers that need a convergence criterion should
    // use the linear residual |A u - b| / sum|b| instead.
    res.momU = maxUChange / momScale;
    res.momV = maxVChange / momScale;
    if constexpr (Dim == 3) res.momW = maxWChange / momScale;
    else res.momW = 0.0;
    return res;
}

} // namespace fvm
