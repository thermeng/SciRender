#pragma once

#include "mesh/BoundaryUtils.h"
#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/SIMPLETypes.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace fvm {

inline constexpr std::size_t kNoNeighbor = static_cast<std::size_t>(-1);

// ---------------------------------------------------------------------------
// Prescribed (Dirichlet) cell values
// ---------------------------------------------------------------------------

// A prescribed value is a CELL value. The mask and the value list are two
// independent optional buffers that callers size separately, so a masked entry
// may only be read when BOTH of them cover the cell.
inline bool prescribedValue(const std::vector<std::vector<char>>* mask,
                            const std::vector<std::vector<double>>* values,
                            std::size_t component, std::size_t cell, double& out) {
    if (!mask || !values) return false;
    if (component >= mask->size() || component >= values->size()) return false;
    if (cell >= (*mask)[component].size() || cell >= (*values)[component].size()) return false;
    if (!(*mask)[component][cell]) return false;
    out = (*values)[component][cell];
    return true;
}

inline bool prescribedValue(const std::vector<std::vector<char>>& mask,
                            const std::vector<std::vector<double>>& values,
                            std::size_t component, std::size_t cell, double& out) {
    return prescribedValue(&mask, &values, component, cell, out);
}

// Single-component form, for callers that assemble one velocity component at a
// time and already hold that component's mask/value pair.
inline bool prescribedValue(const std::vector<char>& mask,
                            const std::vector<double>& values,
                            std::size_t cell, double& out) {
    if (cell >= mask.size() || cell >= values.size()) return false;
    if (!mask[cell]) return false;
    out = values[cell];
    return true;
}

// ---------------------------------------------------------------------------
// Face velocity
// ---------------------------------------------------------------------------

template<std::size_t Dim>
inline double dotNormal(const std::array<double, Dim>& v, const double* normal) {
    double sum = 0.0;
    for (std::size_t d = 0; d < Dim; ++d) sum += v[d] * normal[d];
    return sum;
}

// Index of the component that dominates the (unit) normal. A slip/symmetry
// face imposes the no-penetration condition on that component only; the
// tangential components are free.
template<std::size_t Dim>
inline std::size_t dominantNormalComponent(const double* normal) {
    std::size_t dominant = 0;
    double maxAbs = 0.0;
    for (std::size_t d = 0; d < Dim; ++d) {
        const double a = std::abs(normal[d]);
        if (a > maxAbs) { maxAbs = a; dominant = d; }
    }
    return dominant;
}

// Velocity at the face between `P` and `N`, or at a boundary face when
// `N == kNoNeighbor` (the owner is extrapolated, `weight` is unused).
//
// A cell whose velocity is prescribed owns the WHOLE face it shares with a
// neighbour, and the NEIGHBOUR takes priority. The momentum assembly, the
// divergence, the pressure correction and the continuity residual all need
// this same value: any difference between them is a mass imbalance the
// pressure equation tries to cancel, which corrupts the pressure next to the
// walls.
template<std::size_t Dim>
inline std::array<double, Dim> interpolateFaceVelocity(
    const VectorField<Dim>& velocity,
    std::size_t P, std::size_t N, double weight,
    const std::vector<std::vector<char>>* mask = nullptr,
    const std::vector<std::vector<double>>* values = nullptr) {
    std::array<double, Dim> face{};
    for (std::size_t d = 0; d < Dim; ++d) {
        double prescribed = 0.0;
        if (N != kNoNeighbor && prescribedValue(mask, values, d, N, prescribed)) {
            face[d] = prescribed;
        } else if (prescribedValue(mask, values, d, P, prescribed)) {
            face[d] = prescribed;
        } else if (N != kNoNeighbor) {
            face[d] = (1.0 - weight) * velocity[d][P] + weight * velocity[d][N];
        } else {
            face[d] = velocity[d][P];
        }
    }
    return face;
}

// What a boundary face imposes on the velocity. Every VelocityBCType maps onto
// exactly one of these three behaviours; that mapping is the entire BC policy
// for a face and must not be re-derived per consumer.
enum class FaceVelocityKind {
    Wall,       // NoSlip, Symmetry, SlipWall: no penetration, U.n = 0
    Prescribed, // Inlet, MovingWall: the whole face velocity is given
    Outflow,    // Outlet, PressureOutlet: zero gradient + backflow rule
};

// Classification predicates for consumers that only need to know which
// behaviour a face has (diffusion type, tangential projection, ...). The value
// itself always comes from boundaryFaceState() below.
inline bool isPrescribedVelocityBC(VelocityBCType t) {
    return t == VelocityBCType::Inlet || t == VelocityBCType::MovingWall;
}

inline bool isOutflowVelocityBC(VelocityBCType t) {
    return t == VelocityBCType::Outlet || t == VelocityBCType::PressureOutlet;
}

inline bool isSlipVelocityBC(VelocityBCType t) {
    return t == VelocityBCType::Symmetry || t == VelocityBCType::SlipWall;
}

template<std::size_t Dim>
struct BoundaryFaceState {
    FaceVelocityKind kind = FaceVelocityKind::Outflow;
    // Face velocity, resolved per component. Wall faces carry the tangential
    // projection so convective upwinding can read a component directly.
    std::array<double, Dim> velocity{};
    // Dirichlet value of an inflow through an outlet (backflow).
    std::array<double, Dim> inflowVelocity{};
    // U.n including the no-penetration and backflow rules. This is the only
    // value a flux/divergence consumer should use.
    double normalVelocity = 0.0;
};

template<std::size_t Dim>
inline BoundaryFaceState<Dim> boundaryFaceState(
    const VectorField<Dim>& velocity, std::size_t P,
    const double* normal, const FaceVelocityBC& fbc) {
    BoundaryFaceState<Dim> state;

    switch (fbc.type) {
        case VelocityBCType::NoSlip:
            state.kind = FaceVelocityKind::Wall;
            state.normalVelocity = 0.0;
            return state;

        case VelocityBCType::Symmetry:
        case VelocityBCType::SlipWall: {
            state.kind = FaceVelocityKind::Wall;
            std::array<double, Dim> interior{};
            for (std::size_t d = 0; d < Dim; ++d) interior[d] = velocity[d][P];
            const double un = dotNormal(interior, normal);
            for (std::size_t d = 0; d < Dim; ++d)
                state.velocity[d] = interior[d] - un * normal[d];
            // The normal component is removed by construction; pin it to zero
            // so wall faces are exactly non-penetrating in every consumer.
            state.normalVelocity = 0.0;
            return state;
        }

        case VelocityBCType::MovingWall:
        case VelocityBCType::Inlet:
            state.kind = FaceVelocityKind::Prescribed;
            state.velocity[0] = fbc.u;
            state.velocity[1] = fbc.v;
            if constexpr (Dim == 3) state.velocity[2] = fbc.w;
            state.normalVelocity = dotNormal(state.velocity, normal);
            return state;

        default: // Outlet, PressureOutlet
            state.kind = FaceVelocityKind::Outflow;
            for (std::size_t d = 0; d < Dim; ++d) state.velocity[d] = velocity[d][P];
            state.inflowVelocity[0] = fbc.backflowU;
            state.inflowVelocity[1] = fbc.backflowV;
            if constexpr (Dim == 3) state.inflowVelocity[2] = fbc.backflowW;
            {
                std::array<double, Dim> normalArr{};
                for (std::size_t d = 0; d < Dim; ++d) normalArr[d] = normal[d];
                state.normalVelocity = applyBackflowCorrection<Dim>(
                    dotNormal(state.velocity, normal), normalArr, fbc);
            }
            return state;
    }
    return state;
}

} // namespace fvm
