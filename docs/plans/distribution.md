# Distribution — zero-build for non-engineers (Windows v1)

Status: implemented (no file association)

## Goal
User on clean Windows 10/11 (no Qt, no compiler) gets working app via one download, no terminal.

## Artifacts produced by CI
- `SciRender-win64.zip` — portable, no install, no admin: `SciRender.exe + Qt6*.dll + platforms/qwindows.dll + styles/ + translations/ + libgcc/libstdc++/libwinpthread + shaders/ + README.md`
- `SciRender-Setup.exe` — Inno Setup modern wizard, installs to `{autopf}\SciRender`, Start Menu + optional desktop icon, uninstall.

Both from same `windeployqt` stage; no file association (`[Registry]` omitted per spec).

## Implementation
- `CMakeLists.txt:8` — `install(TARGETS SciRender DESTINATION .)` + `install(DIRECTORY src/shaders DESTINATION shaders)` + `install(FILES README.md)`. `cmake --install build-mingw --prefix package` stages.
- `installer/SciRender.iss` — 45 lines, `Compression=lzma2`, `Source: package\*`, `iscc /DAppVersion=%TAG%` (defaults `0.1.0`), no registry.
- `.github/workflows/release.yml` — `windows-latest`, `jurplel/install-qt-action@v4` Qt 6.11.1 mingw, `cmake -G "MinGW Makefiles"`, `cmake --build`, `cmake --install`, `windeployqt --release --compiler-runtime`, `choco install innosetup`, `iscc`, `Compress-Archive`, `softprops/action-gh-release` uploads both to Release on `push tag v*` or manual dispatch.
- `README.md` — new `Download (no build needed)` section above `Build`, with local reproduction commands.

## Verification
- Local: `cmake -G "MinGW Makefiles" -S . -B build-mingw -DCMAKE_BUILD_TYPE=Release && cmake --build build-mingw -j4 && cmake --install build-mingw --prefix package && windeployqt --release --compiler-runtime package/SciRender.exe` → `package/` runnable without Qt in PATH → ZIP extract on second machine → double-click works.
- CI: tag `v0.1.0` → Release has two assets; fresh VM test ZIP + Setup.

## Deferred
- Signing (SmartScreen Unknown publisher accepted v1), file association (user asked no), macOS/Linux, winget/Store, auto-update.
