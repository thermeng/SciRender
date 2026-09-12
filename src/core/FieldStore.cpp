#include "core/FieldStore.h"
#include <cmath>
#include <limits>
#include <algorithm>
#include <unordered_map>

namespace FieldStore {

namespace {
thread_local std::unordered_map<std::string, std::vector<float>> s_derivedCache;
thread_local const RenderMesh* s_derivedLastMesh = nullptr;

inline int placementToInt(Placement p) { return static_cast<int>(p); }

Range makeRangeFromScan(float mn, float mx, bool have) {
    if (!have) return {0.0f, 1.0f};
    if (std::abs(mx - mn) < 1e-6f) mx = mn + 1.0f;
    return {mn, mx};
}

} // anonymous

void clearCache() {
    s_derivedCache.clear();
    std::unordered_map<std::string, std::vector<float>>().swap(s_derivedCache);
    s_derivedLastMesh = nullptr;
}

Range computeRange(const std::vector<float>& v) {
    if (v.empty()) return {0.0f, 1.0f};
    float mn = std::numeric_limits<float>::max();
    float mx = std::numeric_limits<float>::lowest();
    bool have = false;
    for (float f : v) {
        if (!std::isfinite(f)) continue;
        mn = std::min(mn, f);
        mx = std::max(mx, f);
        have = true;
    }
    if (!have) return {0.0f, 1.0f};
    return makeRangeFromScan(mn, mx, true);
}

Range computeRange(const float* data, size_t count) {
    if (!data || count == 0) return {0.0f, 1.0f};
    float mn = std::numeric_limits<float>::max();
    float mx = std::numeric_limits<float>::lowest();
    bool have = false;
    for (size_t i = 0; i < count; ++i) {
        float f = data[i];
        if (!std::isfinite(f)) continue;
        mn = std::min(mn, f);
        mx = std::max(mx, f);
        have = true;
    }
    if (!have) return {0.0f, 1.0f};
    return makeRangeFromScan(mn, mx, true);
}

// ── Scalar ────────────────────────────────────────────────────────────────

std::string resolveActiveScalar(const RenderMesh& mesh, const std::string& requested, Placement placement) {
    if (!requested.empty()) {
        if (mesh.attributes) {
            if (placement == Placement::CellCenter) {
                auto cit = mesh.attributes->cellScalars.find(requested);
                if (cit != mesh.attributes->cellScalars.end()) return requested;
                auto it = mesh.attributes->pointScalars.find(requested);
                if (it != mesh.attributes->pointScalars.end()) return requested;
            } else {
                auto it = mesh.attributes->pointScalars.find(requested);
                if (it != mesh.attributes->pointScalars.end()) return requested;
                auto cit = mesh.attributes->cellScalars.find(requested);
                if (cit != mesh.attributes->cellScalars.end()) return requested;
            }
        }
        for (auto& n : mesh.availableScalarNames) if (n == requested) return requested;
        for (auto& n : derivedScalarNames(mesh)) if (n == requested) return requested;
    }
    if (placement == Placement::CellCenter && mesh.attributes && !mesh.attributes->cellScalars.empty())
        return mesh.attributes->cellScalars.begin()->first;
    if (!mesh.availableScalarNames.empty()) return mesh.availableScalarNames.front();
    auto d = derivedScalarNames(mesh);
    if (!d.empty()) return d.front();
    if (mesh.attributes && !mesh.attributes->pointScalars.empty())
        return mesh.attributes->pointScalars.begin()->first;
    if (mesh.attributes && !mesh.attributes->cellScalars.empty())
        return mesh.attributes->cellScalars.begin()->first;
    return {};
}

const std::vector<float>* scalarData(const RenderMesh& mesh, const std::string& name,
                                     float& outMin, float& outMax, Placement placement) {
    ScalarField f = resolveScalar(mesh, {name, placement});
    if (!f.data) { outMin = 0; outMax = 1; return nullptr; }
    outMin = f.range.min; outMax = f.range.max;
    return f.data;
}

const std::vector<float>* volumeScalarData(const RenderMesh& mesh, const std::string& name,
                                           float& outMin, float& outMax, Placement placement) {
    ScalarField f = resolveScalarForVolume(mesh, {name, placement});
    if (!f.data) { outMin = 0; outMax = 1; return nullptr; }
    outMin = f.range.min; outMax = f.range.max;
    return f.data;
}

ScalarField resolveScalar(const RenderMesh& mesh, const FieldKey& key) {
    const std::string& name = key.name;
    const Placement placement = key.placement;
    const bool wantCell = (placement == Placement::CellCenter);

    // Attribute-backed fields — authoritative ranges live in pointScalarRanges / cellScalarRanges
    if (mesh.attributes) {
        if (wantCell) {
            auto cit = mesh.attributes->cellScalars.find(name);
            if (cit != mesh.attributes->cellScalars.end()) {
                Range r{0,1};
                auto rit = mesh.attributes->cellScalarRanges.find(name);
                if (rit != mesh.attributes->cellScalarRanges.end()) r = {rit->second.first, rit->second.second};
                else r = computeRange(cit->second);
                // surface needs point-sized data; return extrapolated point copy when it exists but keep cell range
                auto pit = mesh.attributes->pointScalars.find(name);
                if (pit != mesh.attributes->pointScalars.end())
                    return {&pit->second, r, true};
                return {&cit->second, r, true};
            }
            auto it = mesh.attributes->pointScalars.find(name);
            if (it != mesh.attributes->pointScalars.end()) {
                auto rit = mesh.attributes->pointScalarRanges.find(name);
                Range r = (rit != mesh.attributes->pointScalarRanges.end())
                    ? Range{rit->second.first, rit->second.second} : computeRange(it->second);
                return {&it->second, r, false};
            }
        } else {
            auto it = mesh.attributes->pointScalars.find(name);
            if (it != mesh.attributes->pointScalars.end()) {
                auto rit = mesh.attributes->pointScalarRanges.find(name);
                Range r = (rit != mesh.attributes->pointScalarRanges.end())
                    ? Range{rit->second.first, rit->second.second} : computeRange(it->second);
                return {&it->second, r, false};
            }
            auto cit = mesh.attributes->cellScalars.find(name);
            if (cit != mesh.attributes->cellScalars.end()) {
                auto rit = mesh.attributes->cellScalarRanges.find(name);
                Range r = (rit != mesh.attributes->cellScalarRanges.end())
                    ? Range{rit->second.first, rit->second.second} : computeRange(cit->second);
                return {&cit->second, r, true};
            }
        }
        // Range map authoritative but storage may live under different key (extrapolated case)
        auto rit = mesh.attributes->pointScalarRanges.find(name);
        if (rit != mesh.attributes->pointScalarRanges.end()) {
            auto pit = mesh.attributes->pointScalars.find(name);
            if (pit != mesh.attributes->pointScalars.end())
                return {&pit->second, {rit->second.first, rit->second.second}, false};
            auto cit2 = mesh.attributes->cellScalars.find(name);
            if (cit2 != mesh.attributes->cellScalars.end()) {
                auto cr = mesh.attributes->cellScalarRanges.find(name);
                Range r = (cr != mesh.attributes->cellScalarRanges.end())
                    ? Range{cr->second.first, cr->second.second} : computeRange(cit2->second);
                return {&cit2->second, r, true};
            }
        }
    }

    if (!mesh.scalars.empty() && (name == mesh.scalarName || name.empty())) {
        Range r = computeRange(mesh.scalars);
        return {&mesh.scalars, r, false};
    }

    // Derived vector -> scalar
    auto tryDerived = [&](const std::string& suffix, int comp) -> ScalarField {
        if (name.size() <= suffix.size() || name.substr(name.size() - suffix.size()) != suffix)
            return {nullptr, {0,1}, false};
        std::string base = name.substr(0, name.size() - suffix.size());
        VectorField vfPoint = resolveVector(mesh, base, Placement::Vertex);
        VectorField vfCell  = resolveVector(mesh, base, Placement::CellCenter);
        bool hasPoint = vfPoint.data && vfPoint.count > 0;
        bool hasCell  = vfCell.data && vfCell.count > 0;
        if (!hasPoint && !hasCell) return {nullptr, {0,1}, false};
        if (s_derivedLastMesh != &mesh) { s_derivedCache.clear(); s_derivedLastMesh = &mesh; }

        std::string keyPoint = name + "#vert";
        const std::vector<float>* pointPtr = nullptr;
        Range pRange{0,1};
        auto itP = s_derivedCache.find(keyPoint);
        if (itP != s_derivedCache.end()) {
            pointPtr = &itP->second;
            pRange = computeRange(*pointPtr);
        } else if (hasPoint) {
            std::vector<float> derived; derived.reserve(vfPoint.count);
            for (size_t i = 0; i < vfPoint.count; ++i) {
                float v;
                if (suffix == "_magnitude") v = std::sqrt(vfPoint.data[i].x*vfPoint.data[i].x + vfPoint.data[i].y*vfPoint.data[i].y + vfPoint.data[i].z*vfPoint.data[i].z);
                else if (comp == 0) v = vfPoint.data[i].x;
                else if (comp == 1) v = vfPoint.data[i].y;
                else v = vfPoint.data[i].z;
                derived.push_back(v);
            }
            pRange = computeRange(derived);
            auto &slot = s_derivedCache[keyPoint] = std::move(derived);
            pointPtr = &slot;
        }

        if (placement == Placement::CellCenter && hasCell) {
            std::string keyCell = name + "#cell";
            auto itC = s_derivedCache.find(keyCell);
            Range cRange{0,1};
            if (itC != s_derivedCache.end()) {
                cRange = computeRange(itC->second);
            } else {
                std::vector<float> derivedC; derivedC.reserve(vfCell.count);
                for (size_t i = 0; i < vfCell.count; ++i) {
                    float v;
                    if (suffix == "_magnitude") v = std::sqrt(vfCell.data[i].x*vfCell.data[i].x + vfCell.data[i].y*vfCell.data[i].y + vfCell.data[i].z*vfCell.data[i].z);
                    else if (comp == 0) v = vfCell.data[i].x;
                    else if (comp == 1) v = vfCell.data[i].y;
                    else v = vfCell.data[i].z;
                    derivedC.push_back(v);
                }
                cRange = computeRange(derivedC);
                s_derivedCache[keyCell] = std::move(derivedC);
            }
            // surface data stays point-sized but range is cell-wide
            if (pointPtr) return {pointPtr, cRange, true};
            auto itCF = s_derivedCache.find(keyCell);
            if (itCF != s_derivedCache.end()) return {&itCF->second, cRange, true};
        }
        if (pointPtr) return {pointPtr, pRange, false};
        return {nullptr, {0,1}, false};
    };

    ScalarField r;
    r = tryDerived("_magnitude", -1); if (r.data) return r;
    r = tryDerived("_X", 0); if (r.data) return r;
    r = tryDerived("_Y", 1); if (r.data) return r;
    r = tryDerived("_Z", 2); if (r.data) return r;
    return {nullptr, {0,1}, false};
}

ScalarField resolveScalarForVolume(const RenderMesh& mesh, const FieldKey& key) {
    const std::string& name = key.name;
    const Placement placement = key.placement;

    if (mesh.attributes) {
        if (placement == Placement::CellCenter) {
            auto cit = mesh.attributes->cellScalars.find(name);
            if (cit != mesh.attributes->cellScalars.end()) {
                auto rit = mesh.attributes->cellScalarRanges.find(name);
                Range r = (rit != mesh.attributes->cellScalarRanges.end())
                    ? Range{rit->second.first, rit->second.second} : computeRange(cit->second);
                return {&cit->second, r, true};
            }
        } else {
            auto it = mesh.attributes->pointScalars.find(name);
            if (it != mesh.attributes->pointScalars.end()) {
                auto rit = mesh.attributes->pointScalarRanges.find(name);
                Range r = (rit != mesh.attributes->pointScalarRanges.end())
                    ? Range{rit->second.first, rit->second.second} : computeRange(it->second);
                return {&it->second, r, false};
            }
        }
        // fallback
        if (placement == Placement::CellCenter) {
            auto it = mesh.attributes->pointScalars.find(name);
            if (it != mesh.attributes->pointScalars.end()) {
                auto rit = mesh.attributes->pointScalarRanges.find(name);
                Range r = (rit != mesh.attributes->pointScalarRanges.end())
                    ? Range{rit->second.first, rit->second.second} : computeRange(it->second);
                return {&it->second, r, false};
            }
        } else {
            auto cit = mesh.attributes->cellScalars.find(name);
            if (cit != mesh.attributes->cellScalars.end()) {
                auto rit = mesh.attributes->cellScalarRanges.find(name);
                Range r = (rit != mesh.attributes->cellScalarRanges.end())
                    ? Range{rit->second.first, rit->second.second} : computeRange(cit->second);
                return {&cit->second, r, true};
            }
        }
    }
    if (!mesh.scalars.empty() && (name == mesh.scalarName || name.empty())) {
        return {&mesh.scalars, computeRange(mesh.scalars), false};
    }
    auto tryDerivedVol = [&](const std::string& suffix, int comp) -> ScalarField {
        if (name.size() <= suffix.size() || name.substr(name.size() - suffix.size()) != suffix)
            return {nullptr, {0,1}, false};
        std::string base = name.substr(0, name.size() - suffix.size());
        VectorField vf = resolveVector(mesh, base, placement);
        if (!vf.data || vf.count == 0) {
            Placement alt = (placement == Placement::CellCenter) ? Placement::Vertex : Placement::CellCenter;
            vf = resolveVector(mesh, base, alt);
            if (!vf.data || vf.count == 0) return {nullptr, {0,1}, false};
        }
        std::string cacheKey = name + (placement == Placement::CellCenter ? "#volcell" : "#volvert");
        if (s_derivedLastMesh != &mesh) { s_derivedCache.clear(); s_derivedLastMesh = &mesh; }
        auto itc = s_derivedCache.find(cacheKey);
        if (itc != s_derivedCache.end()) {
            return {&itc->second, computeRange(itc->second), vf.isCell};
        }
        std::vector<float> derived; derived.reserve(vf.count);
        for (size_t i = 0; i < vf.count; ++i) {
            float v;
            if (suffix == "_magnitude") v = std::sqrt(vf.data[i].x*vf.data[i].x + vf.data[i].y*vf.data[i].y + vf.data[i].z*vf.data[i].z);
            else if (comp == 0) v = vf.data[i].x;
            else if (comp == 1) v = vf.data[i].y;
            else v = vf.data[i].z;
            derived.push_back(v);
        }
        Range r = computeRange(derived);
        auto &slot = s_derivedCache[cacheKey] = std::move(derived);
        return {&slot, r, vf.isCell};
    };
    ScalarField r;
    r = tryDerivedVol("_magnitude", -1); if (r.data) return r;
    r = tryDerivedVol("_X", 0); if (r.data) return r;
    r = tryDerivedVol("_Y", 1); if (r.data) return r;
    r = tryDerivedVol("_Z", 2); if (r.data) return r;
    return {nullptr, {0,1}, false};
}

// ── Vector ───────────────────────────────────────────────────────────────

VectorField resolveVector(const RenderMesh& mesh, const std::string& requested, Placement placement) {
    VectorField out;
    if (placement == Placement::CellCenter && mesh.meshHasCellVectors()) {
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

std::string resolveVectorName(const RenderMesh& mesh, const std::string& requested, Placement placement) {
    if (!requested.empty()) {
        auto f = resolveVector(mesh, requested, placement);
        if (f.data) return requested;
    }
    if (placement == Placement::CellCenter) {
        if (!mesh.cellVectorName.empty()) {
            size_t c; if (mesh.cellVectorFieldData(mesh.cellVectorName, c) && c) return mesh.cellVectorName;
        }
        if (!mesh.availableCellVectorNames.empty()) return mesh.availableCellVectorNames.front();
    }
    if (!mesh.availableVectorNames.empty()) return mesh.availableVectorNames.front();
    if (!mesh.vectorName.empty()) return mesh.vectorName;
    return requested;
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

std::vector<std::string> availableVectorNames(const RenderMesh& mesh) {
    std::vector<std::string> out;
    out.reserve(mesh.availableVectorNames.size() + mesh.availableCellVectorNames.size());
    out.insert(out.end(), mesh.availableVectorNames.begin(), mesh.availableVectorNames.end());
    out.insert(out.end(), mesh.availableCellVectorNames.begin(), mesh.availableCellVectorNames.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool hasScalar(const RenderMesh& mesh) { return mesh.hasScalarData(); }

} // namespace FieldStore
