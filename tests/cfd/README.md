# CFD fundamentals suite

Unit tests for the FVM operators of the solver (`src/fvm/`). One doctest test
case per operator behaviour, each registered as its own CTest row, so a failure
names the behaviour that broke:

```
ctest --test-dir build-tests -L cfd
```

The suite is part of the main CMake project (`scirender_cfd_tests`), it links
the real `FvmCore` / `CfdLib` targets, and it needs neither Qt nor an OpenGL
context.

## The contracts

Every test states one contract of the operator it covers. The expected values
are hand-derived from the physics, not copied from the implementation's output.

### Mesh geometry (`cfd.mesh.*`)

* the cell volumes and centres, the face areas, normals, distances and the
  exact interpolation weight;
* a stretched grid varies its spacing, the local spacings add up to the domain
  length, and the cell area is the product of the two local spacings;
* an unstructured mesh tiles the domain exactly and refuses the structured-only
  accessors instead of returning nonsense.

### Gradient (`cfd.gradient.*`)

* the least-squares gradient reproduces a linear field **exactly** at the
  interior cells, and a quadratic exactly as well (the symmetric stencil fits
  it without a residual);
* the wall ghost implements a zero-gradient condition, so the boundary cells
  are excluded from the exactness claims;
* the cell-based Green-Gauss must use the mesh's face weight - the exact
  linear interpolation between the two cell centres - and not a plain
  `0.5*(phi_P + phi_N)`. A stretched grid discriminates the two: the cells
  have different widths, so the face is not midway between the centres;
* the node-based Green-Gauss reconstructs the nodes of a structured mesh from
  the grid (a structured mesh has no vertex arrays) and reproduces a linear
  field exactly;
* a prescribed cell keeps its own value: the gradient of such a cell is the
  one-sided gradient towards the interior, and the wall ghost must not dilute
  it.

A **non-orthogonal** (skewed) mesh is a different matter: there the face
centre is off the centre-to-centre line, the mesh's face weight is the
orthogonal projection parameter (bounded, and the standard choice), and a
cell-centred scheme is only first-order accurate. That is a property of the
formulation, not a defect of the operator.

### Divergence (`cfd.divergence.*`)

* the face velocity is the exact linear interpolation between the two cell
  centres;
* at a boundary face the velocity comes from the Dirichlet mask, not from the
  interior cell - the three-argument overload cannot honour that;
* the fluxes telescope: `sum(div * V)` over the cells equals the net flux
  through the boundary faces;
* `maxDivergence()` and the residual must measure the same quantity.

### Laplacian / Poisson (`cfd.poisson.*`)

* `A phi = b` with `A = -div(c grad(phi))`: positive diagonal, non-positive
  off-diagonals, symmetric. A positive source produces a positive bump, which
  is what SIMPLE's pressure equation needs. (The `Poisson.h` header comment
  states the opposite - the code is right, the comment is what has to change.)
* `b = source * V`, i.e. `source` is a density;
* the discrete operator applied to the exact quadratic reproduces the analytic
  Laplacian times the cell volume - that pins the sign, the coefficient and
  the volume weighting in one assertion;
* a masked cell gets an identity row, its unmasked neighbours pick the
  prescribed value up in the right-hand side;
* `Laplacian(k)` and `Poisson(c = k, source = 0)` assemble the same operator.

### Convection (`cfd.convection.*`)

* upwind: `F >= 0 -> A(P,P) += F`, `F < 0 -> A(P,N) = F`; central: `F/2` on
  both;
* second order upwind: the upwind base plus a deferred reconstruction that is
  clamped to the neighbour bounds, with the known term moved to the right-hand
  side (so the full balance is `(A phi) - b`);
* central, second order upwind and QUICK are exact for a linear field;
* a constant transported quantity is conserved by a divergence-free flow;
* the three overloads must assemble the same operator.

### Pressure and velocity correction (`cfd.pressure.*`, `cfd.velocity.*`)

* a uniform flow must not produce a pressure correction;
* the pinned reference cell carries `p' = 0`;
* the correction must remove the divergence it was built for;
* a cell without an interior face must not be silently pinned;
* `u = u* - d * grad(p')`, a prescribed cell is not corrected, and a symmetry
  wall projects the normal component out.

### Boundary conditions (`cfd.bc.*`)

* a wall cell is frozen by the cell mask: all components for NoSlip and
  MovingWall, only the normal component for an inlet;
* a moving wall prescribed on the **faces** must drive the momentum equation;
* on an unstructured mesh the wall condition is applied through the face path
  and the wall stays impermeable.

### Residual (`cfd.residual.*`)

* the continuity residual is the mass imbalance normalised by the total flux
  and covers the boundary cells;
* the momentum residual is the relative per-iteration **change**, not a linear
  residual.

## The known-failure registry

`known_failures.txt` lists the test cases that are known to fail because the
solver deviates from the contract the test states. Such a row is registered
through `cmake/expect_failure.cmake`, which inverts the outcome: the row
**passes while the defect is open** and turns **red as soon as the solver is
fixed**. A name that does not match any test case fails the build, so the
registry cannot silently rot.

The worklist:

```
ctest --test-dir build-tests -L known-broken -V
```

prints every outstanding defect with its real diagnostic.

## Fixing the solver one defect at a time

1. `ctest --test-dir build-tests -L known-broken -V` - pick the top entry; the
   diagnostic names the operator and the expected versus actual values.
2. Fix the solver. One defect per change.
3. Re-run: the row turns **red** (the test now passes) - remove the entry from
   `known_failures.txt` and the row is green again.
4. Repeat.

A `crash:` prefix in the registry marks a defect that takes the test process
down (an out-of-bounds read). The wrapper treats a crash like any other
failure.

## Tolerances

`FvmCore` is compiled with `-ffast-math`, so "machine epsilon" claims are not
available. The suite therefore uses:

| Kind of assertion | Tolerance |
|---|---|
| exactness on a linear field | relative `1e-12` |
| conservation / telescoping sums | relative `1e-12` |
| boundedness (the SOU limiter) | exact, with a `1e-12` slack |
| structural properties (symmetry, M-matrix) | relative `1e-12` |
