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

    double gradP_interp_n = 0.0;
    for (std::size_t d = 0; d < Dim; ++d)
        gradP_interp_n += ((1.0 - w) * pressureGrad[d][P] + w * pressureGrad[d][N]) * fn[d];

    double gradP_face_n = (pressure[N] - pressure[P]) * dotSfd / (dist * dist);

    return dFaceN * (gradP_interp_n - gradP_face_n);
}

} // namespace fvm
