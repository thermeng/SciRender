#pragma once

#include <QDoubleSpinBox>
#include <QString>

// QDoubleSpinBox that renders its value in scientific notation (for example
// "1.000e-03"). Used by tolerance-style fields whose values span many orders of
// magnitude and read poorly with a long run of leading zeros.
//
// The mantissa precision is fixed, so the widest text stays short. The spin
// box's own decimals() remains free to carry the value's representable
// precision and the arrow step, and is deliberately set higher than the
// mantissa so small values stay reachable.
class ScientificDoubleSpinBox : public QDoubleSpinBox {
public:
    explicit ScientificDoubleSpinBox(QWidget* parent = nullptr) : QDoubleSpinBox(parent) {}

protected:
    static constexpr int kMantissaDigits = 3;

    QString textFromValue(double value) const override {
        return QString::number(value, 'e', kMantissaDigits);
    }
    double valueFromText(const QString& text) const override { return text.toDouble(); }
};
