#pragma once

#include "mesh/Mesh.h"
#include "fvm/SIMPLETypes.h"
#include <array>
#include <cstddef>
#include <functional>

namespace fvm {

// Templated to avoid std::function heap allocation per call (P2)
template<std::size_t Dim, typename F>
inline void forEachBoundaryCell(const Mesh<Dim>& mesh, std::size_t d, int sign, F&& callback) {
    std::array<std::size_t, Dim> c{};
    auto traverse = [&](auto&& self, std::size_t dim) -> void {
        if (dim == Dim) {
            callback(mesh.cellIndex(c));
            return;
        }
        if (dim == d) {
            c[dim] = (sign < 0) ? 0 : mesh.n(dim) - 1;
            self(self, dim + 1);
        } else {
            for (std::size_t i = 0; i < mesh.n(dim); ++i) {
                c[dim] = i;
                self(self, dim + 1);
            }
        }
    };
    traverse(traverse, 0);
}

template<std::size_t Dim, typename F>
inline void forEachBoundaryFace(const Mesh<Dim>& mesh, F&& callback) {
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) == std::size_t(-1)) {
            callback(f, mesh.owner(f));
        }
    }
}

template<std::size_t Dim, typename F>
inline void forEachBoundaryFace(const Mesh<Dim>& mesh, std::size_t d, int sign, F&& callback) {
    const auto& cached = mesh.boundaryFaces(d, sign);
    if (!cached.empty()) {
        for (std::size_t f : cached) callback(f, mesh.owner(f));
        return;
    }
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) == std::size_t(-1)) {
            const auto& normal = mesh.faceNormal(f);
            const double nd = normal[d];
            double maxComp = 0.0;
            for (std::size_t dd = 0; dd < Dim; ++dd)
                maxComp = std::max(maxComp, std::abs(normal[dd]));
            if (std::abs(nd) >= 0.9 * maxComp
                && ((sign < 0 && nd < -0.5) || (sign > 0 && nd > 0.5))) {
                callback(f, mesh.owner(f));
            }
        }
    }
}

// Shared backflow correction for PressureOutlet/Outlet BCs.
// Given base normal velocity (un) and face BC, applies Fluent-like backflow handling.
// Returns corrected un.
template<std::size_t Dim>
inline double applyBackflowCorrection(double un,
                                       const std::array<double, Dim>& fn,
                                       const FaceVelocityBC& fbc) {
    if (fbc.type != VelocityBCType::PressureOutlet && fbc.type != VelocityBCType::Outlet) {
        return un;
    }
    if (un >= 0.0) return un; // outflow - no correction needed

    if (fbc.suppressBackflow) {
        return 0.0; // clamp to prevent backflow
    }
    // Use backflow Dirichlet values
    double unb = 0.0;
    for (std::size_t d = 0; d < Dim; ++d) {
        double bf = (d == 0) ? fbc.backflowU : (d == 1) ? fbc.backflowV : fbc.backflowW;
        unb += bf * fn[d];
    }
    return unb;
}

// Backward compat overloads for std::function (kept for external callers, not used in hot path)
template<std::size_t Dim>
void forEachBoundaryCell(const Mesh<Dim>& mesh, std::size_t d, int sign,
                         std::function<void(std::size_t)> callback);

template<std::size_t Dim>
void forEachBoundaryFace(const Mesh<Dim>& mesh, std::function<void(std::size_t, std::size_t)> callback);

template<std::size_t Dim>
void forEachBoundaryFace(const Mesh<Dim>& mesh, std::size_t d, int sign,
                         std::function<void(std::size_t, std::size_t)> callback);

} // namespace fvm
