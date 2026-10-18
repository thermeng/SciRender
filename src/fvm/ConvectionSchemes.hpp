#pragma once

#include <concepts>
#include <cmath>
#include <cstddef>

namespace fvm {

template<typename T>
concept ConvectionScheme = requires(T t, double r) {
    { T::name() } noexcept -> std::convertible_to<const char*>;
    { T::applyLimiter(r) } noexcept -> std::same_as<double>;
};

struct Upwind {
    static constexpr const char* name() { return "Upwind"; }
    static constexpr double applyLimiter(double) { return 0.0; }
};

struct Central {
    static constexpr const char* name() { return "Central"; }
    static constexpr double applyLimiter(double) { return 1.0; }
};

} // namespace fvm
