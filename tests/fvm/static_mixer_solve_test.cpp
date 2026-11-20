// Real unstructured 3D solve on the shipped StaticMixer CGNS sample.
// The focused BC tests pin individual 3D rules; this one proves they hold
// together on a skewed tetrahedral mesh with four distinct patches.

#include "doctest/doctest.h"

#ifdef SCIRENDER_HAVE_CGNS

#include "fvm/SIMPLE.h"
#include "io/CGNSReader.h"
#include "support/test_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

std::array<double, 3> patchOutwardNormal(const fvm::Mesh<3>& mesh,
                                         const std::vector<std::size_t>& faces) {
    std::array<double, 3> n{0.0, 0.0, 0.0};
    for (std::size_t f : faces) {
        const double area = mesh.faceArea(f);
        for (std::size_t d = 0; d < 3; ++d) n[d] += area * mesh.faceNormal(f)[d];
    }
    const double mag = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (mag > 0.0) {
        for (double& v : n) v /= mag;
    }
    return n;
}

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

bool allFinite(const fvm::VectorField<3>& u, const fvm::Field<3>& p) {
    for (std::size_t i = 0; i < u.size(); ++i) {
        const auto v = u.value(i);
        for (double component : v) {
            if (!std::isfinite(component)) return false;
        }
    }
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (!std::isfinite(p[i])) return false;
    }
    return true;
}

double maxSpeed(const fvm::VectorField<3>& u) {
    double worst = 0.0;
    for (std::size_t i = 0; i < u.size(); ++i) {
        const auto v = u.value(i);
        worst = std::max(worst, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
    }
    return worst;
}

} // namespace

TEST_CASE("fvm.static_mixer.short_solve_stays_finite") {
    constexpr std::size_t kIters = 6;
    constexpr double kU = 1.0;
    constexpr double kNu = 1e-2;

    test_support::requireSample("cgns/StaticMixer.cgns");
    const auto r = fvm::CGNSReader<3>::read(
        test_support::samplePath("cgns/StaticMixer.cgns").string());

    CHECK(r.mesh.nCells() == 13761);
    REQUIRE(r.boundaryGroups.size() == 4);
    CHECK(r.boundaryGroups.at("StaticMixer Default").size() == 1582);
    CHECK(r.boundaryGroups.at("in1").size() == 14);
    CHECK(r.boundaryGroups.at("in2").size() == 14);
    CHECK(r.boundaryGroups.at("out").size() == 20);

    const auto& wall = r.boundaryGroups.at("StaticMixer Default");
    const auto& in1 = r.boundaryGroups.at("in1");
    const auto& in2 = r.boundaryGroups.at("in2");
    const auto& out = r.boundaryGroups.at("out");

    fvm::VectorField<3> u(r.mesh, 0.0);
    fvm::Field<3> p(r.mesh, 0.0);

    fvm::SIMPLE<3>::Config cfg;
    cfg.stopping.steady.fixedIterations = kIters;
    cfg.stopping.steady.maxIterations = kIters;
    cfg.pressureCorrectors = 1;
    cfg.printFrequency = 1000000;
    cfg.convectionScheme = fvm::ConvectionScheme::Upwind;

    std::vector<double> continuityHistory;
    cfg.iterationCallback = [&](int, double continuity, double) {
        continuityHistory.push_back(continuity);
    };

    fvm::SIMPLE<3> solver(r.mesh, 1.0, kNu, cfg);
    solver.setVelocityBCOnFaces(wall, fvm::VelocityBCType::NoSlip);

    const auto n1 = patchOutwardNormal(r.mesh, in1);
    const auto n2 = patchOutwardNormal(r.mesh, in2);
    solver.setVelocityBCOnFaces(in1, fvm::VelocityBCType::Inlet,
                                -kU * n1[0], -kU * n1[1], -kU * n1[2]);
    solver.setVelocityBCOnFaces(in2, fvm::VelocityBCType::Inlet,
                                -kU * n2[0], -kU * n2[1], -kU * n2[2]);
    solver.setPressureOutletOnFaces(out, 0.0);

    solver.solve(u, p);

    const double inletFlux = boundaryFlux(r.mesh, in1, u) + boundaryFlux(r.mesh, in2, u);
    const double outletFlux = boundaryFlux(r.mesh, out, u);
    MESSAGE("inlet flux=", inletFlux, " outlet flux=", outletFlux,
            " max|u|=", maxSpeed(u));

    CHECK(solver.failure() == fvm::SIMPLE<3>::Failure::None);
    CHECK(solver.status() == fvm::SIMPLE<3>::Status::NotConverged);
    CHECK(solver.lastIterations() == static_cast<int>(kIters));
    CHECK(continuityHistory.size() == kIters);
    CHECK(std::isfinite(solver.finalContinuityResidual()));
    CHECK(std::isfinite(solver.finalNormalizedContinuityResidual()));
    CHECK(std::isfinite(solver.finalMassImbalance()));
    CHECK(std::isfinite(solver.finalMomentumResidual()));
    CHECK(allFinite(u, p));
    CHECK(maxSpeed(u) > 0.0);
    CHECK(maxSpeed(u) < 100.0);
    CHECK(inletFlux < -1e-9);
    CHECK(outletFlux > 1e-9);
    for (std::size_t f : out) {
        CHECK(std::abs(p[r.mesh.owner(f)]) < 1e-8);
    }
}

#else

TEST_CASE("fvm.static_mixer.cgns_not_built_is_noop") {
    MESSAGE("CGNS not built (SCIRENDER_HAVE_CGNS undefined); StaticMixer solve skipped");
    CHECK(true);
}

#endif // SCIRENDER_HAVE_CGNS
