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

// Copy a std::vector<double> into a fvm::Field<Dim>, clamped to the smaller size.
template<std::size_t Dim>
inline void copyToField(const std::vector<double>& src, fvm::Field<Dim>& dst) {
    const std::size_t n = std::min(src.size(), dst.size());
    for (std::size_t i = 0; i < n; ++i) dst[i] = src[i];
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
    dst.resize(n);
    const auto vals = src.values();
    const std::size_t m = std::min(n, vals.size());
    for (std::size_t i = 0; i < m; ++i) dst[i] = vals[i];
}

// Copy velocity rows: std::vector<std::vector<double>> -> VectorField<Dim>.
template<std::size_t Dim>
inline void copyVelocityToVectorField(const std::vector<std::vector<double>>& src,
                                     fvm::VectorField<Dim>& dst) {
    for (std::size_t d = 0; d < Dim; ++d) {
        if (d >= src.size()) break;
        const std::size_t m = std::min(dst[d].size(), src[d].size());
        for (std::size_t i = 0; i < m; ++i) dst[d][i] = src[d][i];
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
    for (std::size_t i = 0; i < m; ++i) {
        kField[i] = state.k[i];
        omegaField[i] = state.omega[i];
        nutField[i] = state.nut[i];
    }
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
    state.valid = true;
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
    for (std::size_t i = 0; i < m; ++i) {
        state.k[i] = kv[i];
        state.omega[i] = ov[i];
        state.nut[i] = nv[i];
    }
}

} // namespace field_utils
} // namespace cfd
