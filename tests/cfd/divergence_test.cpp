// Divergence operator: the flux balance of a cell.
//
// Contract pinned here:
//   - the face velocity is the exact linear interpolation between the two cell
//     centres (the mesh's faceWeight);
//   - a cell with a prescribed velocity contributes that value to the WHOLE
//     face it shares with a neighbour, and the neighbour takes priority - the
//     same rule as the momentum assembly, so the divergence and the momentum
//     see the same flux;
//   - at a boundary face the velocity is the prescribed value of the cell that
//     owns the face; with no boundary condition the face carries the owner's
//     velocity;
//   - the fluxes telescope: sum(div * V) over the cells equals the net flux
//     through the boundary faces;
//   - maxDivergence() measures the same quantity as the residual.
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
    fvm::computeDivergenceInto(mesh, v, div, {}, {});

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        CAPTURE(i);
        CHECK_MESSAGE(std::abs(div[i]) < 1e-12, "the divergence of a constant field is zero");
    }
}

// At a wall the velocity comes from the boundary condition: the wall carries
// no flux, so the wall cell still shows the outflow through its interior face.
// Without a boundary condition the face carries the owner's velocity, and the
// flow passes through the wall.
TEST_CASE("cfd.divergence.wall_flux_with_and_without_boundary_condition") {
    const int n = 4;
    const double h = 1.0 / n;
    const fvm::Mesh<2> mesh = uniform2D(n);
    fvm::VectorField<2> v(mesh, 0.0);
    v[0].assign(1.0);

    const WallMask w = makeMask(mesh, 0.0);

    fvm::Field<2> divWithMask(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, v, divWithMask, w.mask, w.values);
    fvm::Field<2> divNoMask(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, v, divNoMask, {}, {});

    const std::size_t leftWall = mesh.cellIndex(0, 1);
    const std::size_t neighbour = mesh.cellIndex(1, 1);

    // With the wall condition the wall cell prescribes a zero velocity, so it
    // owns the whole face it shares with its neighbour: the wall face carries
    // no flux AND the interior face carries the prescribed (zero) velocity.
    // The flow is therefore blocked and the imbalance appears in the
    // neighbour, which still pushes its fluid through the blocked face.
    CHECK_MESSAGE(std::abs(divWithMask[leftWall]) < 1e-12, "the wall cell itself is balanced");
    CHECK_MESSAGE(std::abs(divWithMask[neighbour] - 1.0 / h) < 1e-12,
                  "the cell next to the wall carries the imbalance");

    // Without a boundary condition the face carries the owner's velocity, so
    // the flow passes through the wall and nothing is imbalanced.
    CHECK_MESSAGE(std::abs(divNoMask[leftWall]) < 1e-12, "the wall cell is balanced");
    CHECK_MESSAGE(std::abs(divNoMask[neighbour]) < 1e-12,
                  "without a boundary condition the flow passes through the wall");
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
    fvm::computeDivergenceInto(mesh, v, div, {}, {});

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        CAPTURE(i);
        CHECK_MESSAGE(std::abs(div[i] - 2.0) < 1e-12, "div(x, y) == 2");
    }
}

// On a stretched grid the cells have different widths, so the face is not
// midway between the centres: the mesh's face weight is the exact
// interpolation coefficient there (the grid is orthogonal, so the face centre
// lies on the centre-to-centre line) and the divergence of a linear field is
// exact.
//
// A non-orthogonal (skewed) mesh is a different matter: the face centre is
// off that line, the mesh's face weight is the orthogonal projection
// parameter, and a cell-centred scheme is only first-order accurate there.
TEST_CASE("cfd.divergence.stretched_mesh_exact") {
    const fvm::Mesh<2> mesh = stretched2D(8, 1.2);
    fvm::VectorField<2> v(mesh, 0.0);
    fillVector(v, [](double x, double y, double) { return std::array<double, 2>{x, y}; });

    fvm::Field<2> div(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, v, div, {}, {});

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        CAPTURE(i);
        CHECK_MESSAGE(std::abs(div[i] - 2.0) < 1e-12, "div(x, y) == 2 on a stretched grid");
    }
}

// maxDivergence() must measure the same quantity as the residual: the largest
// absolute divergence over the cells whose mass balance the pressure
// correction can still change. A cell with a prescribed velocity is excluded
// (its mass balance cannot be corrected), the boundary cells are included.
TEST_CASE("cfd.divergence.max_divergence_definition") {
    const int n = 4;
    const fvm::Mesh<2> mesh = uniform2D(n);
    fvm::VectorField<2> v(mesh, 0.0);
    v[0].assign(1.0);

    // A no-slip wall: the wall cells carry the largest imbalance (the wall
    // face has no flux) but their mass balance cannot be corrected.
    const WallMask w = makeMask(mesh, 0.0);

    const double withMask = fvm::maxDivergence(mesh, v, w.mask, w.values);
    const double withoutMask = fvm::maxDivergence(mesh, v, {}, {});

    // The reference: the largest |div| over the cells the residual reports.
    fvm::Field<2> div(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, v, div, w.mask, w.values);
    double reference = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (w.mask[0][i] || w.mask[1][i]) continue; // prescribed: excluded
        reference = std::max(reference, std::abs(div[i]));
    }

    MESSAGE("max |div| with the wall condition = ", withMask, ", without = ", withoutMask,
            ", reference = ", reference);
    CHECK_MESSAGE(std::abs(withMask - reference) < 1e-12,
                  "maxDivergence must measure the same quantity as the residual");
    CHECK_MESSAGE(withMask > withoutMask,
                  "the wall condition blocks the flow, so the imbalance is visible");
}

// A cell with a prescribed velocity contributes that value to the whole face it
// shares with a neighbour - the neighbour takes priority - so the divergence
// and the momentum assembly see the same flux.
TEST_CASE("cfd.divergence.prescribed_cell_owns_the_whole_face") {
    const int n = 4;
    const double h = 1.0 / n;
    const fvm::Mesh<2> mesh = uniform2D(n);
    fvm::VectorField<2> v(mesh, 0.0);
    v[0].assign(1.0);

    // Prescribe the velocity of the cell left of the middle one.
    const std::size_t P = mesh.cellIndex(1, 1);
    std::vector<std::vector<char>> mask(2, std::vector<char>(mesh.nCells(), 0));
    std::vector<std::vector<double>> values(2, std::vector<double>(mesh.nCells(), 0.0));
    mask[0][P] = 1;
    values[0][P] = 5.0;

    fvm::Field<2> divWithMask(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, v, divWithMask, mask, values);
    fvm::Field<2> divNoMask(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, v, divNoMask, {}, {});

    // The shared face carries 5 instead of the interpolated 1, so its flux
    // grows by 4*h. The prescribed cell owns BOTH of the faces it touches
    // (the whole-face rule), so the two fluxes cancel and its own balance is
    // unchanged - its mass balance is not the solver's business anyway. The
    // neighbour, whose own face is interpolated, loses the extra flux.
    const std::size_t N = mesh.cellIndex(2, 1);
    const double fluxDelta = 4.0 * h;
    CHECK_MESSAGE(std::abs(divWithMask[P] - divNoMask[P]) < 1e-12,
                  "the prescribed cell owns both of its faces, so its balance is unchanged");
    CHECK_MESSAGE(std::abs((divWithMask[N] - divNoMask[N]) + fluxDelta / mesh.cellVolume(N)) < 1e-12,
                  "the neighbour sees the prescribed value on the shared face");
}

// A mask that does not cover every cell (or every component) must be
// rejected instead of being read out of bounds.
TEST_CASE("cfd.divergence.mask_is_validated") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    fvm::VectorField<2> v(mesh, 0.0);
    v[0].assign(1.0);
    fvm::Field<2> div(mesh, 0.0);

    // A mask that is one cell short.
    std::vector<std::vector<char>> shortMask(2, std::vector<char>(mesh.nCells() - 1, 0));
    std::vector<std::vector<double>> shortValues(2, std::vector<double>(mesh.nCells() - 1, 0.0));
    CHECK_THROWS_AS(fvm::computeDivergenceInto(mesh, v, div, shortMask, shortValues),
                    std::invalid_argument);

    // A mask with fewer components than dimensions.
    std::vector<std::vector<char>> oneComponent(1, std::vector<char>(mesh.nCells(), 0));
    std::vector<std::vector<double>> fullValues(2, std::vector<double>(mesh.nCells(), 0.0));
    CHECK_THROWS_AS(fvm::computeDivergenceInto(mesh, v, div, oneComponent, fullValues),
                    std::invalid_argument);
}

} // TEST_SUITE("cfd")
