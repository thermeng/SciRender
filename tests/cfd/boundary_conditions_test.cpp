// Boundary conditions: the observable flow consequences of walls and inlets.
//
// Contract pinned here (outcomes, not mask internals):
//   - no-slip / moving walls hold their prescribed velocity in the solution;
//   - an inlet drives flow into the domain;
//   - a moving wall prescribed on FACES must actually drive the flow;
//   - on an unstructured mesh the wall must stay impermeable.
#include "doctest/doctest.h"
#include "fvm/SIMPLE.h"
#include "mesh/BoundaryUtils.h"
#include "support/fvm_fixtures.h"

#include <cmath>
#include <vector>

using namespace test_support;

namespace {

// The cells of a wall without the corners: the corner cells are shared by two
// walls and the boundary builder resolves them separately.
std::vector<std::size_t> wallCells(const fvm::Mesh<2>& mesh, std::size_t d, int sign) {
    std::vector<std::size_t> cells;
    const std::size_t n = mesh.dims()[d];
    const std::size_t other = 1 - d;
    for (std::size_t k = 1; k + 1 < n; ++k) {
        std::array<std::size_t, 2> c{0, 0};
        c[d] = (sign > 0) ? n - 1 : 0;
        c[other] = k;
        cells.push_back(mesh.cellIndex(c[0], c[1]));
    }
    return cells;
}

std::vector<std::size_t> boundaryFaces(const fvm::Mesh<2>& mesh, std::size_t d, int sign) {
    std::vector<std::size_t> faces;
    forEachBoundaryFace(mesh, d, sign, [&](std::size_t f, std::size_t) { faces.push_back(f); });
    return faces;
}

fvm::SIMPLE<2>::Config solverConfig() {
    fvm::SIMPLE<2>::Config cfg;
    cfg.maxIterations = 30;
    cfg.tolerance = 1e-6;
    cfg.runFixedIterations = true;
    cfg.convergenceHoldIterations = 2;
    cfg.alphaU = 0.7;
    cfg.alphaP = 0.3;
    cfg.pressureCorrectors = 2;
    cfg.printFrequency = 1000000;
    cfg.convectionScheme = fvm::ConvectionScheme::SecondOrderUpwind;
    cfg.gradientScheme = fvm::GradientScheme::LeastSquares;
    return cfg;
}

} // namespace

TEST_SUITE("cfd") {

// The lid holds its moving-wall velocity and the side walls stay at rest in
// the computed solution: the wall condition is honored by the flow.
TEST_CASE("cfd.bc.wall_cells_hold_prescribed_velocity") {
    const fvm::Mesh<2> mesh = uniform2D(8);
    fvm::SIMPLE<2> solver(mesh, 1.0, 0.01, solverConfig());
    solver.setVelocityBC(0, -1, fvm::VelocityBCType::NoSlip);
    solver.setVelocityBC(0, +1, fvm::VelocityBCType::NoSlip);
    solver.setVelocityBC(1, -1, fvm::VelocityBCType::NoSlip);
    solver.setVelocityBC(1, +1, fvm::VelocityBCType::MovingWall, 1.0, 0.0, 0.0);

    fvm::VectorField<2> velocity(mesh, 0.0);
    fvm::Field<2> pressure(mesh, 0.0);
    solver.solve(velocity, pressure);

    for (std::size_t idx : wallCells(mesh, 1, +1)) {
        CAPTURE(idx);
        CHECK_MESSAGE(std::abs(velocity[0][idx] - 1.0) < 1e-6, "the lid must hold u = 1");
        CHECK_MESSAGE(std::abs(velocity[1][idx]) < 1e-6, "the lid must hold v = 0");
    }
    for (int sign : {-1, +1}) {
        for (std::size_t idx : wallCells(mesh, 0, sign)) {
            CAPTURE(idx);
            CHECK_MESSAGE(std::abs(velocity[0][idx]) < 1e-6, "a no-slip wall must hold u = 0");
            CHECK_MESSAGE(std::abs(velocity[1][idx]) < 1e-6, "a no-slip wall must hold v = 0");
        }
    }
}

// An inlet drives flow into the domain: the inlet cell holds the inlet speed
// and the interior is set in motion.
TEST_CASE("cfd.bc.inlet_drives_flow") {
    const fvm::Mesh<2> mesh = uniform2D(8);
    fvm::SIMPLE<2> solver(mesh, 1.0, 0.01, solverConfig());
    solver.setVelocityBC(0, -1, fvm::VelocityBCType::Inlet, 1.0, 0.0, 0.0);
    solver.setVelocityBC(0, +1, fvm::VelocityBCType::NoSlip);
    solver.setVelocityBC(1, -1, fvm::VelocityBCType::NoSlip);
    solver.setVelocityBC(1, +1, fvm::VelocityBCType::NoSlip);

    fvm::VectorField<2> velocity(mesh, 0.0);
    fvm::Field<2> pressure(mesh, 0.0);
    solver.solve(velocity, pressure);

    const double inletspeed = 1.0;
    for (std::size_t idx : wallCells(mesh, 0, -1)) {
        CAPTURE(idx);
        CHECK_MESSAGE(std::abs(velocity[0][idx] - inletspeed) < 1e-6,
                      "the inlet cell must hold the inlet speed");
    }
    double maxU = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i)
        maxU = std::max(maxU, std::abs(velocity[0][i]));
    MESSAGE("max |u| with an inlet = ", maxU);
    CHECK_MESSAGE(maxU > 0.1, "the inlet must drive flow into the domain");
}

// A moving wall prescribed on the FACES must drive the flow. If the face path
// is not consumed by the momentum assembly the domain stays at rest.
TEST_CASE("cfd.bc.face_path_moving_wall_drives_flow") {
    const fvm::Mesh<2> mesh = uniform2D(8);
    const double lidSpeed = 1.0;

    fvm::SIMPLE<2> solver(mesh, 1.0, 0.01, solverConfig());
    solver.setVelocityBCOnFaces(boundaryFaces(mesh, 1, +1), fvm::VelocityBCType::MovingWall,
                                 lidSpeed, 0.0, 0.0);
    solver.setVelocityBC(0, -1, fvm::VelocityBCType::NoSlip);
    solver.setVelocityBC(0, +1, fvm::VelocityBCType::NoSlip);
    solver.setVelocityBC(1, -1, fvm::VelocityBCType::NoSlip);

    fvm::VectorField<2> velocity(mesh, 0.0);
    fvm::Field<2> pressure(mesh, 0.0);
    solver.solve(velocity, pressure);

    double maxU = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i)
        maxU = std::max(maxU, std::abs(velocity[0][i]));
    MESSAGE("max |u| after a face-prescribed moving wall = ", maxU);
    CHECK_MESSAGE(maxU > 0.1 * lidSpeed,
                  "a moving wall prescribed on the faces must drive the flow");
}

// On an unstructured mesh the wall condition is applied through the face path.
// The wall must stay impermeable: the normal velocity of the wall cells must
// vanish.
TEST_CASE("cfd.bc.unstructured_uses_face_path") {
    const fvm::Mesh<2> mesh = triangles2D();

    fvm::SIMPLE<2> solver(mesh, 1.0, 0.01, solverConfig());
    solver.setVelocityBC(0, -1, fvm::VelocityBCType::NoSlip);
    solver.setVelocityBC(0, +1, fvm::VelocityBCType::NoSlip);
    solver.setVelocityBC(1, -1, fvm::VelocityBCType::NoSlip);
    solver.setVelocityBC(1, +1, fvm::VelocityBCType::NoSlip);

    fvm::VectorField<2> velocity(mesh, 0.0);
    fvm::Field<2> pressure(mesh, 0.0);
    solver.solve(velocity, pressure);

    double worstNormal = 0.0;
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) != std::size_t(-1))
            continue;
        const std::size_t P = mesh.owner(f);
        const auto& n = mesh.faceNormal(f);
        const double un = velocity[0][P] * n[0] + velocity[1][P] * n[1];
        worstNormal = std::max(worstNormal, std::abs(un));
    }
    MESSAGE("worst normal velocity at a wall = ", worstNormal);
    CHECK_MESSAGE(worstNormal < 1e-6, "a no-slip wall must be impermeable");
}

} // TEST_SUITE("cfd")
