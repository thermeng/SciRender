// Divergence operator: the flux balance of a cell.
//
// Contract pinned here:
//   - the face velocity is the exact linear interpolation between the two
//     cell centres (the mesh's faceWeight), never a plain 0.5 average;
//   - at a boundary face the velocity comes from the Dirichlet mask, not from
//     the interior cell - the 3-argument overload cannot honour that and is
//     therefore wrong for meshes with walls;
//   - the fluxes telescope: sum(div * V) over the cells equals the net flux
//     through the boundary faces.
#include "doctest/doctest.h"
#include "fvm/Divergence.h"
#include "support/fvm_fixtures.h"

#include <cmath>
#include <vector>

using namespace test_support;

namespace {

// A no-slip mask on every boundary cell, with an optional inlet velocity on
// the left wall (x = 0).
struct WallMask {
    std::vector<std::vector<char>> mask;
    std::vector<std::vector<double>> values;
};

WallMask makeMask(const fvm::Mesh<2>& mesh, double inletU) {
    WallMask w;
    w.mask.assign(2, std::vector<char>(mesh.nCells(), 0));
    w.values.assign(2, std::vector<double>(mesh.nCells(), 0.0));
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (!mesh.isBoundary(i))
            continue;
        w.mask[0][i] = 1;
        w.mask[1][i] = 1;
    }
    // Left wall (x = 0) carries the inlet velocity.
    for (std::size_t j = 0; j < mesh.dims()[1]; ++j) {
        const std::size_t idx = mesh.cellIndex(0, j);
        w.values[0][idx] = inletU;
        w.values[1][idx] = 0.0;
    }
    return w;
}

// The net flux through the boundary faces, using the same rule as the
// operator: the masked value at a masked cell, the interior value otherwise.
double boundaryFlux(const fvm::Mesh<2>& mesh, const fvm::VectorField<2>& v, const WallMask& w) {
    double total = 0.0;
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) != std::size_t(-1))
            continue;
        const std::size_t P = mesh.owner(f);
        double un = 0.0;
        for (std::size_t d = 0; d < 2; ++d) {
            const double uFace = w.mask[d][P] ? w.values[d][P] : v[d][P];
            un += uFace * mesh.faceNormal(f)[d];
        }
        total += un * mesh.faceArea(f);
    }
    return total;
}

} // namespace

TEST_SUITE("cfd") {

// A constant velocity field has no divergence in the interior.
TEST_CASE("cfd.divergence.constant_field_interior") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    fvm::VectorField<2> v(mesh, 0.0);
    v[0].assign(1.0);

    fvm::Field<2> div(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, v, div);

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        CAPTURE(i);
        CHECK_MESSAGE(std::abs(div[i]) < 1e-12, "the divergence of a constant field is zero");
    }
}

// At a wall the velocity must come from the boundary condition. The 5-argument
// overload honours the mask, the 3-argument overload uses the interior cell
// velocity and therefore lets the flow through the wall.
TEST_CASE("cfd.divergence.no_slip_wall_flux") {
    const int n = 4;
    const double h = 1.0 / n;
    const fvm::Mesh<2> mesh = uniform2D(n);
    fvm::VectorField<2> v(mesh, 0.0);
    v[0].assign(1.0);

    const WallMask w = makeMask(mesh, 0.0);

    fvm::Field<2> divWithMask(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, v, divWithMask, w.mask, w.values);
    fvm::Field<2> divNoMask(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, v, divNoMask);

    // The masked overload: the wall carries no flux, so the left wall cell
    // still shows the outflow through its interior face.
    const std::size_t leftWall = mesh.cellIndex(0, 1);
    CHECK_MESSAGE(std::abs(divWithMask[leftWall] - 1.0 / h) < 1e-12,
                  "the masked divergence of a left wall cell");

    // The unmasked overload must give the same answer.
    CHECK_MESSAGE(std::abs(divNoMask[leftWall] - divWithMask[leftWall]) < 1e-12,
                  "the 3-argument overload must honour the wall boundary condition");
}

// The cell fluxes telescope: the sum of div*V over the cells equals the net
// flux through the boundary faces.
TEST_CASE("cfd.divergence.conservation_telescoping") {
    const int n = 4;
    const double h = 1.0 / n;
    const fvm::Mesh<2> mesh = uniform2D(n);
    fvm::VectorField<2> v(mesh, 0.0);
    v[0].assign(1.0);

    const WallMask w = makeMask(mesh, 1.0); // inlet on the left wall

    fvm::Field<2> div(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, v, div, w.mask, w.values);

    double sum = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i)
        sum += div[i] * mesh.cellVolume(i);

    // The left wall injects n faces of area h with the outward normal -x.
    const double expected = -static_cast<double>(n) * h;
    CHECK_MESSAGE(std::abs(sum - expected) < 1e-12, "the cell fluxes must telescope");
    CHECK_MESSAGE(std::abs(boundaryFlux(mesh, v, w) - expected) < 1e-12,
                  "the boundary flux must match the telescoped sum");
}

// A linear velocity field has a constant divergence.
TEST_CASE("cfd.divergence.linear_field_exact") {
    const fvm::Mesh<2> mesh = uniform2D(6);
    fvm::VectorField<2> v(mesh, 0.0);
    fillVector(v, [](double x, double y, double) { return std::array<double, 2>{x, y}; });

    fvm::Field<2> div(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, v, div);

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        CAPTURE(i);
        CHECK_MESSAGE(std::abs(div[i] - 2.0) < 1e-12, "div(x, y) == 2");
    }
}

// maxDivergence() skips the boundary cells while the residual reports the
// maximum over all cells. The two must measure the same quantity.
TEST_CASE("cfd.divergence.max_divergence_definition") {
    const int n = 4;
    const fvm::Mesh<2> mesh = uniform2D(n);
    fvm::VectorField<2> v(mesh, 0.0);
    v[0].assign(1.0);

    const WallMask w = makeMask(mesh, 0.0);
    fvm::Field<2> div(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, v, div, w.mask, w.values);

    double maxAll = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i)
        maxAll = std::max(maxAll, std::abs(div[i]));

    const double reported = fvm::maxDivergence(mesh, v);
    MESSAGE("max |div| over all cells = ", maxAll, ", maxDivergence() = ", reported);
    CHECK_MESSAGE(std::abs(reported - maxAll) < 1e-12,
                  "maxDivergence must measure the same quantity as the residual");
}

// On a skewed mesh the face velocity must use the exact interpolation
// weight, so a linear field still has an exact divergence.
TEST_CASE("cfd.divergence.unstructured_uses_face_weights") {
    const fvm::Mesh<2> mesh = triangles2D();
    fvm::VectorField<2> v(mesh, 0.0);
    fillVector(v, [](double x, double y, double) { return std::array<double, 2>{x, y}; });

    fvm::Field<2> div(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, v, div);

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        CAPTURE(i);
        CHECK_MESSAGE(std::abs(div[i] - 2.0) < 1e-12, "div(x, y) == 2 on a skewed mesh");
    }
}

} // TEST_SUITE("cfd")
