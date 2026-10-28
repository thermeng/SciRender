#include "cfd/cfd_mesh_adapter.h"
#include "mesh/Mesh.h"
#include "support/test_support.h"
#include <doctest/doctest.h>
#include <iostream>

TEST_CASE("flatplate debug") {
    test_support::requireSample("cfd/FlatPlate.msh");
    auto res = cfd::loadMesh(test_support::samplePath("cfd/FlatPlate.msh").string());
    std::cout << "ERROR: " << res.error << "\n";
    std::cout << "DIM: " << res.dim << "\n";
    if (res.mesh2) {
        std::cout << "CELLS: " << res.mesh2->nCells() << " VERTS: " << res.mesh2->vertices().size() << "\n";
        std::cout << "PATCHES: " << res.patches.size() << "\n";
        for (auto&p: res.patches) std::cout << "  patch: " << p.name << " faces=" << p.faceCount << "\n";
        // min cell volume
        double minV=1e18, maxV=0;
        for (std::size_t i=0;i<res.mesh2->nCells();++i){ double v=res.mesh2->cellVolume(i); minV=std::min(minV,v); maxV=std::max(maxV,v);}
        std::cout << "CELLVOL min/max: " << minV << " " << maxV << "\n";
    }
    REQUIRE(res.error.empty());
}
