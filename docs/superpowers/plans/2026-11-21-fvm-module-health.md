# FVM Module Health Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `src/fvm` easier to change by renaming the misleading BC class, removing driver reach-in to boundary-condition state, removing the steady/transient duplicates left by the last refactor, and fixing stale diagnostics.

**Architecture:** Four sequential phases, each with its own build/test gate and commit. Boundary-condition spec and its compiled view stay one class; `CFDSolver` remains the shared context but exposes a narrow accessor instead of letting drivers touch its BC fields. Numerical solvers (`AMG`, `SSTModel`) and the public `SIMPLE` API are untouched.

**Tech Stack:** C++20, Qt/CMake + Ninja, MinGW GCC, doctest via CTest, OpenMP, explicit template instantiation for Dim 2 and 3.

**Spec:** `docs/superpowers/specs/2026-11-21-fvm-module-health-design.md`

## Global Constraints

- No change to the public `SIMPLE<Dim>` API or its constructor signature.
- No config-type migration out of `cfd/`; no rewrite of `AMG`, `SSTModel` numerics, or linear solver dispatch.
- No full removal of all `friend` declarations in this plan — only the BC-specific reach-in.
- Explicit instantiations for Dim 2 and 3 must stay in sync between header `extern template` and `.cpp` `template class`.
- Phase order is fixed: 1 → 2 → 3 → 4. Each phase must be committed and green before the next starts.
- Verification commands (run exactly these): `cmake --build build-tests -j8`, `ctest --test-dir build-tests -L fvm`, `ctest --test-dir build-tests -L cfd`.
- Green means exit 0 and the expected test count. Current baseline: fvm 35/35, cfd 62/62. `known_failures.txt` tests are `WILL_FAIL` and count as passing.

## Review Focus

These are the failure modes the design implies but no task's own test fully exercises. Each is pinned to a task's verification as noted.

1. **A 3D run breaks after a header/source rename** — a missed include or `extern template` leaves the Dim-3 instantiation unresolved. Pinned in Task 1 verification: full `fvm` suite includes `fvm.channel_3d.*`.
2. **BC changed after a build is not seen by momentum or pressure correction** — the compiled masks stay stale and the flow keeps the old condition. Pinned in Task 2's new test `cfd.bc.compiled_view_rebuilds_only_when_spec_changes`.
3. **BDF2 first step or restart (`dtPrev_ == 0`) changes behavior when the nut-relaxation loop is shared** — expected: the existing Euler fallback and history selection remain exact. Pinned in Task 3 verification: `fvm.transient.*` and `fvm.restart.*` suites.
4. **Transient corrector early exit silently gains a steady hold streak** — expected: one below-tolerance snapshot ends the corrector loop. Pinned in Task 3's new test `cfd.convergence.transient_gate_uses_one_iteration_hold`.
5. **Reusing one solver for a second run reports the previous run's diagnostics** — expected: no stale limiter warnings or steady labels on a transient run. Pinned in Task 4's new test `fvm.transient.final_stats_do_not_use_steady_labels` for the labels; the limiter-counter reset is a one-line fix checked by inspection, not by a test that forces a limiter hit.

## File structure

- `src/fvm/BoundaryConditions.{h,cpp}` — renamed from `SIMPLEBoundaryConditions.{h,cpp}`; owns the spec and the lazily rebuilt compiled view.
- `src/fvm/CFDSolver.{h,cpp}` — shared context; loses the duplicate BC field and the momentum wrapper; gains one BC accessor and one nut-relaxation helper.
- `src/fvm/TransientDriver.{h,cpp}` — stops touching BC fields; calls the shared helpers; uses one convergence monitor.
- `src/cfd/convergence_policy.{h,cpp}` — gains one small factory making a steady-style policy from a transient one.
- `tests/cfd/boundary_conditions_test.cpp` — BC lifecycle test.
- `tests/cfd/convergence_policy_test.cpp` — new file, registered in `tests/CMakeLists.txt`.
- `tests/fvm/transient_channel_test.cpp` — final-stats output test.
- `GLOSSARY.md` — new root file with the three canonical BC terms.

---

### Task 1: Rename the BC class and pin the terms

**Files:**
- Modify: `CMakeLists.txt:245`, `src/fvm/CFDSolver.h:11,285`, `src/fvm/PressureCorrection.h:8`
- Create: `src/fvm/BoundaryConditions.h`, `src/fvm/BoundaryConditions.cpp`, `GLOSSARY.md`
- Delete: `src/fvm/SIMPLEBoundaryConditions.h`, `src/fvm/SIMPLEBoundaryConditions.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `#include "fvm/BoundaryConditions.h"`, type `fvm::BoundaryConditions<Dim>` with the same public setters as the old class. All later tasks use this name.

- [ ] **Step 1: Change every reference to the new path and class name**

Replace `fvm/SIMPLEBoundaryConditions.h` → `fvm/BoundaryConditions.h` in `src/fvm/CFDSolver.h:11` and `src/fvm/PressureCorrection.h:8`. In `CMakeLists.txt:245` change `src/fvm/SIMPLEBoundaryConditions.cpp` → `src/fvm/BoundaryConditions.cpp`. In `src/fvm/CFDSolver.h:285` change the member type to `BoundaryConditions<Dim> bc_;`. Leave the two old files in place for this step.

- [ ] **Step 2: Run the build to verify it fails (RED)**

Run: `cmake --build build-tests -j8`
Expected: exit 1, missing `fvm/BoundaryConditions.h` and unresolved `BoundaryConditions<2>`/`BoundaryConditions<3>`.

- [ ] **Step 3: Move and rename the implementation**

Run `git mv src/fvm/SIMPLEBoundaryConditions.h src/fvm/BoundaryConditions.h` and the same for `.cpp`. Rename the class, its constructor/method qualifiers, its `extern template` block in the header, and its `template class` block at the bottom of the `.cpp` to `BoundaryConditions`. Update the `.cpp`'s own include. Keep every method body and signature otherwise identical.

- [ ] **Step 4: Add the glossary**

Create root `GLOSSARY.md` with one short entry each for `BoundaryConditions` (user-facing set of conditions/patches plus build step), `BCDiscretization` (compiled per-cell/per-face view, output of `BoundaryConditions`), and `PatchBC` (UI input record, not an FVM type). No implementation detail in this file.

- [ ] **Step 5: Build and run both suites**

Run: `cmake --build build-tests -j8`, then `ctest --test-dir build-tests -L fvm`, then `ctest --test-dir build-tests -L cfd`
Expected: build exit 0; fvm 35/35; cfd 62/62 (includes the 3D channel cases).

- [ ] **Step 6: Commit**

`git add -A && git add -f docs/superpowers/specs/2026-11-21-fvm-module-health-design.md docs/superpowers/plans/2026-11-21-fvm-module-health.md && git commit -m "refactor(fvm): rename SIMPLEBoundaryConditions to BoundaryConditions"`. The `-f` is required because `/docs/` is gitignored in this repo (`.gitignore:123`); without it the two design documents stay local only.

---

### Task 2: One BC build lifecycle, no driver reach-in

**Files:**
- Modify: `src/fvm/BoundaryConditions.h`, `src/fvm/BoundaryConditions.cpp`, `src/fvm/CFDSolver.h:285-286`, `src/fvm/CFDSolver.cpp:66,166,192,346-347,359-360,513,766-787,880-916`, `src/fvm/TransientDriver.cpp:67,135-136,200`, `tests/cfd/boundary_conditions_test.cpp`

**Interfaces:**
- Consumes: `fvm::BoundaryConditions<Dim>` from Task 1.
- Produces: `const BCDiscretization& BoundaryConditions<Dim>::discretization() const` — rebuilds into a stable internal object only when dirty; `CFDSolver<Dim>::bcDiscretization() const` returning the same reference. `build()` and `ensureBuilt()` are removed.

- [ ] **Step 1: Write the failing test**

Add to `tests/cfd/boundary_conditions_test.cpp`, after `solverConfig()`:

```cpp
TEST_CASE("cfd.bc.compiled_view_rebuilds_only_when_spec_changes") {
    const fvm::Mesh<2> mesh = uniform2D(6);
    fvm::BoundaryConditions<2> bc(mesh);
    bc.setVelocityBC(0, -1, fvm::VelocityBCType::Inlet, 1.0, 0.0, 0.0);

    const auto& first = bc.discretization();
    const auto* stable = &first;
    const std::size_t cell = mesh.cellIndex(0, 2);
    CHECK(first.dirichletMask(0)[cell] != 0);
    CHECK(first.dirichletVals(0)[cell] == doctest::Approx(1.0));
    CHECK(&bc.discretization() == stable);

    bc.setVelocityBC(0, -1, fvm::VelocityBCType::Inlet, 2.0, 0.0, 0.0);
    CHECK(bc.isDirty());
    const auto& second = bc.discretization();
    CHECK(second.dirichletVals(0)[cell] == doctest::Approx(2.0));
    CHECK(&second == stable);
    CHECK_FALSE(bc.isDirty());
}
```

Add `#include "fvm/BoundaryConditions.h"` at the top of that test file.

- [ ] **Step 2: Run the test to verify it fails (RED)**

Run: `cmake --build build-tests -j8`, then `ctest --test-dir build-tests -R cfd.bc.compiled_view_rebuilds_only_when_spec_changes --output-on-failure`
Expected: build fails with `'discretization' is not a member of 'fvm::BoundaryConditions<2>'`.

- [ ] **Step 3: Implement the lazy compiled view**

In `BoundaryConditions.h`: add `mutable BCDiscretization discretization_;`, declare `const BCDiscretization& discretization() const`, keep `isDirty()`/`markDirty()`, delete `build()` and `ensureBuilt()`. In `BoundaryConditions.cpp`: implement `discretization()` by calling the existing `buildBCMasks(mesh_, spec())` into `discretization_` when dirty, running the existing first-build `debugPrint` once, clearing `dirty_`, and returning the member. Update both `extern template` / `template class` blocks. Do not change any BC setter.

- [ ] **Step 4: Run the targeted test (GREEN)**

Run: `cmake --build build-tests -j8`, then `ctest --test-dir build-tests -R cfd.bc.compiled_view_rebuilds_only_when_spec_changes --output-on-failure`
Expected: build exit 0; test 1/1 passed.

- [ ] **Step 5: Migrate CFDSolver and TransientDriver off the two BC fields**

In `CFDSolver.h`: delete `BCDiscretization bcDiscretization_;` and add a private `const BCDiscretization& bcDiscretization() const { return bc_.discretization(); }`. In `CFDSolver.cpp`: replace every `bcDiscretization_.` with `bcDiscretization().`, and delete the two rebuild statements at lines 166 and 513. In `TransientDriver.cpp`: delete the two `if (solver_.bc_.isDirty()) solver_.bcDiscretization_ = solver_.bc_.build();` statements at lines 67 and 200, and replace `solver_.bcDiscretization_.` with `solver_.bcDiscretization().` at lines 135-136. Keep `SIMPLE`'s eight forwarding setters unchanged — they are the public API.

- [ ] **Step 6: Build and run both suites**

Run: `cmake --build build-tests -j8`, then `ctest --test-dir build-tests -L fvm`, then `ctest --test-dir build-tests -L cfd`
Expected: build exit 0; fvm 35/35; cfd 63/63 (one new BC test).

- [ ] **Step 7: Commit**

`git add -A && git commit -m "refactor(fvm): give boundary conditions one build lifecycle"`

---

### Task 3: Remove the surviving steady/transient duplicates

**Files:**
- Modify: `src/fvm/CFDSolver.h:214-219,362-365`, `src/fvm/CFDSolver.cpp:30-43,516,536-547`, `src/fvm/TransientDriver.h:48-50`, `src/fvm/TransientDriver.cpp:24-39,204-230,510-528,535-569`, `src/cfd/convergence_policy.h`, `src/cfd/convergence_policy.cpp`, `tests/CMakeLists.txt`
- Create: `tests/cfd/convergence_policy_test.cpp`

**Interfaces:**
- Consumes: `cfd::TransientStoppingPolicy`, `cfd::SteadyStoppingPolicy`, `cfd::ConvergenceMonitor` from `src/cfd/convergence_policy.h`.
- Produces: `cfd::SteadyStoppingPolicy cfd::steadyPolicyFromTransient(const cfd::TransientStoppingPolicy&)` — copies active equations and tolerances, sets `convergenceHoldIterations = 1`, `residualGateEnabled = true`. `CFDSolver<Dim>::relaxNut_(const Field<Dim>& previous, double alpha = nutUnderRelax)`.

- [ ] **Step 1: Write the failing convergence test**

Create `tests/cfd/convergence_policy_test.cpp` with `#include "doctest/doctest.h"`, `#include "cfd/convergence_policy.h"`, `#include <optional>` and:

```cpp
TEST_CASE("cfd.convergence.transient_gate_uses_one_iteration_hold") {
    cfd::TransientStoppingPolicy p;
    p.activeEquations = {cfd::ResidualEquation::Continuity, cfd::ResidualEquation::MomentumX};
    p.effectiveTolerances = {1e-3, 1e-3};

    const cfd::SteadyStoppingPolicy steady = cfd::steadyPolicyFromTransient(p);
    CHECK(steady.convergenceHoldIterations == 1);
    CHECK(steady.residualGateEnabled);
    CHECK(steady.activeEquations == p.activeEquations);
    CHECK(steady.effectiveTolerances == p.effectiveTolerances);

    cfd::ConvergenceMonitor monitor(steady);
    CHECK(monitor.update(1e-4, 1e-4, 1e-4, std::nullopt, std::nullopt, std::nullopt));
}
```

Add `cfd/convergence_policy_test.cpp` to the `scirender_cfd_tests` SOURCES block in `tests/CMakeLists.txt` near the other `cfd/` entries.

- [ ] **Step 2: Run the test to verify it fails (RED)**

Run: `cmake --build build-tests -j8`, then `ctest --test-dir build-tests -R cfd.convergence.transient_gate_uses_one_iteration_hold --output-on-failure`
Expected: build fails with `'steadyPolicyFromTransient' is not a member of 'cfd'`.

- [ ] **Step 3: Implement the policy factory and switch the transient gate to it**

Declare `steadyPolicyFromTransient` in `src/cfd/convergence_policy.h`; implement it in `.cpp` by value-copying `activeEquations` and `effectiveTolerances` and setting the two flags. In `src/fvm/TransientDriver.cpp:535-569`, replace the inline switch with one `cfd::ConvergenceMonitor` constructed from `cfd::steadyPolicyFromTransient(s.config_.stoppingPolicy.transientPolicy)` before the outer-corrector loop, then call `monitor.update(...)` with `lastContResid_`, `lastMomResidU_`, `lastMomResidV_`, `std::optional<double>(lastMomResidW_)` only for Dim 3, and `lastKResid_`/`lastOmegaResid_` only when `s.turbulence_` exists. Preserve the existing break-on-true behavior.

- [ ] **Step 4: Run the targeted test (GREEN)**

Run: `cmake --build build-tests -j8`, then `ctest --test-dir build-tests -R cfd.convergence.transient_gate_uses_one_iteration_hold --output-on-failure`
Expected: build exit 0; test 1/1 passed.

- [ ] **Step 5: Remove the momentum wrapper**

Delete `CFDSolver<Dim>::solveMomentum_` from `src/fvm/CFDSolver.h:214` and `src/fvm/CFDSolver.cpp:30-43`. At `src/fvm/CFDSolver.cpp:516`, inline the same per-dimension loop calling `solveMomentumComponent_(d, uStar_, velocityOld_, pressureGrad_)` after `pressureGradientCalc_->compute(...)`, with default `TransientParams<Dim>{}`. Fix the stale comment at `src/fvm/TransientDriver.h:48-49` to say only that `solveMomentumComponent_` is shared.

- [ ] **Step 6: Share the nut relaxation loop**

Declare `void relaxNut_(const Field<Dim>& previous, double alpha = nutUnderRelax);` privately on `CFDSolver`, implement it as the existing OpenMP weighted-average loop over `mesh_.nCells()` writing into `turbulence_->nut()`. Replace the three copies at `CFDSolver.cpp:543-546`, `TransientDriver.cpp:226-229` and `TransientDriver.cpp:524-527` with calls passing `nutOld_`, `solver_.nutOld_` and `dummyNut` respectively. Keep the transient-specific `solveIterationTransient` calls where they are — they are different numerical schemes, not duplication.

- [ ] **Step 7: Build and run both suites**

Run: `cmake --build build-tests -j8`, then `ctest --test-dir build-tests -L fvm`, then `ctest --test-dir build-tests -L cfd`
Expected: build exit 0; fvm 35/35; cfd 64/64 (new convergence test). Existing transient and restart tests must still pass unchanged.

- [ ] **Step 8: Commit**

`git add -A && git commit -m "refactor(fvm): remove duplicated momentum, turbulence and convergence code"`

---

### Task 4: Fix stale state and diagnostics

**Files:**
- Modify: `src/fvm/CFDSolver.h:251,340,362,365`, `src/fvm/CFDSolver.cpp:144,615-688`, `src/fvm/TransientDriver.h:68`, `src/fvm/TransientDriver.cpp:385,581,617-623,626`, `src/fvm/SIMPLE.cpp:307`, `tests/fvm/transient_channel_test.cpp`

**Interfaces:**
- Consumes: nothing new from earlier tasks.
- Produces: `CFDSolver<Dim>::printFinalStats_(bool isTransient) const`; `SIMPLE::solve()` passes `false`, `TransientDriver::run()` passes `true`.

- [ ] **Step 1: Write the failing output test**

Add to `tests/fvm/transient_channel_test.cpp`:

```cpp
TEST_CASE("fvm.transient.final_stats_do_not_use_steady_labels") {
    const fvm::Mesh<2> mesh(std::array<std::size_t, 2>{6, 3},
                            std::array<double, 2>{4.0, 1.0});
    fvm::SIMPLE<2>::Config cfg;
    cfg.transient.enabled = true;
    cfg.transient.dt = 0.01;
    cfg.stopping.transient.tEnd = 0.02;
    cfg.stopping.transient.maxOuterCorrectors = 2;
    cfg.printFrequency = 1000000;
    cfg.solverType = fvm::SolverType::PISO;

    std::vector<std::size_t> inlet, outlet, walls;
    forEachBoundaryFace(mesh, 0, -1, [&](std::size_t f, std::size_t) { inlet.push_back(f); });
    forEachBoundaryFace(mesh, 0, +1, [&](std::size_t f, std::size_t) { outlet.push_back(f); });
    forEachBoundaryFace(mesh, 1, -1, [&](std::size_t f, std::size_t) { walls.push_back(f); });
    forEachBoundaryFace(mesh, 1, +1, [&](std::size_t f, std::size_t) { walls.push_back(f); });

    fvm::SIMPLE<2> solver(mesh, 1.0, 0.01, cfg);
    solver.setVelocityBCOnFacesWithSlip(inlet, "inlet", 1.0, 0.0, 0.0, false, 0.0);
    solver.setVelocityBCOnFacesWithSlip(outlet, "pressure_outlet", 0.0, 0.0, 0.0, false, 0.0);
    solver.setVelocityBCOnFacesWithSlip(walls, "wall", 0.0, 0.0, 0.0, false, 0.0);
    fvm::VectorField<2> velocity(mesh, 0.0);
    fvm::Field<2> pressure(mesh, 0.0);

    std::ostringstream captured;
    auto* previous = std::cerr.rdbuf(captured.rdbuf());
    solver.solveTransient(velocity, pressure);
    std::cerr.rdbuf(previous);

    const std::string log = captured.str();
    CHECK(log.find("steady SIMPLE total") == std::string::npos);
    CHECK(log.find("tolerance=") == std::string::npos);
}
```

- [ ] **Step 2: Run the test to verify it fails (RED)**

Run: `cmake --build build-tests -j8`, then `ctest --test-dir build-tests -R fvm.transient.final_stats_do_not_use_steady_labels --output-on-failure`
Expected: PASS-to-be test fails because the captured log contains `"tolerance="` (steady tolerance line).

- [ ] **Step 3: Make final stats transient-aware and clear stale state**

Change `printFinalStats_()` to `printFinalStats_(bool isTransient) const`. Pass `false` at `src/fvm/SIMPLE.cpp:307` and `true` at `src/fvm/TransientDriver.cpp:626`. When `isTransient`, skip the steady tolerance line (`CFDSolver.cpp:636-640`) and the entire `[timing]` block (`CFDSolver.cpp:661-687`). Add `pPrimeLimiterHits_ = 0;` to `initializeSolve_`'s reset block near line 144. Delete the duplicate transient limiter warning at `TransientDriver.cpp:617-623` — the shared `printFinalStats_()` warning remains the only one. Delete the write-only `pressureN_` field from `TransientDriver.h:68` and its two assignments at `TransientDriver.cpp:385,581`. Delete `steadyMomentumSolveSeconds_` and `steadyVelocityCorrectionSeconds_` from `CFDSolver.h` and their two uses in the timing lines at `CFDSolver.cpp:666,670`.

- [ ] **Step 4: Fix stale comments**

Update these exact stale references: `src/fvm/SIMPLE.h:9-11` (transient accessors still live here), `src/fvm/SIMPLE.cpp:100` (points to a header that no longer has the shared core; remove the mojibake), `src/fvm/CFDSolver.cpp:486` and `src/fvm/TransientDriver.cpp:354` (mojibake), `src/cfd/solver_config_bridge.h:10-12` (false "compile error" claim), `src/cfd/cfd_types.h:150-155,248` (`fvm::SIMPLE::TransientConfig` is now a base alias). Comments only; no behavior change.

- [ ] **Step 5: Build and run both suites**

Run: `cmake --build build-tests -j8`, then `ctest --test-dir build-tests -L fvm`, then `ctest --test-dir build-tests -L cfd`
Expected: build exit 0; fvm 36/36 (new output test); cfd 64/64.

- [ ] **Step 6: Commit**

`git add -A && git commit -m "fix(fvm): reset stale state and label transient output correctly"`
