// Boundary conditions: the cell-mask path and the face path.
//
// Contract pinned here:
//   - on a structured mesh a wall cell is frozen by the cell mask: all
//     components for NoSlip and MovingWall, only the normal component for an
//     inlet (the tangential ones become a slip wall);
//   - a moving wall prescribed on FACES must actually drive the momentum
//     equation - the face path is not a decoration;
//   - on an unstructured mesh the wall condition is applied through the face
//     path, and the wall must stay impermeable.
#include "doctest/doctest.h"
#include "fvm/SIMPLE.h"
#include "fvm/SIMPLEBoundaryConditions.h"
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

// A wall cell is frozen: all components carry the prescribed value.
TEST_CASE("cfd.bc.cell_mask_covers_wall_cells") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    fvm::SIMPLEBoundaryConditions<2> bc(mesh);
    bc.setVelocityBC(0, -1, fvm::VelocityBCType::NoSlip);
    bc.setVelocityBC(0, +1, fvm::VelocityBCType::NoSlip);
    bc.setVelocityBC(1, -1, fvm::VelocityBCType::NoSlip);
    bc.setVelocityBC(1, +1, fvm::VelocityBCType::MovingWall, 1.0, 0.0, 0.0);
    const fvm::BCDiscretization disc = bc.build();

    // The lid: all components prescribed, u = 1 and v = 0.
    for (std::size_t idx : wallCells(mesh, 1, +1)) {
        CAPTURE(idx);
        CHECK_MESSAGE(disc.dirichletMask(0)[idx] == 1, "the lid must prescribe u");
        CHECK_MESSAGE(disc.dirichletMask(1)[idx] == 1, "the lid must prescribe v");
        CHECK_MESSAGE(std::abs(disc.dirichletVals(0)[idx] - 1.0) < 1e-15, "the lid velocity");
        CHECK_MESSAGE(std::abs(disc.dirichletVals(1)[idx]) < 1e-15, "the lid velocity");
    }

    // The walls: all components prescribed, zero.
    for (int sign : {-1, +1}) {
        for (std::size_t idx : wallCells(mesh, 0, sign)) {
            CAPTURE(idx);
            CHECK_MESSAGE(disc.dirichletMask(0)[idx] == 1, "a wall must prescribe u");
            CHECK_MESSAGE(disc.dirichletMask(1)[idx] == 1, "a wall must prescribe v");
            CHECK_MESSAGE(std::abs(disc.dirichletVals(0)[idx]) < 1e-15, "a wall is no-slip");
            CHECK_MESSAGE(std::abs(disc.dirichletVals(1)[idx]) < 1e-15, "a wall is no-slip");
        }
    }
}

// An inlet prescribes only the normal component; the tangential ones become a
// slip wall.
TEST_CASE("cfd.bc.inlet_masks_normal_component_only") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    fvm::SIMPLEBoundaryConditions<2> bc(mesh);
    bc.setVelocityBC(0, -1, fvm::VelocityBCType::Inlet, 1.0, 0.0, 0.0);
    const fvm::BCDiscretization disc = bc.build();

    for (std::size_t idx : wallCells(mesh, 0, -1)) {
        CAPTURE(idx);
        CHECK_MESSAGE(disc.dirichletMask(0)[idx] == 1, "the inlet must prescribe the normal component");
        CHECK_MESSAGE(std::abs(disc.dirichletVals(0)[idx] - 1.0) < 1e-15, "the inlet velocity");
        CHECK_MESSAGE(disc.dirichletMask(1)[idx] == 0,
                      "the inlet must not prescribe the tangential component");
        CHECK_MESSAGE(disc.slipWallMask(1)[idx] == 1, "the tangential component is a slip wall");
    }
}

// A moving wall prescribed on the FACES must drive the flow. If the face path
// is not consumed by the momentum assembly the domain stays at rest.
TEST_CASE("cfd.bc.face_path_moving_wall_drives_flow") {
    const fvm::Mesh<2> mesh = uniform2D(8);
    const double lidSpeed = 1.0;

    fvm::SIMPLE<2> solver(mesh, 1.0, 0.01, solverConfig());
    // The lid is prescribed on its faces ...
    solver.setVelocityBCOnFaces(boundaryFaces(mesh, 1, +1), fvm::VelocityBCType::MovingWall,
                                 lidSpeed, 0.0, 0.0);
    // ... the other three walls are no-slip cells.
    solver.setVelocityBC(0, -1, fvm::VelocityBCType::NoSlip);
    solver.setVelocityBC(0, +1, fvm::VelocityBCType::NoSlip);
    solver.setVelocityBC(1, -1, fvm::VelocityBCType::NoSlip);

    // The face condition must reach the discretisation, and the cells that own
    // a face-conditioned face must NOT be frozen by the cell mask: the
    // momentum assembly skips the boundary faces of a frozen cell, so a frozen
    // lid cell would silently ignore the moving wall.
    {
        fvm::SIMPLEBoundaryConditions<2> probe(mesh);
        probe.setVelocityBCOnFaces(boundaryFaces(mesh, 1, +1), fvm::VelocityBCType::MovingWall,
                                   lidSpeed, 0.0, 0.0);
        probe.setVelocityBC(0, -1, fvm::VelocityBCType::NoSlip);
        probe.setVelocityBC(0, +1, fvm::VelocityBCType::NoSlip);
        probe.setVelocityBC(1, -1, fvm::VelocityBCType::NoSlip);
        const fvm::BCDiscretization disc = probe.build();

        const auto lidFaces = boundaryFaces(mesh, 1, +1);
        int lidFacesWithMovingWall = 0;
        for (std::size_t f : lidFaces) {
            if (f < disc.faceHasBC_.size() && disc.faceHasBC_[f] &&
                disc.faceBC_[f].type == fvm::VelocityBCType::MovingWall)
                ++lidFacesWithMovingWall;
        }
        CHECK_MESSAGE(lidFacesWithMovingWall == static_cast<int>(lidFaces.size()),
                      "every lid face must carry the moving wall condition");

        int lidCellsFrozen = 0;
        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            const auto c = mesh.cellCoords(i);
            const bool onLid = (c[1] == mesh.dims()[1] - 1);
            const bool frozen = disc.dirichletMask_[0][i] || disc.dirichletMask_[1][i];
            if (onLid && frozen)
                ++lidCellsFrozen;
        }
        CHECK_MESSAGE(lidCellsFrozen == 0,
                      "a cell that owns a face-conditioned face must not be frozen");
    }

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
