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
    if (ext == "VTM") {
        return parseMultiBlockXML(filePath);
    }
    if (ext == "PVD") {
        // A .pvd is a time-series index, not a single dataset — it must go
        // through AnimationController (RenderSettings routes it there).
        throw std::runtime_error("'.pvd' is a time-series collection; load it via the animation pipeline (File > Open Mesh)");
    }
    throw std::runtime_error("unsupported file extension '" + ext + "' (expected .stl, .vtk, .obj, .vtu, .vts, .vti, .vtp, .vtr, .vtm)");
}
