#pragma once
#include "core/mesh_loader.h"

// ── OBJ Parser ──────────────────────────────────────────────────────────────
// Parses Wavefront OBJ (.obj) — plain text, `v` vertices + `f` faces.
// Supports the element forms v / v/vt / v/vt/vn / v//vn (only the vertex index
// is used). Faces are fan-triangulated (convex assumption — the OBJ norm).
// ponytail: vt/vn textures & normals are intentionally ignored — no consumer
// in RenderMesh yet. Add a normals[] import when shading-from-OBJ is wanted.
RenderMesh parseOBJ(const std::string& filePath);
