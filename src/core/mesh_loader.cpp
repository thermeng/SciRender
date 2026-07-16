#include "core/mesh_loader.h"
#include <algorithm>
#include <iostream>

// ── Extension-based dispatcher ──────────────────────────────────────────────

RenderMesh loadMeshFile(const std::string& filePath) {
    // Extract extension
    auto dotPos = filePath.rfind('.');
    if (dotPos == std::string::npos) {
        std::cerr << "Mesh Loader: No file extension: " << filePath << std::endl;
        return RenderMesh{};
    }
    std::string ext = mesh_utils::toUpper(filePath.substr(dotPos + 1));

    if (ext == "STL") {
        return parseSTL(filePath);
    }
    // Default: VTK (also handles .vtk)
    return parseVTK(filePath);
}
