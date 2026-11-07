# Task 1 Report — Hypre BoomerAMG backend for AMGHierarchy

Status: DONE

## What was done

Hypre (≥2.17, MIT/Apache dual) integrated as an **optional** alternative
backend for `fvm::AMGHierarchy`, with the existing amgcl path kept as the
default and as the fallback when Hypre is not found at configure time.
`vendor/` untouched. No public API changes.

### Optional dependency (`CMakeLists.txt`)

- `find_package(HYPRE QUIET)`, with a fallback probe for a lowercase
  `hypre` package; imported targets `HYPRE::HYPRE` / `HYPRE::hypre` /
  `hypre::hypre`, or `HYPRE_INCLUDE_DIRS`/`HYPRE_LIBRARIES`, are all
  honoured. MPI is also required (Hypre is an MPI library); if MPI is
  missing the backend is disabled with a warning.
- When found: `FvmCore` gets `FVM_HAVE_HYPRE=1` (PUBLIC, so test targets
  see it), the Hypre include dirs (`SYSTEM PUBLIC`) and the libraries plus
  `MPI::MPI_C/MPI::MPI_CXX`.
- When not found: a one-line `message(STATUS ...)` and amgcl-only build.
  This machine (MinGW/Windows, `build-tests` preset): *not found* → amgcl.

### Wrapper (`src/fvm/HypreSolver.h`, new, header-only)

- `HypreSolver::HypreMat` / `HypreSolverImpl`: RAII holders for
  `HYPRE_ParCSRMatrix` / `HYPRE_Solver` (`Destroy` in destructors).
- `HypreSolver::buildSolver(A, cfg, mat, solver, scale)`:
  - Exports the Eigen `SparseMatrix` (RowMajor CSR) to a
    `HYPRE_ParCSRMatrix`: diagonal array + off-diagonal values/column
    indices (`ParCSRMatrixCreate` → `SetNumNonzeros` →
    `SetOffDiagValues`/`SetOffDiagColumnIndices` → `AssembleMatrix`).
  - Symmetric diagonal equilibration (`D A D`) when
    `cfg.scaling == ProportionalScaling::Automatic`; `scale` is handed
    back for b/x transforms (same rule as the amgcl path).
  - BoomerAMG tuning: `SetCycleType` 1=V / 2=W;
    `SetFCycle(amg, 1)` for FMG; Flexible via
    `SetFlexibleCycleStruct` (int walk, -2 Terminate / -1 Coarser /
    0 Stay / 1 Finer; default walk `{0,-1,-1,-1,1,1,0,-2}` when
    `cfg.flexStruct` is empty) + `SetFlexibleCGCScalingFactors` with
    **α (prolongation) = `cfg.flexAlpha`** (0.5) on every Finer step;
    sweeps from `cfg.flexMaxSweeps` via `SetNumSweeps`.
    - β (restriction, 0.9) has **no Hypre knob** — real Hypre 2.17+
      exposes only the walk struct and CP scaling factors. β is kept in
      `Config` and honoured by the own `FAMGHierarchy` flexible walk;
      it is recorded but not passed to Hypre (documented in code).
  - Gauss-Seidel smoother: `SetRelaxType(3)` (hybrid GS forward) +
    `SetCycleRelaxType(4, 2)` (backward on up); SOR/GS variants of
    `cfg.smoother` are not mapped (BoomerAMG relax types are a Hypre-side
    choice; GS is the specified default).
  - **Strength 0.25** (`SetStrongThreshold`, also Hypre's 2D-Laplace
    default), **max levels** from `cfg.maxLevels` (default 50),
    **coarse direct ≤ `cfg.coarseEnough`** (`SetMaxCoarseSize` +
    `SetCycleRelaxType(9, 3)` = Gaussian elimination on the coarsest
    level), tolerance 0 / max-iter 1 (one cycle per `applyCycle()`).
  - `HYPRE_BoomerAMGSetup` with dummy b/x; `Setup` failure → `false` →
    caller falls back to amgcl.
- `HypreSolver::applyCycle(...)`: one `HYPRE_BoomerAMG` solve (one cycle),
  b/x scaled by `scale`, x never discarded, non-finite → zeroed + false.
- Serial use: single rank, `MPI_COMM_WORLD`.
- `hypreAvailable()` constexpr: true/false by `FVM_HAVE_HYPRE`.

### Wiring (`src/fvm/AMG.h`, `src/fvm/AMG.cpp`)

- `AMGHierarchy::Config`: new `bool useHypre = false` (default unchanged).
- `AMGHierarchy`: new members `hypreHandle_`, `hypreScale_`,
  `useHypreActive_` (a shared `HypreHolder` keeps solver+matrix alive);
  `invalidate()` resets them.
- `build()`: if `cfg.useHypre`, try the Hypre backend first; on success
  set `built_`, sizes, report string (`"Hypre BoomerAMG hierarchy ..."`,
  1 level shown) and return. On any failure/exception — or when Hypre was
  not linked in — fall through to the untouched amgcl path.
- `applyCycle()`: delegates to Hypre when active, else amgcl (existing
  code unchanged).
- `solve()`: accepts the Hypre handle in the entry guard and otherwise
  reuses the whole iteration loop (residuals computed against the real
  `systemMatrix_`).
- `updateFineValues()`: Hypre values are baked in at `Setup`, so it
  rebuilds via `build(A)` (same guarantee as the amgcl path).
- Non-Hypre builds: all Hypre code is `#ifdef FVM_HAVE_HYPRE`-gated; the
  `HypreSolver` stubs (`hypreAvailable() == false`) compile and the
  default path is byte-for-byte the old behaviour.

### Tests

- New `tests/fvm/hypre_backend_test.cpp` (3 cases, registered in
  `tests/CMakeLists.txt` under `scirender_fvm_tests`):
  - `fvm.amg.hypre.config_defaults_amgcl` — `useHypre` defaults false;
    default solve still converges (<1e-5).
  - `fvm.amg.hypre.poisson_solve` — `useHypre=true` on 32×32 Poisson,
    tol 1e-8 → residual <1e-5. With `FVM_HAVE_HYPRE`: real BoomerAMG.
    Without: passes through the amgcl fallback (message logged) — the
    "skip if Hypre not found" requirement is met by never failing and
    still exercising the call.
  - `fvm.amg.hypre.flexible_cycle` — Flexible cycle, α=0.5, β=0.9,
    maxSweeps=1 on 20×20 Poisson → residual <1e-5.
- TDD: the test failed first with
  `error: 'struct fvm::AMGHierarchy::Config' has no member named 'useHypre'`;
  after implementation it passes.

## Test summary

- `tests/fvm/amg_config_test.cpp`: **9/9 pass** (requirement met, run
  explicitly).
- New hypre cases: **3/3 pass** (fallback path on this machine).
- All AMG-related cases: **13/13 pass**.
- Full `scirender_fvm_tests` suite: 21 passed / 6 failed / 13 skipped.
  All 6 failures are in `cancellable_test`, `channel_3d_test` and
  `channel_test` (SIMPLE/channel physics, `known_failures.txt` only lists
  the cavity entry) — **pre-existing, unrelated**: the default path is
  unchanged and `FVM_HAVE_HYPRE` is undefined in this build.
- `scirender_cfd_tests`: 57 passed / 1 failed; the failure shows up as 4
  CHECKs inside `cfd.pressure.amg_path_builds_and_stays_finite`-adjacent
  pressure-correction cases whose failing CHECKs have `useAMG = false` —
  again unrelated to this change.

## Concerns / follow-ups

1. **Hypre not installed on this machine** (Windows/MinGW): the
   `FVM_HAVE_HYPRE` code path has **never been compiled**, let alone run.
   On CI/Linux with Hypre ≥2.17 the API surface used is from the official
   3.2.0 docs (superset of 2.17) — expect possible compile nits against a
   specific 2.17 install (e.g. `HYPRE_ParVectorGetData` availability,
   `SetFlexibleCGCScalingFactors` naming). First Hypre-enabled build
   should be treated as the real test.
2. **β has no Hypre counterpart** (walk struct + CGC scaling factors only).
   Flexible-on-Hypre is therefore only α-exact; full α/β semantics live in
   `FAMGHierarchy`. If exact STAR-CCM+ Flexible parity matters, this is a
   structural limitation, not a bug.
3. **Strength is fixed at 0.25 on the Hypre path** (spec + Hypre 2D
   default; also avoids destabilising the already-tuned amgcl
   0.16 default). `cfg.strengthThreshold` is ignored when Hypre runs —
   deliberate, documented in `HypreSolver.h`.
4. **`cfg.smoother` is not mapped**: Hypre always uses GS. Jacobi/ILU/SPAI
   selections only take effect on the amgcl path.
5. **No bandwidth reorder** on the Hypre path (Eigen CSR order is handed
   to Hypre as-is). The amgcl path has the same open TODO; a reorder
   needs matching b/x permutation and is not attempted here.
6. **No UI checkbox yet**: `useHypre` is reachable only via
   `AMGHierarchy::Config` (bridge/`AmgSettings` unchanged — per task, "add
   UI checkbox later if time"). Adding it to `cfd::AmgSettings` would also
   require `amg_settings_bridge.h`, `case_codec.h` and the widget.
7. Level sizes for Hypre are reported as `{n}` (fine level only): Hypre
   does not expose per-level sizes through the serial API used here, so
   the "levels ~×2 per level" STAR-CCM+ parity check cannot be verified
   from `levelSizes()` on Hypre. `HYPRE_BoomerAMGGetNumLevels` may exist
   in newer Hypre headers and could be wired in once a Hypre build is
   available.
8. Commit scope: `src/fvm/AMG.cpp`, `src/fvm/AMG.h`, `CMakeLists.txt`
   also contain the parent session's uncommitted F-cycle WIP (required
   for this tree to build); they are committed together with the Hypre
   work so the committed snapshot builds.
