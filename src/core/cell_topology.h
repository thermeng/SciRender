#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Cell topology shared by the ANSYS reader, the CFD adapter, the derived
// volume grid and isosurface extraction.
//
// The renderer keeps a flat, canonical VTK corner order per cell. ANSYS writes
// the vertices of a 3D cell in arbitrary order (a union of its face loops), so
// the reader canonicalizes them here; everything downstream (VTU export,
// tetrahedralization, voxelization) can then rely on the VTK layout.
namespace cell_topology {

enum : std::uint8_t {
    kTetra = 10,
    kHexahedron = 12,
    kWedge = 13,
    kPyramid = 14,
    kPolyhedron = 42, // unsupported / unrecognized
};

// VTK cell type inferred from a cell's face loops (face sizes only).
std::uint8_t classify(const std::vector<std::vector<std::size_t>>& faceLoops);

// Same classification from pre-counted face shapes (avoids building loops).
std::uint8_t classifyShape(int faceCount, int triCount, int quadCount);

bool isVolumeCell(std::uint8_t type);
int cornerCount(std::uint8_t type);

// Canonical local faces of a supported type, as corner slots. Empty otherwise.
const std::vector<std::vector<int>>& faces(std::uint8_t type);

// Reorder `verts` (global point ids, arbitrary order) into the canonical VTK
// corner order using the cell's face loops and point coordinates.
//
// Returns the detected cell type, or kPolyhedron when the element is
// unsupported or its face loops do not match the expected layout. On failure
// `verts` is left untouched.
std::uint8_t canonicalize(std::vector<std::size_t>& verts,
                          const std::vector<std::vector<std::size_t>>& faceLoops,
                          const std::vector<std::array<double, 3>>& points);

// Append tetrahedra as corner slots. Slot `cornerCount(type)` is the cell
// centroid. Tetrahedra use a single tet and no centroid.
//
// Every face is rotated to start at its smallest *global* corner id and
// fan-triangulated, so two cells sharing a face split it identically. That is
// what keeps an extracted isosurface watertight across cell boundaries.
void tets(std::uint8_t type, std::span<const std::uint32_t> cellVerts,
          std::vector<std::array<std::uint32_t, 4>>& out);

} // namespace cell_topology
