#include "core/pvd_parser.h"
#include "pugixml.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

// ── Relative path resolution (mirrors .vtm MultiBlock handling) ─────────────
// A DataSet @c file is used verbatim when absolute (drive letter or leading
// slash); otherwise it resolves against the .pvd's directory.

static bool pathIsAbsolute(const std::string& p) {
    if (p.size() >= 2 && p[1] == ':') return true;          // Windows drive
    if (!p.empty() && (p[0] == '/' || p[0] == '\\')) return true;
    if (p.rfind("\\\\", 0) == 0) return true;                // UNC share
    return false;
}

static std::string resolvePvdPath(const std::string& pvdPath, const std::string& dataSetFile) {
    if (dataSetFile.empty()) return dataSetFile;
    if (pathIsAbsolute(dataSetFile)) return dataSetFile;
    size_t lastSlash = pvdPath.find_last_of("/\\");
    if (lastSlash == std::string::npos) return dataSetFile;
    return pvdPath.substr(0, lastSlash + 1) + dataSetFile;
}

std::vector<std::string> PvdSequence::filesForFrame(int i) const {
    std::vector<std::string> files;
    if (i < 0 || i >= static_cast<int>(timesteps.size())) return files;
    const double t = timesteps[i];
    // Invariant: timesteps[] is built bit-identically from entries[].timestep
    // (pvd_parser.cpp:127-130), so exact double equality is intentional here.
    // Any future change that computes t differently (rounding, normalization) must
    // preserve bit identity or switch to an epsilon/index-based lookup.
    // Defensive assert: timesteps must be sorted unique and sourced from entries.
    for (const PvdEntry& e : entries) {
        if (e.timestep == t) {
            files.push_back(e.file);
        } else if (!files.empty()) {
            break; // sorted: we have passed the group for time t
        }
    }
    return files;
}

PvdSequence parsePVD(const std::string& filePath, PvdParseDiagnostics* outDiag) {
    PvdSequence seq;
    seq.sourcePath = filePath;
    if (outDiag) { outDiag->error.clear(); outDiag->warning.clear(); outDiag->skipped = 0; }

    // Read the whole file; strip NULs like the other XML readers do.
    std::ifstream f(filePath, std::ios::binary | std::ios::ate);
    if (!f) {
        std::string msg = "PVD Parser Error: failed to open: " + filePath;
        std::cerr << msg << std::endl;
        if (outDiag) outDiag->error = msg;
        return seq;
    }
    auto fileSize = f.tellg();
    if (fileSize <= 0) {
        std::string msg = "PVD Parser Error: empty file: " + filePath;
        std::cerr << msg << std::endl;
        if (outDiag) outDiag->error = msg;
        return seq;
    }
    std::string xmlBytes(static_cast<size_t>(fileSize), '\0');
    f.seekg(0);
    f.read(xmlBytes.data(), fileSize);
    size_t nullPos = xmlBytes.find('\0');
    if (nullPos != std::string::npos) xmlBytes.resize(nullPos);

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_buffer(xmlBytes.data(), xmlBytes.size(),
        pugi::parse_default | pugi::parse_ws_pcdata);
    if (!result) {
        std::string msg = std::string("PVD Parser Error: XML parse failed: ") + result.description();
        std::cerr << msg << std::endl;
        if (outDiag) outDiag->error = msg;
        return seq;
    }

    pugi::xml_node root = doc.child("VTKFile");
    if (!root) {
        std::string msg = "PVD Parser Error: missing <VTKFile> tag in: " + filePath;
        std::cerr << msg << std::endl;
        if (outDiag) outDiag->error = msg;
        return seq;
    }
    pugi::xml_node collection = root.child("Collection");
    if (!collection) {
        std::string msg = "PVD Parser Error: missing <Collection> tag in: " + filePath;
        std::cerr << msg << std::endl;
        if (outDiag) outDiag->error = msg;
        return seq;
    }

    int skipped = 0;
    for (pugi::xml_node node : collection.children()) {
        if (std::strcmp(node.name(), "DataSet") != 0) continue;

        PvdEntry entry;
        const char* fileAttr = node.attribute("file").value();
        if (!fileAttr || !*fileAttr) { ++skipped; continue; }
        entry.file = resolvePvdPath(filePath, fileAttr);

        const char* tsAttr = node.attribute("timestep").value();
        // Writers that omit @c timestep get time 0 — timeless datasets then
        // group together (and merge multi-part), matching .vtm semantics.
        entry.timestep = 0.0;
        if (tsAttr && *tsAttr) {
            char* endPtr = nullptr;
            double v = std::strtod(tsAttr, &endPtr);
            if (endPtr == tsAttr || !std::isfinite(v)) { ++skipped; continue; }
            entry.timestep = v;
        }

        const char* partAttr = node.attribute("part").value();
        entry.part = (partAttr && *partAttr) ? std::atoi(partAttr) : 0;

        seq.entries.push_back(entry);
    }
    if (skipped > 0) {
        std::string warn = "PVD Parser Warning: skipped " + std::to_string(skipped)
                           + " malformed <DataSet> entries in " + filePath;
        std::cerr << warn << std::endl;
        if (outDiag) { outDiag->warning = warn; outDiag->skipped = skipped; }
    } else if (outDiag) {
        outDiag->skipped = 0;
    }

    if (seq.entries.empty()) {
        std::string msg = "PVD Parser Error: no usable <DataSet> entries in " + filePath;
        std::cerr << msg << std::endl;
        if (outDiag && outDiag->error.empty()) outDiag->error = msg;
        return seq;
    }

    // Stable sort by (timestep, part): parts of one timestep stay grouped in
    // `part` order; equal-key entries preserve document order.
    std::stable_sort(seq.entries.begin(), seq.entries.end(), [](const PvdEntry& a, const PvdEntry& b) {
        if (a.timestep != b.timestep) return a.timestep < b.timestep;
        return a.part < b.part;
    });

    // Deduplicate exact (timestep, part, file) triples (hand-edited .pvd may list same file twice)
    {
        auto it = std::unique(seq.entries.begin(), seq.entries.end(),
            [](const PvdEntry& a, const PvdEntry& b){
                return a.timestep == b.timestep && a.part == b.part && a.file == b.file;
            });
        seq.entries.erase(it, seq.entries.end());
    }

    seq.timesteps.reserve(seq.entries.size());
    for (const PvdEntry& e : seq.entries) {
        if (seq.timesteps.empty() || seq.timesteps.back() != e.timestep)
            seq.timesteps.push_back(e.timestep);
    }
    return seq;
}
