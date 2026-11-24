// Capability probe on a deliberately skewed all-tetrahedral 3D mesh.
//
// The shipped StaticMixer case already runs on a tet mesh, but it needs CGNS.
// This test owns its mesh: a 4 x 1 x 1 box of 12 x 5 x 5 cubes, every cube
// split into 6 tets, with the interior vertices jittered by up to 20% of the
// spacing. A steady SIMPLE run on it must finish, stay finite, keep the fields
// physical and conserve mass; the same case on the unjittered twin is reported
// as the skew penalty rather than asserted, because no accuracy target has been
// measured for this discretisation yet.
//
// This caught a tetrahedral-only divergence that the hexahedral channel missed.
#include "doctest/doctest.h"

#include "fvm/SIMPLE.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kNx = 12;
constexpr std::size_t kNy = 5;
constexpr std::size_t kNz = 5;
constexpr double kLx = 4.0;
constexpr double kLy = 1.0;
constexpr double kLz = 1.0;
constexpr double kInletSpeed = 1.0;
constexpr double kDensity = 1.0;
constexpr double kViscosity = 1.0e-2; // Re = U * Ly / nu = 100, laminar
constexpr int kIterations = 400;
constexpr double kJitterFraction = 0.2;

// Six tets per hex, all sharing the 0-7 body diagonal (Freudenthal split).
constexpr std::size_t kTetCorners[6][4] = {
    {0, 1, 3, 7}, {0, 1, 5, 7}, {0, 2, 3, 7},
    {0, 2, 6, 7}, {0, 4, 5, 7}, {0, 4, 6, 7},
};

// Deterministic pseudo-random number in [-1, 1]; no RNG, so the mesh and the
// reported metrics are identical on every run.
double signedUnit(std::size_t i, std::size_t j, std::size_t k, std::size_t salt) {
    std::uint64_t h = 0x9e3779b97f4a7c15ULL;
    const std::uint64_t parts[4] = {i, j, k, salt};
    for (std::uint64_t p : parts) {
        h ^= p + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
    }
    return static_cast<double>(h % 20001u) / 10000.0 - 1.0;
}

std::size_t vertexIndex(std::size_t i, std::size_t j, std::size_t k) {
    return i + (kNx + 1) * (j + (kNy + 1) * k);
}

fvm::Mesh<3> buildChannel(bool jittered) {
    const double hx = kLx / static_cast<double>(kNx);
    const double hy = kLy / static_cast<double>(kNy);
    const double hz = kLz / static_cast<double>(kNz);

    std::vector<std::array<double, 3>> vertices((kNx + 1) * (kNy + 1) * (kNz + 1));
    for (std::size_t k = 0; k <= kNz; ++k) {
        for (std::size_t j = 0; j <= kNy; ++j) {
            for (std::size_t i = 0; i <= kNx; ++i) {
                double x = static_cast<double>(i) * hx;
                double y = static_cast<double>(j) * hy;
                double z = static_cast<double>(k) * hz;
                // Boundary vertices stay on their planes, so the inlet, outlet
                // and wall patches are exactly flat.
                const bool interior = i > 0 && i < kNx && j > 0 && j < kNy && k > 0 && k < kNz;
                if (jittered && interior) {
                    x += kJitterFraction * hx * signedUnit(i, j, k, 1);
                    y += kJitterFraction * hy * signedUnit(i, j, k, 2);
                    z += kJitterFraction * hz * signedUnit(i, j, k, 3);
                }
                vertices[vertexIndex(i, j, k)] = {x, y, z};
            }
        }
    }

    std::vector<std::vector<std::size_t>> cells;
    cells.reserve(kNx * kNy * kNz * 6);
    for (std::size_t k = 0; k < kNz; ++k) {
        for (std::size_t j = 0; j < kNy; ++j) {
            for (std::size_t i = 0; i < kNx; ++i) {
                std::size_t corner[8];
                for (std::size_t d = 0; d < 2; ++d) {
                    for (std::size_t b = 0; b < 2; ++b) {
                        for (std::size_t a = 0; a < 2; ++a) {
                            corner[a + 2 * b + 4 * d] = vertexIndex(i + a, j + b, k + d);
                        }
                    }
                }
                for (const auto& tet : kTetCorners) {
                    cells.push_back({corner[tet[0]], corner[tet[1]],
                                     corner[tet[2]], corner[tet[3]]});
                }
                if (std::getenv("TET_TEST_HEX")) {
                    cells.pop_back(); cells.pop_back(); cells.pop_back();
                    cells.pop_back(); cells.pop_back(); cells.pop_back();
                    cells.push_back({corner[0], corner[1], corner[3], corner[2],
                                     corner[4], corner[5], corner[7], corner[6]});
                }
            }
        }
    }

    fvm::Mesh<3> mesh(std::move(vertices), std::move(cells));

    std::vector<std::size_t> inlet, outlet, walls;
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) != std::size_t(-1)) continue;
        const double x = mesh.faceCenter(f)[0];
        if (std::abs(x) < 1e-12) {
            inlet.push_back(f);
        } else if (std::abs(x - kLx) < 1e-12) {
            outlet.push_back(f);
        } else {
            walls.push_back(f);
        }
    }
    mesh.setBoundaryGroups({{"inlet", inlet}, {"outlet", outlet}, {"wall", walls}});
    return mesh;
}

struct ProbeResult {
    std::size_t cells = 0;
    std::size_t inletFaces = 0;
    std::size_t outletFaces = 0;
    double minVolume = 0.0;
    double maxVolume = 0.0;
    double totalVolume = 0.0;
    double minBoundaryArea = 0.0;
    double maxNonOrthogonalityDeg = 0.0;
    std::size_t nonPositiveProjectionFaces = 0;
    double minProjectionCos = 1.0;
    double maxSpeed = 0.0;
    double inletFlux = 0.0;
    double outletFlux = 0.0;
    double continuityResidual = 0.0;
    double massImbalance = 0.0;
    bool finite = false;
    fvm::SIMPLE<3>::Failure failure = fvm::SIMPLE<3>::Failure::None;
};

double boundaryFlux(const fvm::Mesh<3>& mesh, const std::vector<std::size_t>& faces,
                    const fvm::VectorField<3>& u) {
    double flux = 0.0;
    for (std::size_t f : faces) {
        const auto& n = mesh.faceNormal(f);
        const auto v = u.value(mesh.owner(f));
        flux += mesh.faceArea(f) * (n[0] * v[0] + n[1] * v[1] + n[2] * v[2]);
    }
    return flux;
}

ProbeResult runProbe(bool jittered) {
    ProbeResult r;
    fvm::Mesh<3> mesh = buildChannel(jittered);
    r.cells = mesh.nCells();
    const auto& groups = mesh.boundaryGroups();
    r.inletFaces = groups.at("inlet").size();
    r.outletFaces = groups.at("outlet").size();

    r.minVolume = r.minBoundaryArea = std::numeric_limits<double>::max();
    for (std::size_t c = 0; c < mesh.nCells(); ++c) {
        r.minVolume = std::min(r.minVolume, mesh.cellVolume(c));
        r.maxVolume = std::max(r.maxVolume, mesh.cellVolume(c));
        r.totalVolume += mesh.cellVolume(c);
    }
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) != std::size_t(-1)) continue;
        r.minBoundaryArea = std::min(r.minBoundaryArea, mesh.faceArea(f));
    }

    // OpenFOAM-style non-orthogonality: the angle between the face normal and
    // the line joining the two cell centres.
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) == std::size_t(-1)) continue;
        const auto& n = mesh.faceNormal(f);
        const auto& p = mesh.cellCenter(mesh.owner(f));
        const auto& q = mesh.cellCenter(mesh.neighbor(f));
        const double d[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
        const double mag = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const double signedCos = (n[0] * d[0] + n[1] * d[1] + n[2] * d[2]) / mag;
        r.minProjectionCos = std::min(r.minProjectionCos, signedCos);
        if (signedCos <= 0.0) ++r.nonPositiveProjectionFaces;
        const double cosAngle = std::clamp(
            std::abs(signedCos), 0.0, 1.0);
        r.maxNonOrthogonalityDeg =
            std::max(r.maxNonOrthogonalityDeg, std::acos(cosAngle) * 180.0 / 3.14159265358979323846);
    }

    fvm::VectorField<3> u(mesh, 0.0);
    fvm::Field<3> p(mesh, 0.0);

    fvm::SIMPLE<3>::Config cfg;
    cfg.stopping.steady.fixedIterations = kIterations;
    cfg.stopping.steady.maxIterations = kIterations;
    cfg.pressureCorrectors = 3;
    cfg.limitPressureCorrection = false;
    cfg.alphaU = 0.7;
    cfg.alphaP = 0.3;
    cfg.printFrequency = 1000000;
    cfg.convectionScheme = fvm::ConvectionScheme::SecondOrderUpwind;
    cfg.gradientScheme = fvm::GradientScheme::LeastSquares;

    fvm::SIMPLE<3> solver(mesh, kDensity, kViscosity, cfg);
    solver.setVelocityInletNormalOnFaces(groups.at("inlet"), kInletSpeed);
    solver.setPressureOutletOnFaces(groups.at("outlet"), 0.0);
    solver.setVelocityBCOnFaces(groups.at("wall"), fvm::VelocityBCType::NoSlip);
    solver.solve(u, p);

    r.failure = solver.failure();
    r.continuityResidual = solver.finalContinuityResidual();
    r.massImbalance = solver.finalMassImbalance();
    r.inletFlux = boundaryFlux(mesh, groups.at("inlet"), u);
    r.outletFlux = boundaryFlux(mesh, groups.at("outlet"), u);

    r.finite = true;
    for (std::size_t c = 0; c < u.size(); ++c) {
        const auto v = u.value(c);
        for (double component : v) {
            if (!std::isfinite(component)) r.finite = false;
        }
        r.maxSpeed = std::max(r.maxSpeed, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
    }
    for (std::size_t c = 0; c < p.size(); ++c) {
        if (!std::isfinite(p[c])) r.finite = false;
    }
    return r;
}

} // namespace

TEST_CASE("fvm.skewed_tet.steady_channel_conserves_mass") {
    const ProbeResult skewed = runProbe(true);
    const ProbeResult straight = runProbe(false);

    MESSAGE("skewed mesh: cells=", skewed.cells,
            " inlet faces=", skewed.inletFaces, " outlet faces=", skewed.outletFaces,
            " volume min/max=", skewed.minVolume, "/", skewed.maxVolume,
            " total=", skewed.totalVolume,
            " min boundary area=", skewed.minBoundaryArea,
            " max non-orthogonality=", skewed.maxNonOrthogonalityDeg, " deg",
            " non-positive projections=", skewed.nonPositiveProjectionFaces,
            " min cos=", skewed.minProjectionCos);
    MESSAGE("skewed solve: failure=", static_cast<int>(skewed.failure),
            " max|u|=", skewed.maxSpeed,
            " inlet flux=", skewed.inletFlux, " outlet flux=", skewed.outletFlux,
            " continuity=", skewed.continuityResidual,
            " mass imbalance=", skewed.massImbalance);
    MESSAGE("unjittered twin: max non-orthogonality=", straight.maxNonOrthogonalityDeg,
            " deg, continuity=", straight.continuityResidual,
            " skew penalty=", skewed.continuityResidual - straight.continuityResidual);

    // Geometry: all tets, all valid, and the tiling adds up to the box.
    REQUIRE(skewed.cells == kNx * kNy * kNz * 6);
    CHECK(skewed.minVolume > 0.0);
    CHECK(std::isfinite(skewed.minVolume));
    CHECK(std::isfinite(skewed.maxVolume));
    CHECK(skewed.totalVolume == doctest::Approx(kLx * kLy * kLz).epsilon(1e-9));
    CHECK(skewed.minBoundaryArea > 0.0);
    // Tets are inherently non-orthogonal; this pins that the fixture did not
    // silently degenerate into an orthogonal (wrong) mesh.
    CHECK(skewed.maxNonOrthogonalityDeg > 10.0);

    // Solve: it has to finish for the right reason and stay physical.
    CHECK(skewed.failure == fvm::SIMPLE<3>::Failure::None);
    CHECK(skewed.finite);
    CHECK(skewed.maxSpeed > 0.0);
    CHECK(skewed.maxSpeed < 100.0);
    CHECK(std::isfinite(skewed.continuityResidual));
    CHECK(std::isfinite(skewed.massImbalance));

    // Flow: in through x = 0, out through x = L, and the two balance.
    CHECK(skewed.inletFaces == (kNy * kNz * 2));
    CHECK(skewed.outletFaces == (kNy * kNz * 2));
    CHECK(skewed.inletFlux < 0.0);
    CHECK(skewed.outletFlux > 0.0);
    CHECK(std::abs(skewed.inletFlux + skewed.outletFlux) <=
          1e-2 * std::abs(skewed.inletFlux));

    // The unjittered twin runs the same case; the skew penalty is reported
    // above, but both runs must avoid a broken solver.
    CHECK(straight.failure == fvm::SIMPLE<3>::Failure::None);
    CHECK(straight.finite);
    CHECK(straight.maxSpeed > 0.0);
    CHECK(straight.maxSpeed < 100.0);
}
