#pragma once

// CGNS mesh import for the CFD solver (and, through cfd::loadMesh, rendering).
//
// Reads the first unstructured zone of the first base: GridCoordinates,
// linear Elements_t sections (TETRA_4 / PYRA_5 / PENTA_6 / HEXA_8 in 3D,
// TRI_3 / QUAD_4 in 2D, including MIXED type-tagged streams) and ZoneBC_t/BC_t
// point lists, which are matched to the mesh's boundary faces by their vertex
// sets. CGNS node ordering for the
// supported element types matches VTK order, so cells land in the canonical
// layout the rest of the pipeline expects; wedge/pyramid face loops are
// supplied explicitly from cell_topology::faces.
//
// Limitations (diagnosed, not silently skipped): structured zones, polyhedral
// NGon/NFace sections, higher-order elements and PointRange BC sets are
// rejected; only the first unstructured zone is read.
//
// Compiled only when SCIRENDER_HAVE_CGNS is defined (root CMakeLists finds
// the vendored CGNS built by tools/build_cgns_deps.py). The interface itself
// has no CGNS types, so callers can include this header unconditionally and
// guard the calls.

#include "mesh/Mesh.h"
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace fvm {

template<std::size_t Dim>
class CGNSReader {
public:
    struct Result {
        Mesh<Dim> mesh;
        std::map<std::string, std::vector<std::size_t>> boundaryGroups;
    };

    static Result read(const std::string& filename);
    // Scans the element section headers of the first unstructured zone and
    // returns 3 when it contains volume elements, 2 otherwise. Call as
    // CGNSReader<2>::detectDimension(path) regardless of the mesh dimension.
    static int detectDimension(const std::string& filename);
};

extern template class CGNSReader<2>;
extern template class CGNSReader<3>;

} // namespace fvm
