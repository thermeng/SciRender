#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <string>

namespace tf {

struct Point {
    float pos = 0.0f;   // normalized scalar position 0..1 (tVal)
    float alpha = 0.0f; // opacity multiplier 0..1
    bool operator==(const Point& o) const { return pos==o.pos && alpha==o.alpha; }
    bool operator!=(const Point& o) const { return !(*this==o); }
};

using Curve = std::vector<Point>;

// Default ramp: 0->0, 1->1
inline Curve defaultCurve() {
    return { {0.0f, 0.0f}, {1.0f, 1.0f} };
}
inline Curve rampCurve() { return defaultCurve(); }
inline Curve stepCurve() {
    return { {0.0f,0.0f}, {0.45f,0.0f}, {0.5f,1.0f}, {1.0f,1.0f} };
}
inline Curve linearLowCurve() {
    return { {0.0f,0.0f}, {0.3f,0.0f}, {0.7f,0.6f}, {1.0f,1.0f} };
}

// Keep sorted by pos, clamp 0..1, deduplicate pos with last wins
inline void normalize(Curve& c) {
    for (auto& p: c) { p.pos = std::clamp(p.pos, 0.0f, 1.0f); p.alpha = std::clamp(p.alpha, 0.0f, 1.0f); }
    std::sort(c.begin(), c.end(), [](const Point& a, const Point& b){ return a.pos < b.pos; });
    // deduplicate
    Curve out;
    out.reserve(c.size());
    for (auto& p: c) {
        if (!out.empty() && std::abs(out.back().pos - p.pos) < 1e-6f) out.back().alpha = p.alpha;
        else out.push_back(p);
    }
    c.swap(out);
    if (c.empty()) c = defaultCurve();
    if (c.size()==1) c.push_back({1.0f, c[0].alpha});
    // ensure endpoints at 0 and 1
    if (c.front().pos > 1e-6f) c.insert(c.begin(), {0.0f, c.front().alpha});
    if (c.back().pos < 1.0f - 1e-6f) c.push_back({1.0f, c.back().alpha});
}

inline float sample(const Curve& c, float t) {
    if (c.empty()) return std::clamp(t,0.0f,1.0f);
    if (c.size()==1) return c[0].alpha;
    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= c.front().pos) return c.front().alpha;
    if (t >= c.back().pos) return c.back().alpha;
    for (size_t i=1;i<c.size();++i) {
        if (t <= c[i].pos) {
            float span = c[i].pos - c[i-1].pos;
            if (span < 1e-8f) return c[i].alpha;
            float f = (t - c[i-1].pos)/span;
            return c[i-1].alpha + f*(c[i].alpha - c[i-1].alpha);
        }
    }
    return c.back().alpha;
}

// Bake to 256 u8 alphas for 1D texture upload
inline std::vector<uint8_t> bake256(const Curve& c) {
    std::vector<uint8_t> out(256);
    for (int i=0;i<256;++i) {
        float t = static_cast<float>(i)/255.0f;
        float a = sample(c, t);
        out[i] = static_cast<uint8_t>(std::clamp<int>(int(std::round(a*255.0f)),0,255));
    }
    return out;
}

// Serialize to string "x:a,x:a" for QSettings
inline std::string serialize(const Curve& c) {
    std::string s;
    for (size_t i=0;i<c.size();++i) {
        if (i) s += ",";
        s += std::to_string(c[i].pos) + ":" + std::to_string(c[i].alpha);
    }
    return s;
}
inline Curve deserialize(const std::string& s) {
    Curve c;
    size_t start=0;
    while (start < s.size()) {
        size_t comma = s.find(',', start);
        std::string token = s.substr(start, comma==std::string::npos? std::string::npos : comma-start);
        size_t colon = token.find(':');
        if (colon!=std::string::npos) {
            try { float p = std::stof(token.substr(0,colon)); float a = std::stof(token.substr(colon+1)); c.push_back({p,a}); } catch(...) {}
        }
        if (comma==std::string::npos) break;
        start = comma+1;
    }
    if (c.empty()) return defaultCurve();
    normalize(c);
    return c;
}

} // namespace tf
