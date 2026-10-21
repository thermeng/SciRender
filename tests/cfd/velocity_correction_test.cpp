// Velocity correction: u = u* - d * grad(p').
//
// Contract pinned here:
//   - the correction uses the operator's own cell gradient of p';
//   - a cell whose velocity is prescribed is not corrected;
//   - a symmetry or slip wall projects the normal component out of the
//     boundary cell's velocity.
#include "doctest/doctest.h"
#include "fvm/VelocityCorrection.h"
#include "support/fvm_fixtures.h"

#include <array>
#include <cmath>
#include <vector>

using namespace test_support;

namespace {

struct Corrected {
    fvm::VectorField<2> velocity;
    std::array<fvm::Field<2>, 2> dpdx;
};

Corrected correct(const fvm::Mesh<2>& mesh, const fvm::VectorField<2>& uStar,
                  const fvm::Field<2>& pPrime, double dCoeff,
                  const std::vector<std::vector<char>>& mask,
                  const std::vector<std::vector<double>>& values,
                  const std::vector<fvm::FaceVelocityBC>* faceBC = nullptr,
                  const std::vector<char>* faceHasBC = nullptr) {
    std::array<std::vector<double>, 2> d{std::vector<double>(mesh.nCells(), dCoeff),
                                         std::vector<double>(mesh.nCells(), dCoeff)};
    std::array<fvm::Field<2>, 2> dpdx{fvm::Field<2>(mesh, 0.0), fvm::Field<2>(mesh, 0.0)};
    fvm::VectorField<2> velocity(mesh, 0.0);
    fvm::correctVelocity(mesh, uStar, pPrime, d, mask, values, dpdx, velocity, faceBC, faceHasBC);
    return {velocity, dpdx};
}

} // namespace

TEST_SUITE("cfd") {

// The correction removes the pressure gradient: u = u* - d * grad(p').
// For a linear p' the operator's gradient is exact, so the result is known
// analytically.
TEST_CASE("cfd.velocity.corrects_by_pressure_gradient") {
    const fvm::Mesh<2> mesh = uniform2D(6);
    fvm::VectorField<2> uStar(mesh, 0.0);
    uStar[0].assign(3.0);

    fvm::Field<2> pPrime(mesh, 0.0);
    fillScalar(pPrime, [](double x, double, double) { return 2.0 * x; });

    const double d = 0.5;
    std::vector<std::vector<char>> mask(2, std::vector<char>(mesh.nCells(), 0));
    std::vector<std::vector<double>> values(2, std::vector<double>(mesh.nCells(), 0.0));

    const Corrected out = correct(mesh, uStar, pPrime, d, mask, values);

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        CAPTURE(i);
        // grad(p') = (2, 0) -> u = 3 - 0.5 * 2 = 2 in x, 0 in y.
        CHECK_MESSAGE(std::abs(out.velocity[0][i] - 2.0) < 1e-12, "the corrected u component");
        CHECK_MESSAGE(std::abs(out.velocity[1][i]) < 1e-12, "the corrected v component");
    }
}

// A cell with a prescribed velocity keeps it: the correction must not touch
// it.
TEST_CASE("cfd.velocity.dirichlet_cell_not_corrected") {
    const fvm::Mesh<2> mesh = uniform2D(6);
    fvm::VectorField<2> uStar(mesh, 0.0);
    uStar[0].assign(3.0);

    fvm::Field<2> pPrime(mesh, 0.0);
    fillScalar(pPrime, [](double x, double, double) { return 2.0 * x; });

    std::vector<std::vector<char>> mask(2, std::vector<char>(mesh.nCells(), 0));
    std::vector<std::vector<double>> values(2, std::vector<double>(mesh.nCells(), 0.0));
    const std::size_t P = mesh.cellIndex(2, 2);
    mask[0][P] = 1;
    mask[1][P] = 1;
    values[0][P] = 7.0;
    values[1][P] = -1.0;

    const Corrected out = correct(mesh, uStar, pPrime, 0.5, mask, values);

    CAPTURE(P);
    CHECK_MESSAGE(std::abs(out.velocity[0][P] - 7.0) < 1e-12,
                  "a prescribed cell keeps its prescribed velocity");
    CHECK_MESSAGE(std::abs(out.velocity[1][P] + 1.0) < 1e-12,
                  "a prescribed cell keeps its prescribed velocity");
}

// A symmetry or slip wall must project the normal component out of the
// boundary cell's velocity.
TEST_CASE("cfd.velocity.symmetry_projects_normal") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    fvm::VectorField<2> uStar(mesh, 0.0);
    uStar[0].assign(1.0);
    uStar[1].assign(1.0);

    fvm::Field<2> pPrime(mesh, 0.0);

    // Mark the left wall (x = 0, normal -x) as a symmetry wall.
    std::vector<fvm::FaceVelocityBC> faceBC(mesh.nFaces());
    std::vector<char> faceHasBC(mesh.nFaces(), 0);
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) != std::size_t(-1))
            continue;
        const auto& n = mesh.faceNormal(f);
        if (std::abs(n[0] + 1.0) < 1e-12) {
            faceBC[f].type = fvm::VelocityBCType::Symmetry;
            faceHasBC[f] = 1;
        }
    }

    std::vector<std::vector<char>> mask(2, std::vector<char>(mesh.nCells(), 0));
    std::vector<std::vector<double>> values(2, std::vector<double>(mesh.nCells(), 0.0));

    const Corrected out = correct(mesh, uStar, pPrime, 1.0, mask, values, &faceBC, &faceHasBC);

    for (std::size_t j = 0; j < mesh.dims()[1]; ++j) {
        const std::size_t P = mesh.cellIndex(0, j);
        CAPTURE(P);
        // The wall normal is -x, so the x component must vanish.
        CHECK_MESSAGE(std::abs(out.velocity[0][P]) < 1e-12,
                      "the normal component must be projected out");
    }
}

} // TEST_SUITE("cfd")
