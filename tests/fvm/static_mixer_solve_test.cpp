// Real unstructured 3D solve on the shipped StaticMixer CGNS sample.
// The focused BC tests pin individual 3D rules; this one proves they hold
// together on a skewed tetrahedral mesh with four distinct patches.

#include "doctest/doctest.h"

#ifdef SCIRENDER_HAVE_CGNS

#include "cfd/material.h"
#include "fvm/SIMPLE.h"
#include "io/CGNSReader.h"
#include "support/test_support.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

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

TEST_CASE("fvm.static_mixer.gui_physics_stays_finite") {
    // GUI repro of the StaticMixer divergence: Air defaults, no turbulence,
    // both inlets at 1 m/s through the normal-velocity feature, pressure
    // outlet with p = 0, everything else wall, steady, UI default config.
    constexpr std::size_t kIters = 10;
    constexpr double kU = 1.0;
    const cfd::Material air = cfd::materials::air();

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
    cfg.limitPressureCorrection = false;
    cfg.alphaU = 0.7;
    cfg.alphaP = 0.3;
    cfg.printFrequency = 1;
    cfg.convectionScheme = fvm::ConvectionScheme::SecondOrderUpwind;
    cfg.momentumAmg.verbosity = cfd::AmgSettings::Verbosity::Diagnosis;  // DBG: AMG diagnostics

    std::vector<double> continuityHistory;
    cfg.iterationCallback = [&](int, double continuity, double) {
        continuityHistory.push_back(continuity);
    };

    fvm::SIMPLE<3> solver(r.mesh, air.rho, air.nu(), cfg);
    solver.setVelocityBCOnFaces(wall, fvm::VelocityBCType::NoSlip);
    solver.setVelocityInletNormalOnFaces(in1, kU);
    solver.setVelocityInletNormalOnFaces(in2, kU);
    solver.setPressureOutletOnFaces(out, 0.0);

    solver.solve(u, p);

    const double inletFlux = boundaryFlux(r.mesh, in1, u) + boundaryFlux(r.mesh, in2, u);
    const double outletFlux = boundaryFlux(r.mesh, out, u);
    MESSAGE("inlet flux=", inletFlux, " outlet flux=", outletFlux,
            " max|u|=", maxSpeed(u));
    double outletMaxP = 0.0;
    for (std::size_t f : out) outletMaxP = std::max(outletMaxP, std::abs(p[r.mesh.owner(f)]));
    MESSAGE("outlet cell max|p|=", outletMaxP);

    CHECK(solver.failure() == fvm::SIMPLE<3>::Failure::None);
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
}

#else

TEST_CASE("fvm.static_mixer.cgns_not_built_is_noop") {
    MESSAGE("CGNS not built (SCIRENDER_HAVE_CGNS undefined); StaticMixer solve skipped");
    CHECK(true);
}

#endif // SCIRENDER_HAVE_CGNS
