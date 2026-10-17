#include "plot_window.h"
#include "render/settings/render_settings.h"
#include "core/FieldStore.h"
#include "core/field_sampler.h"
#include "core/post_fields.h"
#include "render/foundation/renderer.h"
#include "render/passes/PlotKernel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QFileDialog>
#include <QMessageBox>
#include <QPixmap>
#include <QToolButton>
#include <QOpenGLContext>
#include <glm/glm.hpp>
#include <limits>
#include <cmath>
#include <algorithm>

PlotPlacementAvail plotPlacementAvailability(RenderSettings* settings, const QString& field) {
    PlotPlacementAvail avail;
    if (!settings) return avail;
    Renderer* renderer = settings->backend();
    if (!renderer) return avail;
    auto* rm = renderer->lastUploadedMeshForPlot();
    if (!rm) return avail;
    if (field.isEmpty()) return avail;
    std::string fname = field.toStdString();
    const postfields::ComputedSnapshot* snap = settings ? settings->snapshot().computed.get() : nullptr;
    FieldStore::Context ctx(*rm, snap);
    avail.hasPoint = ctx.scalar(fname, FieldStore::Placement::Vertex).data != nullptr;
    avail.hasCell = ctx.scalarForVolume({fname, FieldStore::Placement::CellCenter}).data != nullptr
                 || ctx.scalar(fname, FieldStore::Placement::CellCenter).data != nullptr;
    // snapshot scalars are point-only v1
    if (!avail.hasPoint && snap && snap->scalars.find(fname) != snap->scalars.end()) avail.hasPoint = true;
    return avail;
}

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
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    // Slim top strip: only affordances for controls that live elsewhere.
    auto* strip = new QHBoxLayout;
    strip->setContentsMargins(0, 0, 0, 0);
    strip->setSpacing(2);
    auto* gearBtn = new QToolButton(this);
    gearBtn->setText(QStringLiteral("\u2699"));
    gearBtn->setFixedSize(20, 20);
    gearBtn->setToolTip(tr("Open plot controls (sidebar)"));
    gearBtn->setStyleSheet("QToolButton{border:none;}");
    connect(gearBtn, &QToolButton::clicked, this, &PlotWindow::requestOpenControls);
    auto* closeBtn = new QToolButton(this);
    closeBtn->setText(QStringLiteral("\u00D7"));
    closeBtn->setFixedSize(20, 20);
    closeBtn->setToolTip(tr("Close histogram"));
    closeBtn->setStyleSheet("QToolButton{border:none;}");
    connect(closeBtn, &QToolButton::clicked, this, [this](){ emit requestCloseHistogram(); });
    strip->addStretch(1);
    strip->addWidget(gearBtn);
    strip->addWidget(closeBtn);
    root->addLayout(strip);

    // --- Histogram canvas page ---
    auto* histPage = new QWidget;
    auto* histLay = new QVBoxLayout(histPage);
    histLay->setContentsMargins(0,0,0,0);
    histLay->setSpacing(4);
    m_plot = new PlotWidget(histPage);
    histLay->addWidget(m_plot, 1);
    auto* infoRow = new QHBoxLayout;
    m_statsLabel = new QLabel(tr("No data"));
    m_statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusLabel = new QLabel;
    m_statusLabel->setStyleSheet("color: palette(placeholderText);");
    infoRow->addWidget(m_statsLabel, 1);
    infoRow->addWidget(m_statusLabel);
    histLay->addLayout(infoRow);

    // --- Line plot canvas page ---
    auto* linePage = new QWidget;
    auto* lineLay = new QVBoxLayout(linePage);
    lineLay->setContentsMargins(0,0,0,0);
    lineLay->setSpacing(4);
    m_linePlot = new LinePlotWidget(linePage);
    lineLay->addWidget(m_linePlot,1);
    auto* lineInfo = new QHBoxLayout;
    m_lineStats = new QLabel(tr("No line data")); m_lineStats->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_lineStatus = new QLabel; m_lineStatus->setStyleSheet("color: palette(placeholderText);");
    lineInfo->addWidget(m_lineStats,1); lineInfo->addWidget(m_lineStatus);
    lineLay->addLayout(lineInfo);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(histPage);
    m_stack->addWidget(linePage);
    root->addWidget(m_stack, 1);

    m_recomputeTimer = new QTimer(this);
    m_recomputeTimer->setSingleShot(true);
    m_recomputeTimer->setInterval(160);
    connect(m_recomputeTimer, &QTimer::timeout, this, &PlotWindow::refreshData);

    if (m_settings) {
        connect(m_settings, &RenderSettings::plotSettingsChanged, this, &PlotWindow::onPlotParamsChanged);
        connect(m_settings, &RenderSettings::meshDataUpdated, this, &PlotWindow::onSettingsMeshDataUpdated);
        connect(m_settings, &RenderSettings::meshLoadStateChanged, this, &PlotWindow::onSettingsMeshDataUpdated);
    }

    setMode(m_settings ? m_settings->getPlotMode() : 0);
}

void PlotWindow::setMode(int mode) {
    int m = (mode == 1) ? 1 : 0;
    if (m_mode == m && m_stack && m_stack->currentIndex() == m) return;
    m_mode = m;
    if (m_stack) m_stack->setCurrentIndex(m);
}

void PlotWindow::onPlotParamsChanged() { scheduleRecompute(); }

void PlotWindow::onSettingsMeshDataUpdated() {
    // Field list may have changed wholesale: clear stale canvases when the
    // mesh provides no scalars at all, else refresh immediately.
    if (m_settings && m_settings->getAvailableScalars().isEmpty()) {
        m_plot->clear();
        m_linePlot->clear();
        m_statsLabel->setText(tr("No data"));
        m_lineStats->setText(tr("No line data"));
        return;
    }
    refreshData();
}

void PlotWindow::scheduleRecompute() { m_recomputeTimer->start(); }

void PlotWindow::refreshData() {
    // tick labels follow the user's Probe "Format" setting (Integer/Float/Exponent)
    const int tickFmt = m_settings ? m_settings->getProbeFormat() : 1;
    m_plot->setTickFormat(tickFmt);
    m_linePlot->setTickFormat(tickFmt);
    recomputeHistogram();
    recomputeLine();
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
    QString placement = (m_settings->getHistPlacement() == 1) ? tr("Cell") : tr("Vertex");
    m_statsLabel->setText(tr("Field: %1 [%2]  Range [%3 .. %4]  n=%5  bins=%6  peak=%7")
                          .arg(m_settings->getHistField()).arg(placement)
                          .arg(mn, 0, 'g', 6).arg(mx, 0, 'g', 6)
                          .arg(n).arg(bins.size()).arg(maxB));
}

void PlotWindow::recomputeHistogram() {
    if (!m_settings) return;
    QString field = m_settings->getHistField();
    if (field.isEmpty()) return;

    Renderer* renderer = m_settings->backend();
    if (!renderer) return;

    auto* rm = renderer->lastUploadedMeshForPlot();
    if (!rm) {
        m_statusLabel->setText(tr("No mesh uploaded"));
        return;
    }

    std::string fname = field.toStdString();
    bool useCell = m_settings->getHistPlacement() == 1;
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
        const postfields::ComputedSnapshot* snap = m_settings ? m_settings->snapshot().computed.get() : nullptr;
        // Snapshot shadows mesh fields.
        if (snap) {
            auto it = snap->scalars.find(fname);
            if (it != snap->scalars.end() && it->second && !it->second->empty()) {
                src = it->second.get();
                auto rit = snap->scalarRanges.find(fname);
                if (rit != snap->scalarRanges.end()) { dataMin = rit->second.first; dataMax = rit->second.second; }
                else {
                    auto r = FieldStore::computeRange(*src);
                    dataMin = r.min; dataMax = r.max;
                }
            }
        }
        if (!src) {
            int placement = 0;
            auto p = FieldStore::Placement::Vertex;
            src = FieldStore::scalarData(*rm, fname, dataMin, dataMax, p, snap);
        }
        if (!src || src->empty()) {
            m_statusLabel->setText(tr("Field not found"));
            return;
        }
        // Dedup scalars for vertex placement using vertexSourceIndex.
        // Per-source-point counting: vertex duplicates at sharp edges are deduped
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

    int bins = m_settings->getHistBins();
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

void PlotWindow::exportCsv() {
    if (m_mode == 1) exportLineCsv();
    else exportHistogramCsv();
}

void PlotWindow::exportPng() {
    if (m_mode == 1) exportLinePng();
    else exportHistogramPng();
}

void PlotWindow::exportHistogramCsv() {
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

void PlotWindow::exportHistogramPng() {
    QPixmap pm = m_plot->grab();
    QString path = QFileDialog::getSaveFileName(this, tr("Export Plot PNG"), QString(), tr("PNG (*.png)"));
    if (path.isEmpty()) return;
    if (!pm.save(path, "PNG")) QMessageBox::warning(this, tr("Export PNG"), tr("Failed to save PNG."));
    else m_statusLabel->setText(tr("PNG saved: %1").arg(path));
}

void PlotWindow::exportLineCsv() {
    if(m_lineDists.empty()){ QMessageBox::information(this,tr("Export CSV"),tr("No line data.")); return; }
    QString path=QFileDialog::getSaveFileName(this,tr("Export Line CSV"),QString(),tr("CSV (*.csv)"));
    if(path.isEmpty()) return;
    QFile f(path); if(!f.open(QIODevice::WriteOnly|QIODevice::Text)){ QMessageBox::warning(this,tr("Export CSV"),tr("Cannot open file.")); return; }
    QTextStream s(&f); s<<"distance,value\n";
    for(size_t i=0;i<m_lineDists.size() && i<m_lineVals.size(); ++i) s<<m_lineDists[i]<<","<<m_lineVals[i]<<"\n";
    m_lineStatus->setText(tr("CSV saved: %1").arg(path));
}

void PlotWindow::exportLinePng() {
    QPixmap pm=m_linePlot->grab();
    QString path=QFileDialog::getSaveFileName(this,tr("Export Line PNG"),QString(),tr("PNG (*.png)"));
    if(path.isEmpty()) return;
    if(!pm.save(path,"PNG")) QMessageBox::warning(this,tr("Export PNG"),tr("Failed to save PNG."));
    else m_lineStatus->setText(tr("PNG saved: %1").arg(path));
}

void PlotWindow::recomputeLine(){
    if(!m_settings||!m_linePlot) return;
    QString field=m_settings->getLineField();
    if(field.isEmpty()) return;
    Renderer* r=m_settings->backend(); if(!r) return;
    auto* rm=r->lastUploadedMeshForPlot(); if(!rm){ m_lineStatus->setText(tr("No mesh")); return; }
    std::string fname=field.toStdString();
    bool useCell=m_settings->getLinePlacement()==1;
    float p0x=m_settings->getLineP0x(),p0y=m_settings->getLineP0y(),p0z=m_settings->getLineP0z();
    float p1x=m_settings->getLineP1x(),p1y=m_settings->getLineP1y(),p1z=m_settings->getLineP1z();
    int N=m_settings->getLineSamples(); N=std::clamp(N,32,2048);
    glm::vec3 p0(p0x,p0y,p0z), p1(p1x,p1y,p1z);
    float len=glm::length(p1-p0); if(len<1e-9f){ m_lineStatus->setText(tr("P0==P1")); return; }
    const postfields::ComputedSnapshot* snap = m_settings ? m_settings->snapshot().computed.get() : nullptr;
    FieldStore::Context ctx(*rm, snap);
    // Validate field availability via unified store
    if (useCell && !ctx.scalarForVolume({fname, FieldStore::Placement::CellCenter}).data
                && !ctx.scalar(fname, FieldStore::Placement::CellCenter).data)
        useCell = false;
    {
        auto sf = useCell ? ctx.scalarForVolume({fname, FieldStore::Placement::CellCenter})
                          : ctx.scalar(fname, FieldStore::Placement::Vertex);
        if (!sf.data) sf = ctx.scalar(fname, useCell ? FieldStore::Placement::CellCenter : FieldStore::Placement::Vertex);
        if (!sf.data) { m_lineStatus->setText(tr("Field not found")); return; }
    }
    std::vector<float> dists; dists.reserve(N);
    std::vector<float> vals; vals.reserve(N);
    auto placement = useCell ? FieldStore::Placement::CellCenter : FieldStore::Placement::Vertex;
    for(int i=0;i<N;++i){
        float t=float(i)/float(N-1);
        glm::vec3 pos = p0 + t*(p1-p0);
        float dist = t*len;
        auto opt = FieldSampler::sample(ctx, fname, placement, pos);
        float v = opt ? *opt : std::numeric_limits<float>::quiet_NaN();
        dists.push_back(dist);
        vals.push_back(v);
    }
    // Y axis follows the sampled data, not the whole-field range
    float sMin = std::numeric_limits<float>::max(), sMax = std::numeric_limits<float>::lowest();
    for (float v : vals) { if (!std::isfinite(v)) continue; sMin = std::min(sMin, v); sMax = std::max(sMax, v); }
    if (sMin > sMax) { sMin = 0; sMax = 1; }
    float pad = (std::abs(sMax - sMin) < 1e-9f) ? std::max(1e-3f, std::abs(sMax) * 0.01f)
                                                : (sMax - sMin) * 0.05f;
    sMin -= pad; sMax += pad;
    m_lineDists=dists; m_lineVals=vals; m_lineMin=sMin; m_lineMax=sMax; m_lineField=field; m_linePlacement = useCell?1:0;
    m_linePlot->setData(dists, vals, sMin, sMax, field, m_linePlacement);
    m_lineStats->setText(tr("Field %1 [%2]  %3 samples  range [%4 .. %5]").arg(field).arg(useCell?"Cell":"Vertex").arg(N).arg(sMin,0,'g',4).arg(sMax,0,'g',4));
    m_lineStatus->setText(tr("Sampled %1 points along line").arg(N));
    // Ensure overlay is visible after sampling
    if (m_settings && !m_settings->getShowLineProbe()) {
        m_settings->setShowLineProbe(true);
    }
}

