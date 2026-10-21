// Laplacian and Poisson operators.
//
// Contract pinned here:
//   A phi = b  with  A = -div(c grad(phi))  - a positive diagonal, non-positive
//   off-diagonals, symmetric.  The sign convention matters: a positive source
//   must produce a positive bump, which is what SIMPLE's pressure equation
//   needs.  (The Poisson.h header comment states the opposite - the code is
//   right, the comment is what has to change.)
//
//   b = source * V, i.e. `source` is a density, not an already integrated
//   source.  Boundary faces contribute nothing (pure Neumann) unless the
//   boundary cell is masked; a masked cell gets an identity row and its
//   unmasked neighbours pick the prescribed value up in the right-hand side.
#include "doctest/doctest.h"
#include "fvm/Laplacian.h"
#include "fvm/Poisson.h"
#include "support/fvm_fixtures.h"

#include <Eigen/Dense>

#include <array>
#include <cmath>
#include <vector>

using namespace test_support;

namespace {

Eigen::VectorXd solveDense(const fvm::SparseMatrix& A, const Eigen::VectorXd& b) {
    const Eigen::MatrixXd dense(A);
    return dense.fullPivLu().solve(b);
}

// The boundary cells of a structured mesh, with the analytic value of the
// manufactured solution.
std::vector<char> boundaryMask(const fvm::Mesh<2>& mesh) {
    std::vector<char> mask(mesh.nCells(), 0);
    for (std::size_t i = 0; i < mesh.nCells(); ++i)
        mask[i] = mesh.isBoundary(i) ? 1 : 0;
    return mask;
}

} // namespace

TEST_SUITE("cfd") {

// The discrete operator applied to the exact quadratic must reproduce the
// analytic Laplacian times the cell volume. On a uniform grid the 5-point
// stencil is exact for a quadratic, so this pins the coefficient (k, not k/h^2),
// the sign (-div) and the volume weighting of the source - all at once.
TEST_CASE("cfd.poisson.manufactured_solution") {
    {
        const fvm::Mesh<2> mesh = uniform2D(6);
        fvm::Field<2> c(mesh, 1.0);
        fvm::Field<2> source(mesh, -6.0); // -lap(phi) for phi = x^2 + 2 y^2
        fvm::Poisson<2> poisson(mesh);
        fvm::SparseMatrix A;
        Eigen::VectorXd b;
        poisson.discretize(A, b, c, source);

        Eigen::VectorXd phi(mesh.nCells());
        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            const auto x = mesh.cellCenter(i);
            phi[i] = x[0] * x[0] + 2.0 * x[1] * x[1];
        }
        const Eigen::VectorXd applied = A * phi;

        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            if (mesh.isBoundary(i))
                continue;
            CAPTURE(i);
            const double expected = -6.0 * mesh.cellVolume(i);
            CHECK_MESSAGE(std::abs(applied[i] - expected) < 1e-12 * std::abs(expected),
                          "A*phi must equal -(lap phi) * V");
        }
    }

    {
        const fvm::Mesh<3> mesh = uniform3D(4);
        fvm::Field<3> c(mesh, 1.0);
        fvm::Field<3> source(mesh, -6.0); // -lap(phi) for phi = x^2 + y^2 + z^2
        fvm::Poisson<3> poisson(mesh);
        fvm::SparseMatrix A;
        Eigen::VectorXd b;
        poisson.discretize(A, b, c, source);

        Eigen::VectorXd phi(mesh.nCells());
        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            const auto x = mesh.cellCenter(i);
            phi[i] = x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
        }
        const Eigen::VectorXd applied = A * phi;

        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            if (mesh.isBoundary(i))
                continue;
            CAPTURE(i);
            const double expected = -6.0 * mesh.cellVolume(i);
            CHECK_MESSAGE(std::abs(applied[i] - expected) < 1e-12 * std::abs(expected),
                          "A*phi must equal -(lap phi) * V in 3D");
        }
    }
}

// A positive source must produce a positive bump (the sign convention the
// SIMPLE pressure equation relies on), and the operator must be linear.
TEST_CASE("cfd.poisson.source_sign_contract") {
    const fvm::Mesh<2> mesh = uniform2D(6);
    const std::vector<char> mask = boundaryMask(mesh);
    std::vector<double> values(mesh.nCells(), 0.0);

    fvm::Field<2> c(mesh, 1.0);
    fvm::Field<2> plus(mesh, 1.0);
    fvm::Field<2> minus(mesh, -1.0);

    fvm::Poisson<2> poisson(mesh);
    fvm::SparseMatrix A;
    Eigen::VectorXd bPlus, bMinus;
    poisson.discretize(A, bPlus, c, plus, mask, values);
    poisson.discretize(A, bMinus, c, minus, mask, values);

    const Eigen::VectorXd phiPlus = solveDense(A, bPlus);
    const Eigen::VectorXd phiMinus = solveDense(A, bMinus);

    double maxPlus = -1e30;
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mask[i])
            continue;
        maxPlus = std::max(maxPlus, phiPlus[i]);
        CHECK_MESSAGE(std::abs(phiPlus[i] + phiMinus[i]) < 1e-9 * std::abs(phiPlus[i]),
                      "the operator must be linear in the source");
    }
    CHECK_MESSAGE(maxPlus > 0.0, "a positive source must produce a positive bump");
}

// The matrix must be symmetric with a positive diagonal and non-positive
// off-diagonals (an M-matrix), so that the linear solve is well posed.
TEST_CASE("cfd.poisson.matrix_is_symmetric_m_matrix") {
    const fvm::Mesh<2> mesh = uniform2D(6);
    fvm::Field<2> c(mesh, 1.0);
    fvm::Field<2> source(mesh, 0.0);
    fvm::Poisson<2> poisson(mesh);
    fvm::SparseMatrix A;
    Eigen::VectorXd b;
    poisson.discretize(A, b, c, source);

    const int n = static_cast<int>(mesh.nCells());
    for (int i = 0; i < n; ++i) {
        CAPTURE(i);
        CHECK_MESSAGE(A.coeff(i, i) > 0.0, "the diagonal must be positive");
        for (int j = 0; j < n; ++j) {
            if (i == j)
                continue;
            CAPTURE(j);
            CHECK_MESSAGE(A.coeff(i, j) <= 0.0, "the off-diagonals must be non-positive");
            CHECK_MESSAGE(std::abs(A.coeff(i, j) - A.coeff(j, i)) < 1e-15,
                          "the matrix must be symmetric");
        }
    }
}

// A masked cell gets an identity row: A(P,P) = 1, b(P) = the prescribed value,
// and no coupling to its neighbours.
TEST_CASE("cfd.poisson.dirichlet_row_is_identity") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    fvm::Field<2> c(mesh, 1.0);
    fvm::Field<2> source(mesh, 0.0);

    std::vector<char> mask(mesh.nCells(), 0);
    std::vector<double> values(mesh.nCells(), 0.0);
    const std::size_t P = mesh.cellIndex(0, 1);
    mask[P] = 1;
    values[P] = 2.5;

    fvm::Poisson<2> poisson(mesh);
    fvm::SparseMatrix A;
    Eigen::VectorXd b;
    poisson.discretize(A, b, c, source, mask, values);

    CHECK_MESSAGE(std::abs(A.coeff(static_cast<int>(P), static_cast<int>(P)) - 1.0) < 1e-15,
                  "the masked row must be an identity row");
    CHECK_MESSAGE(std::abs(b[static_cast<int>(P)] - 2.5) < 1e-15,
                  "the masked row must carry the prescribed value");
    for (std::size_t j = 0; j < mesh.nCells(); ++j) {
        if (j == P)
            continue;
        CAPTURE(j);
        CHECK_MESSAGE(A.coeff(static_cast<int>(P), static_cast<int>(j)) == 0.0,
                      "the masked row must not couple to its neighbours");
    }
}

// An unmasked neighbour of a masked cell picks the prescribed value up in the
// right-hand side (the coupling itself disappears from the matrix).
TEST_CASE("cfd.poisson.dirichlet_neighbour_contribution") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    const double h = 1.0 / 4.0;
    fvm::Field<2> c(mesh, 1.0);
    fvm::Field<2> source(mesh, 0.0);

    std::vector<char> mask(mesh.nCells(), 0);
    std::vector<double> values(mesh.nCells(), 0.0);
    const std::size_t P = mesh.cellIndex(0, 1);
    const std::size_t N = mesh.cellIndex(1, 1);
    mask[P] = 1;
    values[P] = 2.5;

    fvm::Poisson<2> poisson(mesh);
    fvm::SparseMatrix A;
    Eigen::VectorXd b;
    poisson.discretize(A, b, c, source, mask, values);

    // The face coefficient is k * A * (S.d) / |d|^2 = 1 * h * h / h^2 = 1.
    CHECK_MESSAGE(std::abs(b[static_cast<int>(N)] - 1.0 * 2.5) < 1e-12,
                  "the neighbour must pick up coeff * prescribed value");
    CHECK_MESSAGE(A.coeff(static_cast<int>(N), static_cast<int>(P)) == 0.0,
                  "the coupling to a masked cell must be removed from the matrix");
}

// Laplacian(k) and Poisson(c = k, source = 0) are two copies of the same
// operator and must produce the same matrix.
TEST_CASE("cfd.poisson.laplacian_matches_poisson") {
    const fvm::Mesh<2> mesh = uniform2D(5);
    fvm::Field<2> c(mesh, 2.5);
    fvm::Field<2> source(mesh, 0.0);

    fvm::Laplacian<2> laplacian(mesh, 2.5);
    fvm::SparseMatrix A;
    Eigen::VectorXd b;
    laplacian.discretize(A, b);

    fvm::Poisson<2> poisson(mesh);
    fvm::SparseMatrix A2;
    Eigen::VectorXd b2;
    poisson.discretize(A2, b2, c, source);

    for (int i = 0; i < static_cast<int>(mesh.nCells()); ++i) {
        CAPTURE(i);
        for (int j = 0; j < static_cast<int>(mesh.nCells()); ++j) {
            CAPTURE(j);
            CHECK_MESSAGE(std::abs(A.coeff(i, j) - A2.coeff(i, j)) < 1e-15,
                          "Laplacian and Poisson must assemble the same operator");
        }
    }
}

// On an orthogonal mesh the non-orthogonal correction vanishes: assembling
// with and without a cell gradient must give the same matrix and the same
// right-hand side.
TEST_CASE("cfd.poisson.non_orthogonal_correction") {
    const fvm::Mesh<2> mesh = uniform2D(5);
    fvm::Field<2> c(mesh, 1.0);
    fvm::Field<2> source(mesh, 0.0);

    fvm::Poisson<2> poisson(mesh);
    fvm::SparseMatrix A;
    Eigen::VectorXd b;
    poisson.discretize(A, b, c, source);

    std::array<fvm::Field<2>, 2> grad{fvm::Field<2>(mesh, 0.0), fvm::Field<2>(mesh, 0.0)};
    fvm::SparseMatrix A2;
    Eigen::VectorXd b2;
    poisson.discretize(A2, b2, c, source, {}, {}, &grad);

    for (int i = 0; i < static_cast<int>(mesh.nCells()); ++i) {
        CAPTURE(i);
        CHECK_MESSAGE(std::abs(b[i] - b2[i]) < 1e-15,
                      "the non-orthogonal correction must vanish on an orthogonal mesh");
        for (int j = 0; j < static_cast<int>(mesh.nCells()); ++j) {
            CAPTURE(j);
            CHECK_MESSAGE(std::abs(A.coeff(i, j) - A2.coeff(i, j)) < 1e-15,
                          "the correction must not change the matrix");
        }
    }
}

// A mask that does not cover every cell must be rejected instead of being
// read out of bounds.
TEST_CASE("cfd.poisson.truncated_mask_is_rejected") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    fvm::Field<2> c(mesh, 1.0);
    fvm::Field<2> source(mesh, 0.0);

    fvm::Poisson<2> poisson(mesh);
    fvm::SparseMatrix A;
    Eigen::VectorXd b;

    // A mask that is one cell short.
    std::vector<char> shortMask(mesh.nCells() - 1, 0);
    std::vector<double> shortValues(mesh.nCells() - 1, 0.0);
    CHECK_THROWS_AS(poisson.discretize(A, b, c, source, shortMask, shortValues),
                    std::invalid_argument);

    // A mask that is one cell too long.
    std::vector<char> longMask(mesh.nCells() + 1, 0);
    std::vector<double> longValues(mesh.nCells() + 1, 0.0);
    CHECK_THROWS_AS(poisson.discretize(A, b, c, source, longMask, longValues),
                    std::invalid_argument);

    // A mask without values.
    std::vector<char> mask(mesh.nCells(), 0);
    CHECK_THROWS_AS(poisson.discretize(A, b, c, source, mask, {}), std::invalid_argument);

    // The Laplacian is the same operator and must validate the same way.
    fvm::Laplacian<2> laplacian(mesh, 1.0);
    CHECK_THROWS_AS(laplacian.discretize(A, b, shortMask, shortValues, {}), std::invalid_argument);
}

} // TEST_SUITE("cfd")
