#include "render/streamlines/geometry/GeometryBuilder.h"
#include <algorithm>
#include <cmath>

namespace streamline {
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

Frame initialFrame(const glm::vec3& tangent) {
    glm::vec3 t = glm::normalize(tangent);
    glm::vec3 up = std::abs(t.y) < 0.9f ? glm::vec3(0,1,0) : glm::vec3(1,0,0);
    glm::vec3 n = glm::normalize(glm::cross(t, up));
    return {n, glm::cross(t, n), t};
}

Frame transportFrame(const Frame& prev, const glm::vec3& tangent) {
    glm::vec3 t = glm::normalize(tangent);
    glm::vec3 n = prev.normal - t * glm::dot(prev.normal, t);
    float len = glm::length(n);
    if (len < 1e-8f) return initialFrame(tangent);
    n /= len;
    return {n, glm::cross(t, n), t};
}

static void pushVert(std::vector<float>& out, const glm::vec3& p, float mag,
                     const glm::vec3& comp, const glm::vec3& n,
                     MagnitudeRanges& ranges, bool& first) {
    out.push_back(p.x); out.push_back(p.y); out.push_back(p.z);
    out.push_back(mag);
    out.push_back(comp.x); out.push_back(comp.y); out.push_back(comp.z);
    out.push_back(n.x); out.push_back(n.y); out.push_back(n.z);
    if (first) {
        ranges.magMin = ranges.magMax = mag;
        for (int c = 0; c < 3; ++c) ranges.compMin[c] = ranges.compMax[c] = comp[c];
        first = false;
    } else {
        ranges.magMin = std::min(ranges.magMin, mag); ranges.magMax = std::max(ranges.magMax, mag);
        for (int c = 0; c < 3; ++c) {
            ranges.compMin[c] = std::min(ranges.compMin[c], comp[c]);
            ranges.compMax[c] = std::max(ranges.compMax[c], comp[c]);
        }
    }
}

GeometryOutput buildGeometry(const StreamlineData& data, const GeometryConfig& cfg) {
    GeometryOutput out;
    MagnitudeRanges ranges{};
    bool first = true;
    const float baseWidth = std::max(cfg.baseWidth, 0.0f);
    const float taper = std::clamp(cfg.taperFactor, 0.0f, 1.0f);
    constexpr int kTubeSegments = 8;

    out.seedVerts.reserve(data.seedPoints.size() * 3);
    for (auto& s : data.seedPoints) { out.seedVerts.push_back(s.x); out.seedVerts.push_back(s.y); out.seedVerts.push_back(s.z); }
    out.seedCount = (int)data.seedPoints.size();

    // Field vectors live on the path (StreamlineData::fieldVecs) so
    // colour-by-component and arrow direction use the true field.
    for (const auto& path : data.paths) {
        if (out.verts.size() / 10 >= cfg.maxVertices) { out.budgetExceeded = true; break; }
        const auto& pts = path.points;
        const float pMax = path.pathMaxMag > 1e-12f ? path.pathMaxMag : 1.0f;
        Frame frame = initialFrame(pts[1] - pts[0]);
        const float denom = (float)std::max<size_t>(1, pts.size() - 1);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            glm::vec3 tangent = pts[i+1] - pts[i];
            float segLen = glm::length(tangent);
            if (segLen < 1e-12f) continue;
            tangent /= segLen;
            if (i > 0) frame = transportFrame(frame, tangent);
            else frame.tangent = tangent;
            float magA = path.speed[i], magB = path.speed[i+1];
            glm::vec3 compA = path.fieldVecs[i], compB = path.fieldVecs[i+1];
            float tA = (float)i / denom, tB = (float)(i+1) / denom;
            float taperA = 1.0f - taper * std::abs(2.0f*tA - 1.0f);
            float taperB = 1.0f - taper * std::abs(2.0f*tB - 1.0f);
            auto radiusF = [&](float m){ return std::clamp(m / pMax, 0.15f, 2.0f); };
            float rA = baseWidth * taperA * radiusF(magA);
            float rB = baseWidth * taperB * radiusF(magB);

            if (cfg.style == 0) {
                pushVert(out.verts, pts[i], magA, compA, tangent, ranges, first);
                pushVert(out.verts, pts[i+1], magB, compB, tangent, ranges, first);
            } else if (cfg.style == 1) {
                for (int s = 0; s < kTubeSegments; ++s) {
                    float a0 = 2.0f * (float)M_PI * s / kTubeSegments;
                    float a1 = 2.0f * (float)M_PI * (s+1) / kTubeSegments;
                    glm::vec3 rad0 = frame.normal * std::cos(a0) + frame.binormal * std::sin(a0);
                    glm::vec3 rad1 = frame.normal * std::cos(a1) + frame.binormal * std::sin(a1);
                    glm::vec3 a_0 = pts[i] + rad0*rA, a_1 = pts[i] + rad1*rA;
                    glm::vec3 b_0 = pts[i+1] + rad0*rB, b_1 = pts[i+1] + rad1*rB;
                    pushVert(out.verts, a_0, magA, compA, rad0, ranges, first);
                    pushVert(out.verts, b_0, magB, compB, rad0, ranges, first);
                    pushVert(out.verts, a_1, magA, compA, rad1, ranges, first);
                    pushVert(out.verts, a_1, magA, compA, rad1, ranges, first);
                    pushVert(out.verts, b_0, magB, compB, rad0, ranges, first);
                    pushVert(out.verts, b_1, magB, compB, rad1, ranges, first);
                }
            } else {
                glm::vec3 side = frame.normal;
                glm::vec3 aL = pts[i] - side*rA, aR = pts[i] + side*rA;
                glm::vec3 bL = pts[i+1] - side*rB, bR = pts[i+1] + side*rB;
                pushVert(out.verts, aL, magA, compA, frame.binormal, ranges, first);
                pushVert(out.verts, bL, magB, compB, frame.binormal, ranges, first);
                pushVert(out.verts, aR, magA, compA, frame.binormal, ranges, first);
                pushVert(out.verts, aR, magA, compA, frame.binormal, ranges, first);
                pushVert(out.verts, bL, magB, compB, frame.binormal, ranges, first);
                pushVert(out.verts, bR, magB, compB, frame.binormal, ranges, first);
            }
        }
    }
    out.lineCount = (int)(out.verts.size() / 10);
    out.ranges = ranges;

    // Arrowheads: evenly spaced by arc length (placement logic preserved
    // from computeArrowPlacement, inlined here over stored arcLength).
    if (cfg.showArrows && cfg.arrowSpacingFrac > 0.0f && cfg.extent > 0.0f) {
        const float arrowHeight = cfg.arrowSize * cfg.extent;
        const float arrowRadius = arrowHeight * 0.35f;
        for (const auto& path : data.paths) {
            float total = path.totalLength;
            if (!(total > 0.0f) || total < 2.0f * arrowHeight) continue;
            float target = std::max(cfg.arrowSpacingFrac * cfg.extent, 2.0f * arrowHeight);
            float margin = arrowHeight * (1.0f + taper);
            float usable = total - 2.0f * margin;
            if (usable <= 0.0f) continue;
            int count = std::max(1, (int)std::lround(usable / target));
            std::vector<float> positions;
            if (count == 1) positions.push_back(0.5f * total);
            else {
                float actual = usable / count;
                for (int k = 0; k < count; ++k) positions.push_back(margin + actual * k);
            }
            size_t seg = 0;
            for (float s : positions) {
                while (seg + 2 < path.points.size() && path.arcLength[seg+1] < s) ++seg;
                float span = std::max(path.arcLength[seg+1] - path.arcLength[seg], 1e-12f);
                float frac = std::clamp((s - path.arcLength[seg]) / span, 0.0f, 1.0f);
                glm::vec3 pos = glm::mix(path.points[seg], path.points[seg+1], frac);
                float mag = glm::mix(path.speed[seg], path.speed[seg+1], frac);
                if (mag <= 1e-12f) continue;
                // Tangent as direction (field direction == path direction for
                // forward-integrated paths; magnitude carries the colour).
                glm::vec3 fv = glm::mix(path.fieldVecs[seg], path.fieldVecs[seg+1], frac);
                glm::vec3 dir = glm::normalize(fv);
                // Cone: apex ahead, base behind, 16 segments, normals as before.
                Frame fr = initialFrame(dir);
                glm::vec3 apex = pos + fr.tangent * arrowHeight;
                glm::vec3 baseC = pos - fr.tangent * (arrowHeight * 0.15f);
                std::vector<glm::vec3> ring;
                for (int i = 0; i <= 16; ++i) {
                    float a = 2.0f * (float)M_PI * i / 16;
                    ring.push_back(baseC + (fr.normal * std::cos(a) + fr.binormal * std::sin(a)) * arrowRadius);
                }
                float slant = std::atan2(arrowRadius, arrowHeight);
                auto pushA = [&](glm::vec3 p, glm::vec3 n) {
                    out.arrowVerts.push_back(p.x); out.arrowVerts.push_back(p.y); out.arrowVerts.push_back(p.z);
                    out.arrowVerts.push_back(mag);
                    out.arrowVerts.push_back(fv.x); out.arrowVerts.push_back(fv.y); out.arrowVerts.push_back(fv.z);
                    out.arrowVerts.push_back(n.x); out.arrowVerts.push_back(n.y); out.arrowVerts.push_back(n.z);
                };
                for (int i = 0; i < 16; ++i) {
                    float mid = 2.0f * (float)M_PI * (i + 0.5f) / 16;
                    glm::vec3 radial = glm::normalize(fr.normal * std::cos(mid) + fr.binormal * std::sin(mid));
                    glm::vec3 fn = glm::normalize(radial * std::cos(slant) + fr.tangent * std::sin(slant));
                    pushA(apex, fn); pushA(ring[i], fn); pushA(ring[i+1], fn);
                }
                for (int i = 0; i < 16; ++i) { pushA(baseC, -fr.tangent); pushA(ring[i], -fr.tangent); pushA(ring[i+1], -fr.tangent); }
            }
        }
        out.arrowCount = (int)(out.arrowVerts.size() / 10);
    }
    return out;
}

} // namespace streamline
