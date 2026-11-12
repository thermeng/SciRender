// Slope limiter: the bound set, the reconstruction increment, the scalar limiter
// and the per-cell pre-pass.
//
// Contract pinned here:
//   - the bound set is the upwind cell TOGETHER WITH its face-adjacent
//     neighbours, and a prescribed cell contributes its prescribed value;
//   - the increment is grad(phi).d with d following the configured span;
//     face-centre and centroid-line agree on an orthogonal mesh;
//   - the scalar limiter returns 1 inside the bounds, the exact fraction on the
//     violated bound, and 0 when the upwind value already lies outside them;
//   - MinMod + CellWide is the Barth-Jespersen per-cell limiter: the most
//     restrictive face of a cell sets the scale factor for every face of that
//     cell, which is what separates it from a per-face limiter;
//   - MinMod + ComponentWise is per-face: only the faces that leave the bound
//     set are clipped;
//   - Venkatakrishnan is smooth and bounded by 1;
//   - Cubic never exceeds 1 and reaches 1 at its transition point;
//   - the centroid-line span scales to the face-plane intersection, which is
//     not the orthogonal-projection weight on a skewed cell;
//   - a mask longer than the prescribed-value list is rejected, not read past,
//     and a non-finite increment is dropped rather than amplified.
#include "doctest/doctest.h"
#include "fvm/SlopeLimiter.h"
#include "support/fvm_fixtures.h"

#include <array>
#include <cmath>
#include <limits>
#include <vector>

using namespace test_support;
using fvm::Field;
using fvm::SlopeLimiter;
using fvm::SlopeLimiterConfig;
using fvm::LimiterKind;
using fvm::LimiterAggregation;
using fvm::LimiterSpan;

namespace {

std::array<Field<2>, 2> constantGradient(const fvm::Mesh<2>& mesh, double gx, double gy) {
    std::array<Field<2>, 2> g{Field<2>(mesh, 0.0), Field<2>(mesh, 0.0)};
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        g[0][i] = gx;
        g[1][i] = gy;
    }
    return g;
}

} // namespace

TEST_SUITE("cfd") {

// The bound set is the cell plus its face-adjacent neighbours.
TEST_CASE("cfd.slope.bounds_cover_cell_and_neighbours") {
    const fvm::Mesh<2> mesh = uniform2D(5);
    Field<2> phi(mesh, 0.0);
    // A field with a unique minimum at (0,0) and a unique maximum at (3,3), so
    // the expected bound set of any cell is unambiguous.
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        const auto c = mesh.cellCoords(i);
        phi[i] = static_cast<double>(c[0] * 4 + c[1]);
    }

    SlopeLimiter<2> limiter(mesh);
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        const auto c = mesh.cellCoords(i);
        const double self = static_cast<double>(c[0] * 4 + c[1]);
        double lo = self, hi = self;
        // The face neighbours of a structured cell are the four axis neighbours.
        if (c[0] > 0) { lo = std::min(lo, self - 4.0); hi = std::max(hi, self - 4.0); }
        if (c[0] + 1 < mesh.dims()[0]) { lo = std::min(lo, self + 4.0); hi = std::max(hi, self + 4.0); }
        if (c[1] > 0) { lo = std::min(lo, self - 1.0); hi = std::max(hi, self - 1.0); }
        if (c[1] + 1 < mesh.dims()[1]) { lo = std::min(lo, self + 1.0); hi = std::max(hi, self + 1.0); }

        double gotLo = 0.0, gotHi = 0.0;
        limiter.bounds(i, phi, nullptr, nullptr, gotLo, gotHi);
        CAPTURE(i);
        CHECK_MESSAGE(std::abs(gotLo - lo) < 1e-12, "the lower bound must cover the neighbours");
        CHECK_MESSAGE(std::abs(gotHi - hi) < 1e-12, "the upper bound must cover the neighbours");
    }
}

// A prescribed cell contributes its prescribed value, not its stored value.
TEST_CASE("cfd.slope.bounds_use_the_prescribed_value") {
    const fvm::Mesh<2> mesh = uniform2D(5);
    Field<2> phi(mesh, 0.0);
    for (std::size_t i = 0; i < mesh.nCells(); ++i) phi[i] = 1.0;

    // The neighbour at (0,1) is stored at 1 but prescribed at 5: the bound of
    // the cell at (1,1) must see the 5.
    const std::size_t target = mesh.cellIndex(1, 1);
    const std::size_t prescribed = mesh.cellIndex(0, 1);

    std::vector<char> mask(mesh.nCells(), 0);
    std::vector<double> values(mesh.nCells(), 0.0);
    mask[prescribed] = 1;
    values[prescribed] = 5.0;

    SlopeLimiter<2> limiter(mesh);
    double lo = 0.0, hi = 0.0;
    limiter.bounds(target, phi, &mask, &values, lo, hi);
    CHECK_MESSAGE(std::abs(hi - 5.0) < 1e-12, "a prescribed neighbour must raise the upper bound");
}

// The increment is grad(phi).d with d the face direction from the upwind cell.
TEST_CASE("cfd.slope.increment_face_centre") {
    const fvm::Mesh<2> mesh = uniform2D(5);
    Field<2> phi(mesh, 0.0);
    fillScalar(phi, [](double x, double, double) { return x; });
    const auto grad = constantGradient(mesh, 2.0, -3.0);

    SlopeLimiter<2> limiter(mesh);
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        const std::size_t P = mesh.owner(f);
        const auto xP = mesh.cellCenter(P);
        const auto xF = mesh.faceCenter(f);
        double expected = 0.0;
        for (std::size_t d = 0; d < 2; ++d) expected += grad[d][P] * (xF[d] - xP[d]);
        CAPTURE(f);
        CHECK_MESSAGE(limiter.increment(P, f, grad) == doctest::Approx(expected), "grad(phi).d");
    }
}

// On an orthogonal mesh the centroid-line span is the face-centre one.
TEST_CASE("cfd.slope.span_equivalent_on_orthogonal_mesh") {
    const fvm::Mesh<2> mesh = uniform2D(5);
    const auto grad = constantGradient(mesh, 2.0, -3.0);

    SlopeLimiter<2> face(mesh);
    SlopeLimiter<2> cell(mesh);
    SlopeLimiterConfig cc;
    cc.span = LimiterSpan::CentroidLine;
    cell.configure(cc);

    for (std::size_t c = 0; c < mesh.nCells(); ++c) {
        for (std::size_t f : mesh.cellFaces(c)) {
            CAPTURE(c);
            CHECK_MESSAGE(face.increment(c, f, grad) == doctest::Approx(cell.increment(c, f, grad)),
                          "the two spans must agree on an orthogonal mesh");
        }
    }
}

// The scalar limiter: 1 inside the bounds, the exact fraction on a violated
// bound, and 0 when the upwind value already lies outside them.
TEST_CASE("cfd.slope.scale_values") {
    const fvm::Mesh<2> mesh = uniform2D(3);
    SlopeLimiter<2> limiter(mesh);

    // Inside the bounds: no limiting.
    CHECK(limiter.alpha(0.0, 1.0, 0.5, 0.25) == doctest::Approx(1.0));
    // Above the upper bound: (hi - phiUp) / corr.
    CHECK(limiter.alpha(0.0, 1.0, 0.5, 0.75) == doctest::Approx(0.5 / 0.75));
    // Below the lower bound: (lo - phiUp) / corr, both negative.
    CHECK(limiter.alpha(0.0, 1.0, 0.5, -0.75) == doctest::Approx(0.5 / 0.75));
    // Exactly on the bound: alpha 1.
    CHECK(limiter.alpha(0.0, 1.0, 0.5, 0.5) == doctest::Approx(1.0));
    // A zero increment is never limited.
    CHECK(limiter.alpha(0.0, 1.0, 0.5, 0.0) == doctest::Approx(1.0));
    // An upwind value already outside the bounds cannot be repaired by the
    // limiter: it clips to zero rather than extrapolating.
    CHECK(limiter.alpha(0.0, 1.0, 1.5, 0.75) == doctest::Approx(0.0));
}

// MinMod + CellWide is PER CELL: the most restrictive face sets the scale factor
// for every face of the cell. A per-face limiter would leave the -x face
// unlimited.
TEST_CASE("cfd.slope.minmod_is_per_cell") {
    const fvm::Mesh<2> mesh = uniform2D(5);
    Field<2> phi(mesh, 0.0);
    fillScalar(phi, [](double x, double, double) { return x; });
    const double gradX = 4.0;
    const auto grad = constantGradient(mesh, gradX, 0.0);   // deliberately too steep

    // Drop the -x neighbour of (1,1) far below the linear value. Its face then
    // reconstructs to a value that is still inside the bound set, so a per-face
    // limiter would not touch it - only the per-cell limiter does.
    const std::size_t P = mesh.cellIndex(1, 1);
    const std::size_t minusX = mesh.cellIndex(0, 1);
    phi[minusX] = phi[P] - 0.6;

    SlopeLimiter<2> limiter(mesh);
    limiter.prepare(phi, grad, nullptr, nullptr);

    // The face sits half a cell from the centre, so corr = gradX * h/2 and the
    // +x bound is one full cell away: scale = h / (gradX * h/2) = 2/gradX.
    const double scalePlusX = 2.0 / gradX;
    const double scaleMinusX = 1.0;   // (0.6) / (gradX * h/2) > 1 -> unclipped
    CHECK_MESSAGE(scaleMinusX > scalePlusX, "the fixture must be discriminating");
    CHECK_MESSAGE(limiter.alphaCell(P) == doctest::Approx(scalePlusX),
                  "the per-cell scale must be the most restrictive face");

    std::size_t minusFace = static_cast<std::size_t>(-1);
    for (std::size_t f : mesh.cellFaces(P)) {
        if (mesh.faceCenter(f)[0] < mesh.cellCenter(P)[0]) { minusFace = f; break; }
    }
    REQUIRE(minusFace != static_cast<std::size_t>(-1));

    const double h = mesh.dx(0);
    const double corr = limiter.correction(P, minusFace);
    // A per-face limiter would have returned the unlimited -gradX*h/2 here.
    CHECK_MESSAGE(corr == doctest::Approx(-scalePlusX * gradX * h / 2.0),
                  "the per-cell scale must govern every face of the cell");
}

// A mask longer than the prescribed-value list is rejected rather than read.
TEST_CASE("cfd.slope.rejects_a_mask_longer_than_the_values") {
    const fvm::Mesh<2> mesh = uniform2D(3);
    Field<2> phi(mesh, 0.0);
    const auto grad = constantGradient(mesh, 1.0, 0.0);

    std::vector<char> mask(mesh.nCells() + 3, 0);
    std::vector<double> values(mesh.nCells(), 0.0);   // shorter than the mask

    SlopeLimiter<2> limiter(mesh);
    CHECK_THROWS(limiter.prepare(phi, grad, &mask, &values));
}

// An unknown limiter kind (e.g. read from a settings file and cast) is a
// configuration error, so configure() must reject it rather than let it fail
// halfway through an iteration.
TEST_CASE("cfd.slope.configure_validates_the_variant") {
    const fvm::Mesh<2> mesh = uniform2D(3);
    SlopeLimiter<2> limiter(mesh);
    SlopeLimiterConfig cfg;

    cfg.kind = static_cast<LimiterKind>(99);
    CHECK_THROWS(limiter.configure(cfg));

    cfg.kind = LimiterKind::MinMod;
    cfg.k = -1.0;
    CHECK_THROWS(limiter.configure(cfg));

    cfg.k = 0.1;
    CHECK_NOTHROW(limiter.configure(cfg));

    // Every kind that is configured successfully must also run.
    const Field<2> phi(mesh, 0.0);
    const auto grad = constantGradient(mesh, 1.0, 0.0);
    for (const auto kind : {LimiterKind::MinMod, LimiterKind::Venkatakrishnan, LimiterKind::Cubic}) {
        cfg.kind = kind;
        limiter.configure(cfg);
        CAPTURE(static_cast<int>(kind));
        CHECK_NOTHROW(limiter.prepare(phi, grad, nullptr, nullptr));
    }
}

// ComponentWise clipping clips only the faces that leave the bound set: the
// -x face of the fixture is left at full reconstruction while MinMod + CellWide
// scales it down with everything else.
TEST_CASE("cfd.slope.componentwise_clips_only_the_violating_face") {
    const fvm::Mesh<2> mesh = uniform2D(5);
    Field<2> phi(mesh, 0.0);
    fillScalar(phi, [](double x, double, double) { return x; });
    const double gradX = 4.0;
    const auto grad = constantGradient(mesh, gradX, 0.0);

    const std::size_t P = mesh.cellIndex(1, 1);
    const std::size_t minusX = mesh.cellIndex(0, 1);
    phi[minusX] = phi[P] - 0.6;

    std::size_t minusFace = static_cast<std::size_t>(-1);
    std::size_t plusFace = static_cast<std::size_t>(-1);
    for (std::size_t f : mesh.cellFaces(P)) {
        if (mesh.faceCenter(f)[0] < mesh.cellCenter(P)[0]) minusFace = f;
        else if (mesh.faceCenter(f)[0] > mesh.cellCenter(P)[0]) plusFace = f;
    }
    REQUIRE(minusFace != static_cast<std::size_t>(-1));
    REQUIRE(plusFace != static_cast<std::size_t>(-1));

    SlopeLimiter<2> cellWide(mesh);
    cellWide.prepare(phi, grad, nullptr, nullptr);

    SlopeLimiter<2> faceWise(mesh);
    SlopeLimiterConfig cfg;
    cfg.kind = LimiterKind::MinMod;
    cfg.aggregation = LimiterAggregation::ComponentWise;
    faceWise.configure(cfg);
    faceWise.prepare(phi, grad, nullptr, nullptr);

    const double h = mesh.dx(0);
    const double scalePlusX = 2.0 / gradX; // (hi - phiP)/(gradX*h/2) < 1
    const double unlimitedMinusX = -gradX * h / 2.0;

    // The face that leaves the bound set is clipped the same way by both.
    CHECK_MESSAGE(cellWide.correction(P, plusFace) == doctest::Approx(scalePlusX * gradX * h / 2.0),
                  "the cell-wide limiter clips the violating face");
    CHECK_MESSAGE(faceWise.correction(P, plusFace) == doctest::Approx(scalePlusX * gradX * h / 2.0),
                  "the per-face limiter clips the violating face");

    // The face that stays inside the bound set is left alone by the per-face
    // limiter, while the cell-wide scale scales it down with everything else.
    CHECK_MESSAGE(faceWise.correction(P, minusFace) == doctest::Approx(unlimitedMinusX),
                  "the per-face limiter must not clip a face that stays inside the bounds");
    CHECK_MESSAGE(cellWide.correction(P, minusFace) == doctest::Approx(scalePlusX * unlimitedMinusX),
                  "the cell-wide limiter clips every face of the cell");
}

TEST_CASE("cfd.slope.venkatakrishnan_is_smooth_and_bounded") {
    const fvm::Mesh<2> mesh = uniform2D(3);
    SlopeLimiter<2> limiter(mesh);
    SlopeLimiterConfig cfg;
    cfg.kind = LimiterKind::Venkatakrishnan;
    cfg.k = 0.0; // eps = 0: the pure (r^2 + 2r)/(r^2 + r + 2) form
    limiter.configure(cfg);

    // No increment, no limiting.
    CHECK_MESSAGE(limiter.alpha(0.0, 1.0, 0.5, 0.0) == doctest::Approx(1.0),
                  "a zero increment is never limited");
    // At the bound (r = 1) the smooth function already clips to 3/4, where the
    // Barth-Jespersen fraction is still 1: that early, smooth action is the
    // whole point of the differentiable limiter.
    CHECK_MESSAGE(limiter.alpha(0.0, 1.0, 0.5, 0.5) == doctest::Approx(0.75),
                  "r = 1 gives 3/4");
    // The fraction grows with the distance to the bound and never exceeds 1.
    double prev = -1.0;
    for (double hi : {0.5, 1.0, 2.0, 4.0}) {
        const double a = limiter.alpha(0.0, hi, 0.0, 1.0);
        CHECK_MESSAGE(a >= prev, "the Venkatakrishnan limiter must be monotone in the bound distance");
        CHECK_MESSAGE(a <= 1.0, "the Venkatakrishnan limiter must not amplify");
        prev = a;
    }
}

TEST_CASE("cfd.slope.cubic_never_exceeds_one") {
    const fvm::Mesh<2> mesh = uniform2D(3);
    SlopeLimiter<2> limiter(mesh);
    SlopeLimiterConfig cfg;
    cfg.kind = LimiterKind::Cubic;
    cfg.cubicTransition = 1.5; // OpenFOAM default
    limiter.configure(cfg);

    // Reaches 1 at the transition point and stays there.
    CHECK_MESSAGE(limiter.alpha(0.0, 1.5, 0.0, 1.0) == doctest::Approx(1.0),
                  "at r = rt the cubic reaches 1");
    CHECK_MESSAGE(limiter.alpha(0.0, 3.0, 0.0, 1.0) == doctest::Approx(1.0),
                  "past the transition it stays at 1");

    // Never exceeds 1 (unlike clipped Venkatakrishnan), monotone in r.
    double prev = -1.0;
    for (double hi : {0.1, 0.5, 1.0, 1.4}) {
        const double a = limiter.alpha(0.0, hi, 0.0, 1.0);
        CHECK_MESSAGE(a >= 0.0, "the cubic limiter must not be negative");
        CHECK_MESSAGE(a <= 1.0, "the cubic limiter must not exceed 1");
        CHECK_MESSAGE(a >= prev, "the cubic limiter must be monotone in the bound distance");
        prev = a;
    }
}

// The cell-to-centroid-line span must scale the increment to the point where the
// centroid line crosses the face plane, not to the orthogonal projection of the
// face centre (the two differ on a skewed cell).
TEST_CASE("cfd.slope.centroid_line_reaches_the_face_plane") {
    const fvm::Mesh<2> mesh = triangles2D();
    std::array<fvm::Field<2>, 2> grad{fvm::Field<2>(mesh, 0.0), fvm::Field<2>(mesh, 0.0)};

    SlopeLimiter<2> toCell(mesh);
    SlopeLimiterConfig cfg;
    cfg.span = LimiterSpan::CentroidLine;
    toCell.configure(cfg);

    std::size_t checked = 0;
    std::size_t discriminating = 0;
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        const std::size_t c = mesh.owner(f);
        const std::size_t o = mesh.neighbor(f);
        if (o == std::size_t(-1)) continue;
        ++checked;

        const double* xC = mesh.cellCenterPtr(c);
        const double* xO = mesh.cellCenterPtr(o);
        const double* xF = mesh.faceCenterPtr(f);
        const double* n = mesh.faceNormalPtr(f);

        // A gradient aligned with the centroid line: grad.(xO - xC) = |d|^2, so
        // increment / |d|^2 is exactly the line parameter t.
        const double dx = xO[0] - xC[0];
        const double dy = xO[1] - xC[1];
        const double len2 = dx * dx + dy * dy;
        grad[0][c] = dx;
        grad[1][c] = dy;

        const double t = toCell.increment(c, f, grad) / len2;

        // The point at parameter t must lie ON the face plane. The face-centre
        // direction and the orthogonal-projection weight do not satisfy this on
        // a skewed face.
        const double plane = n[0] * (xC[0] + t * dx - xF[0]) + n[1] * (xC[1] + t * dy - xF[1]);
        CAPTURE(f);
        CHECK_MESSAGE(std::abs(plane) < 1e-12,
                      "the centroid-line span must land on the face plane");
        CHECK_MESSAGE(t > 0.0, "the face-plane intersection must lie inside the centroid segment");
        CHECK_MESSAGE(t < 1.0, "the face-plane intersection must lie inside the centroid segment");

        const double w = ((xF[0] - xC[0]) * dx + (xF[1] - xC[1]) * dy) / len2;
        if (std::abs(t - w) > 1e-9) ++discriminating;
    }
    CHECK_MESSAGE(checked > 0, "the fixture must have interior faces");
    CHECK_MESSAGE(discriminating > 0,
                  "the fixture must contain a face where the projection weight differs from the "
                  "face-plane intersection, otherwise this test cannot tell the two apart");
}

TEST_CASE("cfd.slope.correction_requires_prepare_and_a_shared_face") {
    const fvm::Mesh<2> mesh = uniform2D(3);
    Field<2> phi(mesh, 0.0);
    const auto grad = constantGradient(mesh, 1.0, 0.0);

    SlopeLimiter<2> limiter(mesh);
    // correction() before prepare() would read an empty table: it must fail
    // loudly instead of indexing out of bounds.
    CHECK_THROWS(limiter.correction(mesh.owner(0), 0));

    limiter.prepare(phi, grad, nullptr, nullptr);
    CHECK(limiter.correction(mesh.owner(0), 0) == doctest::Approx(0.0));

    // A donor that does not share the face is a caller bug, not a silent zero.
    std::size_t interior = static_cast<std::size_t>(-1);
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) != std::size_t(-1)) {
            interior = f;
            break;
        }
    }
    REQUIRE(interior != static_cast<std::size_t>(-1));
    CHECK_THROWS(limiter.correction(mesh.nCells(), interior));
}

TEST_CASE("cfd.slope.a_non_finite_increment_is_not_amplified") {
    const fvm::Mesh<2> mesh = uniform2D(3);
    SlopeLimiter<2> limiter(mesh);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    CHECK(limiter.alpha(0.0, 1.0, 0.5, nan) == doctest::Approx(0.0));
    CHECK(limiter.alpha(0.0, 1.0, 0.5, inf) == doctest::Approx(0.0));
    CHECK(limiter.alpha(0.0, 1.0, nan, 0.5) == doctest::Approx(0.0));

    // And prepare() drops a non-finite increment instead of multiplying it by a
    // limiter value (0 * NaN is NaN).
    Field<2> phi(mesh, 0.0);
    std::array<fvm::Field<2>, 2> grad{fvm::Field<2>(mesh, 0.0), fvm::Field<2>(mesh, 0.0)};
    grad[0][0] = nan;
    limiter.prepare(phi, grad, nullptr, nullptr);
    for (std::size_t f : mesh.cellFaces(0))
        CHECK(limiter.correction(0, f) == doctest::Approx(0.0));
}

} // TEST_SUITE("cfd")
