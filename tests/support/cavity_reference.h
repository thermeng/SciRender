#pragma once

#include "field/Field.h"
#include "mesh/Mesh.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace test_support {

// The lid-driven cavity benchmark of Ghia, Giri & Podkar (1982): the
// velocity component along the vertical centre line of a square cavity whose
// top wall moves with unit speed.
struct CavityRefPoint {
    double y;
    double u;
};

// Ghia et al. (1982), Table I: Re = 100.
inline const std::vector<CavityRefPoint>& ghia100() {
    static const std::vector<CavityRefPoint> r = {
        {1.0000, 1.00000}, {0.9766, 0.75837}, {0.9688, 0.68439}, {0.9609, 0.61756},
        {0.9531, 0.55892}, {0.8516, 0.29093}, {0.7344, 0.16256}, {0.6172, 0.02135},
        {0.5000, -0.11477}, {0.4531, -0.17119}, {0.2813, -0.32726}, {0.1719, -0.24299},
        {0.1016, -0.14612}, {0.0703, -0.10338}, {0.0625, -0.09266}, {0.0547, -0.08186},
        {0.0000, 0.00000},
    };
    return r;
}

// Ghia et al. (1982), Table III: Re = 400.
inline const std::vector<CavityRefPoint>& ghia400() {
    static const std::vector<CavityRefPoint> r = {
        {1.0000, 1.00000}, {0.9766, 0.75024}, {0.9688, 0.70528}, {0.9609, 0.65308},
        {0.9531, 0.59213}, {0.8516, 0.30930}, {0.7344, 0.17122}, {0.6172, 0.02401},
        {0.5000, -0.10333}, {0.4531, -0.14613}, {0.2813, -0.32275}, {0.1719, -0.24925},
        {0.1016, -0.15809}, {0.0703, -0.11812}, {0.0625, -0.10688}, {0.0547, -0.09544},
        {0.0000, 0.00000},
    };
    return r;
}

// Samples a cell-centred field along the vertical line x = xCentre at the
// given heights.
//
// The sample is an inverse-distance weighted average of the four nearest cell
// centres. It reproduces a linear field exactly and is second-order accurate
// otherwise - which matters on the graded cavity mesh, where the cell sizes
// change by an order of magnitude between the wall and the core.
template <std::size_t Dim>
std::vector<double> sampleCentreline(const fvm::Mesh<Dim>& mesh, const fvm::Field<Dim>& u,
                                     double xCentre, const std::vector<double>& heights,
                                     int neighbours = 4) {
    std::vector<std::size_t> order(mesh.nCells());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;

    std::vector<double> out;
    out.reserve(heights.size());
    for (double y : heights) {
        // The four nearest cell centres (by squared distance in the plane).
        std::partial_sort(order.begin(), order.begin() + std::min<std::size_t>(neighbours, order.size()),
                           order.end(), [&](std::size_t a, std::size_t b) {
                               const auto ca = mesh.cellCenter(a);
                               const auto cb = mesh.cellCenter(b);
                               const double da = (ca[0] - xCentre) * (ca[0] - xCentre) +
                                                (ca[1] - y) * (ca[1] - y);
                               const double db = (cb[0] - xCentre) * (cb[0] - xCentre) +
                                                (cb[1] - y) * (cb[1] - y);
                               return da < db;
                           });

        double num = 0.0;
        double den = 0.0;
        for (std::size_t k = 0; k < std::min<std::size_t>(neighbours, order.size()); ++k) {
            const std::size_t c = order[k];
            const auto cc = mesh.cellCenter(c);
            const double dx = cc[0] - xCentre;
            const double dy = cc[1] - y;
            const double d2 = dx * dx + dy * dy;
            const double w = 1.0 / std::max(d2, 1e-30);
            num += w * u[c];
            den += w;
        }
        out.push_back(den > 0.0 ? num / den : 0.0);
    }
    return out;
}

// How a computed centreline profile compares with a published table.
struct CavityComparison {
    double maxError = 0.0;   // worst |u - u_ref|
    double l2 = 0.0;         // RMS error
    double correlation = 0.0; // Pearson correlation with the reference
    double zeroCrossing = 0.0; // height where the profile changes sign
    double referenceZeroCrossing = 0.0;
};

inline CavityComparison compareProfile(const std::vector<double>& sampled,
                                       const std::vector<CavityRefPoint>& ref) {
    CavityComparison c;
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (std::size_t i = 0; i < sampled.size() && i < ref.size(); ++i) {
        const double a = sampled[i];
        const double b = ref[i].u;
        c.maxError = std::max(c.maxError, std::fabs(a - b));
        c.l2 += (a - b) * (a - b);
        sx += a; sy += b; sxx += a * a; syy += b * b; sxy += a * b;
    }
    const double n = static_cast<double>(std::min(sampled.size(), ref.size()));
    if (n > 0) c.l2 = std::sqrt(c.l2 / n);

    const double cov = sxy / n - (sx / n) * (sy / n);
    const double varA = sxx / n - (sx / n) * (sx / n);
    const double varB = syy / n - (sy / n) * (sy / n);
    c.correlation = cov / std::sqrt(std::max(varA * varB, 1e-300));

    // The height where the profile changes sign, scanning downwards from the
    // lid: the centre of the primary vortex.
    for (std::size_t i = 1; i < sampled.size() && i < ref.size(); ++i) {
        const double u0 = sampled[i - 1];
        const double u1 = sampled[i];
        if (u0 > 0.0 && u1 <= 0.0) {
            const double t = u0 / (u0 - u1);
            c.zeroCrossing = ref[i - 1].y + t * (ref[i].y - ref[i - 1].y);
            break;
        }
    }
    for (std::size_t i = 1; i < ref.size(); ++i) {
        const double u0 = ref[i - 1].u;
        const double u1 = ref[i].u;
        if (u0 > 0.0 && u1 <= 0.0) {
            const double t = u0 / (u0 - u1);
            c.referenceZeroCrossing = ref[i - 1].y + t * (ref[i].y - ref[i - 1].y);
            break;
        }
    }
    return c;
}

} // namespace test_support
