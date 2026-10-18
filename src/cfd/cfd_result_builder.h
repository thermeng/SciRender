#pragma once
#include <memory>
#include <string>
#include "core/mesh_loader.h"
#include "cfd/cfd_types.h"

namespace fvm {
template<std::size_t Dim> class Mesh;
template<std::size_t Dim> class VectorField;
template<std::size_t Dim> class Field;
template<std::size_t Dim> class SSTModel;
}

namespace cfd {

std::shared_ptr<const RenderMesh> buildResultMesh(
    const std::shared_ptr<const RenderMesh>& baseMesh,
    const fvm::Mesh<2>& mesh,
    const fvm::VectorField<2>& velocity,
    const fvm::Field<2>& pressure,
    const fvm::SSTModel<2>* turbulence,
    const CaseSettings& settings);

} // namespace cfd