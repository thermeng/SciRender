#include "fvm/SlopeLimiter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace fvm {

double clamp01(double v) {
    if (!(v > 0.0)) return 0.0;   // also catches NaN
    if (v > 1.0) return 1.0;
    return v;
}

namespace {

constexpr std::size_t kNoCell = std::numeric_limits<std::size_t>::max();

// Barth-Jespersen minmod factor of ONE face: 1 while the reconstruction stays
// inside the bound set, the exact fraction that lands on the violated bound
// otherwise.
double minmodFactor(double lo, double hi, double phiUp, double corr) {
    if (corr > 0.0) {
        const double dmax = hi - phiUp;
        if (dmax < corr) return clamp01(dmax / corr);
    } else if (corr < 0.0) {
        const double dmin = lo - phiUp;
        if (dmin > corr) return clamp01(dmin / corr);
    }
    return 1.0;
}

// Venkatakrishnan smooth minmod (Venkatakrishnan 1993; the form used by
// OpenFOAM and Star-CCM+). With beta the distance from the upwind value to the
// violated bound and a = |corr|,
//
//   psi = [ (beta^2 + eps^2) a + 2 a^2 beta ] / [ a (beta^2 + 2 a^2 + beta a + eps^2) ]
//
// which is (r^2 + 2r)/(r^2 + r + 2) with r = beta/a when eps = 0. It is <= 1
// only for r <= 2, so the result is clipped to 1 (the usual trade: the clipped
// function is continuous but not differentiable at the clip point). eps = k *
// (hi - lo) keeps the smoothing relative to the local field variation, so the
// limiter behaves the same on any field scale.
double venkatakrishnan(double lo, double hi, double phiUp, double corr, double k) {
    if (corr == 0.0) return 1.0;
    const double a = std::fabs(corr);
    const double beta = (corr > 0.0) ? (hi - phiUp) : (phiUp - lo);
    // The bound is already reached (or the field is not a proper bracket): no
    // reconstruction is admissible, and eps cannot smooth that away.
    if (!(beta > 0.0)) return 0.0;

    const double range = hi - lo;
    double eps2 = 0.0;
    if (range > 0.0 && std::isfinite(range)) {
        const double e = k * range;
        eps2 = e * e;
    }

    const double b2 = beta * beta;
    const double a2 = a * a;
    const double num = (b2 + eps2) * a + 2.0 * a2 * beta;
    const double den = a * (b2 + 2.0 * a2 + beta * a + eps2);
    if (!(den > 0.0) || !std::isfinite(num)) return 0.0;
    return clamp01(num / den);
}

// Michalak-Ollivier-Gooch cubic fit (AIAA-2008-776): similar to Venkatakrishnan
// but a fit that obeys the value and gradient constraints and never exceeds 1.
// r = beta/a (bound distance over increment); rt is the transition point where
// the limiter reaches 1 (1..2, OpenFOAM uses 1.5).
double cubic(double beta, double a, double rt) {
    if (a == 0.0) return 1.0;
    if (!(beta > 0.0)) return 0.0;
    const double r = beta / a;
    if (!(r < rt)) return 1.0;
    const double a2 = 1.0 / (rt * rt) - 2.0 / (rt * rt * rt);
    const double b = -(3.0 / 2.0) * a2 * rt - (1.0 / 2.0) / rt;
    return ((a2 * r + b) * r + 1.0) * r;
}

} // namespace

template<std::size_t Dim>
SlopeLimiter<Dim>::SlopeLimiter(const Mesh<Dim>& mesh) : mesh_(mesh) {}

template<std::size_t Dim>
void SlopeLimiter<Dim>::configure(const SlopeLimiterConfig& config) {
    switch (config.kind) {
        case LimiterKind::MinMod:
        case LimiterKind::Venkatakrishnan:
        case LimiterKind::Cubic:
            break;
        default:
            throw std::invalid_argument("SlopeLimiter: unknown limiter kind");
    }
    switch (config.aggregation) {
        case LimiterAggregation::CellWide:
        case LimiterAggregation::ComponentWise:
            break;
        default:
            throw std::invalid_argument("SlopeLimiter: unknown aggregation");
    }
    switch (config.span) {
        case LimiterSpan::FaceCentre:
        case LimiterSpan::CentroidLine:
            break;
        default:
            throw std::invalid_argument("SlopeLimiter: unknown span");
    }
    if (!std::isfinite(config.k) || config.k < 0.0)
        throw std::invalid_argument(
            "SlopeLimiter: the Venkatakrishnan smoothing constant must be finite and non-negative");
    if (!std::isfinite(config.cubicTransition) || config.cubicTransition < 1.0)
        throw std::invalid_argument(
            "SlopeLimiter: the Cubic transition point must be finite and >= 1");
    config_ = config;
    alphaCell_.clear();
    limited_.clear();
}

template<std::size_t Dim>
void SlopeLimiter<Dim>::validate(std::size_t cellCount,
                                 const std::vector<char>* mask,
                                 const std::vector<double>* values) const {
    if (!mask || mask->empty()) return;
    // A masked index must never read past the end of the value list: callers do
    // pass a mask that is longer than the list, and a masked index is the one
    // case where the value is read unconditionally.
    if (!values || values->size() < mask->size())
        throw std::invalid_argument(
            "SlopeLimiter: the prescribed-value list is shorter than the mask");
    if (mask->size() < cellCount)
        throw std::invalid_argument(
            "SlopeLimiter: the prescribed mask does not cover every cell");
}

template<std::size_t Dim>
double SlopeLimiter<Dim>::value(std::size_t cell, const Field<Dim>& phi,
                                const std::vector<char>* mask,
                                const std::vector<double>* values) const {
    if (mask && !mask->empty() && (*mask)[cell]) return (*values)[cell];
    return phi[cell];
}

template<std::size_t Dim>
std::size_t SlopeLimiter<Dim>::otherCell(std::size_t cell, std::size_t face) const {
    const std::size_t o = mesh_.owner(face);
    if (o == cell) return mesh_.neighbor(face);
    return o;
}

template<std::size_t Dim>
void SlopeLimiter<Dim>::bounds(std::size_t cell, const Field<Dim>& phi,
                               const std::vector<char>* mask,
                               const std::vector<double>* values,
                               double& lo, double& hi) const {
    const double self = value(cell, phi, mask, values);
    lo = self;
    hi = self;
    for (std::size_t nb : mesh_.cellNeighbors(cell)) {
        const double v = value(nb, phi, mask, values);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
}

template<std::size_t Dim>
double SlopeLimiter<Dim>::increment(std::size_t cell, std::size_t face,
                                    const std::array<Field<Dim>, Dim>& grad) const {
    const double* xC = mesh_.cellCenterPtr(cell);
    const double* xF = mesh_.faceCenterPtr(face);

    double d[Dim];
    if (config_.span == LimiterSpan::CentroidLine) {
        const std::size_t other = otherCell(cell, face);
        bool scaled = false;
        if (other != kNoCell) {
            const double* xO = mesh_.cellCenterPtr(other);
            const double* n = mesh_.faceNormalPtr(face);
            // Parameter of the intersection of the centroid line with the face
            // plane: t = n.(xF - xC) / n.(xO - xC). On an orthogonal mesh this
            // is the face weight, but on a skewed cell it is NOT: faceWeight is
            // the orthogonal projection of the face centre onto the centroid
            // line, and the projection point is not where the line crosses the
            // face. Using the weight there mis-scales the increment and, once
            // the weight clamps to 0 or 1, disables limiting on that face.
            double num = 0.0, den = 0.0, len2 = 0.0;
            for (std::size_t dd = 0; dd < Dim; ++dd) {
                const double dx = xO[dd] - xC[dd];
                num += n[dd] * (xF[dd] - xC[dd]);
                den += n[dd] * dx;
                len2 += dx * dx;
            }
            const double len = std::sqrt(len2);
            if (std::isfinite(num) && std::isfinite(den) && len > 0.0 &&
                std::fabs(den) > 1e-12 * len) {
                const double t = num / den;
                if (t > 0.0 && t < 1.0) {
                    for (std::size_t dd = 0; dd < Dim; ++dd) d[dd] = t * (xO[dd] - xC[dd]);
                    scaled = true;
                }
            }
        }
        if (!scaled) {
            // No usable centroid line (a boundary face, or a cell so skewed
            // that the line misses the face): fall back to the face direction,
            // which is the default method.
            for (std::size_t dd = 0; dd < Dim; ++dd) d[dd] = xF[dd] - xC[dd];
        }
    } else {
        for (std::size_t dd = 0; dd < Dim; ++dd) d[dd] = xF[dd] - xC[dd];
    }

    double corr = 0.0;
    for (std::size_t dd = 0; dd < Dim; ++dd) corr += grad[dd][cell] * d[dd];
    return corr;
}

template<std::size_t Dim>
double SlopeLimiter<Dim>::alpha(double lo, double hi, double phiUp, double corr) const {
    // A non-finite bound or value means the field is already broken; clipping
    // the cell to first order is the only safe answer (and returning 1 here
    // would silently disable the limiter exactly when it is needed).
    if (!std::isfinite(lo) || !std::isfinite(hi) || !std::isfinite(phiUp) || !std::isfinite(corr))
        return 0.0;

    switch (config_.kind) {
        case LimiterKind::MinMod:
            return minmodFactor(lo, hi, phiUp, corr);
        case LimiterKind::Venkatakrishnan:
            return venkatakrishnan(lo, hi, phiUp, corr, config_.k);
        case LimiterKind::Cubic: {
            if (corr == 0.0) return 1.0;
            const double a = std::fabs(corr);
            const double beta = (corr > 0.0) ? (hi - phiUp) : (phiUp - lo);
            if (!(beta > 0.0)) return 0.0;
            return cubic(beta, a, config_.cubicTransition);
        }
    }
    return 0.0;
}

template<std::size_t Dim>
void SlopeLimiter<Dim>::prepare(const Field<Dim>& phi,
                                const std::array<Field<Dim>, Dim>& grad,
                                const std::vector<char>* mask,
                                const std::vector<double>* values) {
    const std::size_t n = mesh_.nCells();
    validate(n, mask, values);

    const std::size_t nF = mesh_.nFaces();
    if (limited_.size() != 2 * nF) limited_.assign(2 * nF, 0.0);
    if (alphaCell_.size() != n) alphaCell_.assign(n, 1.0);

    // Per-cell scratch for the face increments and their per-face limiter
    // values. With CellWide aggregation the cell-wide minimum must be known
    // before any slot is filled, so the faces are visited once into these
    // buffers and then written out.
    std::size_t maxFaces = 0;
    for (std::size_t c = 0; c < n; ++c) maxFaces = std::max(maxFaces, mesh_.cellFaces(c).size());
    std::vector<double> corr(maxFaces), factor(maxFaces);

    for (std::size_t c = 0; c < n; ++c) {
        // The bound set is a property of the cell, so it is scanned once and
        // reused for every face of the cell - not once per face.
        double lo = 0.0, hi = 0.0;
        bounds(c, phi, mask, values, lo, hi);
        const double phiC = value(c, phi, mask, values);

        const auto& faces = mesh_.cellFaces(c);
        double aC = 1.0;
        for (std::size_t i = 0; i < faces.size(); ++i) {
            corr[i] = increment(c, faces[i], grad);
            factor[i] = alpha(lo, hi, phiC, corr[i]);
            if (factor[i] < aC) aC = factor[i];
        }
        alphaCell_[c] = aC;

        for (std::size_t i = 0; i < faces.size(); ++i) {
            // CellWide aggregation: the most restrictive face governs every
            // face of the cell. ComponentWise aggregation: each face keeps its
            // own factor, so only faces that violate are clipped. A non-finite
            // increment is dropped rather than multiplied: 0 * NaN is NaN, and
            // that would put the NaN straight back into the right-hand side.
            const double f =
                (config_.aggregation == LimiterAggregation::CellWide) ? aC : factor[i];
            limited_[slot(c, faces[i])] = std::isfinite(corr[i]) ? f * corr[i] : 0.0;
        }
    }
}

template<std::size_t Dim>
double SlopeLimiter<Dim>::correction(std::size_t up, std::size_t face) const {
    if (limited_.size() != 2 * mesh_.nFaces())
        throw std::logic_error("SlopeLimiter: prepare() must be called before correction()");
    if (face >= mesh_.nFaces())
        throw std::invalid_argument("SlopeLimiter: face index out of range");
    const std::size_t owner = mesh_.owner(face);
    if (up != owner && up != mesh_.neighbor(face))
        throw std::invalid_argument("SlopeLimiter: the donor cell does not share the face");
    return limited_[slot(up, face)];
}

template class SlopeLimiter<2>;
template class SlopeLimiter<3>;

} // namespace fvm
