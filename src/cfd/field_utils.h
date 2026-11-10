#pragma once

#include "cfd/solution_state.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/SSTModel.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace cfd {
namespace field_utils {
namespace detail {

// Copy up to `n` elements from `src` to `dst`, clamped to both sizes.
// Requires `size()` and `operator[]` on both arguments. Centralizes the
// element-wise copy loop previously repeated across copyToField,
// copyFromField, copyVelocityToVectorField, scatterTurbulence and
// fillTurbulence.
template<typename Src, typename Dst>
inline void copyN(const Src& src, Dst& dst, std::size_t n) {
    const std::size_t m = std::min({src.size(), dst.size(), n});
    for (std::size_t i = 0; i < m; ++i) dst[i] = src[i];
}

} // namespace detail

// Copy a std::vector<double> into a fvm::Field<Dim>, clamped to the smaller size.
template<std::size_t Dim>
inline void copyToField(const std::vector<double>& src, fvm::Field<Dim>& dst) {
    detail::copyN(src, dst, std::min(src.size(), dst.size()));
}

// Copy a fvm::Field<Dim> into a std::vector<double> (resized to the field size).
template<std::size_t Dim>
inline void copyFromField(const fvm::Field<Dim>& src, std::vector<double>& dst) {
    const auto vals = src.values();
    dst.assign(vals.begin(), vals.end());
}

// Copy a fvm::Field<Dim> into a std::vector<double> of size n (clamped).
template<std::size_t Dim>
inline void copyFromField(const fvm::Field<Dim>& src, std::vector<double>& dst,
                         std::size_t n) {
    const auto vals = src.values();
    dst.resize(n);
    detail::copyN(vals, dst, std::min(n, vals.size()));
}

// Copy velocity rows: std::vector<std::vector<double>> -> VectorField<Dim>.
template<std::size_t Dim>
inline void copyVelocityToVectorField(const std::vector<std::vector<double>>& src,
                                     fvm::VectorField<Dim>& dst) {
    for (std::size_t d = 0; d < Dim; ++d) {
        if (d >= src.size()) break;
        detail::copyN(src[d], dst[d], std::min(dst[d].size(), src[d].size()));
    }
}

// Copy velocity rows: VectorField<Dim> -> std::vector<std::vector<double>> (resized).
template<std::size_t Dim>
inline void copyVelocityFromVectorField(const fvm::VectorField<Dim>& src,
                                       std::vector<std::vector<double>>& dst) {
    dst.resize(Dim);
    for (std::size_t d = 0; d < Dim; ++d) {
        const auto vals = src[d].values();
        dst[d].assign(vals.begin(), vals.end());
    }
}

// Turbulence: snapshot -> fields.
template<std::size_t Dim>
inline void scatterTurbulence(const cfd::TurbulenceState& state,
                             fvm::Field<Dim>& kField,
                             fvm::Field<Dim>& omegaField,
                             fvm::Field<Dim>& nutField) {
    const std::size_t m = std::min({state.k.size(), state.omega.size(), state.nut.size(),
                                     kField.size(), omegaField.size(), nutField.size()});
    detail::copyN(state.k, kField, m);
    detail::copyN(state.omega, omegaField, m);
    detail::copyN(state.nut, nutField, m);
    // Zero out any destination cells not covered by the saved state so stale
    // values cannot survive into the live field.
    for (std::size_t i = m; i < kField.size(); ++i) kField[i] = 0.0;
    for (std::size_t i = m; i < omegaField.size(); ++i) omegaField[i] = 0.0;
    for (std::size_t i = m; i < nutField.size(); ++i) nutField[i] = 0.0;
}

// Turbulence: model -> snapshot.
template<std::size_t Dim>
inline void fillTurbulence(const fvm::SSTModel<Dim>& model,
                          cfd::TurbulenceState& state,
                          std::size_t n) {
    state.k.resize(n);
    state.omega.resize(n);
    state.nut.resize(n);
    const auto& k = model.k();
    const auto& omega = model.omega();
    const auto& nut = model.nut();
    const auto kv = k.values();
    const auto ov = omega.values();
    const auto nv = nut.values();
    const std::size_t m = std::min({n, kv.size(), ov.size(), nv.size()});
    detail::copyN(kv, state.k, m);
    detail::copyN(ov, state.omega, m);
    detail::copyN(nv, state.nut, m);
    // Only mark valid when the full copy succeeded; partial data remains
    // zero-filled and must not be trusted by consumers.
    state.valid = (m == n);
}

} // namespace field_utils
} // namespace cfd
