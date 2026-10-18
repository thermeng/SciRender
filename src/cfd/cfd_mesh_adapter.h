#pragma once
#include <memory>
#include <string>
#include <vector>
#include "core/mesh_loader.h"

namespace fvm {
template<std::size_t Dim> class Mesh;
}

namespace cfd {

struct PatchInfo { std::string name; std::size_t faceCount = 0; };

struct MeshLoadResult {
    int dim = 2;
    std::shared_ptr<const fvm::Mesh<2>> mesh2;
    std::shared_ptr<const fvm::Mesh<3>> mesh3;
    std::vector<PatchInfo> patches;
    std::shared_ptr<const RenderMesh> renderMesh;
    std::string error;
};

MeshLoadResult loadMesh(const std::string& path);

} // namespace cfd