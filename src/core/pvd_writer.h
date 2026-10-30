#pragma once

// Shared PVD (ParaView Data Collection) writer. Qt-free so both the FVM
// solver (FvmCore) and the Qt controller (CfdLib) emit identical .pvd index
// files from a single implementation.

#include <fstream>
#include <string>
#include <vector>

namespace core {

// One timestep entry in a .pvd collection.
struct PvdFrame {
    std::string time;  // timestep value as written on disk
    std::string file;  // dataset file path
};

// Write a .pvd collection from any range of objects exposing `.time` and
// `.file` (e.g. PvdFrame, or the controller's local PvdEntry).
template<typename Range>
void writePvdCollection(const std::string& path, const Range& frames) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    out << "<?xml version=\"1.0\"?>\n";
    out << "<VTKFile type=\"Collection\" version=\"0.1\">\n";
    out << "  <Collection>\n";
    for (const auto& f : frames) {
        out << "    <DataSet timestep=\"" << f.time << "\" file=\"" << f.file << "\"/>\n";
    }
    out << "  </Collection>\n";
    out << "</VTKFile>\n";
}

} // namespace core
