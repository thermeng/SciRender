#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace cfd {

// Residual equations that can be gated by the stopping-criteria test.
//
// The order matches the printed iteration table (continuity, then momentum
// components, then turbulence). Momentum W and the turbulence equations are
// only applicable in 3D / when turbulence is enabled respectively; the UI
// filters them out via EquationAvailability.
enum class ResidualEquation {
    Continuity,
    MomentumX,
    MomentumY,
    MomentumW,
    TurbulenceK,
    TurbulenceOmega,
};

constexpr std::size_t kResidualEquationCount = 6;

// Whether an equation can participate in the convergence gate for a given
// solve context.
enum class EquationAvailability {
    Always,       // every mesh/dimension/mode
    Requires3D,   // Momentum W only exists in 3D
    RequiresTurbulence, // k/omega only exist when turbulence is enabled
};

struct EquationDescriptor {
    ResidualEquation id;
    const char* name;        // short label shown in the UI
    const char* tip;         // tooltip
    EquationAvailability availability;
};

// All defined equations, in iteration-table order.
const std::vector<EquationDescriptor>& equationCatalog();

// Per-equation convergence criterion. tolerance == nullopt means "inherit the
// global defaultTolerance".
struct EquationCriterion {
    bool enabled = true;
    std::optional<double> tolerance;

    bool operator==(const EquationCriterion&) const = default;
};

// Steady (SIMPLE) stopping policy.
struct SteadyStopping {
    // Safety cap: if the residual gate never fires, stop here and report
    // "max iterations reached" rather than running forever.
    std::size_t maxIterations = 500;
    // Optional exact iteration budget: when set, the residual tolerance gate
    // is bypassed and the solver runs exactly this many iterations.
    std::optional<std::size_t> fixedIterations;
    // Consecutive iterations every active equation must stay below its
    // tolerance before the run is declared converged.
    int convergenceHoldIterations = 3;

    bool operator==(const SteadyStopping&) const = default;
};

// Transient (PISO/SIMPLE transient) stopping policy.
struct TransientStopping {
    // Global end time; the run stops here regardless of residuals.
    double tEnd = 1.0;
    // Maximum pressure-corrector sweeps per time step.
    int maxOuterCorrectors = 3;
    // PISO residual reduction: run at least this many correctors, then stop
    // once the continuity residual has fallen to `residualReduction` times its
    // value at the start of the time step.
    int minOuterCorrectors = 2;
    // 0 disables the reduction test; the loop then always runs to the cap.
    double residualReduction = 0.25;

    bool operator==(const TransientStopping&) const = default;
};

// Single source of truth for stopping criteria across UI, case file I/O and
// the solver. All fields are plain data; validation lives in validate().
struct StoppingCriteria {
    // Tolerance applied to every equation that does not override it.
    double defaultTolerance = 1e-4;
    std::vector<EquationCriterion> equations; // size kResidualEquationCount

    SteadyStopping steady;
    TransientStopping transient;

    bool operator==(const StoppingCriteria&) const = default;
    bool operator!=(const StoppingCriteria&) const = default;
};

// Defaults matching the previous shipped behaviour.
StoppingCriteria defaultStoppingCriteria();

// Validation ranges (kept in one place so UI, model and codec agree).
struct StoppingCriteriaLimits {
    static constexpr double kTolMin = 1e-12;
    static constexpr double kTolMax = 1e-1;
    static constexpr std::size_t kIterMin = 1;
    static constexpr std::size_t kIterMax = 1'000'000;
    static constexpr int kHoldMin = 1;
    static constexpr int kHoldMax = 1'000;
    static constexpr double kTEndMin = 1e-9;
    static constexpr double kTEndMax = 1e9;
    static constexpr int kOuterMin = 1;
    static constexpr int kOuterMax = 100;
    static constexpr double kReductionMin = 0.0;
    static constexpr double kReductionMax = 1.0;
};

// Validation result: per-field errors plus an overall ok flag.
struct ValidationReport {
    bool ok = true;
    struct FieldError {
        std::string field;   // e.g. "defaultTolerance", "equations[1].tolerance"
        std::string message;
    };
    std::vector<FieldError> errors;
};

// Validate the criteria against limits and cross-field rules:
//   * tolerances must be finite, positive, within [kTolMin, kTolMax]
//   * iteration counts must be within [kIterMin, kIterMax]
//   * hold must be within [kHoldMin, kHoldMax]
//   * transient tEnd must be within [kTEndMin, kTEndMax]
//   * maxOuterCorrectors must be within [kOuterMin, kOuterMax]
//   * minOuterCorrectors must be within [kOuterMin, kOuterMax] and <= max
//   * residualReduction must be within [0, 1]
ValidationReport validateStoppingCriteria(const StoppingCriteria& s);

// Resolve the effective tolerance for an equation (override if set, else
// defaultTolerance). Returns std::nullopt if the equation is not enabled.
std::optional<double> effectiveTolerance(const StoppingCriteria& s,
                                          ResidualEquation id);

// Format a tolerance in scientific notation with enough precision to round-trip.
std::string formatTolerance(double v);

// Parse a tolerance string (locale-independent). Returns std::nullopt on
// failure.
std::optional<double> parseTolerance(const std::string& text);

} // namespace cfd
