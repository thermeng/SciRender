# Vendored dependency patches

`vendor/hypre` is fetched as a pristine upstream clone in CI
(`.github/workflows/release.yml`), so fixes to it are carried here as patches and
applied after the clone. Patch the local clone the same way:

```powershell
git clone --depth 1 --branch v3.2.1 https://github.com/hypre-space/hypre.git vendor/hypre
git -C vendor/hypre apply "$PWD/patches/hypre-3.2.1-getgridhierarchy-leak.patch"
```

(`-C` changes Git's working directory, so the patch path must be absolute.)

| Patch | Fix |
|---|---|
| `hypre-3.2.1-getgridhierarchy-leak.patch` | `hypre_BoomerAMGGetGridHierarchy` allocates its 2×local_size workspace through `wbuff` but frees `ibuff` (NULL in non-block mode), leaking `2 × n × sizeof(HYPRE_Int)` per call. Upstream: [hypre-space/hypre#1671](https://github.com/hypre-space/hypre/issues/1671). |

Remove the patch once a release containing the upstream fix is pinned.