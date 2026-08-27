#pragma once
#include "core/mesh_loader.h"   // public RenderMesh only; no parser internals (SRP)
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

struct MeshQuality {
    int degenerateFaces = 0;
    int openEdges = 0;
    int nonManifoldEdges = 0;     // Shared by != 2 faces or invalid winding
    int nonManifoldVerts = 0;     // Disjoint triangle fans at a single vertex
    int zeroLengthEdges = 0;      // Edges collapsed to zero length post-weld
    bool watertight = false;

    std::vector<float> degenerateTriVerts;   // 18 floats / degen face
    std::vector<float> openEdgeVerts;        // 6 floats / open edge
    std::vector<float> nonManifoldEdgeVerts; // 6 floats / non-manifold edge
};

MeshQuality analyzeMeshQuality(const RenderMesh& mesh);