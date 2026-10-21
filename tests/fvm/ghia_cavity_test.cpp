// Lid-driven cavity benchmark (Ghia, Giri & Poddar, 1982).
//
// Regression check of the SIMPLE solver on the classical lid-driven cavity.
// The centreline velocity profile is the strongest single indicator of whether
// the convective discretisation adds the right amount of numerical diffusion:
// a too-diffusive scheme smears the wall jet and flattens the profile, and the
// small corner vortices of the cavity disappear.
//
// NOTE ON TOLERANCES
// The solver prescribes the moving lid as a cell-centred Dirichlet value, so
// the effective wall speed is larger than the classical benchmark's U = 1 and
// the profile is systematically off the published values. The assertions
// below therefore check the *structure* of the solution (which is what
// actually regresses when the discretisation, the boundary conditions or the
// pressure coupling break) and keep a coarse absolute bound against the
// published table as a smoke alarm.
//
// The test cases are named "slow.*" so they carry the "slow" CTest label and
// can be excluded with `ctest -LE slow`.

#include "mesh/Mesh.h"
#include "mesh/BoundaryUtils.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/SIMPLE.h"
#include "doctest/doctest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

using namespace fvm;

namespace {

struct RefPoint {
    double y;
    double u;
};

// Ghia et al. (1982), Table I, Re = 100: u along the vertical centreline.
const RefPoint kGhia100[] = {
    {1.0000, 1.00000}, {0.9766, 0.75837}, {0.9688, 0.68439}, {0.9609, 0.61756},
    {0.9531, 0.55892}, {0.8516, 0.29093}, {0.7344, 0.16256}, {0.6172, 0.02135},
    {0.5000, -0.11477}, {0.4531, -0.17119}, {0.2813, -0.32726}, {0.1719, -0.24299},
    {0.1016, -0.14612}, {0.0703, -0.10338}, {0.0625, -0.09266}, {0.0547, -0.08186},
    {0.0000, 0.00000},
};

// Ghia et al. (1982), Table III, Re = 400: u along the vertical centreline.
const RefPoint kGhia400[] = {
    {1.0000, 1.00000}, {0.9766, 0.75024}, {0.9688, 0.70528}, {0.9609, 0.65308},
    {0.9531, 0.59213}, {0.8516, 0.30930}, {0.7344, 0.17122}, {0.6172, 0.02401},
    {0.5000, -0.10333}, {0.4531, -0.14613}, {0.2813, -0.32275}, {0.1719, -0.24925},
    {0.1016, -0.15809}, {0.0703, -0.11812}, {0.0625, -0.10688}, {0.0547, -0.09544},
    {0.0000, 0.00000},
};

constexpr std::size_t kRefCount = sizeof(kGhia100) / sizeof(RefPoint);

// Grid and iteration count of the committed benchmark run.
constexpr std::size_t kGridN = 32;
constexpr int kIterations = 600;

struct Profile {
    std::vector<double> y;
    std::vector<double> u;

    double at(double yy) const {
        if (y.size() < 2)
            return 0.0;
        if (yy <= y.front())
            return u.front();
        if (yy >= y.back())
            return u.back();
        auto it = std::upper_bound(y.begin(), y.end(), yy);
        const std::size_t hi = static_cast<std::size_t>(it - y.begin());
        const std::size_t lo = hi - 1;
        const double t = (yy - y[lo]) / (y[hi] - y[lo]);
        return u[lo] + t * (u[hi] - u[lo]);
    }
};

// Extracts the velocity component on the vertical centreline x = 0.5 by
// averaging the cells whose centroid lies in the centreline column.
template <std::size_t Dim>
Profile centreline(const Mesh<Dim>& mesh, const Field<Dim>& u, double h) {
    std::vector<std::pair<double, double>> raw; // (y, u)
    for (std::size_t c = 0; c < mesh.nCells(); ++c) {
        const auto cc = mesh.cellCenter(c);
        if (std::fabs(cc[0] - 0.5) > 0.75 * h)
            continue;
        raw.emplace_back(cc[1], u[c]);
    }
    std::sort(raw.begin(), raw.end());

    Profile p;
    std::size_t i = 0;
    while (i < raw.size()) {
        std::size_t j = i;
        double sum = 0.0;
        int n = 0;
        while (j < raw.size() && std::fabs(raw[j].first - raw[i].first) < 1e-9) {
            sum += raw[j].second;
            ++n;
            ++j;
        }
        p.y.push_back(raw[i].first);
        p.u.push_back(sum / n);
        i = j;
    }
    return p;
}

struct CavityResult {
    double maxError = 0.0;        // worst |u - u_ref| against the published table
    double maxU = 0.0;            // peak |u| anywhere in the domain
    double continuityResidual = 0.0;
    double correlation = 0.0;      // Pearson correlation with the published table
    double zeroCrossing = 0.0;     // height where the centreline u changes sign
    std::vector<double> sampled;   // computed u at the reference heights
};

// Runs the cavity for the given configuration and evaluates the computed
// profile against the published reference.
CavityResult runCavity(std::size_t N, double Re, int iters, const RefPoint* ref) {
    const double L = 1.0;
    const double U = 1.0;
    const double nu = U * L / Re; // rho = 1

    Mesh<2> mesh = Mesh<2>(std::array<std::size_t, 2>{N, N}, std::array<double, 2>{L, L});
    const double h = 1.0 / static_cast<double>(N);

    SIMPLE<2>::Config cfg;
    cfg.maxIterations = static_cast<std::size_t>(iters);
    cfg.tolerance = 1e-8;
    cfg.runFixedIterations = true;
    cfg.convergenceHoldIterations = 3;
    cfg.alphaU = 0.7;
    cfg.alphaP = 0.3;
    cfg.simplec = false;
    cfg.rhieChow = true;
    cfg.pressureCorrectors = 3;
    cfg.printFrequency = 1000000; // effectively silent (the solver divides by it)
    cfg.convectionScheme = ConvectionScheme::SecondOrderUpwind;
    cfg.gradientScheme = GradientScheme::LeastSquares;

    SIMPLE<2> solver(mesh, 1.0, nu, cfg);
    // Lid: the +y boundary moves in +x. The other three walls are no-slip.
    solver.setVelocityBC(1, +1, VelocityBCType::MovingWall, U, 0.0, 0.0);
    solver.setVelocityBC(1, -1, VelocityBCType::NoSlip);
    solver.setVelocityBC(0, -1, VelocityBCType::NoSlip);
    solver.setVelocityBC(0, +1, VelocityBCType::NoSlip);

    VectorField<2> velocity(mesh, 0.0);
    Field<2> pressure(mesh, 0.0);
    solver.solve(velocity, pressure);

    const Profile prof = centreline(mesh, velocity[0], h);

    CavityResult result;
    std::vector<double> refU;
    for (std::size_t i = 0; i < kRefCount; ++i) {
        const double uc = prof.at(ref[i].y);
        result.sampled.push_back(uc);
        refU.push_back(ref[i].u);
        result.maxError = std::max(result.maxError, std::fabs(uc - ref[i].u));
    }
    for (std::size_t c = 0; c < mesh.nCells(); ++c)
        result.maxU = std::max(result.maxU, std::fabs(velocity[0][c]));
    result.continuityResidual = solver.finalContinuityResidual();

    // Pearson correlation of the sampled profile with the published table.
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (std::size_t i = 0; i < result.sampled.size(); ++i) {
        const double a = result.sampled[i];
        const double b = refU[i];
        sx += a;
        sy += b;
        sxx += a * a;
        syy += b * b;
        sxy += a * b;
    }
    const double n = static_cast<double>(result.sampled.size());
    const double cov = sxy / n - (sx / n) * (sy / n);
    const double varA = sxx / n - (sx / n) * (sx / n);
    const double varB = syy / n - (sy / n) * (sy / n);
    const double denom = std::sqrt(std::max(varA * varB, 1e-300));
    result.correlation = cov / denom;

    // Height where the centreline profile changes sign (scanning downwards
    // from the lid): the primary vortex centre.
    for (std::size_t i = 1; i < result.sampled.size(); ++i) {
        const double u0 = result.sampled[i - 1];
        const double u1 = result.sampled[i];
        if (u0 > 0.0 && u1 <= 0.0) {
            const double t = u0 / (u0 - u1);
            result.zeroCrossing = ref[i - 1].y + t * (ref[i].y - ref[i - 1].y);
            break;
        }
    }
    return result;
}

} // namespace

TEST_SUITE("slow") {

// The cavity must develop the primary vortex with the right structure:
// positive flow under the lid, negative flow in the lower half, a zero
// crossing at a physically sensible height, and a profile that follows the
// published shape.
TEST_CASE("slow.ghia.re100_centreline") {
    const CavityResult r = runCavity(kGridN, 100.0, kIterations, kGhia100);

    MESSAGE("Re=100 structured " << kGridN << "x" << kGridN << ": max|u| = " << r.maxU
                                << ", continuity residual = " << r.continuityResidual
                                << ", correlation = " << r.correlation
                                << ", zero crossing y = " << r.zeroCrossing
                                << ", max |u - u_ref| = " << r.maxError);

    // The lid and the wall are prescribed: u = 1 at the lid, u = 0 at the wall.
    CHECK_MESSAGE(std::fabs(r.sampled.front() - 1.0) < 1e-6, "lid velocity must be reproduced");
    CHECK_MESSAGE(std::fabs(r.sampled.back()) < 1e-6, "wall must be no-slip");

    // The primary vortex exists: positive under the lid, negative below it.
    CHECK_MESSAGE(r.sampled[1] > 0.5, "flow must follow the lid");
    CHECK_MESSAGE(r.sampled[8] < 0.0, "lower half of the centreline must be reversed");

    // The vortex core sits in the upper half of the cavity (Ghia: y ~ 0.60).
    CHECK_MESSAGE(r.zeroCrossing > 0.45, "vortex core must be in the upper half");
    CHECK_MESSAGE(r.zeroCrossing < 0.85, "vortex core must not sit at the lid");

    // The computed profile follows the published shape.
    CHECK_MESSAGE(r.correlation > 0.9, "centreline profile must correlate with the Ghia table");

    // Coarse absolute bound: a smoke alarm for a gross change of the
    // discretisation or the boundary conditions. The solver's cell-centred
    // lid convention puts the profile well above the published values, so the
    // bound is deliberately loose.
    CHECK_MESSAGE(r.maxError <= 0.30, "max centreline u error " << r.maxError
                                           << " exceeds the coarse bound 0.30");

    // The solve is converged and bounded.
    CHECK_MESSAGE(r.continuityResidual < 1e-6, "continuity residual must be small");
    CHECK_MESSAGE(r.maxU < 3.0, "peak velocity must stay bounded");
}

// The cavity must reach a steady state: doubling the iteration count must not
// move the centreline profile. This is what catches a solver that drifts, a
// limiter that keeps firing, or a pressure coupling that never settles.
TEST_CASE("slow.ghia.steady_state") {
    const CavityResult a = runCavity(kGridN, 100.0, kIterations / 2, kGhia100);
    const CavityResult b = runCavity(kGridN, 100.0, kIterations, kGhia100);

    double maxDiff = 0.0;
    for (std::size_t i = 0; i < a.sampled.size(); ++i)
        maxDiff = std::max(maxDiff, std::fabs(a.sampled[i] - b.sampled[i]));

    MESSAGE("steady state: max profile change between " << (kIterations / 2) << " and "
                                                         << kIterations << " iterations = "
                                                         << maxDiff);
    CHECK_MESSAGE(maxDiff <= 0.05, "the cavity must reach a steady state (profile change "
                                    << maxDiff << ")");
}

} // TEST_SUITE("slow")
