#pragma once
#include <QString>
#include <cmath>

// Shared numeric label formatting. Mirrors the Probe page's "Format" modes
// (RenderSettings::getProbeFormat): 0=Integer, 1=Float (4 significant digits),
// 2=Exponent. Both the Probe value table and the plot canvas tick labels use
// this single implementation so numbers read consistently across the app.
inline QString formatProbeValue(double v, int fmt) {
    if (fmt == 0) return QString::number(int(std::lround(v)));
    if (fmt == 2) return QString::number(v, 'e', 6);
    return QString::number(v, 'g', 4);
}
