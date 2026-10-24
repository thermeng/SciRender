#include "core/FieldStore.h"
#include <cmath>
#include <limits>
#include <algorithm>
#include <unordered_map>
#include <span>

namespace FieldStore {
namespace {

// Derived arrays are shared_ptr-owned so a caller can outlive a cache refill:
// handles returned in ScalarField::owner keep the data alive even after
// clearCache()/a different mesh swaps the map contents.
thread_local std::unordered_map<std::string, std::shared_ptr<const std::vector<float>>> s_derivedCache;
thread_local const RenderMesh* s_derivedLastMesh = nullptr;
thread_local uint64_t s_derivedLastHash = 0;

inline int placementToInt(Placement p) { return static_cast<int>(p); }

Range makeRangeFromScan(float mn, float mx, bool have) {
    if (!have) return {0.0f, 1.0f};
    if (std::abs(mx - mn) < 1e-6f) mx = mn + 1.0f;
    return {mn, mx};
}

// Shared derived synthesis — single implementation for both surface and volume paths.
std::vector<float> synthesizeDerivedArray(const glm::vec3* data, size_t count,
                                           const std::string& suffix, int comp) {
    std::vector<float> out; out.reserve(count);
    bool isMag = (suffix == " [Magnitude]");
    for (size_t i = 0; i < count; ++i) {
        float v;
        if (isMag) v = std::sqrt(data[i].x*data[i].x + data[i].y*data[i].y + data[i].z*data[i].z);
        else if (comp == 0) v = data[i].x;
        else if (comp == 1) v = data[i].y;
        else v = data[i].z;
        out.push_back(v);
    }
    return out;
}

} // anonymous

void clearCache() {
    decltype(s_derivedCache)().swap(s_derivedCache);
    s_derivedLastMesh = nullptr;
    s_derivedLastHash = 0;
}

namespace {
Range rangeFromSpan(std::span<const float> s) {
    if (s.empty()) return {0.0f, 1.0f};
    float mn = std::numeric_limits<float>::max();
    float mx = std::numeric_limits<float>::lowest();
    bool have = false;
    for (float f : s) {
        if (!std::isfinite(f)) continue;
        mn = std::min(mn, f);
        mx = std::max(mx, f);
        have = true;
    }
    if (!have) return {0.0f, 1.0f};
    return makeRangeFromScan(mn, mx, true);
}
} // anon

Range computeRange(const std::vector<float>& v) {
    return rangeFromSpan(std::span<const float>(v.data(), v.size()));
}

Range computeRange(const float* data, size_t count) {
    if (!data || count == 0) return {0.0f, 1.0f};
    return rangeFromSpan(std::span<const float>(data, count));
}

// ── Scalar ────────────────────────────────────────────────────────────────

std::string resolveActiveScalar(const RenderMesh& mesh, const std::string& requested, Placement placement,
                                const postfields::ComputedSnapshot* snap) {
    if (!requested.empty()) {
        if (snap) {
            if (snap->scalars.find(requested) != snap->scalars.end()) return requested;
            if (snap->vectors.find(requested) != snap->vectors.end()) return requested;
            // Synthesized scalars on snapshot vectors ("V_vorticity [Magnitude]"
            // etc.): the snapshot maps hold the raw base name, so strip the
            // derived suffix before the claim test.
            for (const std::string sfx : {std::string(" [Magnitude]"), std::string(" [X]"), std::string(" [Y]"), std::string(" [Z]")}) {
                if (requested.size() > sfx.size() && requested.compare(requested.size() - sfx.size(), sfx.size(), sfx) == 0)
                    if (snap->vectors.count(requested.substr(0, requested.size() - sfx.size())))
                        return requested;
            }
        }
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
                                     float& outMin, float& outMax, Placement placement,
                                     const postfields::ComputedSnapshot* snap) {
    ScalarField f = resolveScalar(mesh, {name, placement}, snap);
    if (!f.data) { outMin = 0; outMax = 1; return nullptr; }
    outMin = f.range.min; outMax = f.range.max;
    return f.data;
}

const std::vector<float>* volumeScalarData(const RenderMesh& mesh, const std::string& name,
                                           float& outMin, float& outMax, Placement placement,
                                           const postfields::ComputedSnapshot* snap) {
    ScalarField f = resolveScalarForVolume(mesh, {name, placement}, snap);
    if (!f.data) { outMin = 0; outMax = 1; return nullptr; }
    outMin = f.range.min; outMax = f.range.max;
    return f.data;
}

ScalarField resolveScalar(const RenderMesh& mesh, const FieldKey& key,
                          const postfields::ComputedSnapshot* snap) {
    const std::string& name = key.name;
    const Placement placement = key.placement;
    const bool wantCell = (placement == Placement::CellCenter);

    // Computed snapshot shadows mesh fields by name (immutable shared payload).
    if (snap) {
        auto it = snap->scalars.find(name);
        if (it != snap->scalars.end() && it->second && !it->second->empty()) {
            auto rit = snap->scalarRanges.find(name);
            Range r = (rit != snap->scalarRanges.end())
                ? Range{rit->second.first, rit->second.second} : computeRange(*it->second);
            return {it->second.get(), r, false};
        }
    }

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

    // Derived vector -> scalar (snapshot-aware for post vorticity etc) — uses shared synthesizer.
    auto tryDerived = [&](const std::string& suffix, int comp) -> ScalarField {
        if (name.size() <= suffix.size() || name.substr(name.size() - suffix.size()) != suffix)
            return {nullptr, {0,1}, false};
        std::string base = name.substr(0, name.size() - suffix.size());
        VectorField vfPoint = resolveVector(mesh, base, Placement::Vertex, snap);
        VectorField vfCell  = resolveVector(mesh, base, Placement::CellCenter, snap);
        bool hasPoint = vfPoint.data && vfPoint.count > 0;
        bool hasCell  = vfCell.data && vfCell.count > 0;
        if (!hasPoint && !hasCell) return {nullptr, {0,1}, false};
        const uint64_t curHash = mesh.geometryHash ? mesh.geometryHash : reinterpret_cast<uint64_t>(&mesh);
        if (s_derivedLastMesh != &mesh || s_derivedLastHash != curHash) { s_derivedCache.clear(); s_derivedLastMesh = &mesh; s_derivedLastHash = curHash; }

        std::string keyPoint = name + "#vert";
        std::shared_ptr<const std::vector<float>> pointOwner;
        Range pRange{0,1};
        auto itP = s_derivedCache.find(keyPoint);
        if (itP != s_derivedCache.end()) {
            pointOwner = itP->second;
            pRange = computeRange(*pointOwner);
        } else if (hasPoint) {
            pointOwner = std::make_shared<const std::vector<float>>(
                synthesizeDerivedArray(vfPoint.data, vfPoint.count, suffix, comp));
            pRange = computeRange(*pointOwner);
            s_derivedCache[keyPoint] = pointOwner;
        }

        if (placement == Placement::CellCenter && hasCell) {
            std::string keyCell = name + "#cell";
            auto itC = s_derivedCache.find(keyCell);
            Range cRange{0,1};
            if (itC != s_derivedCache.end()) {
                cRange = computeRange(*itC->second);
            } else {
                auto derivedC = std::make_shared<const std::vector<float>>(
                    synthesizeDerivedArray(vfCell.data, vfCell.count, suffix, comp));
                cRange = computeRange(*derivedC);
                s_derivedCache[keyCell] = derivedC;
            }
            // surface data stays point-sized but range is cell-wide
            if (pointOwner) return {pointOwner.get(), cRange, true, pointOwner};
            auto itCF = s_derivedCache.find(keyCell);
            if (itCF != s_derivedCache.end())
                return {itCF->second.get(), cRange, true, itCF->second};
        }
        if (pointOwner) return {pointOwner.get(), pRange, false, pointOwner};
        return {nullptr, {0,1}, false};
    };

    ScalarField r;
    r = tryDerived(" [Magnitude]", -1); if (r.data) return r;
    r = tryDerived(" [X]", 0); if (r.data) return r;
    r = tryDerived(" [Y]", 1); if (r.data) return r;
    r = tryDerived(" [Z]", 2); if (r.data) return r;
    return {nullptr, {0,1}, false};
}

ScalarField resolveScalarForVolume(const RenderMesh& mesh, const FieldKey& key,
                                   const postfields::ComputedSnapshot* snap) {
    const std::string& name = key.name;
    const Placement placement = key.placement;

    if (snap) {
        auto it = snap->scalars.find(name);
        if (it != snap->scalars.end() && it->second && !it->second->empty()) {
            auto rit = snap->scalarRanges.find(name);
            Range r = (rit != snap->scalarRanges.end())
                ? Range{rit->second.first, rit->second.second} : computeRange(*it->second);
            return {it->second.get(), r, false};
        }
    }

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
        VectorField vf = resolveVector(mesh, base, placement, snap);
        if (!vf.data || vf.count == 0) {
            Placement alt = (placement == Placement::CellCenter) ? Placement::Vertex : Placement::CellCenter;
            vf = resolveVector(mesh, base, alt, snap);
            if (!vf.data || vf.count == 0) return {nullptr, {0,1}, false};
        }
        std::string cacheKey = name + (placement == Placement::CellCenter ? "#volcell" : "#volvert");
        const uint64_t curHash2 = mesh.geometryHash ? mesh.geometryHash : reinterpret_cast<uint64_t>(&mesh);
        if (s_derivedLastMesh != &mesh || s_derivedLastHash != curHash2) { s_derivedCache.clear(); s_derivedLastMesh = &mesh; s_derivedLastHash = curHash2; }
        auto itc = s_derivedCache.find(cacheKey);
        if (itc != s_derivedCache.end()) {
            return {itc->second.get(), computeRange(*itc->second), vf.isCell, itc->second};
        }
        auto derived = std::make_shared<const std::vector<float>>(
            synthesizeDerivedArray(vf.data, vf.count, suffix, comp));
        Range r = computeRange(*derived);
        s_derivedCache[cacheKey] = derived;
        return {derived.get(), r, vf.isCell, derived};
    };
    ScalarField r;
    r = tryDerivedVol(" [Magnitude]", -1); if (r.data) return r;
    r = tryDerivedVol(" [X]", 0); if (r.data) return r;
    r = tryDerivedVol(" [Y]", 1); if (r.data) return r;
    r = tryDerivedVol(" [Z]", 2); if (r.data) return r;
    return {nullptr, {0,1}, false};
}

// ── Vector ───────────────────────────────────────────────────────────────

VectorField resolveVector(const RenderMesh& mesh, const std::string& requested, Placement placement,
                          const postfields::ComputedSnapshot* snap) {
    VectorField out;
    if (snap && !requested.empty()) {
        auto it = snap->vectors.find(requested);
        if (it != snap->vectors.end() && it->second && !it->second->empty()) {
            out.data = it->second->data();
            out.count = it->second->size();
            out.isCell = false;
            return out;
        }
    }
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

std::string resolveVectorName(const RenderMesh& mesh, const std::string& requested, Placement placement,
                              const postfields::ComputedSnapshot* snap) {
    if (!requested.empty()) {
        if (snap && snap->vectors.find(requested) != snap->vectors.end()) return requested;
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
        out.push_back(n + " [Magnitude]");
        out.push_back(n + " [X]");
        out.push_back(n + " [Y]");
        out.push_back(n + " [Z]");
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

std::vector<std::string> availableScalars(const RenderMesh& mesh, const postfields::ComputedSnapshot* snap) {
    std::vector<std::string> out = mesh.availableScalarNames;
    // cell scalars that lack point counterpart (hasScalarData fix) — expose directly
    if (mesh.attributes) {
        for (auto &kv : mesh.attributes->cellScalars) out.push_back(kv.first);
        for (auto &kv : mesh.attributes->pointScalars) out.push_back(kv.first);
    }
    auto d = derivedScalarNames(mesh);
    out.insert(out.end(), d.begin(), d.end());
    if (snap) {
        for (auto &kv : snap->scalars) out.push_back(kv.first);
        // Snapshot vectors (vorticity, gradP) also expose derived scalars ( [Magnitude]/ [X]/ [Y]/ [Z])
        // so customer can visualize e.g. p_grad [Magnitude] via scalar coloring
        for (auto &kv : snap->vectors) {
            out.push_back(kv.first + " [Magnitude]");
            out.push_back(kv.first + " [X]");
            out.push_back(kv.first + " [Y]");
            out.push_back(kv.first + " [Z]");
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> availableVectors(const RenderMesh& mesh, const postfields::ComputedSnapshot* snap) {
    std::vector<std::string> out = availableVectorNames(mesh);
    if (snap) {
        for (auto &kv : snap->vectors) out.push_back(kv.first);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
    return out;
}

bool hasAnyScalar(const RenderMesh& mesh, const postfields::ComputedSnapshot* snap) {
    if (mesh.hasScalarData()) return true;
    if (snap && !snap->scalars.empty()) return true;
    // derived scalars count as scalars when vectors exist
    if (!derivedScalarNames(mesh).empty()) return true;
    return false;
}

bool hasScalarField(const RenderMesh& mesh, const std::string& name, const postfields::ComputedSnapshot* snap) {
    float mn, mx;
    if (snap && snap->scalars.find(name) != snap->scalars.end()) return true;
    // probe via FieldStore resolution (covers derived as well)
    if (scalarData(mesh, name, mn, mx, Placement::Vertex, snap)) return true;
    if (scalarData(mesh, name, mn, mx, Placement::CellCenter, snap)) return true;
    return false;
}

} // namespace FieldStore
