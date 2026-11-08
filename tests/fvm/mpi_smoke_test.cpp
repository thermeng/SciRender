// MPI smoke test skeleton for the optional Hypre MPI path.
//
// This is a doctest case compiled into scirender_fvm_tests. In the default
// serial build (SCIRENDER_HYPRE_MPI=OFF, HYPRE_SEQUENTIAL=ON) it is a no-op
// that always passes, so the MinGW CI build stays green without an MPI
// runtime. When Hypre is built with MPI (SCIRENDER_HYPRE_MPI=ON), the MPI
// branch verifies the contract documented in src/fvm/HypreSolver.h:
//   - The application (here: the test runner) must call MPI_Init before any
//     Hypre call; ensureHypreInitialized() only *logs* a diagnostic if it
//     was forgotten, it never calls MPI_Init itself.
//   - The current HypreSolver is single-rank replicated: the full 0..n-1
//     global range is passed on MPI_COMM_WORLD with no row partitioning, so
//     the MPI build is expected to run correctly at np=1 (and each rank
//     independently at np>1), not as a distributed solve. See docs/mpi-smoke.md.
//
// NOTE: the shared doctest main() (tests/support/doctest_main.cpp) does not
// call MPI_Init, so a real MPI run needs a launcher that initialises MPI
// first, e.g. `mpiexec -np 1 scirender_fvm_tests.exe --test-case="mpi.*"`,
// with a build configured -DSCIRENDER_HYPRE_MPI=ON on a platform with an
// MPI SDK (MSVC + MS-MPI SDK, or Linux MPICH/OpenMPI). On this MinGW box
// there is a MS-MPI *runtime* (mpiexec.exe) but no SDK, so the MPI branch
// cannot be compiled or linked here; see docs/mpi-smoke.md.

#include "doctest/doctest.h"

#ifdef SCIRENDER_HYPRE_MPI
#include <mpi.h>
#endif

#include "fvm/AMG.h"

#include <Eigen/Sparse>

#include <cmath>
#include <vector>

namespace {

#if defined(SCIRENDER_HYPRE_MPI) && defined(FVM_HAVE_HYPRE)

fvm::SparseMatrix poisson2D(int nx, int ny) {
    const int n = nx * ny;
    std::vector<Eigen::Triplet<double>> trips;
    trips.reserve(static_cast<std::size_t>(n) * 5);
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const int k = j * nx + i;
            trips.emplace_back(k, k, 4.0);
            if (i > 0) trips.emplace_back(k, k - 1, -1.0);
            if (i < nx - 1) trips.emplace_back(k, k + 1, -1.0);
            if (j > 0) trips.emplace_back(k, k - nx, -1.0);
            if (j < ny - 1) trips.emplace_back(k, k + nx, -1.0);
        }
    }
    fvm::SparseMatrix A(n, n);
    A.setFromTriplets(trips.begin(), trips.end());
    return A;
}

double relResidual(const fvm::SparseMatrix& A, const Eigen::VectorXd& b,
                   const Eigen::VectorXd& x) {
    return (b - A * x).norm() / b.norm();
}

#endif // SCIRENDER_HYPRE_MPI && FVM_HAVE_HYPRE

} // namespace

TEST_SUITE("fvm") {

TEST_CASE("mpi.smoke.serial_path_is_noop") {
#if !defined(SCIRENDER_HYPRE_MPI)
    // Default build: Hypre is HYPRE_SEQUENTIAL. The MPI path in
    // HypreSolver.h is compiled out (no <mpi.h>, no MPI_COMM_WORLD use
    // beyond the dummy communicator), so there is nothing to exercise.
    MESSAGE("Serial Hypre build (SCIRENDER_HYPRE_MPI=OFF); MPI smoke test is a no-op");
    CHECK(true);
#endif
}

#ifdef SCIRENDER_HYPRE_MPI
TEST_CASE("mpi.smoke.mpi_initialised_and_single_rank_solve") {
#ifdef FVM_HAVE_HYPRE
    int initialized = 0;
    MPI_Initialized(&initialized);
    CHECK_MESSAGE(initialized, "MPI must be initialised by the launcher/application before Hypre is used");
    if (!initialized) {
        // ensureHypreInitialized() would log
        // "  Hypre: MPI not initialised by application; ..." and still call
        // HYPRE_Initialize(); abort here instead of cascading into Hypre.
        FAIL("MPI not initialised; run under mpiexec with MPI_Init in the launcher");
    }

    int size = 1;
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    // Single-rank replicated assumption (see header comment above): the
    // solver hands the full 0..n-1 range to Hypre on MPI_COMM_WORLD.
    // A truly distributed run would need row partitioning before this test
    // can assert anything stronger than "each rank solves independently".
    MESSAGE("MPI world size = " << size << " (solver is single-rank replicated)");

    const fvm::SparseMatrix A = poisson2D(16, 16);
    const int n = static_cast<int>(A.rows());
    Eigen::VectorXd b = Eigen::VectorXd::Ones(n);
    Eigen::VectorXd x = Eigen::VectorXd::Zero(n);

    fvm::AMGHierarchy::Config cfg;
    cfg.useHypre = true;
    cfg.tolerance = 1e-8;

    fvm::AMGHierarchy amg(cfg);
    amg.build(A);
    CHECK(amg.isBuilt());
    const bool ok = amg.solve(b, x);
    CHECK(ok);
    CHECK(x.allFinite());
    CHECK(relResidual(A, b, x) < 1e-5);
#else
    MESSAGE("SCIRENDER_HYPRE_MPI=ON but Hypre was not found; nothing to smoke-test");
    CHECK(true);
#endif
}
#endif // SCIRENDER_HYPRE_MPI

} // TEST_SUITE("fvm")
