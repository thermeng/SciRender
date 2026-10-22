// Momentum convective operator: the deferred (second order / QUICK) correction.
//
// Contract pinned here:
//   - the momentum assembly is the CONSERVATIVE flux form: the face
//     contributes F*phi_f to the owner row and -F*phi_f to the neighbour row,
//     so A*phi - b is the discretised divergence of rho*u*phi;
//   - a second order reconstruction is applied as a DEFERRED correction:
//     the face value is phi_f = phi_up + corr, the upwind (donor) cell loses
//     |F|*corr from its left-hand side and the downwind cell gains it. Both
//     shares are required - dropping the receiver's share or flipping the
//     sign turns the bounded second order scheme into an unbounded downwind
//     one and breaks the conservation of the operator;
//   - for a LINEAR field every second order reconstruction (SOU, QUICK) is
//     exact, so the momentum operator must reproduce the analytic discrete
//     divergence and agree with the central operator.
//
// The scalar Convection operator (tests/cfd/convection_test.cpp) has the same
// contract, but it is a different class: the momentum assembly had none.
#include "doctest/doctest.h"
#include "fvm/Momentum.h"
#include "support/fvm_fixtures.h"

#include <Eigen/Dense>

#include <array>
#include <cmath>
#include <vector>

using namespace test_support;

namespace {

struct Assembled {
    fvm::SparseMatrix A;
    Eigen::VectorXd b;
};

Assembled assembleMomentum(fvm::Momentum<2>& mom, const fvm::Mesh<2>& mesh,
                            const fvm::VectorField<2>& vel,
                            const std::array<fvm::Field<2>, 2>& velGrad,
                            fvm::ConvectionScheme scheme) {
    const std::size_t n = mesh.nCells();
    Assembled out;
    out.A = fvm::SparseMatrix(n, n);
    out.b = Eigen::VectorXd::Zero(n);

    std::array<fvm::Field<2>, 2> pressureGrad{fvm::Field<2>(mesh, 0.0),
                                              fvm::Field<2>(mesh, 0.0)};
    fvm::Field<2> source;                 // no source
    std::vector<char> dirichletMask;       // no prescribed cells
    std::vector<double> dirichletValues;
    fvm::Field<2> diag;

    mom.assembleWithConvection(0, vel, pressureGrad, source, dirichletMask,
                               dirichletValues, out.A, out.b, diag, scheme,
                               nullptr, &velGrad, fvm::MomentumBCs{});
    return out;
}

// The residual of the momentum equation for the x-momentum component:
// A*u - b is the discretised divergence of rho*u*u_x.
Eigen::VectorXd residual(const Assembled& op, const fvm::VectorField<2>& vel) {
    const std::size_t n = vel.size();
    Eigen::VectorXd u(n);
    for (std::size_t i = 0; i < n; ++i) u(static_cast<Eigen::Index>(i)) = vel[0][i];
    return op.A * u - op.b;
}

// A velocity field that is divergence free and has a linear x-component:
//     u = ( x, -y )   ->   div(u) = 1 - 1 = 0
// The x-momentum convective term is then
//     div( rho * u_x * u ) = rho * div( x * (x, -y) ) = rho * x
// and the exact discrete value on a uniform cell of volume V is rho * x * V.
fvm::VectorField<2> linearDivergenceFree(const fvm::Mesh<2>& mesh) {
    fvm::VectorField<2> vel(mesh, 0.0);
    fillVector<2>(vel, [](double x, double y, double) {
        return std::array<double, 2>{x, -y};
    });
    return vel;
}

// The exact gradient of the x-component of the field above: (1, 0).
std::array<fvm::Field<2>, 2> linearGradient(const fvm::Mesh<2>& mesh) {
    std::array<fvm::Field<2>, 2> grad{fvm::Field<2>(mesh, 0.0), fvm::Field<2>(mesh, 0.0)};
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        grad[0][i] = 1.0;
        grad[1][i] = 0.0;
    }
    return grad;
}

} // namespace

TEST_SUITE("cfd") {

// The second order upwind momentum operator must reproduce the exact discrete
// divergence for a linear field. A sign error or a missing receiver share in
// the deferred correction shows up here immediately.
TEST_CASE("cfd.momentum.sou_linear_field_exact") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    const double rho = 1.0;
    fvm::Momentum<2> mom(mesh, rho, 0.0);   // no diffusion: pure convection
    const fvm::VectorField<2> vel = linearDivergenceFree(mesh);
    const auto grad = linearGradient(mesh);

    const Assembled op = assembleMomentum(mom, mesh, vel, grad,
                                          fvm::ConvectionScheme::SecondOrderUpwind);
    const Eigen::VectorXd r = residual(op, vel);

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i)) continue;   // the boundary faces are a different stencil
        CAPTURE(i);
        const double x = mesh.cellCenter(i)[0];
        const double expected = rho * x * mesh.cellVolume(i);
        CHECK_MESSAGE(std::abs(r(static_cast<Eigen::Index>(i)) - expected) < 1e-12,
                      "second order upwind must be exact for a linear field");
    }
}

// The same field through the central operator: for a linear field every second
// order scheme has to agree with it.
TEST_CASE("cfd.momentum.sou_matches_central_on_linear_field") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    const double rho = 1.0;
    fvm::Momentum<2> mom(mesh, rho, 0.0);
    const fvm::VectorField<2> vel = linearDivergenceFree(mesh);
    const auto grad = linearGradient(mesh);

    const Assembled sou = assembleMomentum(mom, mesh, vel, grad,
                                            fvm::ConvectionScheme::SecondOrderUpwind);
    const Assembled central = assembleMomentum(mom, mesh, vel, grad,
                                                fvm::ConvectionScheme::Central);
    const Eigen::VectorXd rs = residual(sou, vel);
    const Eigen::VectorXd rc = residual(central, vel);

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i)) continue;
        CAPTURE(i);
        CHECK_MESSAGE(std::abs(rs(static_cast<Eigen::Index>(i)) - rc(static_cast<Eigen::Index>(i))) < 1e-12,
                      "on a linear field the second order and the central operators must agree");
    }
}

// QUICK is a three point reconstruction: on a linear field it is exact too, so
// it has to agree with the central operator. This also pins the QUICK branch:
// when it silently fell through to the central branch the test would still
// pass, which is why the quadratic case below exists.
TEST_CASE("cfd.momentum.quick_matches_central_on_linear_field") {
    const fvm::Mesh<2> mesh = uniform2D(6);
    const double rho = 1.0;
    fvm::Momentum<2> mom(mesh, rho, 0.0);
    const fvm::VectorField<2> vel = linearDivergenceFree(mesh);
    const auto grad = linearGradient(mesh);

    const Assembled quick = assembleMomentum(mom, mesh, vel, grad,
                                             fvm::ConvectionScheme::QUICK);
    const Assembled central = assembleMomentum(mom, mesh, vel, grad,
                                                fvm::ConvectionScheme::Central);
    const Eigen::VectorXd rq = residual(quick, vel);
    const Eigen::VectorXd rc = residual(central, vel);

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i)) continue;
        // QUICK is a three point stencil: the up-up cell has to exist, so the
        // two columns next to the upstream boundary fall back to plain upwind.
        if (mesh.cellCoords(i)[0] < 2) continue;
        CAPTURE(i);
        CHECK_MESSAGE(std::abs(rq(static_cast<Eigen::Index>(i)) - rc(static_cast<Eigen::Index>(i))) < 1e-12,
                      "QUICK must be exact for a linear field");
    }
}

// QUICK must not degrade to the central operator. On a quadratic field the
// three point reconstruction is exact while the central one is not, so the two
// operators have to differ.
TEST_CASE("cfd.momentum.quick_is_not_central") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    fvm::Momentum<2> mom(mesh, 1.0, 0.0);

    fvm::VectorField<2> vel(mesh, 0.0);
    fillVector<2>(vel, [](double x, double, double) {
        return std::array<double, 2>{x * x, 0.0};
    });
    std::array<fvm::Field<2>, 2> grad{fvm::Field<2>(mesh, 0.0), fvm::Field<2>(mesh, 0.0)};
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        const double x = mesh.cellCenter(i)[0];
        grad[0][i] = 2.0 * x;   // d(u_x)/dx
        grad[1][i] = 0.0;
    }

    const Assembled quick = assembleMomentum(mom, mesh, vel, grad,
                                             fvm::ConvectionScheme::QUICK);
    const Assembled central = assembleMomentum(mom, mesh, vel, grad,
                                                fvm::ConvectionScheme::Central);
    const Eigen::VectorXd rq = residual(quick, vel);
    const Eigen::VectorXd rc = residual(central, vel);

    double maxDiff = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i)) continue;
        maxDiff = std::max(maxDiff, std::abs(rq(static_cast<Eigen::Index>(i)) - rc(static_cast<Eigen::Index>(i))));
    }
    CHECK_MESSAGE(maxDiff > 1e-9, "QUICK must not silently degrade to central differencing");
}

// The conservative flux form must annihilate a constant transported quantity
// for a divergence free flow: the operator has to conserve the momentum.
TEST_CASE("cfd.momentum.constant_is_conserved_by_divergence_free_flow") {
    const fvm::Mesh<2> mesh = uniform2D(4);
    fvm::Momentum<2> mom(mesh, 1.0, 0.0);

    // Uniform flow in x, uniform x-component: the convective divergence of a
    // constant is zero.
    fvm::VectorField<2> vel(mesh, 0.0);
    vel[0].assign(1.0);
    std::array<fvm::Field<2>, 2> grad{fvm::Field<2>(mesh, 0.0), fvm::Field<2>(mesh, 0.0)};

    for (const auto scheme : {fvm::ConvectionScheme::Upwind,
                              fvm::ConvectionScheme::Central,
                              fvm::ConvectionScheme::SecondOrderUpwind,
                              fvm::ConvectionScheme::QUICK}) {
        CAPTURE(static_cast<int>(scheme));
        const Assembled op = assembleMomentum(mom, mesh, vel, grad, scheme);
        const Eigen::VectorXd r = residual(op, vel);
        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            if (mesh.isBoundary(i)) continue;
            CAPTURE(i);
            CHECK_MESSAGE(std::abs(r(static_cast<Eigen::Index>(i))) < 1e-12,
                          "a divergence free flow must not create momentum out of nothing");
        }
    }
}

} // TEST_SUITE("cfd")
