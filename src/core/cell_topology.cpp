#include "core/cell_topology.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace cell_topology {

namespace {

using Point = std::array<double, 3>;

constexpr double kEps = 1e-24;

Point sub(const Point& a, const Point& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const Point& a, const Point& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double norm2(const Point& a) { return dot(a, a); }

Point cross(const Point& a, const Point& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double distance2(const Point& a, const Point& b) { return norm2(sub(a, b)); }

Point centroidOf(const std::vector<std::size_t>& loop, const std::vector<Point>& points) {
    Point c{0.0, 0.0, 0.0};
    for (std::size_t v : loop) {
        c[0] += points[v][0];
        c[1] += points[v][1];
        c[2] += points[v][2];
    }
    if (!loop.empty()) {
        const double inv = 1.0 / static_cast<double>(loop.size());
        c[0] *= inv;
        c[1] *= inv;
        c[2] *= inv;
    }
    return c;
}

// Newell's method: robust for slightly non-planar polygons.
Point newellNormal(const std::vector<std::size_t>& loop, const std::vector<Point>& points) {
    Point n{0.0, 0.0, 0.0};
    const std::size_t m = loop.size();
    for (std::size_t i = 0; i < m; ++i) {
        const Point& a = points[loop[i]];
        const Point& b = points[loop[(i + 1) % m]];
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    return n;
}

bool hasDuplicate(const std::vector<std::size_t>& loop) {
    std::vector<std::size_t> s = loop;
    std::sort(s.begin(), s.end());
    return std::adjacent_find(s.begin(), s.end()) != s.end();
}

bool hasVertex(const std::vector<std::size_t>& loop, std::size_t v) {
    return std::find(loop.begin(), loop.end(), v) != loop.end();
}

bool disjoint(const std::vector<std::size_t>& a, const std::vector<std::size_t>& b) {
    for (std::size_t v : a) {
        if (hasVertex(b, v)) return false;
    }
    return true;
}

// Order `loop` counter-clockwise about `axis` (right-hand rule). The stored
// face loops may be oriented either way, so the sign of the face normal
// decides whether to reverse. A degenerate normal falls back to an angular
// sort around the centroid in a basis perpendicular to `axis`.
void orderAboutAxis(std::vector<std::size_t>& loop, const Point& axis,
                    const std::vector<Point>& points) {
    if (loop.size() < 3) return;

    const Point n = newellNormal(loop, points);
    const double d = dot(n, axis);
    if (std::fabs(d) > kEps * std::sqrt(norm2(n) * norm2(axis))) {
        if (d < 0.0) std::reverse(loop.begin(), loop.end());
        return;
    }

    // Pick a basis perpendicular to `axis`.
    Point u = (std::fabs(axis[0]) < 0.9) ? Point{1.0, 0.0, 0.0} : Point{0.0, 1.0, 0.0};
    Point v = cross(axis, u);
    double vn = std::sqrt(norm2(v));
    if (vn < 1e-15) return;
    v[0] /= vn; v[1] /= vn; v[2] /= vn;
    u = cross(v, axis);
    double un = std::sqrt(norm2(u));
    if (un < 1e-15) return;
    u[0] /= un; u[1] /= un; u[2] /= un;

    const Point c = centroidOf(loop, points);
    std::sort(loop.begin(), loop.end(), [&](std::size_t a, std::size_t b) {
        const Point da = sub(points[a], c);
        const Point db = sub(points[b], c);
        const double aa = std::atan2(dot(da, v), dot(da, u));
        const double ab = std::atan2(dot(db, v), dot(db, u));
        return aa < ab;
    });
}

// Match each vertex of `a` to its nearest unused vertex of `b`. For a convex
// cell the corresponding bottom/top vertices are the closest pair.
bool pairLoops(const std::vector<std::size_t>& a, const std::vector<std::size_t>& b,
               const std::vector<Point>& points, std::vector<std::size_t>& paired) {
    if (a.size() != b.size()) return false;
    paired.clear();
    paired.reserve(a.size());
    std::vector<bool> used(b.size(), false);
    for (std::size_t av : a) {
        std::size_t best = std::numeric_limits<std::size_t>::max();
        double bestD = std::numeric_limits<double>::infinity();
        for (std::size_t j = 0; j < b.size(); ++j) {
            if (used[j]) continue;
            const double d = distance2(points[av], points[b[j]]);
            if (d < bestD) { bestD = d; best = j; }
        }
        if (best == std::numeric_limits<std::size_t>::max()) return false;
        used[best] = true;
        paired.push_back(b[best]);
    }
    return true;
}

// A side face of a hex/wedge must join one directed edge of `bottom` to the
// matching directed edge of `topPaired`. This rejects a nearest-vertex pairing
// that does not actually reproduce the cell's face topology.
bool validSideFace(const std::vector<std::size_t>& side,
                   const std::vector<std::size_t>& bottom,
                   const std::vector<std::size_t>& topPaired) {
    std::vector<std::size_t> bv, tv;
    for (std::size_t v : side) {
        if (hasVertex(bottom, v)) bv.push_back(v);
        else if (hasVertex(topPaired, v)) tv.push_back(v);
        else return false;
    }
    if (bv.size() != 2 || tv.size() != 2) return false;

    const std::size_t n = bottom.size();
    int edge = -1;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t a = bottom[i];
        const std::size_t b = bottom[(i + 1) % n];
        if ((bv[0] == a && bv[1] == b) || (bv[0] == b && bv[1] == a)) {
            edge = static_cast<int>(i);
            break;
        }
    }
    if (edge < 0) return false;

    const std::size_t t0 = topPaired[static_cast<std::size_t>(edge)];
    const std::size_t t1 = topPaired[(static_cast<std::size_t>(edge) + 1) % n];
    return (tv[0] == t0 && tv[1] == t1) || (tv[0] == t1 && tv[1] == t0);
}

bool validPermutation(const std::vector<std::size_t>& out,
                      const std::vector<std::size_t>& verts) {
    if (out.size() != verts.size()) return false;
    std::vector<std::size_t> a = out;
    std::vector<std::size_t> b = verts;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b && std::adjacent_find(a.begin(), a.end()) == a.end();
}

std::vector<std::size_t> canonicalTet(std::vector<std::size_t> v,
                                      const std::vector<Point>& points) {
    if (v.size() != 4) return {};
    const Point e1 = sub(points[v[1]], points[v[0]]);
    const Point e2 = sub(points[v[2]], points[v[0]]);
    const Point e3 = sub(points[v[3]], points[v[0]]);
    if (dot(cross(e1, e2), e3) < 0.0) std::swap(v[1], v[2]);
    return v;
}

std::vector<std::size_t> canonicalHex(const std::vector<std::size_t>& verts,
                                      const std::vector<std::vector<std::size_t>>& loops,
                                      const std::vector<Point>& points) {
    std::vector<std::size_t> quads;
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if (loops[i].size() == 4 && !hasDuplicate(loops[i])) quads.push_back(i);
    }
    if (quads.size() != 6) return {};

    std::size_t bi = std::numeric_limits<std::size_t>::max();
    std::size_t ti = std::numeric_limits<std::size_t>::max();
    for (std::size_t a = 0; a < quads.size() && ti == std::numeric_limits<std::size_t>::max(); ++a) {
        for (std::size_t b = a + 1; b < quads.size(); ++b) {
            if (disjoint(loops[quads[a]], loops[quads[b]])) {
                bi = quads[a];
                ti = quads[b];
                break;
            }
        }
    }
    if (ti == std::numeric_limits<std::size_t>::max()) return {};

    std::vector<std::size_t> bottom = loops[bi];
    std::vector<std::size_t> top = loops[ti];
    const Point cb = centroidOf(bottom, points);
    const Point ct = centroidOf(top, points);
    const Point axis = sub(ct, cb);
    if (norm2(axis) < kEps) return {};

    orderAboutAxis(bottom, axis, points);
    orderAboutAxis(top, axis, points);

    std::vector<std::size_t> topPaired;
    if (!pairLoops(bottom, top, points, topPaired)) return {};

    for (std::size_t i : quads) {
        if (i == bi || i == ti) continue;
        if (!validSideFace(loops[i], bottom, topPaired)) return {};
    }

    std::vector<std::size_t> out = bottom;
    out.insert(out.end(), topPaired.begin(), topPaired.end());
    return validPermutation(out, verts) ? out : std::vector<std::size_t>{};
}

std::vector<std::size_t> canonicalWedge(const std::vector<std::size_t>& verts,
                                        const std::vector<std::vector<std::size_t>>& loops,
                                        const std::vector<Point>& points) {
    std::vector<std::size_t> tris;
    std::vector<std::size_t> quads;
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if (loops[i].size() == 3 && !hasDuplicate(loops[i])) tris.push_back(i);
        else if (loops[i].size() == 4 && !hasDuplicate(loops[i])) quads.push_back(i);
    }
    if (tris.size() != 2 || quads.size() != 3) return {};

    std::vector<std::size_t> a = loops[tris[0]];
    std::vector<std::size_t> b = loops[tris[1]];
    if (!disjoint(a, b)) return {};

    const Point ca = centroidOf(a, points);
    const Point cb = centroidOf(b, points);
    const Point axis = sub(cb, ca);
    if (norm2(axis) < kEps) return {};

    orderAboutAxis(a, axis, points);
    orderAboutAxis(b, axis, points);

    std::vector<std::size_t> bPaired;
    if (!pairLoops(a, b, points, bPaired)) return {};

    for (std::size_t qi : quads) {
        if (!validSideFace(loops[qi], a, bPaired)) return {};
    }

    std::vector<std::size_t> out = a;
    out.insert(out.end(), bPaired.begin(), bPaired.end());
    return validPermutation(out, verts) ? out : std::vector<std::size_t>{};
}

std::vector<std::size_t> canonicalPyramid(const std::vector<std::size_t>& verts,
                                          const std::vector<std::vector<std::size_t>>& loops,
                                          const std::vector<Point>& points) {
    std::size_t baseIdx = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> tris;
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if (loops[i].size() == 4 && !hasDuplicate(loops[i])) {
            if (baseIdx != std::numeric_limits<std::size_t>::max()) return {};
            baseIdx = i;
        } else if (loops[i].size() == 3 && !hasDuplicate(loops[i])) {
            tris.push_back(i);
        }
    }
    if (baseIdx == std::numeric_limits<std::size_t>::max() || tris.size() != 4) return {};

    std::vector<std::size_t> base = loops[baseIdx];
    std::size_t apex = std::numeric_limits<std::size_t>::max();
    for (std::size_t v : verts) {
        if (!hasVertex(base, v)) {
            if (apex != std::numeric_limits<std::size_t>::max()) return {};
            apex = v;
        }
    }
    if (apex == std::numeric_limits<std::size_t>::max()) return {};

    const Point axis = sub(points[apex], centroidOf(base, points));
    if (norm2(axis) < kEps) return {};
    orderAboutAxis(base, axis, points);

    for (std::size_t ti : tris) {
        if (!hasVertex(loops[ti], apex)) return {};
        std::vector<std::size_t> edge;
        for (std::size_t v : loops[ti]) {
            if (v != apex) edge.push_back(v);
        }
        if (edge.size() != 2) return {};
        bool adjacent = false;
        for (std::size_t i = 0; i < 4; ++i) {
            const std::size_t a = base[i];
            const std::size_t b = base[(i + 1) % 4];
            if ((edge[0] == a && edge[1] == b) || (edge[0] == b && edge[1] == a)) {
                adjacent = true;
                break;
            }
        }
        if (!adjacent) return {};
    }

    std::vector<std::size_t> out = base;
    out.push_back(apex);
    return validPermutation(out, verts) ? out : std::vector<std::size_t>{};
}

const std::vector<std::vector<int>> kTetraFaces = {
    {0, 1, 2}, {0, 1, 3}, {1, 2, 3}, {0, 2, 3},
};
const std::vector<std::vector<int>> kHexFaces = {
    {0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4},
    {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7},
};
const std::vector<std::vector<int>> kWedgeFaces = {
    {0, 1, 2}, {3, 4, 5}, {0, 1, 4, 3}, {1, 2, 5, 4}, {2, 0, 3, 5},
};
const std::vector<std::vector<int>> kPyramidFaces = {
    {0, 1, 2, 3}, {0, 1, 4}, {1, 2, 4}, {2, 3, 4}, {3, 0, 4},
};
const std::vector<std::vector<int>> kNoFaces;

} // namespace

std::uint8_t classify(const std::vector<std::vector<std::size_t>>& faceLoops) {
    int tris = 0;
    int quads = 0;
    for (const auto& loop : faceLoops) {
        if (loop.size() == 3) ++tris;
        else if (loop.size() == 4) ++quads;
    }
    return classifyShape(static_cast<int>(faceLoops.size()), tris, quads);
}

std::uint8_t classifyShape(int faceCount, int triCount, int quadCount) {
    if (faceCount == 4 && triCount == 4) return kTetra;
    if (faceCount == 6 && quadCount == 6) return kHexahedron;
    if (faceCount == 5 && triCount == 2 && quadCount == 3) return kWedge;
    if (faceCount == 5 && triCount == 4 && quadCount == 1) return kPyramid;
    return kPolyhedron;
}

bool isVolumeCell(std::uint8_t type) {
    return type == kTetra || type == kHexahedron || type == kWedge || type == kPyramid;
}

int cornerCount(std::uint8_t type) {
    switch (type) {
        case kTetra: return 4;
        case kHexahedron: return 8;
        case kWedge: return 6;
        case kPyramid: return 5;
        default: return 0;
    }
}

const std::vector<std::vector<int>>& faces(std::uint8_t type) {
    switch (type) {
        case kTetra: return kTetraFaces;
        case kHexahedron: return kHexFaces;
        case kWedge: return kWedgeFaces;
        case kPyramid: return kPyramidFaces;
        default: return kNoFaces;
    }
}

std::uint8_t canonicalize(std::vector<std::size_t>& verts,
                          const std::vector<std::vector<std::size_t>>& faceLoops,
                          const std::vector<std::array<double, 3>>& points) {
    const std::uint8_t type = classify(faceLoops);
    if (!isVolumeCell(type)) return kPolyhedron;
    if (verts.size() != static_cast<std::size_t>(cornerCount(type))) return kPolyhedron;
    for (std::size_t v : verts) {
        if (v >= points.size()) return kPolyhedron;
    }

    std::vector<std::size_t> out;
    switch (type) {
        case kTetra: out = canonicalTet(verts, points); break;
        case kHexahedron: out = canonicalHex(verts, faceLoops, points); break;
        case kWedge: out = canonicalWedge(verts, faceLoops, points); break;
        case kPyramid: out = canonicalPyramid(verts, faceLoops, points); break;
        default: return kPolyhedron;
    }
    if (out.empty()) return kPolyhedron;

    verts = std::move(out);
    return type;
}

void tets(std::uint8_t type, std::span<const std::uint32_t> cellVerts,
          std::vector<std::array<std::uint32_t, 4>>& out) {
    if (!isVolumeCell(type)) return;
    if (cellVerts.size() < static_cast<std::size_t>(cornerCount(type))) return;

    if (type == kTetra) {
        out.push_back({0, 1, 2, 3});
        return;
    }

    const auto& cellFaces = faces(type);
    const auto center = static_cast<std::uint32_t>(cornerCount(type));
    for (const auto& face : cellFaces) {
        const std::size_t n = face.size();
        if (n < 3) continue;

        // Rotate to the smallest global id: both incident cells then fan the
        // shared face from the same corner and along the same cycle, so the
        // triangle sets match (possibly with opposite winding).
        std::size_t start = 0;
        for (std::size_t k = 1; k < n; ++k) {
            if (cellVerts[static_cast<std::size_t>(face[k])] <
                cellVerts[static_cast<std::size_t>(face[start])]) {
                start = k;
            }
        }

        for (std::size_t k = 1; k + 1 < n; ++k) {
            out.push_back({
                static_cast<std::uint32_t>(face[start]),
                static_cast<std::uint32_t>(face[(start + k) % n]),
                static_cast<std::uint32_t>(face[(start + k + 1) % n]),
                center,
            });
        }
    }
}

} // namespace cell_topology
