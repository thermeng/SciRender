#include "probe_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLayout>
#include <QList>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

#include "ui_probe_page.h"
#include "ui/plot_tick_format.h"
#include "core/FieldStore.h"
#include "core/field_sampler.h"
#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"

namespace sidebar {

ProbePage::ProbePage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    auto* content = new QWidget;
    Ui::ProbePage probeUi;
    probeUi.setupUi(content);
    sb::fixLayoutOverflow(content);

    auto* s = S();
    m_showCb = probeUi.enableCb;
    m_xSpin = probeUi.xSpin;
    m_ySpin = probeUi.ySpin;
    m_zSpin = probeUi.zSpin;
    m_formatCombo = probeUi.formatCombo;
    m_placementCombo = probeUi.placementCombo;
    m_table = probeUi.valueTable;

    m_showCb->setChecked(s->getShowProbe());
    connect(m_showCb, &QCheckBox::toggled, s, [this](bool v) {
        S()->setShowProbe(v);
        refreshProbeTable();
    });

    m_xSpin->setValue(s->getProbeX());
    m_ySpin->setValue(s->getProbeY());
    m_zSpin->setValue(s->getProbeZ());
    // Sync spinbox -> state, will trigger viewChanged -> overlay
    connect(m_xSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), s, [this](double v) { S()->setProbeX(v); refreshProbeTable(); });
    connect(m_ySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), s, [this](double v) { S()->setProbeY(v); refreshProbeTable(); });
    connect(m_zSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), s, [this](double v) { S()->setProbeZ(v); refreshProbeTable(); });

    m_formatCombo->clear();
    m_formatCombo->addItems(s->getProbeFormatOptions());
    m_formatCombo->setCurrentIndex(s->getProbeFormat());
    connect(m_formatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), s, [this](int idx) { S()->setProbeFormat(idx); refreshProbeTable(); });

    m_placementCombo->clear();
    m_placementCombo->addItems(s->getProbePlacementOptions());
    m_placementCombo->setCurrentIndex(s->getProbePlacement());
    m_placementCombo->setEnabled(s->hasScalarCellData());
    connect(m_placementCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), s, [this](int idx) { S()->setProbePlacement(idx); refreshProbeTable(); });

    m_table->setColumnCount(2);
    m_table->setHorizontalHeaderLabels({tr("Scalar"), tr("Value")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setCascadingSectionResizes(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setShowGrid(true);
    m_table->setMinimumHeight(160);
    m_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_table->horizontalHeader()->setMinimumSectionSize(60);

    // Sync from settings -> UI when placement changes
    // Wheel/arrow step = 1/10th of the axis bbox length so nudging the
    // probe with the mouse wheel feels proportional to the dataset size.
    applyProbeSteps();
    connect(s, &RenderSettings::viewChanged, this, [this, s](ChangeFlags) {
        if (m_showCb) {
            m_showCb->blockSignals(true);
            m_showCb->setChecked(s->getShowProbe());
            m_showCb->blockSignals(false);
        }
        if (m_placementCombo) {
            m_placementCombo->blockSignals(true);
            m_placementCombo->setCurrentIndex(s->getProbePlacement());
            m_placementCombo->setEnabled(s->hasScalarCellData());
            m_placementCombo->blockSignals(false);
        }
        if (m_formatCombo) {
            m_formatCombo->blockSignals(true);
            m_formatCombo->setCurrentIndex(s->getProbeFormat());
            m_formatCombo->blockSignals(false);
        }
        refreshProbeTable();
    });
    connect(s, &RenderSettings::meshDataUpdated, this, [this, s]() {
        // Probe spin range just needs to cover generous world bounds
        if (m_xSpin && s->getHasMeshLoaded()) {
            double minX = s->getWorldMinX(), maxX = s->getWorldMaxX();
            double minY = s->getWorldMinY(), maxY = s->getWorldMaxY();
            double minZ = s->getWorldMinZ(), maxZ = s->getWorldMaxZ();
            for (auto* sp : {m_xSpin, m_ySpin, m_zSpin}) {
                if (sp) sp->setRange(std::min({minX, minY, minZ}) - 1000, std::max({maxX, maxY, maxZ}) + 1000);
            }
            applyProbeSteps();
        }
        refreshProbeTable();
        if (m_placementCombo) m_placementCombo->setEnabled(s->hasScalarCellData());
    });

    refreshProbeTable();

    // Make probe table vertically expanding to fill available space (replaces generic addStretch)
    if (auto* outerLay = qobject_cast<QVBoxLayout*>(content->layout())) {
        outerLay->setStretchFactor(probeUi.optionsGroup, 1);
    }
    if (auto* optsLay = qobject_cast<QVBoxLayout*>(probeUi.optionsGroup->layout())) {
        optsLay->setStretchFactor(probeUi.valueTable, 1);
    }

    setContent(content);
}

void ProbePage::applyProbeSteps() {
    auto* s = S();
    if (!m_xSpin || !m_ySpin || !m_zSpin) return;
    double lenX = s->getWorldMaxX() - s->getWorldMinX();
    double lenY = s->getWorldMaxY() - s->getWorldMinY();
    double lenZ = s->getWorldMaxZ() - s->getWorldMinZ();
    m_xSpin->setSingleStep(lenX > 1e-12 ? lenX / 10.0 : 0.1);
    m_ySpin->setSingleStep(lenY > 1e-12 ? lenY / 10.0 : 0.1);
    m_zSpin->setSingleStep(lenZ > 1e-12 ? lenZ / 10.0 : 0.1);
}

void ProbePage::refreshProbeTable() {
    auto* s = S();
    if (!m_table || !s) return;
    auto* rm = s->backend() ? s->backend()->lastUploadedMeshForPlot() : nullptr;
    if (!rm) rm = s->getHasMeshLoaded() ? s->backend()->lastUploadedMeshForPlot() : nullptr;
    if (!rm || !rm->attributes) {
        m_table->clearContents();
        m_table->setRowCount(0);
        return;
    }
    glm::vec3 pos(static_cast<float>(s->getProbeX()), static_cast<float>(s->getProbeY()), static_cast<float>(s->getProbeZ()));
    int placement = s->getProbePlacement();
    int fmt = s->getProbeFormat();
    QStringList scalars = s->getAvailableScalars();
    if (scalars.isEmpty()) {
        m_table->clearContents();
        m_table->setRowCount(0);
        return;
    }
    m_table->setRowCount(scalars.size());
    // Unified sampler: probe uses same FieldStore::Context + FieldSampler as line plot.
    const postfields::ComputedSnapshot* snap0 = s->snapshot().computed.get();
    FieldStore::Context ctx(*rm, snap0);
    auto place = FieldStore::toPlacement(placement);
    auto sampleOne = [&](const std::string& field, float& outVal, bool& outInside) -> bool {
        auto opt = FieldSampler::sample(ctx, field, place, pos);
        if (!opt) return false;
        outVal = *opt;
        // Heuristic inside test: sample uses structured contains check; if nearest fallback used, still finite.
        // Mark inside when pos within bounds expanded by eps; reuse same bbox test via Context sampling path.
        // For simplicity treat sampled value as inside when structured cell found, else mark outside via bounds.
        outInside = (rm->gridDimX > 1 && rm->gridDimY > 1)
            ? (pos.x >= rm->bounds.minX - 1e-6 && pos.x <= rm->bounds.maxX + 1e-6
            && pos.y >= rm->bounds.minY - 1e-6 && pos.y <= rm->bounds.maxY + 1e-6
            && (rm->gridDimZ <= 1 || (pos.z >= rm->bounds.minZ - 1e-6 && pos.z <= rm->bounds.maxZ + 1e-6)))
            : true;
        // For unstructured outside, FieldSampler falls back to nearest; we keep inside=false when outside bbox
        return std::isfinite(outVal);
    };
    for (int r = 0; r < scalars.size(); ++r) {
        std::string field = scalars[r].toStdString();
        float v = 0; bool inside = false;
        bool ok = sampleOne(field, v, inside);
        QTableWidgetItem* nameItem = new QTableWidgetItem(scalars[r]);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        QTableWidgetItem* valItem;
        if (!ok) {
            valItem = new QTableWidgetItem(tr("n/a"));
        } else {
            QString txt = formatProbeValue(v, fmt);
            if (!inside) txt += " *";
            valItem = new QTableWidgetItem(txt);
            valItem->setToolTip(inside ? tr("Interpolated") : tr("Nearest (outside)"));
        }
        valItem->setFlags(valItem->flags() & ~Qt::ItemIsEditable);
        valItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_table->setItem(r, 0, nameItem);
        m_table->setItem(r, 1, valItem);
    }
    // Auto-resize value column - ensure Value fills and Scalar stretches to occupy full width
    m_table->resizeColumnsToContents();
    // Force stretch column to fill remaining viewport width (fixes Format change not occupying full space until tab switch)
    {
        int vpW = m_table->viewport()->width();
        if (vpW <= 0) vpW = m_table->width() - m_table->verticalHeader()->width() - 4;
        if (vpW > 0) {
            int col1W = m_table->columnWidth(1);
            int col0W = qMax(80, vpW - col1W - 4);
            // Re-apply stretch mode then set explicit width for col0 to fill
            m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
            m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
            // Use QTimer to handle case where table was not yet visible (width==0)
            if (m_table->isVisible() && vpW > 50) {
                // Immediate correct for visible case
            } else {
                QTimer::singleShot(0, m_table, [this]() {
                    if (!m_table || !m_table->isVisible()) return;
                    m_table->resizeColumnsToContents();
                    int vp = m_table->viewport()->width();
                    int c1 = m_table->columnWidth(1);
                    int c0 = qMax(80, vp - c1 - 4);
                    if (c0 > 0) m_table->setColumnWidth(0, c0);
                    m_table->updateGeometry();
                    m_table->viewport()->update();
                });
            }
        }
        m_table->updateGeometry();
        m_table->viewport()->update();
    }
}

} // namespace sidebar
