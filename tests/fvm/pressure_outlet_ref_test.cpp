#include "doctest/doctest.h"
#include "fvm/SIMPLE.h"
#include "mesh/BoundaryUtils.h"
#include "support/fvm_fixtures.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

using namespace test_support;

TEST_SUITE("fvm") {

TEST_CASE("fvm.pressure_outlet.per_face_reference_at_corner") {
    const fvm::Mesh<3> mesh = uniform3D(2);
    std::vector<std::size_t> xminus, yminus;
    forEachBoundaryFace(mesh, 0, -1, [&](std::size_t f, std::size_t) { xminus.push_back(f); });
    forEachBoundaryFace(mesh, 1, -1, [&](std::size_t f, std::size_t) { yminus.push_back(f); });
    REQUIRE_FALSE(xminus.empty());
    REQUIRE_FALSE(yminus.empty());

    fvm::SIMPLE<3>::Config cfg;
    cfg.stopping.steady.fixedIterations = 1;
    cfg.stopping.steady.maxIterations = 1;
    cfg.pressureCorrectors = 1;
    cfg.printFrequency = 1000000;
    fvm::SIMPLE<3> solver(mesh, 1.0, 0.01, cfg);

    solver.setPressureOutletOnFaces(xminus, 10.0);
    solver.setPressureOutletOnFaces(yminus, 0.0);
    for (std::size_t d = 0; d < 3; ++d) {
        solver.setVelocityBC(d, -1, fvm::VelocityBCType::NoSlip);
        solver.setVelocityBC(d, +1, fvm::VelocityBCType::NoSlip);
    }

    std::vector<std::pair<std::size_t, double>> outletFaces;
    for (std::size_t f : xminus) outletFaces.emplace_back(f, 10.0);
    for (std::size_t f : yminus) outletFaces.emplace_back(f, 0.0);
    std::sort(outletFaces.begin(), outletFaces.end());

    const std::size_t shared = mesh.cellIndex(0, 0, 0);
    // The lowest-numbered outlet face owns the shared cell's pRef.
    double expected = 0.0;
    bool found = false;
    for (const auto& [f, p] : outletFaces) {
        if (mesh.owner(f) == shared) { expected = p; found = true; break; }
    }
    REQUIRE(found);

    fvm::VectorField<3> u(mesh, 0.0);
    fvm::Field<3> p(mesh, 0.0);
    solver.solve(u, p);

    CHECK(p[shared] == doctest::Approx(expected));
    CHECK(p[mesh.cellIndex(0, 1, 0)] == doctest::Approx(10.0));
    CHECK(p[mesh.cellIndex(1, 0, 0)] == doctest::Approx(0.0));
}

} // TEST_SUITE("fvm")
