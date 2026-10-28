#include "doctest/doctest.h"
#include "fvm/Poisson.h"
#include "fvm/SSTModel.h"
#include "io/Checkpoint.h"
#include "mesh/BoundaryUtils.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace fvm;

namespace {

template<std::size_t Dim>
std::vector<FaceVelocityBC> sideBCs(const Mesh<Dim>& mesh,
                                    VelocityBCType xMin, VelocityBCType xMax,
                                    VelocityBCType yMin, VelocityBCType yMax) {
    std::vector<FaceVelocityBC> bcs(mesh.nFaces());
    auto setSide = [&](std::size_t d, int sign, VelocityBCType type) {
        forEachBoundaryFace(mesh, d, sign, [&](std::size_t f, std::size_t) {
            bcs[f].type = type;
            // A non-zero value makes the face velocity distinguishable from
            // the owner cell value in the strain-rate test.
            bcs[f].u = (type == VelocityBCType::Inlet) ? 1.0 : 0.0;
        });
    };
    setSide(0, -1, xMin);
    setSide(0, +1, xMax);
    if constexpr (Dim >= 2) {
        setSide(1, -1, yMin);
        setSide(1, +1, yMax);
    }
    return bcs;
}

std::filesystem::path tempCheckpointPath(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / "scirender_sst_tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

} // namespace

TEST_CASE("SST turbulence BCs come from the per-face momentum BCs") {
    Mesh<2> mesh(std::array<std::size_t, 2>{8, 4},
                 std::array<double, 2>{1.0, 0.4});
    // Left inlet, right outlet, bottom wall, top symmetry. This is exactly the
    // configuration the application creates with setVelocityBCOnFaces; the
    // per-side velBCs stay at their NoSlip default.
    auto bcs = sideBCs(mesh, VelocityBCType::Inlet, VelocityBCType::Outlet,
                       VelocityBCType::NoSlip, VelocityBCType::Symmetry);

    SSTModel<2>::Config cfg;
    cfg.kInf = 3e-2;
    cfg.omegaInf = 4.0;
    SSTModel<2> model(mesh, cfg);
    model.initialize(1e-3, 1.0);
    model.setBoundaryConditions(bcs);

    const auto& kMask = model.kDirichletMask();
    const auto& kVal = model.kDirichletValues();
    const auto& wMask = model.omegaDirichletMask();
    const auto& wVal = model.omegaDirichletValues();

    // Interior cells must never be masked.
    for (std::size_t i = 1; i < 7; ++i) {
        for (std::size_t j = 1; j < 3; ++j) {
            const auto c = mesh.cellIndex(std::array<std::size_t, 2>{i, j});
            CHECK(kMask[c] == 0);
            CHECK(wMask[c] == 0);
        }
    }

    // Inlet column: prescribed kInf/omegaInf.
    for (std::size_t j = 1; j < 3; ++j) {
        const auto c = mesh.cellIndex(std::array<std::size_t, 2>{0, j});
        CHECK(kMask[c] == 1);
        CHECK(kVal[c] == doctest::Approx(3e-2));
        CHECK(wMask[c] == 1);
        CHECK(wVal[c] == doctest::Approx(4.0));
    }

    // Outlet column: zero-gradient, no Dirichlet mask.
    for (std::size_t j = 1; j < 3; ++j) {
        const auto c = mesh.cellIndex(std::array<std::size_t, 2>{7, j});
        CHECK(kMask[c] == 0);
        CHECK(wMask[c] == 0);
    }

    // Bottom wall row: k = 0 and the omega wall value.
    for (std::size_t i = 1; i < 7; ++i) {
        const auto c = mesh.cellIndex(std::array<std::size_t, 2>{i, 0});
        CHECK(kMask[c] == 1);
        CHECK(kVal[c] == doctest::Approx(0.0));
        CHECK(wMask[c] == 1);
        CHECK(wVal[c] > 0.0);
    }

    // The bottom-left cell owns both an inlet face and a wall face. The wall
    // condition must win regardless of the face visit order: k stays 0 there.
    {
        const auto c = mesh.cellIndex(std::array<std::size_t, 2>{0, 0});
        CHECK(kMask[c] == 1);
        CHECK(kVal[c] == doctest::Approx(0.0));
        CHECK(wMask[c] == 1);
        CHECK(wVal[c] > 0.0);
    }

    // Top symmetry row: natural BC, no mask.
    for (std::size_t i = 1; i < 7; ++i) {
        const auto c = mesh.cellIndex(std::array<std::size_t, 2>{i, 3});
        CHECK(kMask[c] == 0);
        CHECK(wMask[c] == 0);
    }
}

TEST_CASE("SST wall distance is seeded from wall faces only") {
    Mesh<2> mesh(std::array<std::size_t, 2>{10, 5},
                 std::array<double, 2>{1.0, 0.5});
    auto bcs = sideBCs(mesh, VelocityBCType::Inlet, VelocityBCType::Outlet,
                       VelocityBCType::NoSlip, VelocityBCType::Symmetry);

    SSTModel<2>::Config cfg;
    SSTModel<2> model(mesh, cfg);
    model.initialize(1e-3, 1.0);
    model.setBoundaryConditions(bcs);

    const double yBottom = model.wallDistance()[mesh.cellIndex(
        std::array<std::size_t, 2>{5, 0})];
    const double yTop = model.wallDistance()[mesh.cellIndex(
        std::array<std::size_t, 2>{5, 4})];

    // dy = 0.1: the wall-adjacent cell sits 0.05 from the wall; the top cell
    // is 0.45 away. Seeding from every boundary face (the old behaviour) made
    // both 0.05, because the symmetry plane was treated as a wall.
    CHECK(yBottom == doctest::Approx(0.05).epsilon(0.05));
    CHECK(yTop > 5.0 * yBottom);
    CHECK(model.maxWallDistance() > 0.0);
}

TEST_CASE("Checkpoint load keeps the stream aligned when turbulence is not requested") {
    Mesh<2> mesh(std::array<std::size_t, 2>{4, 3},
                 std::array<double, 2>{1.0, 1.0});

    VectorField<2> velocity(mesh, 0.0);
    velocity.assign(0, 1.0);
    velocity.assign(1, 2.0);
    Field<2> pressure(mesh, 0.0);
    pressure.assign(3.0);
    Field<2> k(mesh, 0.0);
    k.assign(0.25);
    Field<2> omega(mesh, 0.0);
    omega.assign(5.0);
    Field<2> nut(mesh, 0.0);
    nut.assign(0.5);

    CheckpointConfig cc;
    cc.file = tempCheckpointPath("sst_roundtrip.cpt").string();
    cc.retainCount = 0; // do not rotate/delete anything
    cc.saveEvery = 1;

    REQUIRE(Checkpoint::save(cc, 7, 1e-4, 2e-4, 0.3, 1, 0.0,
                             velocity, pressure, &k, &omega, &nut));

    std::size_t iter = 0;
    double cont = 0.0, mom = 0.0, alphaP = 0.0, prev = 0.0;
    std::size_t stall = 0;

    // Load WITHOUT turbulence pointers. The writer recorded k/omega/nut, so
    // the reader must consume them rather than reading only what the caller
    // happened to ask for.
    {
        VectorField<2> v2(mesh, 0.0);
        Field<2> p2(mesh, 0.0);
        REQUIRE(Checkpoint::load(cc.file, mesh, iter, cont, mom, alphaP,
                                 stall, prev, v2, p2));
        CHECK(iter == 7);
        CHECK(v2[0][0] == doctest::Approx(1.0));
        CHECK(v2[1][0] == doctest::Approx(2.0));
        CHECK(p2[0] == doctest::Approx(3.0));
    }

    // Load WITH turbulence pointers: values must be the saved ones.
    {
        VectorField<2> v2(mesh, 0.0);
        Field<2> p2(mesh, 0.0);
        Field<2> k2(mesh, 0.0), omega2(mesh, 0.0), nut2(mesh, 0.0);
        REQUIRE(Checkpoint::load(cc.file, mesh, iter, cont, mom, alphaP,
                                 stall, prev, v2, p2, &k2, &omega2, &nut2));
        CHECK(k2[0] == doctest::Approx(0.25));
        CHECK(omega2[0] == doctest::Approx(5.0));
        CHECK(nut2[0] == doctest::Approx(0.5));
    }
}
