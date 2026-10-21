// Convection operator and the four schemes.
//
// Contract pinned here:
//   - the face flux is F = (u_f . n) * A with the face velocity interpolated
//     with the mesh's faceWeight;
//   - upwind: F >= 0 -> A(P,P) += F, F < 0 -> A(P,N) = F;
//   - central: A(P,P) += F/2 and A(P,N) = F/2;
//   - second order upwind: the upwind base plus a deferred reconstruction
//     phi_f = phi_up + grad(phi_up) . r, clamped to the neighbour bounds; the
//     known term moves to the right-hand side with a sign flip, so the full
//     balance is (A phi) - b;
//   - a constant transported quantity is conserved by a divergence-free flow;
//   - the three overloads must assemble the same operator.
#include "doctest/doctest.h"
#include "fvm/Convection.h"
#include "fvm/Gradient.h"
#include "support/fvm_fixtures.h"

#include <Eigen/Dense>

#include <array>
#include <cmath>
#include <vector>

using namespace test_support;

namespace {

struct Assembled {
    Eigen::MatrixXd A;
    Eigen::VectorXd b;
};

std::array<const fvm::Field<2>*, 2> velocityPtrs(const fvm::VectorField<2>& v) {
    return {&v[0], &v[1]};
}

// The SparseMatrix overload.
Assembled assembleSparse(fvm::Convection<2>& conv, const fvm::Mesh<2>& mesh,
                         const fvm::Field<2>& phi, const fvm::VectorField<2>& vel,
                         const std::vector<char>& mask = {},
                         const std::vector<double>& values = {},
                         const std::array<fvm::Field<2>, 2>* grad = nullptr) {
    const std::size_t n = mesh.nCells();
    fvm::SparseMatrix A(n, n);
    Eigen::VectorXd b = Eigen::VectorXd::Zero(n);
    conv.discretize(A, b, phi, velocityPtrs(vel), mask, values, grad);
    return {Eigen::MatrixXd(A), b};
}

// The triplet overloads: the plain one and the "unified" one that takes the
// per-component Dirichlet masks and the diagonal accumulator.
Assembled assembleTriplets(fvm::Convection<2>& conv, const fvm::Mesh<2>& mesh,
                           const fvm::Field<2>& phi, const fvm::VectorField<2>& vel,
                           bool unified, const std::vector<char>& mask = {},
                           const std::vector<double>& values = {},
                           const std::array<fvm::Field<2>, 2>* grad = nullptr,
                           const std::vector<std::vector<char>>& maskAll = {},
                           const std::vector<std::vector<double>>& valsAll = {}) {
    const std::size_t n = mesh.nCells();
    std::vector<fvm::Triplet> triplets;
    Eigen::VectorXd b = Eigen::VectorXd::Zero(n);

    if (unified) {
        std::vector<double> diagInc(n, 0.0);
        conv.discretize(triplets, b, diagInc, phi, velocityPtrs(vel), mask, values, grad, maskAll,
                        valsAll);
        for (std::size_t i = 0; i < n; ++i)
            triplets.emplace_back(static_cast<int>(i), static_cast<int>(i), diagInc[i]);
    } else {
        conv.discretize(triplets, b, phi, velocityPtrs(vel), mask, values, grad);
    }

    fvm::SparseMatrix A(n, n);
    A.setFromTriplets(triplets.begin(), triplets.end());
    return {Eigen::MatrixXd(A), b};
}

} // namespace

TEST_SUITE("cfd") {

// A constant transported quantity must be conserved by a divergence-free
// flow: the operator annihilates the constant.
TEST_CASE("cfd.convection.constant_field_annihilated") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    fvm::Field<2> phi(mesh, 3.0);
    fvm::VectorField<2> vel(mesh, 0.0);
    vel[0].assign(1.0);

    const fvm::ConvectionScheme schemes[] = {
        fvm::ConvectionScheme::Upwind, fvm::ConvectionScheme::Central,
        fvm::ConvectionScheme::SecondOrderUpwind, fvm::ConvectionScheme::QUICK};

    for (const auto scheme : schemes) {
        CAPTURE(static_cast<int>(scheme));
        fvm::Convection<2> conv(mesh);
        conv.setScheme(scheme);
        const Assembled op = assembleSparse(conv, mesh, phi, vel);

        const Eigen::VectorXd ones = Eigen::VectorXd::Ones(mesh.nCells());
        const Eigen::VectorXd applied = op.A * ones;
        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            if (mesh.isBoundary(i))
                continue;
            CAPTURE(i);
            CHECK_MESSAGE(std::abs(applied[i]) < 1e-12, "a constant must be annihilated");
        }
    }
}

// The upwind row: the outgoing flux goes to the diagonal, the incoming flux to
// the upstream neighbour.
TEST_CASE("cfd.convection.upwind_row_structure") {
    const int n = 4;
    const double h = 1.0 / n;
    const fvm::Mesh<2> mesh = uniform2D(n);
    fvm::Field<2> phi(mesh, 1.0);
    fvm::VectorField<2> vel(mesh, 0.0);
    vel[0].assign(1.0);

    fvm::Convection<2> conv(mesh);
    conv.setScheme(fvm::ConvectionScheme::Upwind);
    const Assembled op = assembleSparse(conv, mesh, phi, vel);

    const std::size_t P = mesh.cellIndex(1, 1);
    const std::size_t up = mesh.cellIndex(0, 1); // the upstream neighbour
    const int ip = static_cast<int>(P);
    const int iu = static_cast<int>(up);

    // The outgoing flux is u . n * A = 1 * 1 * h.
    CHECK_MESSAGE(std::abs(op.A(ip, ip) - h) < 1e-12, "the diagonal must hold the outgoing flux");
    CHECK_MESSAGE(std::abs(op.A(ip, iu) + h) < 1e-12, "the upstream coupling must be -F");
    for (int j = 0; j < static_cast<int>(mesh.nCells()); ++j) {
        if (j == ip || j == iu)
            continue;
        CAPTURE(j);
        CHECK_MESSAGE(std::abs(op.A(ip, j)) < 1e-15, "no other coupling is allowed");
    }
}

// Central differencing is exact for a linear field: the operator applied to
// phi reproduces the integral of u . grad(phi) over the cell.
TEST_CASE("cfd.convection.central_linear_exact") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    fvm::Field<2> phi(mesh, 0.0);
    fillScalar(phi, [](double x, double, double) { return x; });
    fvm::VectorField<2> vel(mesh, 0.0);
    vel[0].assign(1.0);

    fvm::Convection<2> conv(mesh);
    conv.setScheme(fvm::ConvectionScheme::Central);
    const Assembled op = assembleSparse(conv, mesh, phi, vel);

    const Eigen::VectorXd phiVec = Eigen::VectorXd::Map(phi.values().data(), phi.size());
    const Eigen::VectorXd applied = op.A * phiVec;
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        CAPTURE(i);
        // The integral of u . grad(phi) = the integral of 1 = V.
        CHECK_MESSAGE(std::abs(applied[i] - mesh.cellVolume(i)) < 1e-12,
                      "central differencing must be exact for a linear field");
    }
}

// Second order upwind with the exact gradient reproduces a linear field: the
// deferred correction completes the balance, so (A phi) - b is the exact
// integral of u . grad(phi).
//
// The check is restricted to the cells whose upwind neighbours are interior
// cells as well: the wall ghost makes the boundary gradient a zero-gradient
// condition, and that is a separate defect (cfd.gradient.*) which must not
// leak into the scheme's own contract.
TEST_CASE("cfd.convection.sou_linear_exact") {
    const int n = 6;
    const fvm::Mesh<2> mesh = uniform2D(n);
    fvm::Field<2> phi(mesh, 0.0);
    fillScalar(phi, [](double x, double, double) { return x; });
    fvm::VectorField<2> vel(mesh, 0.0);
    vel[0].assign(1.0);

    fvm::Gradient<2> gradient(mesh, fvm::GradientScheme::LeastSquares);
    std::array<fvm::Field<2>, 2> grad{fvm::Field<2>(mesh, 0.0), fvm::Field<2>(mesh, 0.0)};
    gradient.compute(phi, grad);

    fvm::Convection<2> conv(mesh);
    conv.setScheme(fvm::ConvectionScheme::SecondOrderUpwind);
    const Assembled op = assembleSparse(conv, mesh, phi, vel, {}, {}, &grad);

    const Eigen::VectorXd phiVec = Eigen::VectorXd::Map(phi.values().data(), phi.size());
    const Eigen::VectorXd applied = op.A * phiVec;
    int checked = 0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        const auto c = mesh.cellCoords(i);
        if (c[0] < 2 || c[0] > static_cast<std::size_t>(n) - 2)
            continue;
        CAPTURE(i);
        ++checked;
        CHECK_MESSAGE(std::abs((applied[i] - op.b[static_cast<int>(i)]) - mesh.cellVolume(i)) < 1e-12,
                      "the second order upwind scheme must be exact for a linear field");
    }
    CHECK_MESSAGE(checked > 0, "the test must actually check some cells");
}

// The reconstruction is clamped to the bounds of the upwind cell's
// neighbours, so the scheme can never create a new maximum.
TEST_CASE("cfd.convection.sou_limiter_bounds") {
    const fvm::Mesh<2> mesh = uniform2D(6);
    fvm::VectorField<2> vel(mesh, 0.0);
    vel[0].assign(1.0);

    // A sharp front and a spike: the classic fields that make an unbounded
    // reconstruction overshoot.
    for (int kind = 0; kind < 2; ++kind) {
        CAPTURE(kind);
        fvm::Field<2> phi(mesh, 0.0);
        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            const auto c = mesh.cellCoords(i);
            if (kind == 0) {
                phi[i] = (c[0] >= 3) ? 1.0 : 0.0; // a step
            } else {
                phi[i] = (c[0] == 2 && c[1] == 2) ? 10.0 : 0.0; // a spike
            }
        }

        fvm::Gradient<2> gradient(mesh, fvm::GradientScheme::LeastSquares);
        std::array<fvm::Field<2>, 2> grad{fvm::Field<2>(mesh, 0.0), fvm::Field<2>(mesh, 0.0)};
        gradient.compute(phi, grad);

        fvm::Convection<2> conv(mesh);
        conv.setScheme(fvm::ConvectionScheme::SecondOrderUpwind);
        const Assembled op = assembleSparse(conv, mesh, phi, vel, {}, {}, &grad);

        // In the first column the cell is the upwind cell of its +x face and
        // every other face carries no flux, so b holds that face's correction
        // alone: b(P) = -F * (phi_f - phi_up).
        const double flux = vel[0][0] * mesh.dx(1); // u * face length
        for (std::size_t j = 0; j < mesh.dims()[1]; ++j) {
            const std::size_t P = mesh.cellIndex(0, j);
            const double limited = -op.b[static_cast<int>(P)] / flux;
            const double reconstructed = phi[P] + limited;

            double lo = phi[P];
            double hi = phi[P];
            for (std::size_t nb : mesh.cellNeighbors(P)) {
                lo = std::min(lo, phi[nb]);
                hi = std::max(hi, phi[nb]);
            }
            CHECK_MESSAGE(reconstructed >= lo - 1e-12, "the reconstruction must not undershoot");
            CHECK_MESSAGE(reconstructed <= hi + 1e-12, "the reconstruction must not overshoot");
        }
    }
}

// The three-point quadratic reconstruction reproduces a linear field.
TEST_CASE("cfd.convection.quick_linear_exact") {
    const int n = 6;
    const fvm::Mesh<2> mesh = uniform2D(n);
    fvm::Field<2> phi(mesh, 0.0);
    fillScalar(phi, [](double x, double, double) { return x; });
    fvm::VectorField<2> vel(mesh, 0.0);
    vel[0].assign(1.0);

    fvm::Convection<2> conv(mesh);
    conv.setScheme(fvm::ConvectionScheme::QUICK);
    const Assembled op = assembleSparse(conv, mesh, phi, vel);

    const Eigen::VectorXd phiVec = Eigen::VectorXd::Map(phi.values().data(), phi.size());
    const Eigen::VectorXd applied = op.A * phiVec;
    int checked = 0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        // The quadratic reconstruction needs three distinct cells along the
        // flow direction, so the upwind cell of the face must have a cell
        // behind it as well.
        const auto c = mesh.cellCoords(i);
        if (c[0] < 2 || c[0] > static_cast<std::size_t>(n) - 2)
            continue;
        CAPTURE(i);
        ++checked;
        CHECK_MESSAGE(std::abs((applied[i] - op.b[static_cast<int>(i)]) - mesh.cellVolume(i)) < 1e-12,
                      "the QUICK reconstruction must be exact for a linear field");
    }
    CHECK_MESSAGE(checked > 0, "the test must actually check some cells");
}

// The three overloads must assemble the same operator. They differ today on
// the treatment of a boundary inflow and on the Dirichlet face rule.
TEST_CASE("cfd.convection.overloads_agree") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    fvm::Field<2> phi(mesh, 1.0);
    fvm::VectorField<2> vel(mesh, 0.0);
    // Flow towards -x: the +x wall becomes a boundary inflow.
    vel[0].assign(-1.0);

    fvm::Convection<2> conv(mesh);
    conv.setScheme(fvm::ConvectionScheme::Central);

    const Assembled sparse = assembleSparse(conv, mesh, phi, vel);
    const Assembled plain = assembleTriplets(conv, mesh, phi, vel, false);
    const Assembled unified = assembleTriplets(conv, mesh, phi, vel, true);

    for (int i = 0; i < static_cast<int>(mesh.nCells()); ++i) {
        CAPTURE(i);
        for (int j = 0; j < static_cast<int>(mesh.nCells()); ++j) {
            CAPTURE(j);
            CHECK_MESSAGE(std::abs(sparse.A(i, j) - plain.A(i, j)) < 1e-15,
                          "the SparseMatrix and the triplet overload must agree");
            CHECK_MESSAGE(std::abs(sparse.A(i, j) - unified.A(i, j)) < 1e-15,
                          "the unified overload must assemble the same operator");
        }
    }
}

// Second order upwind needs the cell gradient. Without it the scheme must not
// silently fall back to plain upwind.
TEST_CASE("cfd.convection.sou_requires_gradient") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    fvm::Field<2> phi(mesh, 0.0);
    fillScalar(phi, [](double x, double, double) { return x * x; });
    fvm::VectorField<2> vel(mesh, 0.0);
    vel[0].assign(1.0);

    fvm::Convection<2> conv(mesh);
    conv.setScheme(fvm::ConvectionScheme::SecondOrderUpwind);
    fvm::SparseMatrix A(mesh.nCells(), mesh.nCells());
    Eigen::VectorXd b = Eigen::VectorXd::Zero(mesh.nCells());

    CHECK_THROWS_AS(conv.discretize(A, b, phi, velocityPtrs(vel)), std::invalid_argument);
}

// A cell with a prescribed velocity contributes that value to the whole face,
// so the momentum and the continuity see the same flux. Every overload must
// implement the rule.
TEST_CASE("cfd.convection.dirichlet_neighbour_rule") {
    const int n = 4;
    const double h = 1.0 / n;
    const fvm::Mesh<2> mesh = uniform2D(n);
    fvm::Field<2> phi(mesh, 0.0);
    fvm::VectorField<2> vel(mesh, 0.0);
    vel[0].assign(1.0);

    // Prescribe the velocity of the cell left of the middle one.
    const std::size_t P = mesh.cellIndex(1, 1);
    std::vector<char> mask(mesh.nCells(), 0);
    std::vector<double> values(mesh.nCells(), 0.0);
    mask[P] = 1;
    values[P] = 5.0;

    // The per-component form of the same mask.
    std::vector<std::vector<char>> maskAll(2, std::vector<char>(mesh.nCells(), 0));
    std::vector<std::vector<double>> valsAll(2, std::vector<double>(mesh.nCells(), 0.0));
    maskAll[0][P] = 1;
    valsAll[0][P] = 5.0;

    fvm::Convection<2> conv(mesh);
    conv.setScheme(fvm::ConvectionScheme::Upwind);

    const Assembled sparse = assembleSparse(conv, mesh, phi, vel, mask, values);
    const Assembled plain = assembleTriplets(conv, mesh, phi, vel, false, mask, values);
    const Assembled unified = assembleTriplets(conv, mesh, phi, vel, true, mask, values, nullptr,
                                               maskAll, valsAll);

    // The neighbour to the right sees the prescribed value on the shared face:
    // its diagonal picks up -(5 - 1) * h, on top of the +h of its own outflow.
    const std::size_t N = mesh.cellIndex(2, 1);
    const int iN = static_cast<int>(N);
    MESSAGE("neighbour diagonal: sparse=", sparse.A(iN, iN), " plain=", plain.A(iN, iN),
            " unified=", unified.A(iN, iN));
    CHECK_MESSAGE(std::abs(sparse.A(iN, iN) - plain.A(iN, iN)) < 1e-15,
                  "the overloads must agree");
    CHECK_MESSAGE(std::abs(sparse.A(iN, iN) - unified.A(iN, iN)) < 1e-15,
                  "the overloads must agree");
    CHECK_MESSAGE(std::abs(std::abs(sparse.A(iN, iN)) - 4.0 * h) < 1e-12,
                  "the prescribed cell must contribute its value to the whole face");
}

} // TEST_SUITE("cfd")
