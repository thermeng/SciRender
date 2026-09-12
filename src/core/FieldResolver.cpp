#include "core/FieldResolver.h"
#include "core/FieldStore.h"

namespace FieldResolver {

static FieldStore::Placement toPlacement(int p) {
    return p == 1 ? FieldStore::Placement::CellCenter : FieldStore::Placement::Vertex;
}

void clearCache() { FieldStore::clearCache(); }

std::string resolveActiveScalar(const RenderMesh& mesh, const std::string& requested, int placement) {
    return FieldStore::resolveActiveScalar(mesh, requested, toPlacement(placement));
}

const std::vector<float>* scalarData(const RenderMesh& mesh, const std::string& name, float& outMin, float& outMax, int placement) {
    return FieldStore::scalarData(mesh, name, outMin, outMax, toPlacement(placement));
}

const std::vector<float>* volumeScalarData(const RenderMesh& mesh, const std::string& name, float& outMin, float& outMax, int placement) {
    return FieldStore::volumeScalarData(mesh, name, outMin, outMax, toPlacement(placement));
}

std::vector<std::string> derivedScalarNames(const RenderMesh& mesh) { return FieldStore::derivedScalarNames(mesh); }
std::vector<std::string> availableScalarNamesWithDerived(const RenderMesh& mesh) { return FieldStore::availableScalarNamesWithDerived(mesh); }
bool hasScalar(const RenderMesh& mesh) { return FieldStore::hasScalar(mesh); }

VectorField resolveVector(const RenderMesh& mesh, const std::string& requested, int placement) {
    auto f = FieldStore::resolveVector(mesh, requested, toPlacement(placement));
    return {f.data, f.count, f.isCell};
}

std::string resolveVectorName(const RenderMesh& mesh, const std::string& requested, int placement) {
    return FieldStore::resolveVectorName(mesh, requested, toPlacement(placement));
}

std::vector<std::string> availableVectorNames(const RenderMesh& mesh) { return FieldStore::availableVectorNames(mesh); }

} // namespace FieldResolver
