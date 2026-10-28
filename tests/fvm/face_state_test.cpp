// Unit tests for the shared face-state policy layer (src/fvm/FaceState.h).
//
// These are the rules the momentum assembly, the divergence, the pressure
// correction, the continuity residual and the velocity correction all share.
// Any disagreement between two of them is a mass imbalance that the pressure
// equation cannot see, so each rule is pinned here directly instead of only
// through a full solve.

#include "fvm/FaceState.h"

#include "doctest/doctest.h"

#include <array>
#include <cmath>
#include <vector>

namespace {

#define CHECK_NEAR(got, want)                                                                      \
    CHECK_MESSAGE(std::fabs((got) - (want)) <= 1e-12 * (1.0 + std::fabs(want)),                    \
                  #got " ~= " #want " (got " << (got) << ", want " << (want) << ")")

fvm::VectorField<2> makeVelocity(const fvm::Mesh<2>& mesh) {
    fvm::VectorField<2> velocity(mesh, 0.0);
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        velocity[0][i] = 1.0 + static_cast<double>(i);
        velocity[1][i] = 10.0 + static_cast<double>(i);
    }
    return velocity;
}

fvm::Mesh<2> smallMesh() {
    return fvm::Mesh<2>(std::array<std::size_t, 2>{2, 2}, std::array<double, 2>{1.0, 1.0});
}

std::vector<std::vector<char>> fullMask(std::size_t components, std::size_t cells, char value) {
    return std::vector<std::vector<char>>(components, std::vector<char>(cells, value));
}

std::vector<std::vector<double>> fullValues(std::size_t components, std::size_t cells,
                                            double value) {
    return std::vector<std::vector<double>>(components, std::vector<double>(cells, value));
}

} // namespace

TEST_CASE("fvm.face_state.prescribed_value_requires_both_buffers") {
    const std::vector<std::vector<char>> mask = fullMask(2, 3, 0);
    const std::vector<std::vector<double>> values = fullValues(2, 3, 0.0);

    double out = -1.0;
    CHECK_FALSE(fvm::prescribedValue(mask, values, 0, 1, out));

    auto activeMask = mask;
    activeMask[0][1] = 1;
    CHECK(fvm::prescribedValue(activeMask, values, 0, 1, out));
    CHECK_NEAR(out, 0.0);

    // A mask entry without a matching value slot must never be read.
    auto shortValues = values;
    shortValues[0].resize(1);
    out = -1.0;
    CHECK_FALSE(fvm::prescribedValue(activeMask, shortValues, 0, 1, out));
    CHECK_NEAR(out, -1.0);

    // Component out of range.
    CHECK_FALSE(fvm::prescribedValue(activeMask, values, 2, 1, out));

    // Null buffers (the optional-pointer form used by Momentum).
    CHECK_FALSE(fvm::prescribedValue(nullptr, nullptr, 0, 1, out));
    CHECK_FALSE(fvm::prescribedValue(&activeMask, nullptr, 0, 1, out));
    CHECK(fvm::prescribedValue(&activeMask, &values, 0, 1, out));

    // Single-component form of the same guard.
    const std::vector<char> cellMask{0, 1, 0};
    out = -1.0;
    const std::vector<double> shortCellValues(1, 7.0); // cell 1 is not covered
    CHECK_FALSE(fvm::prescribedValue(cellMask, shortCellValues, 1, out));
    CHECK_NEAR(out, -1.0);

    const std::vector<double> cellValues(3, 7.0);
    CHECK(fvm::prescribedValue(cellMask, cellValues, 1, out));
    CHECK_NEAR(out, 7.0);
    CHECK_FALSE(fvm::prescribedValue(cellMask, cellValues, 0, out)); // masked out
    CHECK_FALSE(fvm::prescribedValue(cellMask, cellValues, 3, out)); // out of range
}

TEST_CASE("fvm.face_state.interior_face_velocity_follows_prescribed_priority") {
    const fvm::Mesh<2> mesh = smallMesh();
    const fvm::VectorField<2> velocity = makeVelocity(mesh);
    const std::size_t P = 0;
    const std::size_t N = 1;

    SUBCASE("no prescribed value: distance-weighted interpolation") {
        const auto face = fvm::interpolateFaceVelocity(velocity, P, N, 0.25);
        CHECK_NEAR(face[0], 0.75 * 1.0 + 0.25 * 2.0);
        CHECK_NEAR(face[1], 0.75 * 10.0 + 0.25 * 11.0);
    }

    SUBCASE("a prescribed neighbour owns the face") {
        auto mask = fullMask(2, mesh.nCells(), 0);
        auto values = fullValues(2, mesh.nCells(), 0.0);
        mask[0][N] = 1;
        values[0][N] = 42.0;
        const auto face = fvm::interpolateFaceVelocity(velocity, P, N, 0.25, &mask, &values);
        CHECK_NEAR(face[0], 42.0);
        // The other component is unaffected.
        CHECK_NEAR(face[1], 0.75 * 10.0 + 0.25 * 11.0);
    }

    SUBCASE("a prescribed owner owns the face") {
        auto mask = fullMask(2, mesh.nCells(), 0);
        auto values = fullValues(2, mesh.nCells(), 0.0);
        mask[0][P] = 1;
        values[0][P] = 7.0;
        const auto face = fvm::interpolateFaceVelocity(velocity, P, N, 0.25, &mask, &values);
        CHECK_NEAR(face[0], 7.0);
    }

    SUBCASE("the neighbour takes priority when both are prescribed") {
        auto mask = fullMask(2, mesh.nCells(), 0);
        auto values = fullValues(2, mesh.nCells(), 0.0);
        mask[0][P] = mask[0][N] = 1;
        values[0][P] = 7.0;
        values[0][N] = 9.0;
        const auto face = fvm::interpolateFaceVelocity(velocity, P, N, 0.25, &mask, &values);
        CHECK_NEAR(face[0], 9.0);
    }

    SUBCASE("a boundary face extrapolates the owner") {
        const auto face = fvm::interpolateFaceVelocity(
            velocity, P, fvm::kNoNeighbor, 0.25);
        CHECK_NEAR(face[0], velocity[0][P]);
        CHECK_NEAR(face[1], velocity[1][P]);
    }
}

TEST_CASE("fvm.face_state.boundary_policy_maps_each_bc_type") {
    const fvm::Mesh<2> mesh = smallMesh();
    const fvm::VectorField<2> velocity = makeVelocity(mesh);
    const std::size_t P = 0;
    const std::array<double, 2> normal{0.6, 0.8};

    auto stateFor = [&](fvm::VelocityBCType type) {
        fvm::FaceVelocityBC bc;
        bc.type = type;
        bc.u = 2.0;
        bc.v = -3.0;
        return fvm::boundaryFaceState<2>(velocity, P, normal.data(), bc);
    };

    SUBCASE("no-slip wall is exactly non-penetrating") {
        const auto state = stateFor(fvm::VelocityBCType::NoSlip);
        CHECK(state.kind == fvm::FaceVelocityKind::Wall);
        CHECK_NEAR(state.normalVelocity, 0.0);
        CHECK_NEAR(state.velocity[0], 0.0);
        CHECK_NEAR(state.velocity[1], 0.0);
    }

    SUBCASE("symmetry removes the normal component and nothing else") {
        const auto state = stateFor(fvm::VelocityBCType::Symmetry);
        CHECK(state.kind == fvm::FaceVelocityKind::Wall);
        CHECK_NEAR(state.normalVelocity, 0.0);
        // The tangential part is the interior vector minus its normal part.
        const double un = velocity[0][P] * normal[0] + velocity[1][P] * normal[1];
        CHECK_NEAR(state.velocity[0], velocity[0][P] - un * normal[0]);
        CHECK_NEAR(state.velocity[1], velocity[1][P] - un * normal[1]);
    }

    SUBCASE("slip wall matches symmetry") {
        const auto slip = stateFor(fvm::VelocityBCType::SlipWall);
        const auto sym = stateFor(fvm::VelocityBCType::Symmetry);
        CHECK_NEAR(slip.velocity[0], sym.velocity[0]);
        CHECK_NEAR(slip.velocity[1], sym.velocity[1]);
    }

    SUBCASE("inlet and moving wall carry the prescribed velocity") {
        for (auto type : {fvm::VelocityBCType::Inlet, fvm::VelocityBCType::MovingWall}) {
            const auto state = stateFor(type);
            CHECK(state.kind == fvm::FaceVelocityKind::Prescribed);
            CHECK_NEAR(state.velocity[0], 2.0);
            CHECK_NEAR(state.velocity[1], -3.0);
            CHECK_NEAR(state.normalVelocity, 2.0 * normal[0] - 3.0 * normal[1]);
        }
    }

    SUBCASE("an outflow uses the interior velocity") {
        const auto state = stateFor(fvm::VelocityBCType::Outlet);
        CHECK(state.kind == fvm::FaceVelocityKind::Outflow);
        const double expected = velocity[0][P] * normal[0] + velocity[1][P] * normal[1];
        CHECK_NEAR(state.normalVelocity, expected);
        CHECK_NEAR(state.velocity[0], velocity[0][P]);
        CHECK_NEAR(state.velocity[1], velocity[1][P]);
    }

    SUBCASE("backflow through an outlet uses the backflow Dirichlet value") {
        // Inward-pointing outward normal: the interior velocity is an inflow.
        const std::array<double, 2> inward{-0.6, -0.8};
        fvm::FaceVelocityBC bc;
        bc.type = fvm::VelocityBCType::PressureOutlet;
        bc.backflowU = -1.0;
        bc.backflowV = -2.0;
        const auto state = fvm::boundaryFaceState<2>(velocity, P, inward.data(), bc);
        CHECK(state.kind == fvm::FaceVelocityKind::Outflow);
        CHECK_NEAR(state.inflowVelocity[0], -1.0);
        CHECK_NEAR(state.inflowVelocity[1], -2.0);
        const double expected = -1.0 * inward[0] - 2.0 * inward[1];
        CHECK_NEAR(state.normalVelocity, expected);
    }

    SUBCASE("suppressed backflow clamps the inflow flux to zero") {
        const std::array<double, 2> inward{-0.6, -0.8};
        fvm::FaceVelocityBC bc;
        bc.type = fvm::VelocityBCType::PressureOutlet;
        bc.suppressBackflow = true;
        const auto state = fvm::boundaryFaceState<2>(velocity, P, inward.data(), bc);
        CHECK_NEAR(state.normalVelocity, 0.0);
    }
}

TEST_CASE("fvm.face_state.dominant_normal_component") {
    const std::array<double, 2> diagonal{0.6, 0.8};
    CHECK(fvm::dominantNormalComponent<2>(diagonal.data()) == 1);

    const std::array<double, 2> xNormal{-1.0, 0.0};
    CHECK(fvm::dominantNormalComponent<2>(xNormal.data()) == 0);

    const std::array<double, 3> n3{0.2, -0.9, 0.3};
    CHECK(fvm::dominantNormalComponent<3>(n3.data()) == 1);

    CHECK_NEAR(fvm::dotNormal<2>(std::array<double, 2>{2.0, 3.0}, diagonal.data()),
               2.0 * 0.6 + 3.0 * 0.8);
}
