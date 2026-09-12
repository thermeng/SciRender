#include "plot_window.h"
#include "render/settings/render_settings.h"
#include "core/FieldResolver.h"
#include "render/foundation/renderer.h"
#include "render/passes/PlotKernel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QPixmap>
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include <QTimer>
#include <QOpenGLContext>
#include <QStandardItemModel>
#include <QToolButton>
#include <glm/glm.hpp>
#include <limits>
#include <cmath>
#include <algorithm>

PlotWindow::PlotWindow(RenderSettings* settings, QWidget* parent)
    : QWidget(parent), m_settings(settings) {
    // When parent is nullptr → floating tool window (old behaviour for detached use).
    // When embedded in MainWindow's splitter, parent is the splitter pane → keep as plain widget.
    if (!parent) {
        setWindowTitle(tr("Histogram — GPU Kernel"));
        setWindowFlags(Qt::Window);
        resize(720, 480);
        setAttribute(Qt::WA_DeleteOnClose, false);
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    m_tabs = new QTabWidget(this);
    // --- Histogram tab ---
    auto* histTab = new QWidget;
    auto* histLay = new QVBoxLayout(histTab);
    histLay->setContentsMargins(0,0,0,0);
    histLay->setSpacing(6);
    auto* ctrlRow = new QHBoxLayout;
    ctrlRow->setSpacing(6);
    auto* fieldLabel = new QLabel(tr("Field:"));
    m_fieldCombo = new QComboBox;
    m_fieldCombo->setMinimumWidth(140);
    m_fieldCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    auto* placementLabel = new QLabel(tr("Sample:"));
    m_placementCombo = new QComboBox;
    m_placementCombo->addItems({tr("Vertex"), tr("Cell")});
    m_placementCombo->setCurrentIndex(0);
    m_placementCombo->setToolTip(tr("Vertex = per-vertex (point) values, post-split, extrapolated if needed. Cell = raw per-cell values."));
    auto* binsLabel = new QLabel(tr("Bins:"));
    m_binsSpin = new QSpinBox;
    m_binsSpin->setRange(8, 512);
    m_binsSpin->setValue(64);
    m_binsSpin->setSingleStep(8);
    m_recomputeBtn = new QPushButton(tr("Recompute (GPU)"));
    m_csvBtn = new QPushButton(tr("Export CSV…"));
    m_pngBtn = new QPushButton(tr("Export PNG…"));
    ctrlRow->addWidget(fieldLabel);
    ctrlRow->addWidget(m_fieldCombo, 1);
    ctrlRow->addWidget(placementLabel);
    ctrlRow->addWidget(m_placementCombo);
    ctrlRow->addWidget(binsLabel);
    ctrlRow->addWidget(m_binsSpin);
    ctrlRow->addWidget(m_recomputeBtn);
    ctrlRow->addWidget(m_csvBtn);
    ctrlRow->addWidget(m_pngBtn);
    histLay->addLayout(ctrlRow);
    m_plot = new PlotWidget(histTab);
    histLay->addWidget(m_plot, 1);
    auto* infoRow = new QHBoxLayout;
    m_statsLabel = new QLabel(tr("No data"));
    m_statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusLabel = new QLabel;
    m_statusLabel->setStyleSheet("color: palette(placeholderText);");
    infoRow->addWidget(m_statsLabel, 1);
    infoRow->addWidget(m_statusLabel);
    histLay->addLayout(infoRow);
    m_tabs->addTab(histTab, tr("Histogram"));
    // --- Line plot tab ---
    auto* lineTab = new QWidget;
    auto* lineLay = new QVBoxLayout(lineTab);
    lineLay->setContentsMargins(0,0,0,0);
    lineLay->setSpacing(6);
    auto* lineCtrl1 = new QHBoxLayout;
    lineCtrl1->setSpacing(6);
    lineCtrl1->addWidget(new QLabel(tr("Field:")));
    m_lineFieldCombo = new QComboBox; m_lineFieldCombo->setMinimumWidth(120);
    m_linePlacementCombo = new QComboBox; m_linePlacementCombo->addItems({tr("Vertex"), tr("Cell")});
    m_linePlacementCombo->setToolTip(tr("Vertex = trilinear point, Cell = nearest cell"));
    lineCtrl1->addWidget(m_lineFieldCombo,1);
    lineCtrl1->addWidget(new QLabel(tr("Sample:"))); lineCtrl1->addWidget(m_linePlacementCombo);
    lineCtrl1->addWidget(new QLabel(tr("Samples:")));
    m_samplesSpin = new QSpinBox; m_samplesSpin->setRange(32,2048); m_samplesSpin->setValue(256); m_samplesSpin->setSingleStep(32);
    lineCtrl1->addWidget(m_samplesSpin);
    m_lineSampleBtn = new QPushButton(tr("Sample"));
    m_lineCsvBtn = new QPushButton(tr("Export CSV…"));
    m_linePngBtn = new QPushButton(tr("Export PNG…"));
    lineCtrl1->addWidget(m_lineSampleBtn); lineCtrl1->addWidget(m_lineCsvBtn); lineCtrl1->addWidget(m_linePngBtn);
    lineLay->addLayout(lineCtrl1);
    auto* lineCtrl2 = new QHBoxLayout;
    lineCtrl2->setSpacing(4);
    lineCtrl2->addWidget(new QLabel(tr("P0:")));
    m_p0x = new QDoubleSpinBox; m_p0x->setRange(-1e6,1e6); m_p0x->setDecimals(3); m_p0x->setMinimumWidth(62); m_p0x->setMaximumWidth(78);
    m_p0y = new QDoubleSpinBox; m_p0y->setRange(-1e6,1e6); m_p0y->setDecimals(3); m_p0y->setMinimumWidth(62); m_p0y->setMaximumWidth(78);
    m_p0z = new QDoubleSpinBox; m_p0z->setRange(-1e6,1e6); m_p0z->setDecimals(3); m_p0z->setMinimumWidth(62); m_p0z->setMaximumWidth(78);
    lineCtrl2->addWidget(m_p0x); lineCtrl2->addWidget(m_p0y); lineCtrl2->addWidget(m_p0z);
    lineCtrl2->addWidget(new QLabel(tr("P1:")));
    m_p1x = new QDoubleSpinBox; m_p1x->setRange(-1e6,1e6); m_p1x->setDecimals(3); m_p1x->setMinimumWidth(62); m_p1x->setMaximumWidth(78);
    m_p1y = new QDoubleSpinBox; m_p1y->setRange(-1e6,1e6); m_p1y->setDecimals(3); m_p1y->setMinimumWidth(62); m_p1y->setMaximumWidth(78);
    m_p1z = new QDoubleSpinBox; m_p1z->setRange(-1e6,1e6); m_p1z->setDecimals(3); m_p1z->setMinimumWidth(62); m_p1z->setMaximumWidth(78);
    lineCtrl2->addWidget(m_p1x); lineCtrl2->addWidget(m_p1y); lineCtrl2->addWidget(m_p1z);
    m_showProbeCb = new QCheckBox(tr("Show line"));
    m_showProbeCb->setChecked(m_settings ? m_settings->getShowLineProbe() : false);
    m_showProbeCb->setToolTip(tr("Draw P0→P1 line and endpoints in the 3D viewport"));
    lineCtrl2->addWidget(m_showProbeCb);
    lineCtrl2->addStretch();
    lineLay->addLayout(lineCtrl2);
    m_linePlot = new LinePlotWidget(lineTab);
    lineLay->addWidget(m_linePlot,1);
    auto* lineInfo = new QHBoxLayout;
    m_lineStats = new QLabel(tr("No line data")); m_lineStats->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_lineStatus = new QLabel; m_lineStatus->setStyleSheet("color: palette(placeholderText);");
    lineInfo->addWidget(m_lineStats,1); lineInfo->addWidget(m_lineStatus);
    lineLay->addLayout(lineInfo);
    m_tabs->addTab(lineTab, tr("Line Plot"));
    // Close button inline with tabs (cleaner than separate header)
    {
        auto* closeBtn = new QToolButton(m_tabs);
        closeBtn->setText(QStringLiteral("\u00D7"));
        closeBtn->setFixedSize(18,18);
        closeBtn->setToolTip(tr("Close histogram"));
        closeBtn->setStyleSheet("QToolButton{border:none;}");
        m_tabs->setCornerWidget(closeBtn, Qt::TopRightCorner);
        connect(closeBtn, &QToolButton::clicked, this, [this](){ emit requestCloseHistogram(); });
    }
    root->addWidget(m_tabs,1);

    connect(m_fieldCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int){ updatePlacementOptions(); recomputeHistogram(); });
    connect(m_placementCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &PlotWindow::onRecompute);
    connect(m_recomputeBtn, &QPushButton::clicked, this, &PlotWindow::onRecompute);
    connect(m_csvBtn, &QPushButton::clicked, this, &PlotWindow::onExportCsv);
    connect(m_pngBtn, &QPushButton::clicked, this, &PlotWindow::onExportPng);
    connect(m_binsSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &PlotWindow::onBinsChanged);
    connect(m_lineFieldCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int){ updatePlacementOptions(); });
    connect(m_linePlacementCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &PlotWindow::onSampleLine);
    connect(m_lineSampleBtn, &QPushButton::clicked, this, &PlotWindow::onSampleLine);
    connect(m_lineCsvBtn, &QPushButton::clicked, this, &PlotWindow::onLineExportCsv);
    connect(m_linePngBtn, &QPushButton::clicked, this, &PlotWindow::onLineExportPng);
    connect(m_showProbeCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowLineProbe);
    connect(m_p0x, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if(m_settings) m_settings->setLineP0x(v); });
    connect(m_p0y, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if(m_settings) m_settings->setLineP0y(v); });
    connect(m_p0z, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if(m_settings) m_settings->setLineP0z(v); });
    connect(m_p1x, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if(m_settings) m_settings->setLineP1x(v); });
    connect(m_p1y, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if(m_settings) m_settings->setLineP1y(v); });
    connect(m_p1z, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if(m_settings) m_settings->setLineP1z(v); });
    // Sync line probe UI when global state changes
    connect(m_settings, &RenderSettings::viewChanged, this, [this](ChangeFlags){ syncLineProbeUIFromSettings(); });
    connect(m_settings, &RenderSettings::meshDataUpdated, this, [this](){ syncLineProbeUIFromSettings(); });
    connect(m_settings, &RenderSettings::meshLoadStateChanged, this, [this](){ syncLineProbeUIFromSettings(); });

    if (m_settings) {
        connect(m_settings, &RenderSettings::meshDataUpdated, this, &PlotWindow::onSettingsMeshDataUpdated);
        connect(m_settings, &RenderSettings::meshLoadStateChanged, this, &PlotWindow::onSettingsMeshDataUpdated);
    }

    refreshFieldList();
}

void PlotWindow::refreshFieldList() {
    if (!m_fieldCombo || !m_settings) return;
    QString prev = m_fieldCombo->currentText();
    QString prevLine = m_lineFieldCombo ? m_lineFieldCombo->currentText() : QString();
    m_fieldCombo->blockSignals(true);
    if (m_lineFieldCombo) m_lineFieldCombo->blockSignals(true);
    m_fieldCombo->clear();
    if (m_lineFieldCombo) m_lineFieldCombo->clear();
    QStringList fields = m_settings->getAvailableScalars();
    if (fields.isEmpty()) {
        m_fieldCombo->addItem(tr("(no scalar fields)"));
        m_fieldCombo->setEnabled(false);
        m_recomputeBtn->setEnabled(false);
        if (m_placementCombo) m_placementCombo->setEnabled(false);
        if (m_lineFieldCombo) { m_lineFieldCombo->addItem(tr("(no scalar fields)")); m_lineFieldCombo->setEnabled(false); if(m_lineSampleBtn) m_lineSampleBtn->setEnabled(false); }
    } else {
        m_fieldCombo->addItems(fields);
        m_fieldCombo->setEnabled(true);
        m_recomputeBtn->setEnabled(true);
        int idx = m_fieldCombo->findText(prev);
        if (idx >= 0) m_fieldCombo->setCurrentIndex(idx);
        else {
            QString active = m_settings->getActiveScalarNameQml();
            int ai = m_fieldCombo->findText(active);
            if (ai >= 0) m_fieldCombo->setCurrentIndex(ai);
        }
        if (m_lineFieldCombo) {
            m_lineFieldCombo->addItems(fields);
            m_lineFieldCombo->setEnabled(true);
            if(m_lineSampleBtn) m_lineSampleBtn->setEnabled(true);
            int lidx = m_lineFieldCombo->findText(prevLine);
            if (lidx >= 0) m_lineFieldCombo->setCurrentIndex(lidx);
            else {
                QString active = m_settings->getActiveScalarNameQml();
                int ai = m_lineFieldCombo->findText(active);
                if (ai >= 0) m_lineFieldCombo->setCurrentIndex(ai);
            }
        }
    }
    m_fieldCombo->blockSignals(false);
    if (m_lineFieldCombo) m_lineFieldCombo->blockSignals(false);
    // Init line P0/P1 to mesh bounds diagonal if not yet set
    if (m_settings && m_settings->getHasMeshLoaded() && m_p0x && m_p1x) {
        Renderer* r = m_settings->backend();
        if (r) if (auto* rm = r->lastUploadedMeshForPlot()) {
            double minX = rm->bounds.minX, maxX = rm->bounds.maxX;
            double minY = rm->bounds.minY, maxY = rm->bounds.maxY;
            double minZ = rm->bounds.minZ, maxZ = rm->bounds.maxZ;
            // Only auto-init if P0==P1 (first load)
            bool same = (m_p0x->value()==m_p1x->value() && m_p0y->value()==m_p1y->value() && m_p0z->value()==m_p1z->value());
            if (same) {
                m_p0x->setValue(minX); m_p0y->setValue(minY); m_p0z->setValue(minZ);
                m_p1x->setValue(maxX); m_p1y->setValue(maxY); m_p1z->setValue(maxZ);
            }
        }
    }
    updatePlacementOptions();
    if (m_settings && m_settings->getHasMeshLoaded() && !fields.isEmpty()) {
        QTimer::singleShot(0, this, [this]() { recomputeHistogram(); });
    } else {
        m_plot->clear();
        m_statsLabel->setText(tr("No data"));
        if(m_linePlot) m_linePlot->clear();
        if(m_lineStats) m_lineStats->setText(tr("No line data"));
    }
}

void PlotWindow::onRecompute() { recomputeHistogram(); }

void PlotWindow::onBinsChanged(int) { recomputeHistogram(); }

void PlotWindow::onSettingsMeshDataUpdated() { refreshFieldList(); }

void PlotWindow::updatePlacementOptions() {
    if (!m_fieldCombo || !m_placementCombo || !m_settings) return;
    Renderer* renderer = m_settings->backend();
    if (!renderer) return;
    auto* rm = renderer->lastUploadedMeshForPlot();
    if (!rm || !rm->attributes) {
        m_placementCombo->setEnabled(false);
        return;
    }
    QString field = m_fieldCombo->currentText();
    if (field.isEmpty()) { m_placementCombo->setEnabled(false); return; }
    std::string fname = field.toStdString();
    bool hasPoint = false, hasCell = false;
    auto& attr = *rm->attributes;
    if (attr.pointScalars.find(fname) != attr.pointScalars.end()) hasPoint = true;
    if (attr.cellScalars.find(fname) != attr.cellScalars.end()) hasCell = true;
    // Derived scalars (e.g. velocity_magnitude) are point-derived when backend materializes them
    // but they have no cell storage; check via FieldResolver derived list is not needed here.
    // If neither map contains the field, it may be derived or from scalars vector - treat as Vertex only.
    if (!hasPoint && !hasCell) {
        // Check if field resolves at all - if so, show Vertex only
        float mn, mx;
        if (FieldResolver::scalarData(*rm, fname, mn, mx)) hasPoint = true;
    }
    m_placementCombo->blockSignals(true);
    // Preserve user selection if still valid — do not force to global here.
    // Global scalarPlacement is the default for scalar surface/volume, plot keeps its own local choice.
    int cur = m_placementCombo->currentIndex(); // 0 Vertex, 1 Cell
    if (!hasCell && cur == 1) cur = 0;
    if (!hasPoint && cur == 0 && hasCell) cur = 1;
    if (cur < 0) cur = hasPoint ? 0 : (hasCell ? 1 : 0);
    m_placementCombo->setCurrentIndex(cur);
    m_placementCombo->setEnabled(hasPoint || hasCell);
    // Properly enable/disable the Cell item via model flags (Qt UserRole-1 hack is unreliable)
    if (auto *model = qobject_cast<QStandardItemModel*>(m_placementCombo->model())) {
        if (auto *item = model->item(1)) {
            item->setEnabled(hasCell);
        }
    } else {
        // Fallback to previous hack if model is not QStandardItemModel
        m_placementCombo->setItemData(1, hasCell ? QVariant(true) : QVariant(false), Qt::UserRole - 1);
    }
    m_placementCombo->setToolTip(hasCell ? tr("Cell available (%1 cells)").arg(attr.cellScalars.count(fname) ? (int)attr.cellScalars.at(fname).size() : 0)
                                         : tr("No raw cell data for this field — Vertex only"));
    m_placementCombo->blockSignals(false);
}

void PlotWindow::recomputeHistogram() {
    if (!m_settings || !m_fieldCombo) return;
    QString field = m_fieldCombo->currentText();
    if (field.isEmpty() || field == tr("(no scalar fields)")) return;

    Renderer* renderer = m_settings->backend();
    if (!renderer) return;

    auto* rm = renderer->lastUploadedMeshForPlot();
    if (!rm) {
        m_statusLabel->setText(tr("No mesh uploaded"));
        return;
    }

    std::string fname = field.toStdString();
    bool useCell = m_placementCombo && m_placementCombo->currentIndex() == 1;
    float dataMin = 0, dataMax = 1;
    const std::vector<float>* src = nullptr;
    std::vector<float> dedupedScalars; // per-source-point averaged vertex scalars

    if (useCell && rm->attributes) {
        auto it = rm->attributes->cellScalars.find(fname);
        if (it != rm->attributes->cellScalars.end() && !it->second.empty()) {
            src = &it->second;
            auto rit = rm->attributes->cellScalarRanges.find(fname);
            if (rit != rm->attributes->cellScalarRanges.end()) {
                dataMin = rit->second.first;
                dataMax = rit->second.second;
            } else {
                float mn = std::numeric_limits<float>::max();
                float mx = std::numeric_limits<float>::lowest();
                for (float v : *src) { if (!std::isfinite(v)) continue; mn = std::min(mn, v); mx = std::max(mx, v); }
                if (mn > mx) { mn = 0; mx = 1; }
                if (std::abs(mx - mn) < 1e-6f) mx = mn + 1.0f;
                dataMin = mn; dataMax = mx;
            }
        } else {
            // No raw cell data for this field — fallback to vertex (extrapolated) path
            useCell = false;
        }
    }
    if (!useCell) {
        // Placement-aware resolve: 0=Vertex (point). Use global scalarPlacement where possible
        // but the plot window has its own local placement combo, so we force Vertex here.
        int placement = 0;
        src = FieldResolver::scalarData(*rm, fname, dataMin, dataMax, placement);
        if (!src || src->empty()) {
            m_statusLabel->setText(tr("Field not found"));
            return;
        }
        // Dedup scalars for vertex placement using vertexSourceIndex.
        // Per-source-point counting (ParaView parity): vertex duplicates at sharp edges are deduped
        // so histogram n == sourcePointCount. Without dedup, n == post-split vertex count.
        if (rm && !rm->vertexSourceIndex.empty() && rm->vertexSourceIndex.size() == src->size()) {
            const auto& lut = rm->vertexSourceIndex;
            size_t sourcePointCount = rm->sourcePointCount > 0 ? static_cast<size_t>(rm->sourcePointCount) : 0;
            if (sourcePointCount == 0) {
                size_t mxIdx = 0;
                for (uint32_t v : lut) mxIdx = std::max<size_t>(mxIdx, v);
                sourcePointCount = mxIdx + 1;
            }
            sourcePointCount = std::max<size_t>(1, sourcePointCount);
            dedupedScalars.assign(sourcePointCount, 0.0f);
            std::vector<size_t> sourceCounts(sourcePointCount, 0);
            std::vector<double> sum(sourcePointCount, 0.0);
            for (size_t v = 0; v < lut.size() && v < src->size(); ++v) {
                int s = static_cast<int>(lut[v]);
                if (s >= 0 && static_cast<size_t>(s) < sourcePointCount) {
                    sum[s] += (*src)[v];
                    sourceCounts[s]++;
                }
            }
            for (size_t s = 0; s < sourcePointCount; ++s) {
                if (sourceCounts[s] > 0) dedupedScalars[s] = static_cast<float>(sum[s] / sourceCounts[s]);
                else if (s < src->size()) dedupedScalars[s] = (*src)[s];
            }
            src = &dedupedScalars;
        }
    }

    // Title includes placement for clarity
    QString titleField = useCell ? QString("%1 (Cell)").arg(field) : QString("%1 (Vertex)").arg(field);

    int bins = m_binsSpin->value();
    bool dispatched = false;
    if (QOpenGLContext::currentContext()) {
        renderer->plotKernel().requestHistogram(*src, bins, dataMin, dataMax);
        dispatched = renderer->plotKernel().tryDispatch();
    } else {
        renderer->plotKernel().requestHistogramCpu(*src, bins, dataMin, dataMax);
        dispatched = true;
    }
    if (dispatched) {
        auto resultBins = renderer->plotKernel().bins();
        float rMin = renderer->plotKernel().dataMin();
        float rMax = renderer->plotKernel().dataMax();
        size_t nVals = renderer->plotKernel().numValues();
        m_plot->setHistogram(resultBins, rMin, rMax, titleField, nVals);
        updateStatsLabel();
        QString err = QString::fromStdString(renderer->plotKernel().lastError());
        QString placementTag = useCell ? tr("Cell") : tr("Vertex");
        if (err.isEmpty()) {
            uint32_t peak = resultBins.empty() ? 0 : *std::max_element(resultBins.begin(), resultBins.end());
            m_statusLabel->setText(tr("%1 bins: %2  max=%3  n=%4").arg(placementTag).arg(bins).arg(peak).arg(nVals));
        } else {
            m_statusLabel->setText(err);
            qWarning() << err;
        }
    }
}

void PlotWindow::updateStatsLabel() {
    if (!m_settings) return;
    Renderer* r = m_settings->backend();
    if (!r || !r->plotKernel().hasResult()) { m_statsLabel->setText(tr("No data")); return; }
    auto bins = r->plotKernel().bins();
    float mn = r->plotKernel().dataMin();
    float mx = r->plotKernel().dataMax();
    size_t n = r->plotKernel().numValues();
    uint32_t maxB = bins.empty() ? 0 : *std::max_element(bins.begin(), bins.end());
    QString placement = (m_placementCombo && m_placementCombo->currentIndex() == 1) ? tr("Cell") : tr("Vertex");
    m_statsLabel->setText(tr("Field: %1 [%2]  Range [%3 .. %4]  n=%5  bins=%6  peak=%7")
                          .arg(m_fieldCombo->currentText()).arg(placement)
                          .arg(mn, 0, 'g', 6).arg(mx, 0, 'g', 6)
                          .arg(n).arg(bins.size()).arg(maxB));
}

void PlotWindow::onExportCsv() {
    Renderer* r = m_settings ? m_settings->backend() : nullptr;
    if (!r || !r->plotKernel().hasResult()) { QMessageBox::information(this, tr("Export CSV"), tr("No histogram to export.")); return; }
    QString path = QFileDialog::getSaveFileName(this, tr("Export Histogram CSV"), QString(), tr("CSV (*.csv)"));
    if (path.isEmpty()) return;
    auto bins = r->plotKernel().bins();
    float mn = r->plotKernel().dataMin();
    float mx = r->plotKernel().dataMax();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) { QMessageBox::warning(this, tr("Export CSV"), tr("Cannot open file.")); return; }
    QTextStream s(&f);
    s << "bin,bin_lo,bin_hi,count\n";
    for (int i = 0; i < (int)bins.size(); ++i) {
        float lo = mn + (mx - mn) * (i / float(bins.size()));
        float hi = mn + (mx - mn) * ((i + 1) / float(bins.size()));
        s << i << "," << lo << "," << hi << "," << bins[i] << "\n";
    }
    m_statusLabel->setText(tr("CSV saved: %1").arg(path));
}

void PlotWindow::onExportPng() {
    QPixmap pm = m_plot->grab();
    QString path = QFileDialog::getSaveFileName(this, tr("Export Plot PNG"), QString(), tr("PNG (*.png)"));
    if (path.isEmpty()) return;
    if (!pm.save(path, "PNG")) QMessageBox::warning(this, tr("Export PNG"), tr("Failed to save PNG."));
    else m_statusLabel->setText(tr("PNG saved: %1").arg(path));
}

void PlotWindow::onSampleLine(){ recomputeLine(); }
void PlotWindow::onLineExportCsv(){
    if(m_lineDists.empty()){ QMessageBox::information(this,tr("Export CSV"),tr("No line data.")); return; }
    QString path=QFileDialog::getSaveFileName(this,tr("Export Line CSV"),QString(),tr("CSV (*.csv)"));
    if(path.isEmpty()) return;
    QFile f(path); if(!f.open(QIODevice::WriteOnly|QIODevice::Text)){ QMessageBox::warning(this,tr("Export CSV"),tr("Cannot open file.")); return; }
    QTextStream s(&f); s<<"distance,value\n";
    for(size_t i=0;i<m_lineDists.size() && i<m_lineVals.size(); ++i) s<<m_lineDists[i]<<","<<m_lineVals[i]<<"\n";
    m_lineStatus->setText(tr("CSV saved: %1").arg(path));
}
void PlotWindow::onLineExportPng(){
    QPixmap pm=m_linePlot->grab();
    QString path=QFileDialog::getSaveFileName(this,tr("Export Line PNG"),QString(),tr("PNG (*.png)"));
    if(path.isEmpty()) return;
    if(!pm.save(path,"PNG")) QMessageBox::warning(this,tr("Export PNG"),tr("Failed to save PNG."));
    else m_lineStatus->setText(tr("PNG saved: %1").arg(path));
}
void PlotWindow::syncLineProbeUIFromSettings(){
    if(!m_settings) return;
    // Wheel/arrow step = 1/10th of the axis bbox length, matching Probe page.
    {
        double lenX = m_settings->getWorldMaxX() - m_settings->getWorldMinX();
        double lenY = m_settings->getWorldMaxY() - m_settings->getWorldMinY();
        double lenZ = m_settings->getWorldMaxZ() - m_settings->getWorldMinZ();
        double sx = lenX > 1e-12 ? lenX / 10.0 : 0.1;
        double sy = lenY > 1e-12 ? lenY / 10.0 : 0.1;
        double sz = lenZ > 1e-12 ? lenZ / 10.0 : 0.1;
        if(m_p0x) m_p0x->setSingleStep(sx); if(m_p1x) m_p1x->setSingleStep(sx);
        if(m_p0y) m_p0y->setSingleStep(sy); if(m_p1y) m_p1y->setSingleStep(sy);
        if(m_p0z) m_p0z->setSingleStep(sz); if(m_p1z) m_p1z->setSingleStep(sz);
    }
    if(m_showProbeCb){
        m_showProbeCb->blockSignals(true);
        m_showProbeCb->setChecked(m_settings->getShowLineProbe());
        m_showProbeCb->blockSignals(false);
    }
    auto syncSpin=[this](QDoubleSpinBox* sb,double v){
        if(!sb) return;
        // avoid feedback loop
        if(std::abs(sb->value()-v) < 1e-9) return;
        sb->blockSignals(true); sb->setValue(v); sb->blockSignals(false);
    };
    syncSpin(m_p0x, m_settings->getLineP0x());
    syncSpin(m_p0y, m_settings->getLineP0y());
    syncSpin(m_p0z, m_settings->getLineP0z());
    syncSpin(m_p1x, m_settings->getLineP1x());
    syncSpin(m_p1y, m_settings->getLineP1y());
    syncSpin(m_p1z, m_settings->getLineP1z());
}

void PlotWindow::recomputeLine(){
    if(!m_settings||!m_lineFieldCombo||!m_linePlot) return;
    QString field=m_lineFieldCombo->currentText();
    if(field.isEmpty()||field==tr("(no scalar fields)")) return;
    Renderer* r=m_settings->backend(); if(!r) return;
    auto* rm=r->lastUploadedMeshForPlot(); if(!rm){ m_lineStatus->setText(tr("No mesh")); return; }
    std::string fname=field.toStdString();
    bool useCell=m_linePlacementCombo && m_linePlacementCombo->currentIndex()==1;
    // Validate cell availability
    if(useCell && rm->attributes){
        auto it=rm->attributes->cellScalars.find(fname);
        if(it==rm->attributes->cellScalars.end() || it->second.empty()) useCell=false;
    }
    float p0x=m_p0x->value(),p0y=m_p0y->value(),p0z=m_p0z->value();
    float p1x=m_p1x->value(),p1y=m_p1y->value(),p1z=m_p1z->value();
    int N=m_samplesSpin?m_samplesSpin->value():256; N=std::clamp(N,32,2048);
    glm::vec3 p0(p0x,p0y,p0z), p1(p1x,p1y,p1z);
    float len=glm::length(p1-p0); if(len<1e-9f){ m_lineStatus->setText(tr("P0==P1")); return; }
    // Resolve scalar array and range for stats (placement-aware)
    float rMin=0,rMax=1;
    const std::vector<float>* src=nullptr;
    if(useCell){
        if(rm->attributes){
            auto it=rm->attributes->cellScalars.find(fname);
            if(it!=rm->attributes->cellScalars.end()){ src=&it->second; auto rit=rm->attributes->cellScalarRanges.find(fname); if(rit!=rm->attributes->cellScalarRanges.end()){ rMin=rit->second.first; rMax=rit->second.second; } }
        }
        if(!src) src=FieldResolver::scalarData(*rm,fname,rMin,rMax,1);
    } else {
        src=FieldResolver::scalarData(*rm,fname,rMin,rMax,0);
    }
    if(!src||src->empty()){ m_lineStatus->setText(tr("Field not found")); return; }
    // Prepare sampling
    std::vector<float> dists; dists.reserve(N);
    std::vector<float> vals; vals.reserve(N);
    // Structured grid fast path
    bool isStructured = rm->gridDimX>1 && rm->gridDimY>1;
    // Use brute-force cell search for trilinear/nearest
    int dX=rm->gridDimX, dY=rm->gridDimY, dZ=rm->gridDimZ;
    const float* verts = rm->vertices.data();
    auto idx=[&](int x,int y,int z){ return x + y*dX + z*dX*dY; };
    for(int i=0;i<N;++i){
        float t=float(i)/float(N-1);
        glm::vec3 pos = p0 + t*(p1-p0);
        float dist = t*len;
        float v = std::numeric_limits<float>::quiet_NaN();
        if(isStructured && dX>0 && dY>0 && dZ>0 && verts){
            // Find containing cell
            bool found=false;
            int cdX=std::max(1,dX-1), cdY=std::max(1,dY-1), cdZ=std::max(1,dZ-1);
            // For ImageData 2D cdZ=1? handle 2D: if dZ==1, cdZ=1 but cells are quads
            // Brute: iterate cells
            for(int cz=0; cz<cdZ && !found; ++cz) for(int cy=0; cy<cdY && !found; ++cy) for(int cx=0; cx<cdX && !found; ++cx){
                // quick bbox filter using 8 corners min/max
                float minx=1e30f,miny=1e30f,minz=1e30f,maxx=-1e30f,maxy=-1e30f,maxz=-1e30f;
                // For 2D dZ==1 only 4 corners
                int z0= cz, z1= std::min(cz+1,dZ-1);
                int y0= cy, y1= std::min(cy+1,dY-1);
                int x0= cx, x1= std::min(cx+1,dX-1);
                // gather corners
                int corners[8][3]={{x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}};
                for(int k=0;k<8;++k){
                    int n=idx(corners[k][0],corners[k][1],corners[k][2]);
                    if(n<0|| size_t(n*3+2)>=rm->vertices.size()) continue;
                    float px=verts[n*3+0], py=verts[n*3+1], pz=verts[n*3+2];
                    minx=std::min(minx,px); maxx=std::max(maxx,px);
                    miny=std::min(miny,py); maxy=std::max(maxy,py);
                    minz=std::min(minz,pz); maxz=std::max(maxz,pz);
                }
                const float eps=1e-6f;
                if(pos.x < minx - eps || pos.x > maxx + eps) continue;
                if(pos.y < miny - eps || pos.y > maxy + eps) continue;
                if(dZ>1 && (pos.z < minz - eps || pos.z > maxz + eps)) continue;
                // Inside this cell
                if(useCell){
                    // cell index
                    int cIdx = cx + cy*cdX + cz*cdX*cdY;
                    if(rm->attributes){
                        auto it=rm->attributes->cellScalars.find(fname);
                        if(it!=rm->attributes->cellScalars.end() && cIdx < (int)it->second.size()) v=it->second[cIdx];
                    }
                    if(!std::isfinite(v)){
                        // fallback to vertex trilinear of this cell
                        useCell=false; // fall through
                    } else found=true;
                }
                if(!useCell){
                    // trilinear from 8 corner point values
                    // gather 8 scalar values
                    float s[8];
                    bool ok=true;
                    for(int k=0;k<8;++k){
                        int n=idx(corners[k][0],corners[k][1],corners[k][2]);
                        if(n<0 || size_t(n) >= src->size()){ ok=false; break; }
                        // For point placement src is pointScalars (size = nPoints). For cell fallback we already handled.
                        // If src is pointScalars but we are sampling cell interior, use point values
                        s[k]=(*src)[n];
                        if(!std::isfinite(s[k])) ok=false;
                    }
                    if(!ok) continue;
                    float dx = (maxx-minx) > 1e-12f ? (pos.x - minx)/(maxx-minx) : 0;
                    float dy = (maxy-miny) > 1e-12f ? (pos.y - miny)/(maxy-miny) : 0;
                    float dz = (dZ>1 && (maxz-minz)>1e-12f) ? (pos.z - minz)/(maxz-minz) : 0;
                    dx=std::clamp(dx,0.0f,1.0f); dy=std::clamp(dy,0.0f,1.0f); dz=std::clamp(dz,0.0f,1.0f);
                    // trilinear: c00 = s0*(1-dx)+s1*dx etc.
                    float c00 = s[0]*(1-dx)+s[1]*dx;
                    float c10 = s[3]*(1-dx)+s[2]*dx;
                    float c01 = s[4]*(1-dx)+s[5]*dx;
                    float c11 = s[7]*(1-dx)+s[6]*dx;
                    float c0 = c00*(1-dy)+c10*dy;
                    float c1 = c01*(1-dy)+c11*dy;
                    v = c0*(1-dz)+c1*dz;
                    found=true;
                }
            }
            if(!found){
                // Outside mesh: nearest vertex fallback
                float bestD=1e30f; int best=-1;
                size_t nPt = rm->vertices.size()/3;
                for(size_t n=0;n<nPt;++n){
                    float dx=verts[n*3+0]-pos.x, dy=verts[n*3+1]-pos.y, dz=verts[n*3+2]-pos.z;
                    float dd=dx*dx+dy*dy+dz*dz;
                    if(dd<bestD){ bestD=dd; best=int(n); }
                }
                if(best>=0 && size_t(best)<src->size()) v=(*src)[best];
            }
        } else {
            // Unstructured fallback: nearest vertex
            float bestD=1e30f; int best=-1;
            size_t nPt = rm->vertices.size()/3;
            const float* verts2 = rm->vertices.data();
            for(size_t n=0;n<nPt;++n){
                float dx=verts2[n*3+0]-pos.x, dy=verts2[n*3+1]-pos.y, dz=verts2[n*3+2]-pos.z;
                float dd=dx*dx+dy*dy+dz*dz;
                if(dd<bestD){ bestD=dd; best=int(n); }
            }
            if(best>=0 && size_t(best)<src->size()) v=(*src)[best];
            else {
                // cell fallback nearest cell center
                if(useCell && !rm->cellCenters.empty()){
                    float bestC=1e30f; int bestCidx=-1;
                    for(size_t c=0;c<rm->cellCenters.size();++c){
                        float dx=rm->cellCenters[c].x-pos.x, dy=rm->cellCenters[c].y-pos.y, dz=rm->cellCenters[c].z-pos.z;
                        float dd=dx*dx+dy*dy+dz*dz;
                        if(dd<bestC){ bestC=dd; bestCidx=int(c); }
                    }
                    if(bestCidx>=0 && size_t(bestCidx)<src->size()) v=(*src)[bestCidx];
                }
            }
        }
        if(!std::isfinite(v)) v=rMin;
        dists.push_back(dist);
        vals.push_back(v);
        // update range for display (keep original rMin/rMax for axis, but values are as sampled)
    }
    m_lineDists=dists; m_lineVals=vals; m_lineMin=rMin; m_lineMax=rMax; m_lineField=field; m_linePlacement = useCell?1:0;
    m_linePlot->setData(dists, vals, rMin, rMax, field, m_linePlacement);
    m_lineStats->setText(tr("Field %1 [%2]  %3 samples  range [%4 .. %5]").arg(field).arg(useCell?"Cell":"Vertex").arg(N).arg(rMin,0,'g',4).arg(rMax,0,'g',4));
    m_lineStatus->setText(tr("Sampled %1 points along line").arg(N));
    // Ensure overlay is visible after sampling
    if (m_settings && !m_settings->getShowLineProbe()) {
        m_settings->setShowLineProbe(true);
        if (m_showProbeCb) { m_showProbeCb->blockSignals(true); m_showProbeCb->setChecked(true); m_showProbeCb->blockSignals(false); }
    }
}
