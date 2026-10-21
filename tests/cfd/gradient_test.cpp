// Gradient operator: the three schemes (least squares, cell-based and
// node-based Green-Gauss) and the wall treatment.
//
// Contract pinned here:
//   - the least-squares gradient reproduces a linear field exactly at the
//     interior cells (the wall ghost implements a zero-gradient condition, so
//     the boundary cells are deliberately excluded);
//   - the Green-Gauss cell-based gradient must use the exact face
//     interpolation (the mesh's faceWeight), not a plain 0.5 average - with
//     the exact interpolation it also reproduces a linear field exactly, on
//     any mesh;
//   - a prescribed (Dirichlet) cell value must not be diluted by the wall
//     ghost: the wall cell's gradient is the one-sided gradient.
#include "doctest/doctest.h"
#include "fvm/Gradient.h"
#include "support/fvm_fixtures.h"

#include <array>
#include <cmath>

using namespace test_support;
using fvm::Field;
using fvm::Gradient;
using fvm::GradientScheme;

namespace {

// A linear field phi(x, y) = ax * x + ay * y + b.
struct Linear {
    double ax, ay, b;
    double operator()(double x, double y, double) const { return ax * x + ay * y + b; }
    std::array<double, 2> grad() const { return {ax, ay}; }
};

std::array<Field<2>, 2> makeGrad(const fvm::Mesh<2>& mesh) {
    return {Field<2>(mesh, 0.0), Field<2>(mesh, 0.0)};
}

} // namespace

TEST_SUITE("cfd") {

// The least-squares gradient is exact for a linear field at every interior
// cell. The wall cells are excluded on purpose: the wall ghost is a
// zero-gradient (Neumann) condition, so the gradient there is a boundary
// treatment, not a reproduction of the analytic field.
TEST_CASE("cfd.gradient.least_squares_linear_exact") {
    const Linear lin{2.0, -3.0, 1.0};

    {
        const fvm::Mesh<2> mesh = uniform2D(6);
        Field<2> phi(mesh, 0.0);
        fillScalar(phi, lin);
        Gradient<2> grad(mesh, GradientScheme::LeastSquares);
        auto g = makeGrad(mesh);
        grad.compute(phi, g);

        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            if (mesh.isBoundary(i))
                continue;
            CAPTURE(i);
            CHECK_MESSAGE(std::abs(g[0][i] - lin.ax) < 1e-12, "d(phi)/dx");
            CHECK_MESSAGE(std::abs(g[1][i] - lin.ay) < 1e-12, "d(phi)/dy");
        }
    }

    {
        const fvm::Mesh<3> mesh = uniform3D(5);
        Field<3> phi(mesh, 0.0);
        fillScalar(phi, [](double x, double y, double z) { return 1.5 * x - 0.5 * y + 2.0 * z; });
        Gradient<3> grad(mesh, GradientScheme::LeastSquares);
        std::array<Field<3>, 3> g{Field<3>(mesh, 0.0), Field<3>(mesh, 0.0), Field<3>(mesh, 0.0)};
        grad.compute(phi, g);

        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            if (mesh.isBoundary(i))
                continue;
            CAPTURE(i);
            CHECK_MESSAGE(std::abs(g[0][i] - 1.5) < 1e-12, "d(phi)/dx");
            CHECK_MESSAGE(std::abs(g[1][i] + 0.5) < 1e-12, "d(phi)/dy");
            CHECK_MESSAGE(std::abs(g[2][i] - 2.0) < 1e-12, "d(phi)/dz");
        }
    }
}

// The least-squares gradient of a quadratic is exact: the symmetric stencil
// fits a quadratic without a residual, so the gradient carries no truncation
// error at all.
TEST_CASE("cfd.gradient.least_squares_quadratic_exact") {
    const fvm::Mesh<2> mesh = uniform2D(8);
    Field<2> phi(mesh, 0.0);
    fillScalar(phi, [](double x, double y, double) { return x * x + y * y; });
    Gradient<2> grad(mesh, GradientScheme::LeastSquares);
    auto g = makeGrad(mesh);
    grad.compute(phi, g);

    double worst = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        const auto c = mesh.cellCenter(i);
        worst = std::max(worst, std::abs(g[0][i] - 2.0 * c[0]));
        worst = std::max(worst, std::abs(g[1][i] - 2.0 * c[1]));
    }
    MESSAGE("quadratic gradient error = ", worst);
    CHECK_MESSAGE(worst < 1e-12, "the least-squares gradient must be exact for a quadratic");
}

// The cell-based Green-Gauss gradient is exact for a linear field on a uniform
// grid.
TEST_CASE("cfd.gradient.green_gauss_cell_linear_exact") {
    const Linear lin{2.0, -3.0, 1.0};
    const fvm::Mesh<2> mesh = uniform2D(6);
    Field<2> phi(mesh, 0.0);
    fillScalar(phi, lin);
    Gradient<2> grad(mesh, GradientScheme::GreenGaussCellBased);
    auto g = makeGrad(mesh);
    grad.compute(phi, g);

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        CAPTURE(i);
        CHECK_MESSAGE(std::abs(g[0][i] - lin.ax) < 1e-12, "d(phi)/dx");
        CHECK_MESSAGE(std::abs(g[1][i] - lin.ay) < 1e-12, "d(phi)/dy");
    }
}

// The cell-based Green-Gauss must use the mesh's face weight - the exact
// linear interpolation between the two cell centres - and not a plain
// 0.5 average, which is only correct when the two cells have the same size.
//
// A stretched grid discriminates the two: the cells have different widths, so
// the face is NOT midway between the centres and the 0.5 average is plainly
// wrong, while the mesh's face weight is the exact interpolation coefficient
// (the face centre lies on the centre-to-centre line of an orthogonal grid).
//
// A non-orthogonal (skewed) mesh is a different matter: there the face centre
// is off the centre-to-centre line, the mesh's face weight is the orthogonal
// projection parameter, and a cell-centred scheme is only first-order accurate.
// That is a property of the formulation, not a defect of the operator.
TEST_CASE("cfd.gradient.green_gauss_cell_stretched_exact") {
    const Linear lin{2.0, -3.0, 1.0};
    const fvm::Mesh<2> mesh = stretched2D(8, 1.2);
    Field<2> phi(mesh, 0.0);
    fillScalar(phi, lin);
    Gradient<2> grad(mesh, GradientScheme::GreenGaussCellBased);
    auto g = makeGrad(mesh);
    grad.compute(phi, g);

    double worst = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        CAPTURE(i);
        worst = std::max(worst, std::abs(g[0][i] - lin.ax));
        worst = std::max(worst, std::abs(g[1][i] - lin.ay));
    }
    MESSAGE("stretched-mesh Green-Gauss gradient error = ", worst);
    CHECK_MESSAGE(worst < 1e-12, "the Green-Gauss gradient must use the mesh's face weight");
}

// The node-based Green-Gauss scheme needs the mesh vertices and the cell /
// face vertex lists. Those are only populated for unstructured meshes, so on
// a structured mesh the scheme must either work or fail cleanly - it must
// not read out of bounds.
TEST_CASE("cfd.gradient.green_gauss_node_based_structured") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    Field<2> phi(mesh, 0.0);
    fillScalar(phi, [](double x, double y, double) { return 2.0 * x - 3.0 * y; });
    Gradient<2> grad(mesh, GradientScheme::GreenGaussNodeBased);
    auto g = makeGrad(mesh);
    grad.compute(phi, g);

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        CAPTURE(i);
        for (std::size_t d = 0; d < 2; ++d)
            CHECK_MESSAGE(std::isfinite(g[d][i]), "the node-based gradient must be finite");
    }
}

// On a uniform grid the least-squares and the cell-based Green-Gauss
// gradients agree for a linear field (both are exact at the interior cells).
TEST_CASE("cfd.gradient.schemes_agree_uniform") {
    const fvm::Mesh<2> mesh = uniform2D(6);
    Field<2> phi(mesh, 0.0);
    fillScalar(phi, [](double x, double y, double) { return 2.0 * x - 3.0 * y + 1.0; });

    Gradient<2> lsq(mesh, GradientScheme::LeastSquares);
    auto gLsq = makeGrad(mesh);
    lsq.compute(phi, gLsq);

    Gradient<2> gg(mesh, GradientScheme::GreenGaussCellBased);
    auto gGg = makeGrad(mesh);
    gg.compute(phi, gGg);

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        CAPTURE(i);
        CHECK_MESSAGE(std::abs(gLsq[0][i] - gGg[0][i]) < 1e-12, "d(phi)/dx must agree");
        CHECK_MESSAGE(std::abs(gLsq[1][i] - gGg[1][i]) < 1e-12, "d(phi)/dy must agree");
    }
}

// A prescribed (Dirichlet) cell value is a CELL value. The wall ghost must
// not dilute the gradient of that cell: the gradient has to be the one-sided
// gradient towards the interior, (phi_interior - phi_prescribed) / h.
TEST_CASE("cfd.gradient.dirichlet_value_is_not_a_face_value") {
    const int n = 6;
    const double h = 1.0 / n;
    const fvm::Mesh<2> mesh = uniform2D(n);

    // The whole field is zero except the left wall, whose cells are prescribed
    // the value 1.
    Field<2> phi(mesh, 0.0);
    std::vector<char> mask(mesh.nCells(), 0);
    std::vector<double> values(mesh.nCells(), 0.0);
    for (std::size_t j = 0; j < static_cast<std::size_t>(n); ++j) {
        const std::size_t idx = mesh.cellIndex(0, j);
        mask[idx] = 1;
        values[idx] = 1.0;
        phi[idx] = 1.0;
    }

    Gradient<2> grad(mesh, GradientScheme::LeastSquares);
    auto g = makeGrad(mesh);
    grad.compute(phi, g, mask, values);

    // The one-sided gradient of a left wall cell: its interior neighbour holds
    // 0, the cell itself holds the prescribed 1, and the centres are h apart.
    const double expected = -1.0 / h;
    for (std::size_t j = 1; j + 1 < static_cast<std::size_t>(n); ++j) {
        const std::size_t idx = mesh.cellIndex(0, j);
        CAPTURE(idx);
        const double oneSided = expected;
        // The wall ghost must not halve the gradient.
        CHECK_MESSAGE(g[0][idx] * oneSided > 0.0, "the gradient must point into the domain");
        CHECK_MESSAGE(std::abs(std::abs(g[0][idx]) - std::abs(oneSided)) < 0.1 * std::abs(oneSided),
                      "the wall ghost must not dilute the prescribed-cell gradient (got "
                          << g[0][idx] << ", expected about " << oneSided << ")");
    }
}

} // TEST_SUITE("cfd")
