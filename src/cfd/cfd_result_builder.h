#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "cfd/cfd_types.h"
#include "cfd/solution_state.h"
#include "core/mesh_loader.h"

namespace fvm {
template<std::size_t Dim> class Mesh;
}

namespace cfd {

// Builds the renderable result mesh from an immutable snapshot. Template on
// the dimension so the 2D case keeps its z = 0 plane and the 3D case exposes
// the real z component of the velocity field.
template<std::size_t Dim>
std::shared_ptr<const RenderMesh> buildResultMesh(
    const std::shared_ptr<const RenderMesh>& baseMesh,
    const fvm::Mesh<Dim>& mesh,
    const SolutionSnapshot& state,
    const CaseSettings& settings,
    const std::vector<PatchBC>& patchBCs = {});

extern template std::shared_ptr<const RenderMesh> buildResultMesh<2>(
    const std::shared_ptr<const RenderMesh>&,
    const fvm::Mesh<2>&,
    const SolutionSnapshot&,
    const CaseSettings&,
    const std::vector<PatchBC>&);

extern template std::shared_ptr<const RenderMesh> buildResultMesh<3>(
    const std::shared_ptr<const RenderMesh>&,
    const fvm::Mesh<3>&,
    const SolutionSnapshot&,
    const CaseSettings&,
    const std::vector<PatchBC>&);

} // namespace cfd
