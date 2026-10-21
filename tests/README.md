# Tests

The regression suite of SciRender. It is a **part of the main CMake project** (not a separate build system) and uses **doctest** + **CTest**.

Two properties drive the design:

* **Every test case is its own CTest row.** `ctest` names the behaviour that broke instead of reporting "the binary failed".
* **A test-only build needs neither Qt nor an OpenGL context.** `cmake --preset tests` configures the suite without the application, so it runs anywhere the compiler runs.

```
135 test cases
  core    65   parsers, isosurface, post-processing fields, camera math
  fvm      7   AMG configuration, transient settings, the open channel
  render   9   the streamline operator (CPU side, no GL)
  cfd     49   the CFD fundamentals: mesh geometry, gradient, divergence,
               Laplacian/Poisson, convection schemes, pressure and velocity
               correction, boundary conditions, residual
  slow     5   the cavity benchmarks: the structured cavity (Ghia, Re = 100)
               and the ANSYS-mesh cavity (Ghia, Re = 400) - a label, not a
               suite
```

## Layout

```
tests/
├── CMakeLists.txt              the four test binaries and the registry wiring
├── cmake/
│   ├── DiscoverDoctestTests.cmake   generates the CTest matrix (one row per case)
│   └── expect_failure.cmake         inverts the outcome of a known failure
├── support/                    shared helpers
│   ├── doctest_main.cpp        the one translation unit with main()
│   ├── test_support.{h,cpp}    sample fixtures, scratch directories
│   ├── mesh_fixtures.{h,cpp}   core::RenderMesh fixtures
│   └── fvm_fixtures.{h,cpp}    fvm::Mesh fixtures + analytic field fillers
├── core/                       core/ layer
├── fvm/                        fvm/ layer
│   └── known_failures.txt      the registry of known defects (fvm suite)
├── render/                     render/ layer
├── cfd/                        cfd/ layer - see cfd/README.md for the contracts
│   ├── known_failures.txt      the registry of known defects (cfd suite)
│   └── README.md
├── run_tests.bat               configure + build + run (Windows)
└── run_tests.sh                configure + build + run (POSIX)
```

## Building and running

```bash
cmake --preset tests              # configure build-tests (no Qt needed)
cmake --build --preset tests      # build the four test binaries
ctest --preset tests              # run everything, with the failure output
```

Or in one step:

```bash
tests/run_tests.bat               # Windows
tests/run_tests.sh                # Linux / macOS / Git Bash
```

Both scripts configure `build-tests` if needed, build the test binaries and run CTest. Extra arguments are passed straight to `ctest`:

```bash
tests/run_tests.bat -R "cfd.gradient"     # one group of tests
tests/run_tests.bat -LE slow              # without the slow benchmark
```

### Useful commands

| Command | Effect |
|---|---|
| `ctest --test-dir build-tests -N` | list every row without running it |
| `ctest --test-dir build-tests -L core` | only the `core` layer |
| `ctest --test-dir build-tests -L cfd` | only the CFD fundamentals |
| `ctest --test-dir build-tests -L known-broken -V` | the outstanding defects, with their diagnostics |
| `ctest --test-dir build-tests -LE slow` | everything but the slow benchmark |
| `ctest --test-dir build-tests -R "core.parse"` | by name pattern |
| `ctest --test-dir build-tests --output-on-failure` | print the failing assertion of every failed row |
| `ctest --test-dir build-tests -V` | print the output of every test |

The labels are derived from the test-case name: the part before the first dot.

### Running a single case

Each CTest row runs the test binary with a single case selected, so a case can
also be run directly:

```bash
build-tests/tests/scirender_cfd_tests --test-case="cfd.gradient.*"
build-tests/tests/scirender_cfd_tests --test-suite=cfd
build-tests/tests/scirender_cfd_tests --list-test-cases
```

## Writing a test

**Where it goes.** One file per operator area, in the layer directory that
matches the prefix of the test name: `core/`, `fvm/`, `render/`, `cfd/`.

**The name.** `<layer>.<area>.<behaviour>`, e.g.
`cfd.gradient.green_gauss_cell_stretched_exact`. The name is the CTest row
name, so it should say what breaks - not what the code does.

**The shape.**

```cpp
#include "doctest/doctest.h"
#include "fvm/Gradient.h"
#include "support/fvm_fixtures.h"

TEST_SUITE("cfd") {

TEST_CASE("cfd.gradient.some_behaviour") {
    const fvm::Mesh<2> mesh = uniform2D(6);      // a fixture from support/
    Field<2> phi(mesh, 0.0);
    fillScalar(phi, [](double x, double y, double) { return 2.0 * x - 3.0 * y; });

    Gradient<2> grad(mesh, GradientScheme::LeastSquares);
    std::array<Field<2>, 2> g{Field<2>(mesh, 0.0), Field<2>(mesh, 0.0)};
    grad.compute(phi, g);

    CAPTURE(mesh.nCells());
    CHECK_MESSAGE(std::abs(g[0][0] - 2.0) < 1e-12, "d(phi)/dx must be exact");
}

} // TEST_SUITE("cfd")
```

**The rules.**

* `CHECK` for a property, `REQUIRE` when the rest of the case cannot run
  without it, `CHECK_MESSAGE` when the failure needs a sentence.
* `CAPTURE(...)` to add the context (the cell index, the mesh size) that the
  failure message needs.
* A doctest assertion must be a **single comparison** - `a && b` and `a || b`
  are rejected by the expression decomposer. Compute the combined condition
  into a named `bool` first.
* Every expected value is **hand-derived from the physics**, not copied from
  the implementation's output. The contract is stated in the comment above the
  assertion, and the CFD contracts are written down in
  [cfd/README.md](cfd/README.md).

**Tolerances.** `FvmCore` is compiled with `-ffast-math`, so "machine epsilon"
is not available. Use:

| Kind of assertion | Tolerance |
|---|---|
| exactness on a linear field | relative `1e-12` |
| conservation / telescoping sums | relative `1e-12` |
| boundedness | exact, with a `1e-12` slack |
| structural properties (symmetry, M-matrix) | relative `1e-12` |
| second-order accuracy | an order test (the error at `h` and at `h/2`), not an absolute bound |

**Fixtures** live in `tests/support/`:

* `test_support.h` - `requireSample("file.vtu")` (fails when the fixture is
  missing - a missing fixture is never a silent skip), `samplePath(...)`,
  `TempDir` (a scratch directory under the build tree, removed afterwards).
* `mesh_fixtures.h` - `core::RenderMesh` fixtures and analytic field fillers.
* `fvm_fixtures.h` - `fvm::Mesh` fixtures (uniform, stretched, unstructured
  triangles) and the analytic field fillers.

## The known-failure registry

`tests/cfd/known_failures.txt` lists the test cases that are known to fail
because the solver deviates from the contract the test states. Each listed
case runs through `cmake/expect_failure.cmake`, which **inverts** the outcome:

* the defect is open -> the row **passes** (by failing);
* the solver is fixed -> the row turns **red** -> that is the signal to remove
  the entry;
* a listed name that matches no test case -> the **build fails**, so the
  registry cannot silently rot.

```
ctest --test-dir build-tests -L known-broken -V     # the worklist
```

Working through it:

1. pick the top entry - the diagnostic names the operator and the expected
   versus the actual values;
2. fix the solver, one defect per change;
3. re-run: the row turns red - remove the name from `known_failures.txt`;
4. the row is green again and the backlog is one shorter.

## The CFD contracts

The `cfd/` suite states one contract per operator behaviour. They are written
down in **[cfd/README.md](cfd/README.md)** - the mesh geometry, the gradient
schemes, the divergence, the Laplacian/Poisson operator, the convection
schemes, the pressure and velocity correction, the boundary conditions and the
residual reporting. Read it before changing a solver operator: it says what
the code is supposed to do, and which parts are still open.

## The cavity benchmarks

The lid-driven cavity is the flagship validation case, in two variants:

* `slow.ghia.*` - a generated structured grid (32x32), the classical Ghia
  Re = 100 reference, and the check that the state is converged.
* `slow.cavity_msh.*` - the ANSYS mesh `samples/cfd/CavityQuad_40x40.msh`
  (the graded 40x40 quad grid Fluent was run on), the Ghia Re = 400 reference,
  and the accuracy target.

The `.msh` variant is set up **the way an end user sets it up in the
application**: the mesh's named boundary groups are the "patches" (the lid group
in that mesh is called `movingwall`), and every patch is a boundary condition
applied to the faces of that group through `setVelocityBCOnFacesWithSlip` -
exactly the call the CFD case page makes.

The cases report the peak velocity, the mass balance, the centreline profile
against the published table and the centre of the primary vortex, so a solver
fix can be measured against them.

## The open channel

`fvm.channel.open_outlet_divergence` is the case that exposes the
pressure-velocity coupling. A closed cavity cannot: it has no net outflow, so
the spurious near-null mode of the pure-Neumann pressure operator produces no
gradient and the corruption cancels. An open channel (a velocity inlet, a
pressure outlet, no-slip walls) does - the pressure correction has a real
right-hand side.

The case reports the interior divergence *per column*, which localises the
defect: the divergence is largest exactly where the predictor is divergent
(the entry region and the pressure outlet) and small everywhere else.

## Notes

* Each CTest row is a **separate process**. A case that crashes (an
  out-of-bounds read, an assertion in a constructor) fails on its own and
  cannot take the rest of the suite down.
* The suite is not wired into CI yet - there is no workflow that runs it.
* The test build does not build the application, so `SCIRENDER_BUILD_APP=OFF`
  is what keeps the suite Qt-free.
