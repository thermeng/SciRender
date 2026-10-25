#pragma once
#include <cstddef>
#include <vector>

namespace cfd {

// Turbulence fields carried alongside a solution snapshot.
//
// The values are stored by value (not as a pointer to a live solver) because
// snapshots are published to worker threads and written to disk: a frame must
// never observe the solver overwriting k/omega while it is being rendered.
struct TurbulenceState {
    bool valid = false;
    std::vector<double> k;
    std::vector<double> omega;
    std::vector<double> nut;

    // BDF2 history. Restoring a transient run without it silently falls back
    // to backward Euler for the first step.
    bool hasHistory = false;
    std::vector<double> kNM1;
    std::vector<double> omegaNM1;

    bool matches(std::size_t cells) const {
        return valid && k.size() == cells && omega.size() == cells && nut.size() == cells;
    }

    bool historyMatches(std::size_t cells) const {
        return hasHistory && kNM1.size() == cells && omegaNM1.size() == cells;
    }

    void clear() { *this = TurbulenceState{}; }
};

// A complete, dimension-agnostic solution state.
//
// This is the one type the controller, the save/load codec, the frame bus and
// the exporter agree on. It exists so none of them has to know whether the
// case is 2D or 3D: `dim` selects how many velocity components are present,
// and every other block is a flat cell array.
struct SolutionSnapshot {
    int dim = 0;             // 0 = empty snapshot
    std::size_t cells = 0;

    // Velocity has exactly `dim` components, each `cells` long. Keeping rows
    // (rather than an interleaved array) matches Field/VectorField and the
    // on-disk layout.
    std::vector<std::vector<double>> velocity;
    std::vector<double> pressure;

    TurbulenceState turbulence;

    // Transient clock and BDF2 history. Absent for a steady run.
    bool transientValid = false;
    double time = 0.0;
    double dt = 0.0;
    double dtPrev = 0.0;
    int step = 0;
    bool hasVelocityHistory = false;
    std::vector<std::vector<double>> velocityNM1;

    bool hasFields() const {
        if (dim <= 0 || cells == 0) return false;
        if (velocity.size() != static_cast<std::size_t>(dim)) return false;
        for (const auto& component : velocity) {
            if (component.size() != cells) return false;
        }
        return pressure.size() == cells;
    }

    bool hasTurbulence() const {
        return hasFields() && turbulence.matches(cells);
    }

    bool hasTurbulenceHistory() const {
        return transientValid && turbulence.valid &&
               turbulence.historyMatches(cells);
    }

    bool hasVelocityHistorySized() const {
        if (!transientValid || !hasVelocityHistory) return false;
        if (velocityNM1.size() != static_cast<std::size_t>(dim)) return false;
        for (const auto& component : velocityNM1) {
            if (component.size() != cells) return false;
        }
        return true;
    }

    void clear() { *this = SolutionSnapshot{}; }
};

} // namespace cfd
