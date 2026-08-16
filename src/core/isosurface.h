#pragma once

#include <vector>
#include <string>

#include "core/mesh_loader.h"

namespace isosurface {

// Extract isosurface mesh(es) from a structured-grid scalar field using
// marching cubes (Lorensen & Catterall, 1987).
//
// The source mesh must be a structured grid: gridDimX/Y/Z > 1 and the active
// per-node scalar field (`mesh.scalars`, or the named field resolved against
// `mesh.attributes->pointScalars`) laid out in IJK order
//   idx(x,y,z) = x + y*dX + z*dX*dY
// with node positions in `mesh.vertices` in the same order and a bounding box
// in `mesh.bounds`. This is exactly what the VTK XML/legacy structured-grid and
// image-data parsers produce (see vtk_xml_parser.cpp BUILD_TOPOLOGY section).
//
// Parameters:
//   volumeMesh  - the parsed structured-grid RenderMesh (read-only).
//   isovalues   - one or more scalar thresholds. Each contributes a closed
//                 surface; vertices from different contours are NOT merged.
//   field       - name of the point-scalar field to contour. Pass "" to use
//                 the mesh's active `scalars` array.
//
// Result: a RenderMesh carrying vertices, triangle indices, smooth normals
// (via mesh_utils::computeNormals), and a per-vertex `scalars` array equal to
// the isovalue each vertex belongs to. This drops straight into the existing
// MeshGLManager -> MeshPass pipeline: it is shaded by the colormap LUT when
// meshUseScalarColor is on, lit by the PBR lighting model, and participates in
// depth-peel transparency, wireframes, LOD, and screenshots -- with NO new GL
// code. Pass an empty isovalues vector to get an empty (no-op) mesh.

RenderMesh extractIsosurface(const RenderMesh& volumeMesh,
                             const std::vector<float>& isovalues,
                             const std::string& field = "");

// True when the mesh carries the structured-grid data the extractor needs.
// Delegates to RenderMesh::hasVolumeGrid() (gridDim > 1 per axis) and
// RenderMesh::hasScalarData() (mesh.scalars or attributes->pointScalars),
// additionally requiring non-empty vertices for interpolation.
bool canExtract(const RenderMesh& volumeMesh);

} // namespace isosurface
