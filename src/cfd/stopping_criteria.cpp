#include "cfd/stopping_criteria.h"

#include <algorithm>
#include <limits>

namespace cfd {

namespace {

const std::vector<EquationDescriptor>& catalog() {
    static const std::vector<EquationDescriptor> kCatalog{
        {ResidualEquation::Continuity,    "Continuity",
         "Mass imbalance residual.", EquationAvailability::Always},
        {ResidualEquation::MomentumX,     "Momentum U",
         "X-momentum linear residual.", EquationAvailability::Always},
        {ResidualEquation::MomentumY,     "Momentum V",
         "Y-momentum linear residual.", EquationAvailability::Always},
        {ResidualEquation::MomentumW,     "Momentum W",
         "Z-momentum linear residual (3D only).", EquationAvailability::Requires3D},
        {ResidualEquation::TurbulenceK,   "Turbulence k",
         "Turbulent kinetic energy residual.", EquationAvailability::RequiresTurbulence},
        {ResidualEquation::TurbulenceOmega, "Turbulence omega",
         "Specific dissipation rate residual.", EquationAvailability::RequiresTurbulence},
    };
    return kCatalog;
}

} // namespace

const std::vector<EquationDescriptor>& equationCatalog() {
    return catalog();
}

StoppingCriteria defaultStoppingCriteria() {
    StoppingCriteria s;
    s.equations.resize(kResidualEquationCount);
    for (auto& eq : s.equations) eq.enabled = true;
    return s;
}

ValidationReport validateStoppingCriteria(const StoppingCriteria& s) {
    ValidationReport report;

    auto checkTol = [&](const std::string& field, std::optional<double> v) {
        if (!v.has_value()) return;
        double t = *v;
        if (!std::isfinite(t) || t <= 0.0 ||
            t < StoppingCriteriaLimits::kTolMin ||
            t > StoppingCriteriaLimits::kTolMax) {
            report.ok = false;
            report.errors.push_back({field,
                "tolerance must be finite, positive and within [1e-12, 1e-1]"});
        }
    };

    checkTol("defaultTolerance", s.defaultTolerance);

    for (std::size_t i = 0; i < s.equations.size() && i < kResidualEquationCount; ++i) {
        const auto& eq = s.equations[i];
        const char* name = equationCatalog()[i].name;
        std::string field = "equations[" + std::to_string(i) + "].tolerance (" + name + ")";
        checkTol(field, eq.tolerance);
    }

    auto checkIter = [&](const std::string& field, std::size_t v) {
        if (v < StoppingCriteriaLimits::kIterMin ||
            v > StoppingCriteriaLimits::kIterMax) {
            report.ok = false;
            report.errors.push_back({field,
                "iteration count must be within [1, 1000000]"});
        }
    };

    checkIter("steady.maxIterations", s.steady.maxIterations);
    if (s.steady.fixedIterations) {
        checkIter("steady.fixedIterations", *s.steady.fixedIterations);
    }

    if (s.steady.convergenceHoldIterations < StoppingCriteriaLimits::kHoldMin ||
        s.steady.convergenceHoldIterations > StoppingCriteriaLimits::kHoldMax) {
        report.ok = false;
        report.errors.push_back({"steady.convergenceHoldIterations",
            "hold iterations must be within [1, 1000]"});
    }

    if (!std::isfinite(s.transient.tEnd) ||
        s.transient.tEnd < StoppingCriteriaLimits::kTEndMin ||
        s.transient.tEnd > StoppingCriteriaLimits::kTEndMax) {
        report.ok = false;
        report.errors.push_back({"transient.tEnd",
            "end time must be finite and within [1e-9, 1e9]"});
    }

    if (s.transient.maxOuterCorrectors < StoppingCriteriaLimits::kOuterMin ||
        s.transient.maxOuterCorrectors > StoppingCriteriaLimits::kOuterMax) {
        report.ok = false;
        report.errors.push_back({"transient.maxOuterCorrectors",
            "max outer correctors must be within [1, 100]"});
    }

    return report;
}

std::optional<double> effectiveTolerance(const StoppingCriteria& s,
                                          ResidualEquation id) {
    std::size_t idx = static_cast<std::size_t>(id);
    if (idx >= s.equations.size() || !s.equations[idx].enabled) return std::nullopt;
    if (s.equations[idx].tolerance) return *s.equations[idx].tolerance;
    return s.defaultTolerance;
}

std::string formatTolerance(double v) {
    // Enough precision to round-trip exactly, locale-independent.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

std::optional<double> parseTolerance(const std::string& text) {
    if (text.empty()) return std::nullopt;
    char* end = nullptr;
    double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') return std::nullopt;
    if (!std::isfinite(v) || v <= 0.0) return std::nullopt;
    return v;
}

} // namespace cfd
