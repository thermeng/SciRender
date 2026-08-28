#include "ui/range_editor.h"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QShortcut>
#include <QKeyEvent>

RangeEditor::RangeEditor(QWidget* parent) : QWidget(parent) {
    m_minSpin = new QDoubleSpinBox(this);
    m_maxSpin = new QDoubleSpinBox(this);
    m_resetBtn = new QPushButton("Reset", this);

    for (auto* sp : {m_minSpin, m_maxSpin}) {
        sp->setDecimals(3);
        sp->setRange(-1e12, 1e12);
        sp->setSingleStep(0.1);
        sp->setCorrectionMode(QDoubleSpinBox::CorrectToNearestValue);
        sp->setButtonSymbols(QDoubleSpinBox::UpDownArrows);
        sp->setMinimumWidth(56);
        sp->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QFont f = sp->font();
        f.setFamily("JetBrains Mono");
        f.setStyleHint(QFont::Monospace);
        f.setPixelSize(11);
        sp->setFont(f);
    }

    m_resetBtn->setToolTip("Reset to full data range");
    m_resetBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);
    lay->addWidget(m_minSpin);
    lay->addWidget(m_maxSpin);
    lay->addWidget(m_resetBtn);

    connect(m_minSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (v > m_maxSpin->value()) m_maxSpin->setValue(v);
                syncFromSpinboxes();
            });
    connect(m_maxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (v < m_minSpin->value()) m_minSpin->setValue(v);
                syncFromSpinboxes();
            });
    connect(m_resetBtn, &QPushButton::clicked, this, [this]() {
        setWindow(m_boundLo, m_boundHi);
        syncFromSpinboxes();
    });
}

void RangeEditor::setBounds(double lo, double hi) {
    if (hi < lo) std::swap(lo, hi);
    m_boundLo = lo;
    m_boundHi = hi;
    for (auto* sp : {m_minSpin, m_maxSpin}) {
        sp->blockSignals(true);
        sp->setRange(lo, hi);
        sp->blockSignals(false);
    }
}

void RangeEditor::setWindow(double lo, double hi) {
    lo = std::max(m_boundLo, std::min(m_boundHi, lo));
    hi = std::max(m_boundLo, std::min(m_boundHi, hi));
    if (lo > hi) std::swap(lo, hi);
    for (auto* sp : {m_minSpin, m_maxSpin}) sp->blockSignals(true);
    m_minSpin->setValue(lo);
    m_maxSpin->setValue(hi);
    for (auto* sp : {m_minSpin, m_maxSpin}) sp->blockSignals(false);
}

std::pair<double, double> RangeEditor::window() const {
    return {m_minSpin->value(), m_maxSpin->value()};
}

void RangeEditor::setEnabled(bool enabled) {
    QWidget::setEnabled(enabled);
    if (m_minSpin) m_minSpin->setEnabled(enabled);
    if (m_maxSpin) m_maxSpin->setEnabled(enabled);
    if (m_resetBtn) m_resetBtn->setEnabled(enabled);
}

void RangeEditor::syncFromSpinboxes() {
    emit windowEdited(m_minSpin->value(), m_maxSpin->value());
}
