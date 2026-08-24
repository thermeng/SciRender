#pragma once

#include <QLocale>
#include <QString>

// Adaptive number formatting for colorbar tick labels.
//
// Replaces fixed-precision `QString::number(v, 'f', 3)` (which always emits
// trailing zeros, e.g. "1024.000", "0.001") with compact general notation:
//   1024  instead of 1024.000
//   0.001 instead of 0.001
//   1.5e6 for large magnitudes
// Shorter labels = less visual noise over the colorbar.
inline QString formatLabel(double value, int sigDigits = 6) {
    if (value == 0.0) return "0";
    // 'g' = general format, `sigDigits` significant digits. The C locale keeps
    // '.' as the decimal separator regardless of the host system locale.
    QString s = QLocale::c().toString(value, 'g', qBound(1, sigDigits, 15));
    // Compact scientific notation: Qt emits "1.5e+06"; drop the '+' and any
    // leading zeros in the exponent so it reads "1.5e6" / "1.5e-6".
    const int e = s.indexOf('e');
    if (e >= 0) {
        const QString mant = s.left(e);
        QString exp = s.mid(e + 1);
        bool neg = false;
        int i = 0;
        if (i < exp.size() && exp[i] == '+') { ++i; }
        else if (i < exp.size() && exp[i] == '-') { neg = true; ++i; }
        while (i < exp.size() && exp[i] == '0') ++i;
        if (i >= exp.size()) exp = "0";
        else exp = exp.mid(i);
        s = mant + "e" + (neg ? "-" : "") + exp;
    }
    return s;
}