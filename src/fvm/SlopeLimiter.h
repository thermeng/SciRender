#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "fvm/Types.h"
#include <array>
#include <cstddef>
#include <vector>

namespace fvm {

// Slope (gradient) limiters for the second-order-upwind reconstruction.
//
// A slope limiter enforces the monotonicity principle on the reconstructed face
// value: the value built from a cell gradient may not exceed the maximum or
// fall below the minimum of the upwind cell and the cells that bound it. The
// limiter is a scalar in [0,1] multiplying the reconstruction increment: 0
// reproduces plain upwind exactly and 1 reproduces the unlimited reconstruction
// exactly.
//
// Scope: the limiter applies to the gradient of the reconstructed variable
// ONLY. The pressure operator, the diffusive fluxes and the turbulence
// production terms are deliberately left unlimited.
//
// All variants share one per-face scalar (Phi_f = Phi + psi * grad(phi).d). They
// differ in the limiter FUNCTION that produces psi and in how the per-face
// values are aggregated over a cell:
//
//   Kind:
//     MinMod            Barth-Jespersen minmod: psi = min(1, r)
//     Venkatakrishnan   smooth Venkatakrishnan: psi = (r^2+2r)/(r^2+r+2)
//     Cubic               Michalak-Ollivier-Gooch cubic fit: never exceeds 1
//   Aggregation:
//     CellWide          one scale factor per cell (most restrictive face clips
//                       every face of the cell)
//     ComponentWise     one scale factor per face (only faces that violate are
//                       clipped)
//   Span:
//     FaceCentre        d = x_face - x_upwind
//     CentroidLine      d along the line joining the two cell centroids,
//                       scaled to the point where that line crosses the face plane
enum class LimiterKind {
    MinMod,
    Venkatakrishnan,
    Cubic,
};

enum class LimiterAggregation {
    CellWide,      // one scale factor per cell
    ComponentWise, // one scale factor per face
};

enum class LimiterSpan {
    FaceCentre,    // d = x_face - x_upwind
    CentroidLine,  // d along the centroid line, scaled to where it crosses the
                   // face plane
};

struct SlopeLimiterConfig {
    LimiterKind kind = LimiterKind::MinMod;
    LimiterAggregation aggregation = LimiterAggregation::CellWide;
    LimiterSpan span = LimiterSpan::FaceCentre;
    // Venkatakrishnan smoothing: eps = k * (hi - lo), relative to the local
    // field variation of the bound set. Ignored by MinMod and Cubic.
    double k = 0.1;
    // Cubic transition point where the limiter function reaches 1. Should be
    // in [1,2] (OpenFOAM uses 1.5).
    double cubicTransition = 1.5;
};

// The scalar limiter value alpha in [0,1] for ONE face.
//
// corr is the unlimited reconstruction increment grad(phi).d and [lo,hi] is the
// bound set of the upwind cell. 1 inside the bounds, the fraction landing
// exactly on the violated bound otherwise.
double clamp01(double v);

template<std::size_t Dim>
class SlopeLimiter {
public:
    explicit SlopeLimiter(const Mesh<Dim>& mesh);

    // Validates and stores the configuration. Fails here rather than in the
    // middle of an iteration: an unknown limiter kind or a non-finite
    // smoothing constant is a configuration error, not a runtime event.
    void configure(const SlopeLimiterConfig& config);
    const SlopeLimiterConfig& config() const { return config_; }

    // The bound set of a cell: [lo,hi] over the cell itself and its
    // face-adjacent neighbours. A prescribed (Dirichlet) cell contributes its
    // prescribed value, not its stored value, so a Dirichlet wall bounds the
    // reconstruction the way the boundary condition does.
    void bounds(std::size_t cell, const Field<Dim>& phi,
                const std::vector<char>* mask, const std::vector<double>* values,
                double& lo, double& hi) const;

    // The unlimited reconstruction increment grad(phi).d at a face, seen from
    // one of the cells sharing it. d follows the configured span.
    double increment(std::size_t cell, std::size_t face,
                     const std::array<Field<Dim>, Dim>& grad) const;

    // The scalar limiter value of ONE face of a cell whose bound set is
    // [lo,hi].
    double alpha(double lo, double hi, double phiUp, double corr) const;

    // Pre-pass. Fills the per-face limiter table from the field and the
    // gradient.
    //
    // The result depends on the geometry, the field and the gradient only, so
    // it is independent of the flux: one prepare() serves every face and both
    // possible donors, and the same values can be reused by every entry point.
    // Must be called before correction() with the SAME field and gradient.
    void prepare(const Field<Dim>& phi,
                 const std::array<Field<Dim>, Dim>& grad,
                 const std::vector<char>* mask,
                 const std::vector<double>* values);

    // The limited reconstruction increment to apply at a face whose upwind
    // (donor) cell is up. Reads the table computed by prepare(); must be
    // called with the same face and donor the assembler used.
    double correction(std::size_t up, std::size_t face) const;

    // The most restrictive alpha of a cell (the minimum over its faces), for
    // diagnostics and tests. With CellWide aggregation this is also the alpha
    // applied to every face of the cell.
    double alphaCell(std::size_t cell) const { return alphaCell_[cell]; }

private:
    // The value of a cell as the limiter sees it: the prescribed value for a
    // prescribed cell, the stored value otherwise.
    double value(std::size_t cell, const Field<Dim>& phi,
                 const std::vector<char>* mask, const std::vector<double>* values) const;

    // The cell on the other side of a face, seen from cell.
    std::size_t otherCell(std::size_t cell, std::size_t face) const;

    // Rejects a mask that is longer than the value list: a masked index must
    // never read past the end of the values.
    void validate(std::size_t cellCount,
                  const std::vector<char>* mask,
                  const std::vector<double>* values) const;

    // Index of the slot of limited_ that belongs to (cell, face).
    std::size_t slot(std::size_t cell, std::size_t face) const {
        return 2 * face + (mesh_.owner(face) == cell ? 0 : 1);
    }

    const Mesh<Dim>& mesh_;
    SlopeLimiterConfig config_;
    // Per-cell most restrictive alpha (min over the cell's faces), diagnostic
    // only when aggregation is ComponentWise.
    std::vector<double> alphaCell_;
    // Per-(face, donor side) limited reconstruction increment: slot(c,f) holds
    // the increment the face gets when c is its donor. Two slots per face
    // because either of the two cells sharing it can be the donor.
    std::vector<double> limited_;
};

extern template class SlopeLimiter<2>;
extern template class SlopeLimiter<3>;

} // namespace fvm
