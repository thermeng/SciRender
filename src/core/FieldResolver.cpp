#include "core/FieldResolver.h"

namespace FieldResolver {

std::string resolveActiveScalar(const RenderMesh& mesh, const std::string& requested) {
    if (!requested.empty()) {
        if (mesh.attributes) {
            auto it = mesh.attributes->pointScalars.find(requested);
            if (it != mesh.attributes->pointScalars.end()) return requested;
        }
        for (auto& n : mesh.availableScalarNames) if (n == requested) return requested;
    }
    if (!mesh.availableScalarNames.empty()) return mesh.availableScalarNames.front();
    if (mesh.attributes && !mesh.attributes->pointScalars.empty())
        return mesh.attributes->pointScalars.begin()->first;
    return {};
}

const std::vector<float>* scalarData(const RenderMesh& mesh, const std::string& name, float& outMin, float& outMax) {
    if (mesh.attributes) {
        auto it = mesh.attributes->pointScalars.find(name);
        if (it != mesh.attributes->pointScalars.end()) {
            outMin = mesh.attributes->scalarMin;
            outMax = mesh.attributes->scalarMax;
            return &it->second;
        }
        auto cit = mesh.attributes->cellScalars.find(name);
        if (cit != mesh.attributes->cellScalars.end()) {
            outMin = mesh.attributes->scalarMin;
            outMax = mesh.attributes->scalarMax;
            return &cit->second;
        }
    }
    if (!mesh.scalars.empty()) {
        outMin = mesh.attributes ? mesh.attributes->scalarMin : 0.f;
        outMax = mesh.attributes ? mesh.attributes->scalarMax : 1.f;
        return &mesh.scalars;
    }
    outMin = 0; outMax = 1; return nullptr;
}

bool hasScalar(const RenderMesh& mesh) { return mesh.hasScalarData(); }

VectorField resolveVector(const RenderMesh& mesh, const std::string& requested, int placement) {
    VectorField out;
    // placement 1 prefers cell vectors when available
    if (placement == 1 && mesh.meshHasCellVectors()) {
        std::string name = requested;
        if (name.empty() && !mesh.availableCellVectorNames.empty()) name = mesh.availableCellVectorNames.front();
        if (name.empty() && !mesh.cellVectorName.empty()) name = mesh.cellVectorName;
        size_t cnt = 0;
        const glm::vec3* d = mesh.cellVectorFieldData(name, cnt);
        if (d && cnt) { out.data = d; out.count = cnt; out.isCell = true; return out; }
    }
    std::string name = requested;
    if (name.empty() && !mesh.availableVectorNames.empty()) name = mesh.availableVectorNames.front();
    size_t cnt = 0;
    const glm::vec3* d = mesh.vectorFieldData(name, cnt);
    if (d && cnt) { out.data = d; out.count = cnt; out.isCell = false; }
    return out;
}

std::string resolveVectorName(const RenderMesh& mesh, const std::string& requested, int placement) {
    if (!requested.empty()) {
        auto f = resolveVector(mesh, requested, placement);
        if (f.data) return requested;
    }
    if (placement == 1) {
        if (!mesh.cellVectorName.empty()) {
            size_t c; if (mesh.cellVectorFieldData(mesh.cellVectorName, c) && c) return mesh.cellVectorName;
        }
        if (!mesh.availableCellVectorNames.empty()) return mesh.availableCellVectorNames.front();
    }
    if (!mesh.availableVectorNames.empty()) return mesh.availableVectorNames.front();
    if (!mesh.vectorName.empty()) return mesh.vectorName;
    return requested;
}

std::vector<std::string> availableVectorNames(const RenderMesh& mesh) {
    std::vector<std::string> out;
    out.reserve(mesh.availableVectorNames.size() + mesh.availableCellVectorNames.size());
    out.insert(out.end(), mesh.availableVectorNames.begin(), mesh.availableVectorNames.end());
    out.insert(out.end(), mesh.availableCellVectorNames.begin(), mesh.availableCellVectorNames.end());
    return out;
}

} // namespace FieldResolver
