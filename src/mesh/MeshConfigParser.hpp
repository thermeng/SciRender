#pragma once

#include "mesh/Mesh.h"
#include "io/GmshReader.h"
#include "io/ANSYSMeshReader.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <array>
#include <cstddef>
#include <string>

namespace fvm {

inline std::string detectMeshFormat(const std::string& meshFile, const nlohmann::json& meshCfg) {
    if (meshCfg.contains("format")) {
        std::string fmt = meshCfg.value("format", "");
        if (fmt == "ansys") return "ansys";
        if (fmt == "gmsh") return "gmsh";
    }
    std::ifstream probe(meshFile, std::ios::binary);
    if (!probe) return "gmsh";
    char buf[16] = {0};
    probe.read(buf, 15);
    std::string header(buf);
    if (header.find("(0") == 0) return "ansys";
    if (header.find("$Mesh") != std::string::npos) return "gmsh";
    return "gmsh";
}

template<std::size_t Dim>
Mesh<Dim> makeMeshFromConfig(const nlohmann::json& meshCfg) {
    if (meshCfg.contains("file")) {
        std::string meshFile = meshCfg["file"];
        std::string format = detectMeshFormat(meshFile, meshCfg);
        if (format == "ansys") {
            std::cout << "Loading ANSYS mesh from: " << meshFile << "\n";
            auto result = ANSYSMeshReader<Dim>::read(meshFile);
            Mesh<Dim> mesh = std::move(result.mesh);
            if (!result.boundaryGroups.empty()) {
                mesh.setBoundaryGroups(std::move(result.boundaryGroups));
            }
            std::cout << "Loaded mesh from: " << meshFile << " ("
                      << mesh.nCells() << " cells, "
                      << mesh.nFaces() << " faces)\n";
            const auto& bGroups = mesh.boundaryGroups();
            std::cout << "  boundary groups: " << bGroups.size() << "\n";
            for (const auto& [gname, gfaces] : bGroups) {
                std::cout << "    '" << gname << "': " << gfaces.size() << " faces\n";
            }
            return mesh;
        } else {
            auto result = GmshReader<Dim>::read(meshFile);
            Mesh<Dim> mesh = std::move(result.mesh);
            if (!result.boundaryGroups.empty()) {
                mesh.setBoundaryGroups(std::move(result.boundaryGroups));
            }
            std::cout << "Loaded mesh from: " << meshFile << " ("
                      << mesh.nCells() << " cells, "
                      << mesh.nFaces() << " faces)\n";
            const auto& bGroups = mesh.boundaryGroups();
            std::cout << "  boundary groups: " << bGroups.size() << "\n";
            for (const auto& [gname, gfaces] : bGroups) {
                std::cout << "    '" << gname << "': " << gfaces.size() << " faces\n";
            }
            return mesh;
        }
    }

    std::array<std::size_t, Dim> nCells{};
    std::array<double, Dim> lengths{};
    std::array<double, Dim> ratios{};
    bool hasStretching = false;

    nCells[0] = meshCfg.value("nx", Dim == 3 ? 16u : 50u);
    nCells[1] = meshCfg.value("ny", Dim == 3 ? 16u : 50u);
    if constexpr (Dim == 3) nCells[2] = meshCfg.value("nz", 16u);

    lengths[0] = meshCfg.value("lx", 1.0);
    lengths[1] = meshCfg.value("ly", 1.0);
    if constexpr (Dim == 3) lengths[2] = meshCfg.value("lz", 1.0);

    ratios[0] = meshCfg.value("ratio_x", 1.0);
    ratios[1] = meshCfg.value("ratio_y", 1.0);
    if constexpr (Dim == 3) ratios[2] = meshCfg.value("ratio_z", 1.0);

    for (std::size_t d = 0; d < Dim; ++d) {
        if (ratios[d] != 1.0) hasStretching = true;
    }

    if (hasStretching) {
        return Mesh<Dim>::makeStretching(nCells, lengths, ratios);
    }
    return Mesh<Dim>(nCells, lengths);
}

} // namespace fvm
