#pragma once

#include <string>
#include <vector>

// ── PVD (ParaView Data) Collection ──────────────────────────────────────────
// A .pvd file is an XML index over a time series of dataset files:
//
//   <VTKFile type="Collection" version="0.1">
//     <Collection>
//       <DataSet timestep="0"   part="0" file="sol-0000.vtu"/>
//       <DataSet timestep="0.5" part="0" file="sol-0005.vtu"/>
//     </Collection>
//   </VTKFile>
//
// One timestep may be split across several `part` entries (files); parts of
// the same timestep are merged into one mesh by the loader. Relative `file`
// paths resolve against the .pvd's directory (same rule as .vtm MultiBlock).

struct PvdEntry {
    double timestep = 0.0;
    int part = 0;
    std::string file;      // resolved path (absolute, ready to open)
};

struct PvdSequence {
    std::vector<PvdEntry> entries;   // sorted by (timestep, part)
    std::vector<double> timesteps;   // unique, ascending — bit-identical to entries[].timestep
    std::string sourcePath;          // the .pvd itself

    int frameCount() const { return static_cast<int>(timesteps.size()); }
    double frameTime(int i) const {
        return (i >= 0 && i < static_cast<int>(timesteps.size())) ? timesteps[i] : 0.0;
    }
    // Files (parts) belonging to unique-timestep index i, in `part` order.
    // Uses exact double equality against timesteps[i]; safe because timesteps
    // is built bit-identically from entries[].timestep (no rounding).
    std::vector<std::string> filesForFrame(int i) const;
};

// Diagnostics for UI-visible error reporting (replaces invisible std::cerr).
struct PvdParseDiagnostics {
    std::string error;   // user-facing error for empty sequence (XML fail, missing tags, etc.)
    std::string warning; // non-fatal warning (e.g., skipped malformed entries)
    int skipped = 0;
};

// Parses a .pvd collection. Returns an empty sequence (frameCount() == 0) on
// failure; malformed <DataSet> entries are individually skipped so one bad
// line cannot discard an otherwise usable animation.
// If outDiag is non-null, it is filled with user-facing diagnostics (error/warning)
// instead of only writing to std::cerr — callers that show a status bar should pass it.
PvdSequence parsePVD(const std::string& filePath, PvdParseDiagnostics* outDiag = nullptr);
