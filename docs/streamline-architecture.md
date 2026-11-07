# Streamline Architecture

Reimplemented pipeline (plan: `C:\Users\Neel\.opencode\plan\streamline-reimplementation-plan.md`).

```
RenderSettings (validated setters, enum direction/seed-mode, deterministic rngSeed)
        │ IntegrationParameters
        ▼
FieldDomain (owned, validated: finite, coherent structured grid, int64 products)
        │  makeSampler()
        ▼
IFieldSampler ── TrilinearSampler (structured) / NearestSampler (UniformGrid, cellSize ≥ cutoff)
        │
IntegrationService::integrateAll ── RK4Integrator (IIntegrator strategy), budget-checked
        │  std::shared_ptr<const StreamlineData> (immutable artifact)
        ▼
GeometryBuilder ── Tube / Ribbon / Lines / Arrowheads (parallel-transport frames)
        │  GeometryOutput (interleaved 10-float vertices, GL_STATIC_DRAW)
        ▼
StreamlineSet::uploadGL (GL thread) ── StreamlinePass / SeedPass / ParticleSystem
```

## Threading

- Worker thread calls only `StreamlineSet::computeStatic` (no instance state) inside
  `try/catch`; it never touches GL/Qt/live render objects.
- Render thread uploads via `consumeResult` → `uploadGL`, then `initParticles`.
- Cancellation is an atomic token checked per RK4 step.

## Budgets (enforced before allocation)

| Budget | Value | Where |
|---|---|---|
| maxSeeds | 50 000 | `validateParameters` + `Budget` |
| maxStepsPerPath | 5 000 (and `maxSteps` slider) | `effectiveMaxSteps` = min of both caps |
| maxTotalPoints | 4 000 000 | `integrateAll` |
| maxVertices (geometry) | 4 000 000 | `GeometryConfig::maxVertices` |

## Performance targets

| Scenario | Target |
|---|---|
| Fixture 5³, 27 seeds | < 5 ms (measured 3.7 ms) |
| 32³, 1k seeds, tube style | ~3 s (measured; dominated by tube vertex generation, 1.75M verts) — use line style for interactive budgets |
| 100³, 10k seeds, 1k steps | Run `scirender_tool_bench_streamline` for current numbers; tube vertex count is the ceiling, not integration |
| Particle update, 2k particles | < 0.5 ms/frame |
| Idle | 0 allocations, 0 UBO uploads |

## Extension points

- New integrator: implement `IIntegrator`, register in `integrateAll`.
- New seeder: extend `SeedMode` + `generateSeeds`.
- GPU compute: produce the same `StreamlineData` artifact from a compute shader;
  the queue/geometry/particle layers are unchanged.
- Pathtlines/dashes: use the stored per-point `arcLength` attribute in a shader variant.

## Diagnostics

Empty results carry `Diagnostics::Reason` (`InvalidDomain`, `ZeroField`,
`AllSeedsOutside`, `BudgetExceeded`, `Cancelled`, `NoPaths`) surfaced via
`StreamlineData::diagnostics` and the sidebar summary.
