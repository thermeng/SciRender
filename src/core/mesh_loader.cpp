#include "core/mesh_loader.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

// ── Extension-based dispatcher ──────────────────────────────────────────────

RenderMesh loadMeshFile(const std::string& filePath) {
    // Extract extension
    auto dotPos = filePath.rfind('.');
    if (dotPos == std::string::npos) {
        throw std::runtime_error("file has no extension (expected .stl or .vtk)");
    }
    std::string ext = mesh_utils::toUpper(filePath.substr(dotPos + 1));

    if (ext == "STL") {
        return parseSTL(filePath);
    }
    if (ext == "VTK") {
        return parseVTK(filePath);
    }
    if (ext == "OBJ") {
        return parseOBJ(filePath);
    }
    if (ext == "VTU" || ext == "VTS" || ext == "VTI" || ext == "VTP" || ext == "VTR") {
        return parseVTKXML(filePath);
    }
    throw std::runtime_error("unsupported file extension '" + ext + "' (expected .stl, .vtk, .obj, .vtu, .vts, .vti, .vtp, .vtr)");
}
