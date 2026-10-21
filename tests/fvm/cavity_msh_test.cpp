// The lid-driven cavity on the ANSYS mesh (samples/cfd/CavityQuad_40x40.msh),
// set up the way an end user sets it up in the application: the mesh's named
// boundary groups are the "patches", and every patch is a boundary condition
// applied to the faces of that group.
//
// The reference is Ghia, Giri & Podkar (1982), Table III (Re = 400): the
// velocity component along the vertical centre line of a square cavity whose
// top wall moves with unit speed.
//
// The test is a diagnostic as much as a gate: it reports the peak velocity,
// the centreline profile against the published table, the mass balance and the
// centre of the primary vortex, so every solver fix can be measured against
// them.
#include "doctest/doctest.h"
#include "fvm/SIMPLE.h"
#include "io/ANSYSMeshReader.h"
#include "support/cavity_reference.h"
#include "support/test_support.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace test_support;

namespace {

constexpr double kLidSpeed = 1.0;
constexpr double kReynolds = 400.0;
constexpr int kIterations = 800;

struct CavitySetup {
    fvm::Mesh<2> mesh;
    std::string lidGroup;
    double xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    std::size_t topFaces = 0;
    std::size_t topInLid = 0;
};

// Loads the mesh and works out which named group is the lid.
CavitySetup loadCavity() {
    requireSample("cfd/CavityQuad_40x40.msh");
    const std::string path = samplePath("cfd/CavityQuad_40x40.msh").string();

    CavitySetup s;
    auto read = fvm::ANSYSMeshReader<2>::read(path);
    s.mesh = std::move(read.mesh);

    s.xmin = s.ymin = 1e300;
    s.xmax = s.ymax = -1e300;
    for (std::size_t f = 0; f < s.mesh.nFaces(); ++f) {
        if (s.mesh.neighbor(f) != std::size_t(-1))
            continue;
        const double* c = s.mesh.faceCenterPtr(f);
        s.xmin = std::min(s.xmin, c[0]);
        s.xmax = std::max(s.xmax, c[0]);
        s.ymin = std::min(s.ymin, c[1]);
        s.ymax = std::max(s.ymax, c[1]);
    }

    const auto& groups = s.mesh.boundaryGroups();
    double bestY = -1e300;
    for (const auto& [name, faces] : groups) {
        if (faces.empty())
            continue;
        double meanY = 0.0;
        for (std::size_t f : faces)
            meanY += s.mesh.faceCenterPtr(f)[1];
        meanY /= static_cast<double>(faces.size());
        if (meanY > bestY) {
            bestY = meanY;
            s.lidGroup = name;
        }
    }

    // Every boundary face on the top wall has to belong to the lid group,
    // otherwise part of the lid would be a no-slip wall.
    const auto& lidFaces = groups.at(s.lidGroup);
    for (std::size_t f = 0; f < s.mesh.nFaces(); ++f) {
        if (s.mesh.neighbor(f) != std::size_t(-1))
            continue;
        if (std::abs(s.mesh.faceCenterPtr(f)[1] - s.ymax) > 1e-6)
            continue;
        ++s.topFaces;
        if (std::find(lidFaces.begin(), lidFaces.end(), f) != lidFaces.end())
            ++s.topInLid;
    }
    return s;
}

struct CavityRun {
    double maxU = 0.0;
    double continuity = 0.0;
    std::vector<double> sampled; // the centreline u at the interior reference heights
    CavityComparison cmp;
};

// Runs the cavity and evaluates the centreline profile against the published
// table. The two wall points (y = 0 and y = 1) are excluded: the published
// values there are the WALL values, while the solver reports the value in the
// cell next to the wall.
CavityRun runCavity(const CavitySetup& setup, int iterations) {
    const double L = setup.ymax - setup.ymin;
    const double nu = kLidSpeed * L / kReynolds; // rho = 1

    fvm::SIMPLE<2>::Config cfg;
    cfg.maxIterations = static_cast<std::size_t>(iterations);
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

    fvm::SIMPLE<2> solver(setup.mesh, 1.0, nu, cfg);

    // Apply the patches exactly as the application does: a named group plus a
    // boundary condition type, applied to the faces of that group.
    for (const auto& [name, faces] : setup.mesh.boundaryGroups()) {
        if (faces.empty())
            continue;
        if (name == setup.lidGroup)
            solver.setVelocityBCOnFacesWithSlip(faces, "moving_wall", kLidSpeed, 0.0, 0.0, false, 0.0);
        else
            solver.setVelocityBCOnFacesWithSlip(faces, "wall", 0.0, 0.0, 0.0, false, 0.0);
    }

    fvm::VectorField<2> velocity(setup.mesh, 0.0);
    fvm::Field<2> pressure(setup.mesh, 0.0);
    solver.solve(velocity, pressure);

    CavityRun run;
    for (std::size_t c = 0; c < setup.mesh.nCells(); ++c)
        run.maxU = std::max(run.maxU, std::abs(velocity[0][c]));
    run.continuity = solver.finalContinuityResidual();

    // The interior reference points (the wall points are excluded).
    const auto& ref = ghia400();
    std::vector<CavityRefPoint> interior;
    std::vector<double> heights;
    for (std::size_t i = 1; i + 1 < ref.size(); ++i) {
        interior.push_back(ref[i]);
        heights.push_back(setup.ymin + ref[i].y * L);
    }
    run.sampled = sampleCentreline(setup.mesh, velocity[0], 0.5 * (setup.xmin + setup.xmax), heights);
    run.cmp = compareProfile(run.sampled, interior);
    return run;
}

} // namespace

TEST_SUITE("slow") {

// The cavity on the ANSYS mesh - the same case Fluent was run on.
//
// This is the CAPABILITY gate: what the solver reproduces today on this mesh.
// The shape of the flow, the vortex centre, the mass balance and the peak
// velocity must all be right, and the profile must agree with Ghia's
// published table to within what a 40x40 second-order upwind mesh can deliver
// - the remaining error sits in the near-wall region, where the scheme's
// numerical diffusion dominates.
//
// The tighter accuracy target is its own case below.
TEST_CASE("slow.cavity_msh.re400_centreline") {
    const CavitySetup setup = loadCavity();
    MESSAGE("mesh: ", setup.mesh.nCells(), " cells, ", setup.mesh.nFaces(), " faces, domain x[",
            setup.xmin, ", ", setup.xmax, "] y[", setup.ymin, ", ", setup.ymax, "], lid group: ",
            setup.lidGroup);

    REQUIRE_MESSAGE(setup.topFaces > 0, "the mesh must have a top wall");
    CHECK_MESSAGE(setup.topInLid == setup.topFaces,
                  "every top wall face must belong to the lid group (" << setup.topInLid << " of "
                                                                       << setup.topFaces << ")");

    const CavityRun r = runCavity(setup, kIterations);
    MESSAGE("Re=400 ANSYS mesh: max|u| = ", r.maxU, ", continuity = ", r.continuity,
            ", max |u-u_ref| = ", r.cmp.maxError, ", L2 = ", r.cmp.l2, ", correlation = ",
            r.cmp.correlation, ", zero crossing = ", r.cmp.zeroCrossing, " (reference ",
            r.cmp.referenceZeroCrossing, ")");

    // 1. The moving wall is prescribed on the face, so the flow may not exceed
    //    the lid speed. A cell-centred lid doubles it.
    CHECK_MESSAGE(r.maxU <= 1.05 * kLidSpeed,
                  "the peak velocity must not exceed the lid speed, got " << r.maxU);

    // 2. The mass balance must close.
    CHECK_MESSAGE(r.continuity < 1e-6, "the continuity residual must be small");

    // 3. The profile must follow the published shape.
    CHECK_MESSAGE(r.cmp.correlation > 0.95, "the centreline profile must correlate with Ghia");
    CHECK_MESSAGE(std::abs(r.cmp.zeroCrossing - r.cmp.referenceZeroCrossing) < 0.05,
                  "the primary vortex centre must match the published one");

    // 4. The absolute agreement with the published table, at the level a 40x40
    //    second-order upwind mesh reaches.
    CHECK_MESSAGE(r.cmp.maxError <= 0.10 * kLidSpeed,
                  "max centreline u error " << r.cmp.maxError << " exceeds 10% of the lid speed");
    CHECK_MESSAGE(r.cmp.l2 <= 0.05 * kLidSpeed,
                  "the centreline L2 error " << r.cmp.l2 << " exceeds 5% of the lid speed");
}

// The accuracy target: the centreline profile of the cavity must agree with
// Ghia's published table to within a few percent. This is the goal of the
// solver work, and it needs a better scheme (or a finer mesh) - not more
// iterations: the state is already converged (see the steady-state case).
TEST_CASE("slow.cavity_msh.re400_accuracy_target") {
    const CavitySetup setup = loadCavity();
    const CavityRun r = runCavity(setup, kIterations);

    MESSAGE("accuracy target: max |u-u_ref| = ", r.cmp.maxError, ", L2 = ", r.cmp.l2);
    CHECK_MESSAGE(r.cmp.maxError <= 0.04 * kLidSpeed,
                  "max centreline u error " << r.cmp.maxError << " exceeds 4% of the lid speed");
    CHECK_MESSAGE(r.cmp.l2 <= 0.03 * kLidSpeed,
                  "the centreline L2 error " << r.cmp.l2 << " exceeds 3% of the lid speed");
}

// The cavity must reach a steady state: doubling the iteration count must not
// move the centreline profile.
TEST_CASE("slow.cavity_msh.steady_state") {
    const CavitySetup setup = loadCavity();
    const CavityRun a = runCavity(setup, kIterations / 2);
    const CavityRun b = runCavity(setup, kIterations);

    double maxDiff = 0.0;
    for (std::size_t i = 0; i < a.sampled.size() && i < b.sampled.size(); ++i)
        maxDiff = std::max(maxDiff, std::fabs(a.sampled[i] - b.sampled[i]));

    MESSAGE("steady state: max profile change between " << (kIterations / 2) << " and " << kIterations
                                                         << " iterations = " << maxDiff);
    CHECK_MESSAGE(maxDiff <= 0.01, "the cavity must reach a steady state (profile change " << maxDiff
                                     << ")");
}

} // TEST_SUITE("slow")
