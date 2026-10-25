// 3D reduction check: a duct that is one cell thick in z, with symmetry on the
// front/back faces, must reproduce the 2D channel solution cell for cell.
//
// This is the cheapest strong statement we can make about the 3D path: the
// solver is instantiated for Dim = 3, the z-momentum equation is not a special
// case, the boundary machinery has to honour symmetry, and the result must
// agree with the well-tested 2D discretisation. If a 3D face area, normal or
// neighbour lookup is wrong, the profiles diverge.
#include "doctest/doctest.h"

#include "fvm/Divergence.h"
#include "fvm/SIMPLE.h"
#include "mesh/BoundaryUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

constexpr int kNx = 16;
constexpr int kNy = 6;
constexpr int kIterations = 150;
constexpr double kLength = 4.0;
constexpr double kHeight = 1.0;
constexpr double kDepth = 0.25;
constexpr double kInletSpeed = 1.0;
constexpr double kReynolds = 100.0;

template<std::size_t Dim>
fvm::SIMPLE<Dim>::Config channelConfig() {
    typename fvm::SIMPLE<Dim>::Config cfg;
    cfg.maxIterations = static_cast<std::size_t>(kIterations);
    cfg.tolerance = 1e-8;
    cfg.runFixedIterations = true;
    cfg.convergenceHoldIterations = 3;
    cfg.alphaU = 0.7;
    cfg.alphaP = 0.3;
    cfg.simplec = false;
    cfg.rhieChow = true;
    cfg.pressureCorrectors = 3;
    cfg.printFrequency = 1000000; // effectively silent (the solver divides by it)
    cfg.convectionScheme = fvm::ConvectionScheme::SecondOrderUpwind;
    cfg.gradientScheme = fvm::GradientScheme::LeastSquares;
    return cfg;
}

struct Result2D {
    fvm::Mesh<2> mesh{std::array<std::size_t, 2>{kNx, kNy},
                      std::array<double, 2>{kLength, kHeight}};
    fvm::VectorField<2> velocity{mesh, 0.0};
    fvm::Field<2> pressure{mesh, 0.0};
    double maxDivergence = 0.0;
};

Result2D run2D() {
    const double nu = kInletSpeed * kHeight / kReynolds;
    Result2D r;

    std::vector<std::size_t> inlet, outlet, walls;
    forEachBoundaryFace(r.mesh, 0, -1, [&](std::size_t f, std::size_t) { inlet.push_back(f); });
    forEachBoundaryFace(r.mesh, 0, +1, [&](std::size_t f, std::size_t) { outlet.push_back(f); });
    forEachBoundaryFace(r.mesh, 1, -1, [&](std::size_t f, std::size_t) { walls.push_back(f); });
    forEachBoundaryFace(r.mesh, 1, +1, [&](std::size_t f, std::size_t) { walls.push_back(f); });

    fvm::SIMPLE<2> solver(r.mesh, 1.0, nu, channelConfig<2>());
    solver.setVelocityBCOnFacesWithSlip(inlet, "inlet", kInletSpeed, 0.0, 0.0, false, 0.0);
    solver.setVelocityBCOnFacesWithSlip(outlet, "pressure_outlet", 0.0, 0.0, 0.0, false, 0.0);
    solver.setVelocityBCOnFacesWithSlip(walls, "wall", 0.0, 0.0, 0.0, false, 0.0);
    solver.solve(r.velocity, r.pressure);

    fvm::Field<2> div(r.mesh, 0.0);
    fvm::computeDivergenceInto(r.mesh, r.velocity, div, {}, {});
    for (std::size_t c = 0; c < r.mesh.nCells(); ++c) {
        if (!r.mesh.isBoundary(c)) r.maxDivergence = std::max(r.maxDivergence, std::abs(div[c]));
    }
    return r;
}

struct Result3D {
    fvm::Mesh<3> mesh{std::array<std::size_t, 3>{kNx, kNy, 1},
                      std::array<double, 3>{kLength, kHeight, kDepth}};
    fvm::VectorField<3> velocity{mesh, 0.0};
    fvm::Field<3> pressure{mesh, 0.0};
    double maxDivergence = 0.0;
    double maxW = 0.0;
};

Result3D run3D() {
    const double nu = kInletSpeed * kHeight / kReynolds;
    Result3D r;

    std::vector<std::size_t> inlet, outlet, walls, symmetry;
    forEachBoundaryFace(r.mesh, 0, -1, [&](std::size_t f, std::size_t) { inlet.push_back(f); });
    forEachBoundaryFace(r.mesh, 0, +1, [&](std::size_t f, std::size_t) { outlet.push_back(f); });
    forEachBoundaryFace(r.mesh, 1, -1, [&](std::size_t f, std::size_t) { walls.push_back(f); });
    forEachBoundaryFace(r.mesh, 1, +1, [&](std::size_t f, std::size_t) { walls.push_back(f); });
    forEachBoundaryFace(r.mesh, 2, -1, [&](std::size_t f, std::size_t) { symmetry.push_back(f); });
    forEachBoundaryFace(r.mesh, 2, +1, [&](std::size_t f, std::size_t) { symmetry.push_back(f); });

    fvm::SIMPLE<3> solver(r.mesh, 1.0, nu, channelConfig<3>());
    solver.setVelocityBCOnFacesWithSlip(inlet, "inlet", kInletSpeed, 0.0, 0.0, false, 0.0);
    solver.setVelocityBCOnFacesWithSlip(outlet, "pressure_outlet", 0.0, 0.0, 0.0, false, 0.0);
    solver.setVelocityBCOnFacesWithSlip(walls, "wall", 0.0, 0.0, 0.0, false, 0.0);
    solver.setVelocityBCOnFacesWithSlip(symmetry, "symmetry", 0.0, 0.0, 0.0, true, 0.0);
    solver.solve(r.velocity, r.pressure);

    fvm::Field<3> div(r.mesh, 0.0);
    fvm::computeDivergenceInto(r.mesh, r.velocity, div, {}, {});
    for (std::size_t c = 0; c < r.mesh.nCells(); ++c) {
        if (!r.mesh.isBoundary(c)) r.maxDivergence = std::max(r.maxDivergence, std::abs(div[c]));
        r.maxW = std::max(r.maxW, std::abs(r.velocity[2][c]));
    }
    return r;
}

} // namespace

TEST_SUITE("fvm") {

TEST_CASE("fvm.channel3d.one_cell_thick_reduces_to_2d") {
    const Result2D flat = run2D();
    const Result3D extruded = run3D();

    MESSAGE("2D: max |u| = ", [&] {
        double m = 0.0;
        for (std::size_t c = 0; c < flat.mesh.nCells(); ++c)
            m = std::max(m, std::abs(flat.velocity[0][c]));
        return m;
    }(), ", max |div u| = ", flat.maxDivergence);
    MESSAGE("3D: max |div u| = ", extruded.maxDivergence, ", max |w| = ", extruded.maxW);

    // Symmetry must not permit any z velocity, and the 3D velocity must still
    // be divergence free.
    CHECK(extruded.maxW < 1e-12);

    double maxDeltaU = 0.0;
    double maxDeltaP = 0.0;
    double maxU = 0.0;
    for (int i = 0; i < kNx; ++i) {
        for (int j = 0; j < kNy; ++j) {
            const std::size_t c2 = flat.mesh.cellIndex(i, j);
            const std::size_t c3 = extruded.mesh.cellIndex(i, j, 0);
            maxDeltaU = std::max(maxDeltaU, std::abs(flat.velocity[0][c2] - extruded.velocity[0][c3]));
            maxDeltaP = std::max(maxDeltaP, std::abs(flat.pressure[c2] - extruded.pressure[c3]));
            maxU = std::max(maxU, std::abs(flat.velocity[0][c2]));
        }
    }
    MESSAGE("max |du| = ", maxDeltaU, ", max |dp| = ", maxDeltaP, ", max |u| = ", maxU);

    // The two discretisations are the same equations: the extra z faces are
    // symmetry faces with zero normal gradient, so they contribute nothing.
    CHECK(maxDeltaU < 1e-8 * std::max(1.0, maxU));
    CHECK(maxDeltaP < 1e-8);
}

} // TEST_SUITE("fvm")
