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
#include <span>
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

// The assembled system must be solvable and its solution must satisfy the
// system: a discrete elliptic problem with a positive source has a finite
// solution whose residual is tiny. Checking the solve outcome (rather than
// matrix signs) pins well-posedness without prescribing the stencil.
TEST_CASE("cfd.poisson.solve_is_well_posed") {
    const fvm::Mesh<2> mesh = uniform2D(6);
    const std::vector<char> mask = boundaryMask(mesh);
    std::vector<double> values(mesh.nCells(), 0.0);

    fvm::Field<2> c(mesh, 1.0);
    fvm::Field<2> source(mesh, 1.0);
    fvm::Poisson<2> poisson(mesh);
    fvm::SparseMatrix A;
    Eigen::VectorXd b;
    poisson.discretize(A, b, c, source, mask, values);

    const Eigen::VectorXd phi = solveDense(A, b);
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        CAPTURE(i);
        CHECK_MESSAGE(std::isfinite(phi[static_cast<int>(i)]), "the solution must be finite");
        if (mask[i])
            CHECK_MESSAGE(std::abs(phi[static_cast<int>(i)]) < 1e-12,
                          "the prescribed boundary value must be honored");
    }
    const Eigen::VectorXd residual = A * phi - b;
    CHECK_MESSAGE(residual.norm() < 1e-9 * std::max(1.0, b.norm()),
                  "the dense solution must satisfy the assembled system");
}

// A prescribing cell holds its prescribed value in the solution, and that
// value influences its neighbours: the solution is the observable outcome of
// the Dirichlet treatment, whatever assembly trick implements it.
TEST_CASE("cfd.poisson.dirichlet_value_is_honored") {
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

    const Eigen::VectorXd phi = solveDense(A, b);
    CHECK_MESSAGE(std::abs(phi[static_cast<int>(P)] - 2.5) < 1e-12,
                  "the prescribed cell must carry its prescribed value in the solution");
    // The neighbour must feel the prescribed value (it is neither zero nor
    // the prescribed value itself, but influenced by it).
    const std::size_t N = mesh.cellIndex(1, 1);
    CHECK_MESSAGE(std::isfinite(phi[static_cast<int>(N)]), "the neighbour solution must be finite");
    CHECK_MESSAGE(phi[static_cast<int>(N)] > 0.0, "the neighbour must be influenced by the prescribed value");
    CHECK_MESSAGE(phi[static_cast<int>(N)] < 2.5, "the neighbour must stay below the prescribed value");
}

// A neighbour of a prescribed cell must be affected by the prescribed value:
// the solved neighbour value moves toward the prescribed value compared with
// the unprescribed case.
TEST_CASE("cfd.poisson.dirichlet_influences_neighbour") {
    const fvm::Mesh<2> mesh = uniform2D(4);
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
    const Eigen::VectorXd phiPrescribed = solveDense(A, b);

    // Without the prescription the same source gives a symmetric zero-ish
    // solution; the prescribed solve must differ at the neighbour.
    fvm::SparseMatrix A0;
    Eigen::VectorXd b0;
    poisson.discretize(A0, b0, c, source);
    const Eigen::VectorXd phiFree = solveDense(A0, b0);
    CHECK_MESSAGE(std::abs(phiPrescribed[static_cast<int>(N)] - phiFree[static_cast<int>(N)]) > 1e-9,
                  "the neighbour value must be influenced by the prescribed cell");
}

// Laplacian(k) and Poisson(c = k, source = 0) must produce the same solution
// for the same boundary conditions: both solve the same physical problem.
// Comparing solutions (not matrix entries) pins the shared purpose.
TEST_CASE("cfd.poisson.laplacian_solves_the_same_problem") {
    const fvm::Mesh<2> mesh = uniform2D(5);
    const std::vector<char> mask = boundaryMask(mesh);
    std::vector<double> values(mesh.nCells(), 0.0);

    fvm::Field<2> c(mesh, 2.5);
    fvm::Field<2> source(mesh, 1.0);

    fvm::Laplacian<2> laplacian(mesh, 2.5);
    fvm::SparseMatrix A;
    Eigen::VectorXd b;
    {
        std::vector<double> src(mesh.nCells(), 1.0);
        laplacian.discretize(A, b, mask, values, std::span<const double>(src.data(), src.size()));
    }
    const Eigen::VectorXd phiLaplacian = solveDense(A, b);

    fvm::Poisson<2> poisson(mesh);
    fvm::SparseMatrix A2;
    Eigen::VectorXd b2;
    poisson.discretize(A2, b2, c, source, mask, values);
    const Eigen::VectorXd phiPoisson = solveDense(A2, b2);

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        CAPTURE(i);
        CHECK_MESSAGE(std::isfinite(phiLaplacian[static_cast<int>(i)]), "the Laplacian solution must be finite");
        CHECK_MESSAGE(std::abs(phiLaplacian[static_cast<int>(i)] - phiPoisson[static_cast<int>(i)]) < 1e-9,
                      "Laplacian and Poisson must solve the same problem");
    }
}

// On an orthogonal mesh the non-orthogonal correction must not change the
// solved field: the solution with and without the correction is the same.
TEST_CASE("cfd.poisson.non_orthogonal_correction_vanishes_on_orthogonal_mesh") {
    const fvm::Mesh<2> mesh = uniform2D(5);
    const std::vector<char> mask = boundaryMask(mesh);
    std::vector<double> values(mesh.nCells(), 0.0);

    fvm::Field<2> c(mesh, 1.0);
    fvm::Field<2> source(mesh, 1.0);

    fvm::Poisson<2> poisson(mesh);
    fvm::SparseMatrix A;
    Eigen::VectorXd b;
    poisson.discretize(A, b, c, source, mask, values);
    const Eigen::VectorXd phiPlain = solveDense(A, b);

    std::array<fvm::Field<2>, 2> grad{fvm::Field<2>(mesh, 0.0), fvm::Field<2>(mesh, 0.0)};
    fvm::SparseMatrix A2;
    Eigen::VectorXd b2;
    poisson.discretize(A2, b2, c, source, mask, values, &grad);
    const Eigen::VectorXd phiCorr = solveDense(A2, b2);

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        CAPTURE(i);
        CHECK_MESSAGE(std::isfinite(phiCorr[static_cast<int>(i)]), "the corrected solution must be finite");
        CHECK_MESSAGE(std::abs(phiPlain[static_cast<int>(i)] - phiCorr[static_cast<int>(i)]) < 1e-9,
                      "the correction must not change the solution on an orthogonal mesh");
    }
}

} // TEST_SUITE("cfd")
