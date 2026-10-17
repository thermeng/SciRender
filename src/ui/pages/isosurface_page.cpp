#include "isosurface_page.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QPushButton>
#include <algorithm>
#include <cmath>

#include "ui_isosurface_page.h"
#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"

namespace sidebar {

IsosurfacePage::IsosurfacePage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    auto* content = new QWidget;
    Ui::IsosurfacePage isoUi;
    isoUi.setupUi(content);
    sb::fixLayoutOverflow(content);

    auto* s = S();

    // -- Isosurface enable checkbox --
    m_enableCb = isoUi.enableCb;
    m_enableCb->setChecked(s->getShowIsosurface());
    m_enableCb->setEnabled(s->getIsosurfaceAvailable() || s->hasVolumeData());
    if (!s->getIsosurfaceAvailable() && !s->hasVolumeData())
        m_enableCb->setChecked(false);
    connect(m_enableCb, &QCheckBox::toggled, s, &RenderSettings::setShowIsosurface);
    connect(m_enableCb, &QCheckBox::toggled, isoUi.optionsGroup, &QWidget::setEnabled);
    isoUi.optionsGroup->setEnabled(m_enableCb->isChecked() && m_enableCb->isEnabled());

    // -- Contour field (independent of the surface active scalar) --
    m_fieldCombo = isoUi.fieldCombo;
    m_fieldCombo->addItems(s->getAvailableScalars());
    m_fieldCombo->setCurrentText(s->getIsosurfaceFieldQml());
    m_fieldCombo->setEnabled(s->hasMeshScalars());
    m_fieldCombo->setMinimumWidth(sb::kSidebarWidth - sb::kNavWidthApprox - 20);
    connect(m_fieldCombo, &QComboBox::activated, s, [this, s](int idx) {
        s->setIsosurfaceField(m_fieldCombo->itemText(idx));
    });

    // -- Isovalue spin (exact entry, bounded by the contour field's range) --
    m_valueSpin = isoUi.valueSpin;
    refreshIsosurfaceValue();
    // legacy single-value path kept: editing spin updates selected contour if multi, else lone isovalue
    connect(m_valueSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        if (m_list && m_list->currentRow() >= 0) S()->setIsovalueAt(m_list->currentRow(), v);
        else S()->setIsovalue(v);
    });

    // --- Multi-contour list ---
    auto* isoListLabel = new QLabel("Contours");
    isoListLabel->setObjectName("isoListHeader");
    isoUi.optionsGroup->layout()->addWidget(isoListLabel);
    m_list = new QListWidget;
    m_list->setMinimumHeight(90);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    isoUi.optionsGroup->layout()->addWidget(m_list);
    refreshIsosurfaceList();
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0) return;
        auto vals = S()->getIsovalues();
        if (row < vals.size()) {
            double v = vals[row].toDouble();
            m_valueSpin->blockSignals(true);
            m_valueSpin->setValue(v);
            m_valueSpin->blockSignals(false);
        }
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        if (m_valueSpin) m_valueSpin->setFocus();
    });
    auto* isoBtnRow = new QWidget;
    auto* isoBtnLay = new QHBoxLayout(isoBtnRow);
    isoBtnLay->setContentsMargins(0, 0, 0, 0);
    auto* addBtn = new QPushButton("Add");
    m_removeBtn = new QPushButton("Remove");
    isoBtnLay->addWidget(addBtn);
    isoBtnLay->addWidget(m_removeBtn);
    isoUi.optionsGroup->layout()->addWidget(isoBtnRow);
    connect(addBtn, &QPushButton::clicked, this, [this, addBtn]() {
        double v = m_valueSpin ? m_valueSpin->value() : 0;
        auto [lo, hi] = S()->isosurfaceFieldRange();
        // propose slightly offset to avoid duplicate
        v = std::clamp(v + (hi - lo) * 0.05, static_cast<double>(lo), static_cast<double>(hi));
        S()->addIsovalue(v);
        refreshIsosurfaceList();
        if (m_list) m_list->setCurrentRow(m_list->count() - 1);
    });
    connect(m_removeBtn, &QPushButton::clicked, this, [this]() {
        int r = m_list ? m_list->currentRow() : -1;
        if (r >= 0) {
            S()->removeIsovalue(r);
            refreshIsosurfaceList();
        }
    });

    // -- Placement (Vertex / Cell Center) --
    auto* placementCombo = isoUi.placementCombo;
    placementCombo->addItems(s->getIsosurfacePlacementOptions());
    placementCombo->setCurrentIndex(s->getIsosurfacePlacement());
    placementCombo->setEnabled(s->hasIsosurfaceCellScalars());
    connect(placementCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            s, &RenderSettings::setIsosurfacePlacement);

    isoUi.optionsGroup->layout()->replaceWidget(
        isoUi.isoColorBtn,
        sb::detail::createColorButton(this, s, "Isosurface", s->getIsosurfaceColorQml(),
                                      &m_colorDialog, &RenderSettings::setIsosurfaceColorQml));
    delete isoUi.isoColorBtn;

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

    // Refresh from the former syncVolumePage (its isosurface half): placement
    // sync, availability gating, value/list refresh. Ran on every viewChanged.
    auto refreshPage = [this, s, placementCombo]() {
        if (m_enableCb) {
            const bool isoAvail = s->getIsosurfaceAvailable();
            m_enableCb->setEnabled(isoAvail || s->hasVolumeData());
            if (!isoAvail) m_enableCb->setChecked(false);
        }
        if (placementCombo) {
            placementCombo->blockSignals(true);
            placementCombo->setCurrentIndex(s->getIsosurfacePlacement());
            placementCombo->setEnabled(s->hasIsosurfaceCellScalars());
            placementCombo->blockSignals(false);
        }
        refreshIsosurfaceValue();
        refreshIsosurfaceList();
    };
    connect(s, &RenderSettings::viewChanged, this, refreshPage);
    connect(s, &RenderSettings::meshLoadStateChanged, this, refreshPage);
    // Combo rebuild + value/list refresh when mesh data changes.
    connect(s, &RenderSettings::meshDataUpdated, this, [this, s]() {
        if (m_fieldCombo) {
            m_fieldCombo->blockSignals(true);
            m_fieldCombo->clear();
            m_fieldCombo->addItems(s->getAvailableScalars());
            m_fieldCombo->setCurrentText(s->getIsosurfaceFieldQml());
            m_fieldCombo->setEnabled(s->hasMeshScalars());
            m_fieldCombo->blockSignals(false);
        }
        refreshIsosurfaceValue();
        refreshIsosurfaceList();
    });

    setContent(content);
}

void IsosurfacePage::refreshIsosurfaceValue() {
    auto* s = S();
    if (!m_valueSpin) return;
    auto [flo, fhi] = s->isosurfaceFieldRange();
    double lo = flo, hi = fhi;
    if (!(hi > lo)) hi = lo + 1.0;
    const double span = hi - lo;
    // Step ~= span/200 with matching display decimals (2..6) for precision entry.
    double step = span / 200.0;
    int decimals = 4;
    if (step > 0.0 && std::isfinite(step)) {
        const double mag = std::floor(std::log10(step));
        step = std::pow(10.0, mag);
        decimals = std::clamp<int>(static_cast<int>(-mag), 2, 6);
    } else {
        step = 0.001;
    }
    // Self-heal stale state: clamp a drifted isovalue back into range once.
    if (s->getIsovalue() < lo || s->getIsovalue() > hi)
        s->setIsovalue(std::clamp(s->getIsovalue(), lo, hi));
    m_valueSpin->blockSignals(true);
    m_valueSpin->setDecimals(decimals);
    m_valueSpin->setRange(lo, hi);
    m_valueSpin->setSingleStep(step);
    double cur = s->getIsovalue();
    if (m_list && m_list->currentRow() >= 0) {
        auto vals = s->getIsovalues();
        if (m_list->currentRow() < vals.size()) cur = vals[m_list->currentRow()].toDouble();
    }
    m_valueSpin->setValue(cur);
    m_valueSpin->blockSignals(false);
}

void IsosurfacePage::refreshIsosurfaceList() {
    auto* s = S();
    if (!m_list) return;
    m_list->blockSignals(true);
    m_list->clear();
    auto vals = s->getIsovalues();
    for (auto& v : vals) m_list->addItem(QString::number(v.toDouble(), 'g', 6));
    if (m_list->count() > 0 && m_list->currentRow() < 0) m_list->setCurrentRow(0);
    if (m_removeBtn) m_removeBtn->setEnabled(m_list->count() > 1);
    m_list->blockSignals(false);
}

} // namespace sidebar
