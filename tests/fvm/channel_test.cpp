// An open channel: a velocity inlet on the left, a pressure outlet on the
// right, no-slip walls.
//
// The lid-driven cavity cannot expose the pressure-velocity coupling defect:
// a closed cavity has no net outflow, so the spurious near-null mode of the
// pure-Neumann pressure operator produces no gradient and the corruption
// cancels. An open channel does - the outlet lets the flow adjust, so the
// pressure correction has a real, non-zero right-hand side.
//
// The steady state of a straight channel is a developed flow, so:
//   - the divergence of the final velocity must vanish in the interior;
//   - the mass flux through the inlet and through the outlet must balance;
//   - the centreline profile must be smooth (no checkerboard).
#include "doctest/doctest.h"
#include "fvm/Divergence.h"
#include "fvm/SIMPLE.h"
#include "mesh/BoundaryUtils.h"
#include "support/fvm_fixtures.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace test_support;

namespace {

constexpr int kNx = 40;
constexpr int kNy = 8;
constexpr double kLength = 4.0;
constexpr double kHeight = 1.0;
constexpr double kInletSpeed = 1.0;
constexpr double kReynolds = 100.0; // based on the height and the mean velocity
constexpr int kIterations = 600;

struct ChannelResult {
    double maxDivergence = 0.0;  // the largest |div u| in the interior
    double inletFlux = 0.0;
    double outletFlux = 0.0;
    double maxU = 0.0;
    std::vector<double> centreline;
    // The largest |div u| per column, so the corruption can be located.
    std::vector<double> divergencePerColumn;
    std::vector<double> meanPerColumn;
};

ChannelResult runChannel() {
    const double nu = kInletSpeed * kHeight / kReynolds; // rho = 1

    fvm::Mesh<2> mesh = fvm::Mesh<2>(std::array<std::size_t, 2>{static_cast<std::size_t>(kNx),
                                                                 static_cast<std::size_t>(kNy)},
                                      std::array<double, 2>{kLength, kHeight});

    // The boundary faces of the channel.
    std::vector<std::size_t> inlet, outlet, walls;
    forEachBoundaryFace(mesh, 0, -1, [&](std::size_t f, std::size_t) { inlet.push_back(f); });
    forEachBoundaryFace(mesh, 0, +1, [&](std::size_t f, std::size_t) { outlet.push_back(f); });
    forEachBoundaryFace(mesh, 1, -1, [&](std::size_t f, std::size_t) { walls.push_back(f); });
    forEachBoundaryFace(mesh, 1, +1, [&](std::size_t f, std::size_t) { walls.push_back(f); });

    fvm::SIMPLE<2>::Config cfg;
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

    fvm::SIMPLE<2> solver(mesh, 1.0, nu, cfg);

    // The patches, the way the application applies them: a boundary condition
    // on the faces of a group.
    solver.setVelocityBCOnFacesWithSlip(inlet, "inlet", kInletSpeed, 0.0, 0.0, false, 0.0);
    solver.setVelocityBCOnFacesWithSlip(outlet, "pressure_outlet", 0.0, 0.0, 0.0, false, 0.0);
    solver.setVelocityBCOnFacesWithSlip(walls, "wall", 0.0, 0.0, 0.0, false, 0.0);

    fvm::VectorField<2> velocity(mesh, 0.0);
    fvm::Field<2> pressure(mesh, 0.0);
    solver.solve(velocity, pressure);

    ChannelResult r;
    for (std::size_t c = 0; c < mesh.nCells(); ++c)
        r.maxU = std::max(r.maxU, std::abs(velocity[0][c]));

    // The divergence of the final velocity. This is the property the pressure
    // correction is supposed to deliver: a divergence-free velocity.
    fvm::Field<2> div(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, velocity, div, {}, {});
    r.divergencePerColumn.assign(static_cast<std::size_t>(kNx), 0.0);
    r.meanPerColumn.assign(static_cast<std::size_t>(kNx), 0.0);
    std::vector<int> columnCells(static_cast<std::size_t>(kNx), 0);
    for (std::size_t c = 0; c < mesh.nCells(); ++c) {
        const auto coords = mesh.cellCoords(c);
        const std::size_t col = static_cast<std::size_t>(coords[0]);
        r.divergencePerColumn[col] = std::max(r.divergencePerColumn[col], std::abs(div[c]));
        r.meanPerColumn[col] += velocity[0][c];
        ++columnCells[col];
        if (!mesh.isBoundary(c))
            r.maxDivergence = std::max(r.maxDivergence, std::abs(div[c]));
    }
    for (std::size_t col = 0; col < r.meanPerColumn.size(); ++col)
        if (columnCells[col] > 0)
            r.meanPerColumn[col] /= static_cast<double>(columnCells[col]);

    // The flux through the inlet and through the outlet.
    auto fluxOf = [&](const std::vector<std::size_t>& faces) {
        double flux = 0.0;
        for (std::size_t f : faces) {
            const std::size_t P = mesh.owner(f);
            const auto& n = mesh.faceNormal(f);
            flux += velocity[0][P] * n[0] * mesh.faceArea(f);
        }
        return flux;
    };
    r.inletFlux = fluxOf(inlet);
    r.outletFlux = fluxOf(outlet);

    // The centreline profile in the developed part of the channel.
    for (std::size_t i = 0; i < static_cast<std::size_t>(kNx); ++i)
        r.centreline.push_back(velocity[0][mesh.cellIndex(i, kNy / 2)]);
    return r;
}

} // namespace

TEST_SUITE("fvm") {

// The open channel must reach a divergence-free steady state: the pressure
// correction has to cancel the divergence of the predictor exactly.
TEST_CASE("fvm.channel.open_outlet_divergence") {
    const ChannelResult r = runChannel();

    MESSAGE("open channel: max|u| = ", r.maxU, ", max |div u| (interior) = ", r.maxDivergence,
            ", inlet flux = ", r.inletFlux, ", outlet flux = ", r.outletFlux);
    for (std::size_t col = 0; col < r.divergencePerColumn.size(); ++col)
        MESSAGE("  column ", col, ": max |div u| = ", r.divergencePerColumn[col], ", mean u = ",
                r.meanPerColumn[col]);

    // The scale of the velocity gradient: |u| over the channel length.
    const double gradScale = r.maxU / kLength;

    // 1. The velocity must be divergence free. The pressure-velocity coupling
    //    is what delivers this, and it is what the open boundary exercises.
    CHECK_MESSAGE(r.maxDivergence < 1e-3 * gradScale,
                  "the steady velocity must be divergence free (max |div u| = " << r.maxDivergence
                                                                                << ")");

    // 2. Mass must be conserved: what goes in comes out. The inlet's outward
    //    normal points upstream, so the fluxes have opposite signs.
    CHECK_MESSAGE(std::abs(std::abs(r.inletFlux) - std::abs(r.outletFlux)) <
                      1e-3 * std::abs(r.inletFlux),
                  "the inlet and the outlet fluxes must balance");

    // 3. The profile must be free of a checkerboard.
    double roughest = 0.0;
    for (std::size_t i = 1; i < r.centreline.size(); ++i)
        roughest = std::max(roughest, std::abs(r.centreline[i] - r.centreline[i - 1]));
    MESSAGE("centreline roughness = ", roughest, ", centreline:");
    for (std::size_t i = 0; i < r.centreline.size(); ++i)
        MESSAGE("  x = ", i, " u = ", r.centreline[i]);
    CHECK_MESSAGE(roughest < 0.05 * r.maxU, "the centreline profile must be smooth");
}

} // TEST_SUITE("fvm")
