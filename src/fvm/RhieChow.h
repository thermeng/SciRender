#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "fvm/Types.h"
#include <array>
#include <cstddef>
#include <vector>

namespace fvm {

template<std::size_t Dim>
inline double rhieChowCorrection(
    const Mesh<Dim>& mesh,
    std::size_t f,
    const std::array<std::vector<double>, Dim>& dCoeff,
    const Field<Dim>& pressure,
    const std::array<Field<Dim>, Dim>& pressureGrad,
    const std::vector<char>& pMask) {

    const std::size_t P = mesh.owner(f);
    const std::size_t N = mesh.neighbor(f);

    const auto& fn = mesh.faceNormal(f);
    const auto& xP = mesh.cellCenter(P);
    const auto& xN = mesh.cellCenter(N);

    std::array<double, Dim> dVec{};
    for (std::size_t dd = 0; dd < Dim; ++dd)
        dVec[dd] = xN[dd] - xP[dd];

    double dotSfd = 0.0;
    for (std::size_t dd = 0; dd < Dim; ++dd)
        dotSfd += fn[dd] * dVec[dd];

    double dist = mesh.faceDist(f);

    double w = mesh.faceWeight(f);
    double dFaceN = 0.0;
    for (std::size_t d = 0; d < Dim; ++d) {
        double nd = fn[d];
        double dP_eff = pMask[P] ? 0.0 : dCoeff[d][P];
        double dN_eff = pMask[N] ? 0.0 : dCoeff[d][N];
        dFaceN += ((1.0 - w) * dP_eff + w * dN_eff) * nd * nd;
    }

    // The EXACT face flux that correctVelocity() removes: the least-squares
    // gradient of p' interpolated to the face as the vector (d * grad p) and
    // dotted with the normal. Using the actual interpolated (d * grad p) --
    // not dFaceN * (interpolated grad p) -- is what makes the conversion exact
    // for variable dCoeff and non-orthogonal meshes: the difference
    // dFaceN * gradP_interp_n - interpolate(d * grad p) is O(1) on a skewed
    // tet with a varying momentum diagonal, which is precisely the mass
    // imbalance that made the compact operator unusable on unstructured meshes.
    double dGradInterp_n = 0.0;
    for (std::size_t d = 0; d < Dim; ++d) {
        const double dP_eff = pMask[P] ? 0.0 : dCoeff[d][P];
        const double dN_eff = pMask[N] ? 0.0 : dCoeff[d][N];
        dGradInterp_n += ((1.0 - w) * dP_eff * pressureGrad[d][P] +
                          w * dN_eff * pressureGrad[d][N]) * fn[d];
    }

    double gradP_face_n = (pressure[N] - pressure[P]) * dotSfd / (dist * dist);

    // Convert the LS face flux (what correctVelocity produces) into the compact
    // two-point face flux the SPD operator is assembled from.
    return dGradInterp_n - dFaceN * gradP_face_n;
}

} // namespace fvm
