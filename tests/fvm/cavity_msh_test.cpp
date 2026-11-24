// The lid-driven cavity on the imported mesh (samples/cfd/CavityQuad_129x129.msh),
// set up the way an end user sets it up in the application: the mesh's named
// boundary groups are the "patches", and every patch is a boundary condition
// applied to the faces of that group.
//
// The reference is Ghia, Giri & Podkar (1982), Table III (Re = 400): the
// velocity component along the vertical centre line of a square cavity whose
// top wall moves with unit speed.
//
// The run is a fixed 2000-iteration budget: no residual gate, no early stop.
// Convergence criteria depend on machine-specific iteration counts and make the
// test flap between machines. The field after the fixed budget is what gets
// checked, directly against the published centreline.
//
// The test case is named "slow.*" so it carries the "slow" CTest label and can
// be excluded with `ctest -LE slow`.
#include "doctest/doctest.h"
#include "fvm/SIMPLE.h"
#include "io/ANSYSMeshReader.h"
#include "support/cavity_reference.h"
#include "support/test_support.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <omp.h>

using namespace test_support;

namespace {

constexpr double kLidSpeed = 1.0;
constexpr double kReynolds = 400.0;
constexpr int kIterations = 2000;

struct CavitySetup {
    fvm::Mesh<2> mesh;
    std::string lidGroup;
    double xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    std::size_t topFaces = 0;
    std::size_t topInLid = 0;
};

// Loads the mesh and works out which named group is the lid.
CavitySetup loadCavity() {
    requireSample("cfd/CavityQuad_129x129.msh");
    const std::string path = samplePath("cfd/CavityQuad_129x129.msh").string();

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
    std::vector<double> sampled; // centreline u at the interior reference heights
    CavityComparison cmp;
};

// The two wall points (y = 0 and y = 1) are excluded: the published values
// there are WALL values, while the solver reports the value in the adjacent
// cell centre.
CavityRun runCavity(const CavitySetup& setup) {
    // Single-threaded so the field is deterministic regardless of core count.
    omp_set_num_threads(1);

    const double L = setup.ymax - setup.ymin;
    const double nu = kLidSpeed * L / kReynolds; // rho = 1

    fvm::SIMPLE<2>::Config cfg;
    cfg.stopping.steady.fixedIterations = kIterations;
    cfg.stopping.steady.maxIterations = kIterations;
    cfg.printFrequency = 1000000;
    cfg.pressureCorrectors = 1;
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

TEST_CASE("slow.cavity_msh.re400_centreline") {
    const CavitySetup setup = loadCavity();

    REQUIRE_MESSAGE(setup.topFaces > 0, "the mesh must have a top wall");
    CHECK_MESSAGE(setup.topInLid == setup.topFaces,
                  "every top wall face must belong to the lid group (" << setup.topInLid << " of "
                                                                     << setup.topFaces << ")");

    const CavityRun r = runCavity(setup);
    MESSAGE("Re=400 quad mesh: max|u| = ", r.maxU,
            ", max |u-u_ref| = ", r.cmp.maxError, ", L2 = ", r.cmp.l2, ", correlation = ",
            r.cmp.correlation, ", zero crossing = ", r.cmp.zeroCrossing, " (reference ",
            r.cmp.referenceZeroCrossing, ")");

    // The field: bounded speed, flow following the lid and reversed flow in
    // the lower half, and the primary vortex in the published position.
    CHECK_MESSAGE(r.maxU <= 1.05 * kLidSpeed,
                  "the peak velocity must not exceed the lid speed, got " << r.maxU);
    CHECK_MESSAGE(r.sampled[1] > 0.5, "flow must follow the lid");
    CHECK_MESSAGE(r.sampled[8] < 0.0, "lower half of the centreline must be reversed");
    CHECK_MESSAGE(r.cmp.correlation > 0.95, "the centreline profile must correlate with Ghia");
    CHECK_MESSAGE(std::abs(r.cmp.zeroCrossing - r.cmp.referenceZeroCrossing) < 0.05,
                  "the primary vortex centre must match the published one");
    CHECK_MESSAGE(r.cmp.zeroCrossing > 0.45, "vortex core must be in the upper half");
    CHECK_MESSAGE(r.cmp.zeroCrossing < 0.85, "vortex core must not sit at the lid");

    // Absolute agreement at the level a 129x129 second-order upwind mesh reaches.
    CHECK_MESSAGE(r.cmp.maxError <= 0.15 * kLidSpeed,
                  "max centreline u error " << r.cmp.maxError << " exceeds 15% of the lid speed");
    CHECK_MESSAGE(r.cmp.l2 <= 0.10 * kLidSpeed,
                  "the centreline L2 error " << r.cmp.l2 << " exceeds 10% of the lid speed");
}

} // TEST_SUITE("slow")
