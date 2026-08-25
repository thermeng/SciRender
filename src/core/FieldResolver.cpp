#include "core/FieldResolver.h"
#include <cmath>
#include <limits>
#include <unordered_map>

namespace FieldResolver {

std::string resolveActiveScalar(const RenderMesh& mesh, const std::string& requested) {
    if (!requested.empty()) {
        if (mesh.attributes) {
            auto it = mesh.attributes->pointScalars.find(requested);
            if (it != mesh.attributes->pointScalars.end()) return requested;
            auto cit = mesh.attributes->cellScalars.find(requested);
            if (cit != mesh.attributes->cellScalars.end()) return requested;
        }
        for (auto& n : mesh.availableScalarNames) if (n == requested) return requested;
        for (auto& n : derivedScalarNames(mesh)) if (n == requested) return requested;
    }
    if (!mesh.availableScalarNames.empty()) return mesh.availableScalarNames.front();
    auto d = derivedScalarNames(mesh);
    if (!d.empty()) return d.front();
    if (mesh.attributes && !mesh.attributes->pointScalars.empty())
        return mesh.attributes->pointScalars.begin()->first;
    return {};
}

const std::vector<float>* scalarData(const RenderMesh& mesh, const std::string& name, float& outMin, float& outMax) {
    auto computeRange = [](const std::vector<float>& v, float& mn, float& mx){
        if (v.empty()) { mn=0; mx=1; return; }
        mn = std::numeric_limits<float>::max();
        mx = std::numeric_limits<float>::lowest();
        for (float f: v) { if(!std::isfinite(f)) continue; mn = std::min(mn,f); mx = std::max(mx,f); }
        if (mn > mx) { mn=0; mx=1; }
        if (std::abs(mx - mn) < 1e-6f) mx = mn + 1.0f;
    };
    if (mesh.attributes) {
        auto it = mesh.attributes->pointScalars.find(name);
        if (it != mesh.attributes->pointScalars.end()) {
            // Prefer stored per-field range, fall back to scan (robust if stale)
            auto rit = mesh.attributes->pointScalarRanges.find(name);
            if (rit != mesh.attributes->pointScalarRanges.end()) {
                outMin = rit->second.first; outMax = rit->second.second;
            } else {
                computeRange(it->second, outMin, outMax);
            }
            return &it->second;
        }
        auto cit = mesh.attributes->cellScalars.find(name);
        if (cit != mesh.attributes->cellScalars.end()) {
            auto rit = mesh.attributes->cellScalarRanges.find(name);
            if (rit != mesh.attributes->cellScalarRanges.end()) {
                outMin = rit->second.first; outMax = rit->second.second;
            } else {
                computeRange(cit->second, outMin, outMax);
            }
            return &cit->second;
        }
        // Also check per-field ranges directly (covers extrapolated cell->point case where
        // pointScalars already contains the averaged data but range map is authoritative)
        auto rit = mesh.attributes->pointScalarRanges.find(name);
        if (rit != mesh.attributes->pointScalarRanges.end()) {
            // Find actual storage (could be pointScalars or cellScalars)
            auto pit = mesh.attributes->pointScalars.find(name);
            if (pit != mesh.attributes->pointScalars.end()) {
                outMin = rit->second.first; outMax = rit->second.second;
                return &pit->second;
            }
            auto cit2 = mesh.attributes->cellScalars.find(name);
            if (cit2 != mesh.attributes->cellScalars.end()) {
                auto cr = mesh.attributes->cellScalarRanges.find(name);
                if (cr != mesh.attributes->cellScalarRanges.end()) { outMin=cr->second.first; outMax=cr->second.second; }
                else computeRange(cit2->second, outMin, outMax);
                return &cit2->second;
            }
        }
    }
    if (!mesh.scalars.empty() && (name == mesh.scalarName || name.empty())) {
        // Scan the active scalars vector directly (robust after computeNormals split)
        computeRange(mesh.scalars, outMin, outMax);
        return &mesh.scalars;
    }
    // Lazy derived vector→scalar: <base>_magnitude / _X / _Y / _Z
    auto tryDerived = [&](const std::string& suffix, int comp) -> const std::vector<float>* {
        if (name.size() <= suffix.size() || name.substr(name.size()-suffix.size()) != suffix) return nullptr;
        std::string base = name.substr(0, name.size()-suffix.size());
        // Prefer point vectors (already extrapolated from cell via extrapolateCellDataToPoints)
        VectorField vf = resolveVector(mesh, base, 0);
        if (!vf.data || vf.count==0) vf = resolveVector(mesh, base, 1);
        if (!vf.data || vf.count==0) return nullptr;
        // Thread-local cache keyed by mesh address + derived name
        thread_local std::unordered_map<std::string, std::vector<float>> cache;
        thread_local const RenderMesh* lastMesh = nullptr;
        if (lastMesh != &mesh) { cache.clear(); lastMesh = &mesh; }
        auto itc = cache.find(name);
        if (itc != cache.end()) {
            // recompute min/max from cached
            float mn = std::numeric_limits<float>::max(), mx = -std::numeric_limits<float>::max();
            for (float v: itc->second) { if (!std::isfinite(v)) continue; mn = std::min(mn,v); mx = std::max(mx,v); }
            if (mn > mx) { mn = 0; mx = 1; } if (mx - mn < 1e-6f) mx = mn + 1.f;
            outMin = mn; outMax = mx;
            return &itc->second;
        }
        std::vector<float> derived; derived.reserve(vf.count);
        float mn = std::numeric_limits<float>::max(), mx = -std::numeric_limits<float>::max();
        for (size_t i=0;i<vf.count;++i) {
            float v;
            if (suffix == "_magnitude") v = std::sqrt(vf.data[i].x*vf.data[i].x + vf.data[i].y*vf.data[i].y + vf.data[i].z*vf.data[i].z);
            else if (comp==0) v = vf.data[i].x;
            else if (comp==1) v = vf.data[i].y;
            else v = vf.data[i].z;
            derived.push_back(v);
            if (std::isfinite(v)) { mn = std::min(mn,v); mx = std::max(mx,v); }
        }
        if (mn > mx) { mn = 0; mx = 1; } if (mx - mn < 1e-6f) mx = mn + 1.f;
        outMin = mn; outMax = mx;
        auto &slot = cache[name] = std::move(derived);
        return &slot;
    };
    if (auto* p = tryDerived("_magnitude", -1)) return p;
    if (auto* p = tryDerived("_X", 0)) return p;
    if (auto* p = tryDerived("_Y", 1)) return p;
    if (auto* p = tryDerived("_Z", 2)) return p;
    outMin = 0; outMax = 1; return nullptr;
}

std::vector<std::string> derivedScalarNames(const RenderMesh& mesh) {
    std::vector<std::string> out;
    for (auto &n : availableVectorNames(mesh)) {
        out.push_back(n + "_magnitude");
        out.push_back(n + "_X");
        out.push_back(n + "_Y");
        out.push_back(n + "_Z");
    }
    return out;
}
std::vector<std::string> availableScalarNamesWithDerived(const RenderMesh& mesh) {
    std::vector<std::string> out = mesh.availableScalarNames;
    auto d = derivedScalarNames(mesh);
    out.insert(out.end(), d.begin(), d.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
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
    // A cell-only field also exists under the same name as the extrapolated
    // point copy (extrapolateCellDataToPoints), so point+cell lists commonly
    // overlap — list each field once; the placement combo picks the source.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace FieldResolver
