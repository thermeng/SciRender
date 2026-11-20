# FVM module health — design

Date: 2026-11-21
Status: draft, awaiting approval

## Intent

The FVM module works, but it is hard to change safely. The recent architecture
work (commits `87aefc3`..`884a2f6`, plus `c37a8fd`) extracted shared machinery
into `CFDSolver` and split the time-dependent work into `TransientDriver`. The
split left three kinds of residue: misleading names, duplicated logic that the
split was meant to remove, and bidirectional friend access between the base and
the driver.

The concrete trigger is `fvm::SIMPLEBoundaryConditions`. It is not
SIMPLE-specific: `TransientDriver` uses the same object for PISO and PIMPLE
(`src/fvm/TransientDriver.cpp:67,200`). The prefix is historical — when the
class was introduced in `062445b`, a separate generic `fvm/BoundaryConditions`
module existed; that module was deleted as dead in `888bf11`.

Goal: make `src/fvm` a module a developer can change in one place, with names
that say what things are. Do this as a sequence of small, tested, committed
steps rather than a rewrite.

## Goals

1. Rename the boundary-condition spec class to a name that reflects its actual
   scope, and pin the three distinct "BC" concepts in a glossary.
2. Give boundary conditions a single build lifecycle owned by the BC class, and
   remove drivers' direct reach-in to the two BC fields on `CFDSolver`.
3. Remove the steady/transient duplicates that survived the split: the momentum
   wrapper loop, the turbulence-but-nut-update sequence, and the two different
   convergence switches.
4. Fix stale or wrong state that actively misleads: dead timing counters, an
   unreset limiter counter, diagnostics that label transient runs as steady,
   stale comments and mojibake left by the refactor.
5. Every phase ends with a green build and the `fvm` and `cfd` suites, and its
   own commit.

## Non-goals

- No change to the public `SIMPLE<Dim>` API. Tests and the application construct
  `SIMPLE(mesh, rho, nu, config)` and call its BC setters; those signatures stay.
- No rewrite of `AMG`, `SSTModel`'s numerical internals, or the linear solver
  dispatch. They are large, tested, and not the source of the coupling being
  addressed.
- No migration of the `cfd::CaseSettings` / `cfd::ResolvedStoppingPolicy` config
  types into `fvm`. `fvm` headers currently include `cfd/` types
  (`CFDSolver.h:3-4`, `AMG.h:3`); changing that is a config-wide project with a
  much larger blast radius. It is recorded as known debt, not fixed here.
- No removal of all `friend` declarations. Those cover ~90 `solver_.` accesses
  in `TransientDriver.cpp`; removing them all is a separate, larger phase.
  This work removes the BC-specific reach-in and makes the remaining coupling
  explicit in the ledger.

## Anti-goals

- A "clean architecture" pass that reshapes everything at once and leaves the
  module untestable for a week.
- Renaming for its own sake. Every rename here removes a specific wrong
  assumption about scope (SIMPLE-only BCs) or stale reference (deleted headers,
  absorbed methods).

## Constraints

- Explicit template instantiations for Dim 2 and 3 live in the `.cpp` files and
  are declared with `extern template` in headers. Renaming files must keep the
  include path in `CMakeLists.txt`, `CFDSolver.h`, and `PressureCorrection.h` in
  sync, or linking fails.
- `tests/support/fvm_fixtures.h` computes divergence through production
  `fvm::computeResiduals`; changes to residual/continuity code are observable in
  tests and must be checked.
- `known_failures.txt` registries mark some tests `WILL_FAIL`; a green suite can
  include those, so test counts must be compared, not just exit status.
- Verification commands used in this repo: `cmake --build build-tests -j8`,
  `ctest --test-dir build-tests -L fvm`, `ctest --test-dir build-tests -L cfd`.

## Terminology (canonical)

- **BoundaryConditions** — the user-facing set of conditions and patches, plus
  the build step that turns them into solver data. Replaces the misleading
  `SIMPLEBoundaryConditions`.
- **BCDiscretization** — the compiled view: per-cell masks/values and per-face
  typed BCs consumed by momentum, pressure correction and residuals. It is not
  a second BC API; it is the output of `BoundaryConditions::build()`.
- **PatchBC** — the UI layer's input record (`cfd/cfd_types.h`), not an FVM type.

This goes into a new root `GLOSSARY.md` in phase 1. No implementation detail in
that file.

## Approaches considered

- **C — rename only.** 5 files, almost no risk, fixes the immediate confusion
  and nothing else. Rejected as insufficient for the stated goal.
- **B — rename plus extract BC ownership out of `CFDSolver`.** Would require an
  injected `BoundaryConditions` object and changes to every `SIMPLE`
  construction site. `CFDSolver` is the shared context by design: momentum,
  pressure correction and turbulence all consume the same compiled BC data.
  Moving it out buys little and breaks the public constructor path. Rejected.
- **A — staged module health program (chosen).** Rename first, then narrow the
  BC seam, then remove the specific duplications, then the stale state. Each
  phase is independently testable and revertible. Chosen because it fixes root
  causes while keeping the module working after every commit, and because the
  user asked for a healthy module, not a one-off rename.

## Design

### Phase 1 — rename and name things

- `git mv src/fvm/SIMPLEBoundaryConditions.{h,cpp}` to
  `src/fvm/BoundaryConditions.{h,cpp}`; rename the class to
  `BoundaryConditions<Dim>`.
- Update the three include sites (`CFDSolver.h:11`, `PressureCorrection.h:8`,
  the implementation's own include), the `CMakeLists.txt:245` source entry, and
  the member declaration `CFDSolver.h:285`.
- Add root `GLOSSARY.md` with the three terms above.
- No behavior change. This is the sanity check that the rename is complete:
  build + both suites.

### Phase 2 — one BC build lifecycle

- Move the dirty check into the BC class: `const BCDiscretization& discretization()`
  rebuilds when `isDirty()` and returns the compiled view. Delete
  `ensureBuilt()` if still unused after the call sites move.
- `CFDSolver` keeps ownership of the BC object (it is shared solver state) but
  exposes a narrow accessor used by the drivers. `TransientDriver` stops
  reading `solver_.bc_` / `solver_.bcDiscretization_` directly
  (`TransientDriver.cpp:67,200`, plus any use inside the momentum/pressure
  helpers) and calls the accessor.
- `CFDSolver::initializeSolve_` (`CFDSolver.cpp:166`) uses the same lifecycle.
- Remove `SIMPLE`'s eight forwarding setters only if they can be replaced by
  one `boundaryConditions()` accessor without breaking the public API; if not,
  keep them and document why. **Builder judgment call.**
- Test: add a unit test that mutating a BC after a build causes the next
  `discretization()` call to see the new value, and that an unchanged spec
  keeps the same built object. This is new non-trivial logic, so it gets a
  test before the implementation.

### Phase 3 — remove the surviving duplicates

- Delete the `solveMomentum_` wrapper in `CFDSolver.cpp:30-43`; both callers use
  `solveMomentumComponent_` with `TransientParams` (the transient driver already
  has `momentumTp_()` at `TransientDriver.h:52`). Fix the stale comment at
  `TransientDriver.h:48-49`.
- Extract one turbulence/nut update helper used by the steady loop
  (`CFDSolver.cpp:536-547`) and both transient sites
  (`TransientDriver.cpp:204-230,510-528`). The helper takes explicit arguments
  (`dt`, `isFirstStep`, `transient`) instead of reading driver state.
- Unify the convergence switch: the transient path's inline gate
  (`TransientDriver.cpp:535-569`) uses `cfd::ConvergenceMonitor` with a policy
  whose hold streak is 1, so `ConvergenceMonitor::update()` at
  `convergence_policy.cpp:78-115` becomes the single implementation.
- Each extraction is covered by existing steady and transient channel tests;
  add a focused test only where existing coverage cannot distinguish the two
  semantics (the hold-streak behavior is the likely case).

### Phase 4 — stale state and diagnostics

- Reset `pPrimeLimiterHits_` in `initializeSolve_`; keep one warning site.
- Delete `steadyMomentumSolveSeconds_` and `steadyVelocityCorrectionSeconds_`
  and their log fields (`CFDSolver.h:362,365`). They are never accumulated, so
  accumulation would mean adding new timers and deciding what "steady" means
  for a shared path; deletion removes a permanently-zero diagnostic instead.
  **Builder call** — no new timing state. Remove `pressureN_` if it stays
  write-only (`TransientDriver.h:68`).
- Make `printFinalStats_()` suppress steady-only lines for transient runs: it
  currently reports the steady tolerance and "steady SIMPLE total"
  (`CFDSolver.cpp:615-688`, called at `TransientDriver.cpp:626`). No new
  transient timers or fields are introduced; a missing diagnostic is better
  than a wrong one. **Builder call** — no new per-run state.
- Fix stale comments and mojibake (`SIMPLE.cpp:100`, `CFDSolver.cpp:486`,
  `TransientDriver.cpp:354`), and the false config-bridge comment.
- These are observability fixes; each gets a focused assertion on the relevant
  status/counter if a test can observe it without scraping log text.

## What is left to the builder

- Exact helper names and signatures inside each phase.
- Exact test names and which existing tests are sufficient for each extraction.
- Commit granularity within a phase (one commit per phase is the default).
- Order of phases is fixed: 1 -> 2 -> 3 -> 4. Do not reorder; each phase assumes
  the previous one is committed and green.
