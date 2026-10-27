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

    // --- Monitor plot canvas page ---
    auto* monitorPage = new QWidget;
    auto* monitorLay = new QVBoxLayout(monitorPage);
    monitorLay->setContentsMargins(0,0,0,0);
    monitorLay->setSpacing(4);
    m_monitorPlot = new MonitorPlotWidget(monitorPage);
    monitorLay->addWidget(m_monitorPlot,1);
    auto* monitorInfo = new QHBoxLayout;
    m_residualStats = new QLabel(tr("No monitor data")); m_residualStats->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_residualStatus = new QLabel; m_residualStatus->setStyleSheet("color: palette(placeholderText);");
    monitorInfo->addWidget(m_residualStats,1); monitorInfo->addWidget(m_residualStatus);
    monitorLay->addLayout(monitorInfo);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(histPage);
    m_stack->addWidget(linePage);
    m_stack->addWidget(monitorPage);
    root->addWidget(m_stack, 1);

    m_recomputeTimer = new QTimer(this);
    m_recomputeTimer->setSingleShot(true);
    m_recomputeTimer->setInterval(160);
    connect(m_recomputeTimer, &QTimer::timeout, this, &PlotWindow::refreshData);

    connect(m_linePlot, &LinePlotWidget::pinnedVisibilityToggled, this, &PlotWindow::togglePinnedLine);
    connect(m_linePlot, &LinePlotWidget::pinnedRemoveRequested, this, &PlotWindow::removePinnedLine);

    if (m_settings) {
        connect(m_settings, &RenderSettings::plotSettingsChanged, this, &PlotWindow::onPlotParamsChanged);
        connect(m_settings, &RenderSettings::meshDataUpdated, this, &PlotWindow::onSettingsMeshDataUpdated);
        connect(m_settings, &RenderSettings::meshLoadStateChanged, this, &PlotWindow::onSettingsMeshDataUpdated);
    }

    setMode(m_settings ? m_settings->getPlotMode() : 0);
}

void PlotWindow::exportMonitorCsv() {
    if (m_monitorSeries.empty()) { QMessageBox::information(this, tr("Export CSV"), tr("No monitor data to export.")); return; }
    QString path = QFileDialog::getSaveFileName(this, tr("Export Monitor CSV"), QString(), tr("CSV (*.csv)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) { QMessageBox::warning(this, tr("Export CSV"), tr("Cannot open file.")); return; }
    QTextStream out(&f);
    QStringList cols;
    cols << "iteration";
    for (const auto& s : m_monitorSeries) cols << s.name;
    out << cols.join(",") << "\n";
    const size_t n = m_monitorSeries.front().xs.size();
    for (size_t i = 0; i < n; ++i) {
        QStringList row;
        row << QString::number(m_monitorSeries.front().xs[i], 'g', 10);
        for (const auto& s : m_monitorSeries) {
            if (i < s.ys.size()) row << QString::number(s.ys[i], 'g', 10);
            else row << "";
        }
        out << row.join(",") << "\n";
    }
    f.close();
}

void PlotWindow::exportMonitorPng() {
    if (!m_monitorPlot) return;
    QString path = QFileDialog::getSaveFileName(this, tr("Export Monitor PNG"), QString(), tr("PNG (*.png)"));
    if (path.isEmpty()) return;
    QPixmap pm = m_monitorPlot->grab();
    if (!pm.save(path)) QMessageBox::warning(this, tr("Export PNG"), tr("Cannot save file."));
}

void PlotWindow::onPlotParamsChanged() { scheduleRecompute(); }

void PlotWindow::setMode(int mode) {
    int m = (mode >= 0 && mode <= 2) ? mode : 0;
    if (m_mode == m && m_stack && m_stack->currentIndex() == m) return;
    m_mode = m;
    if (m_stack) m_stack->setCurrentIndex(m);
    if (m == 2) updateResidualPlot();
}

void PlotWindow::clearResiduals() {
    m_monitorSeries.clear();
    m_monitorAvailableSeries.clear();
    emit monitorSeriesChanged();
    if (m_monitorPlot) m_monitorPlot->clear();
    if (m_residualStats) m_residualStats->setText(tr("No monitor data"));
    if (m_residualStatus) m_residualStatus->setText(QString());
    if (m_mode == 2) updateResidualPlot();
}

void PlotWindow::appendResidual(int iteration, int dim, double continuity,
                               double momU, double momV, double momW,
                               double k, double omega, bool hasTurb) {
    m_residualDim = dim;
    m_residualHasTurb = hasTurb;

    const float x = static_cast<float>(iteration);

    auto append = [this, x](const QString& name, QColor color, float y) {
        for (auto& s : m_monitorSeries) {
            if (s.name == name) {
                s.xs.push_back(x);
                s.ys.push_back(y);
                return;
            }
        }
        PlotSeries s;
        s.name = name;
        s.color = color;
        s.xs.push_back(x);
        s.ys.push_back(y);
        m_monitorSeries.push_back(s);
    };

    append(tr("Continuity"), QColor(0x4a, 0x90, 0xe2), static_cast<float>(continuity));
    append(tr("Momentum X"), QColor(0xB0, 0x54, 0x3A), static_cast<float>(momU));
    append(tr("Momentum Y"), QColor(0x3A, 0xA0, 0x54), static_cast<float>(momV));
    if (dim >= 3) append(tr("Momentum Z"), QColor(0xD0, 0x90, 0x30), static_cast<float>(momW));
    if (hasTurb) {
        append(tr("k"), QColor(0x8A, 0x54, 0xB0), static_cast<float>(k));
        append(tr("omega"), QColor(0x30, 0xA0, 0xA0), static_cast<float>(omega));
    }

    QStringList avail;
    avail << tr("Continuity") << tr("Momentum X") << tr("Momentum Y");
    if (m_residualDim >= 3) avail << tr("Momentum Z");
    if (hasTurb) { avail << tr("k") << tr("omega"); }
    if (avail != m_monitorAvailableSeries) {
        m_monitorAvailableSeries = avail;
        emit monitorSeriesChanged();
    }

    if (m_monitorHistoryWindow > 0) {
        for (auto& s : m_monitorSeries) {
            if ((int)s.xs.size() > m_monitorHistoryWindow) {
                s.xs.erase(s.xs.begin(), s.xs.end() - m_monitorHistoryWindow);
                s.ys.erase(s.ys.begin(), s.ys.end() - m_monitorHistoryWindow);
            }
        }
    }

    if (m_mode == 2) updateResidualPlot();
}

void PlotWindow::updateResidualPlot() {
    if (!m_monitorPlot) return;
    if (m_monitorSeries.empty()) {
        m_monitorPlot->clear();
        if (m_residualStats) m_residualStats->setText(tr("No monitor data"));
        return;
    }
    m_monitorPlot->clear();
    for (const auto& s : m_monitorSeries) {
        m_monitorPlot->setSeries(s.name, s.xs, s.ys);
    }
    if (m_residualStats) m_residualStats->setText(tr("Monitor: %1 points").arg(m_monitorSeries.front().xs.size()));
    if (m_residualStatus) m_residualStatus->setText(QString());
}

void PlotWindow::setMonitorLogScale(bool on) {
    if (m_monitorPlot) m_monitorPlot->setLogScale(on);
}

void PlotWindow::setMonitorLegendVisible(bool on) {
    if (m_monitorPlot) m_monitorPlot->setLegendVisible(on);
}

void PlotWindow::setMonitorHistoryWindow(int n) {
    m_monitorHistoryWindow = n;
}

void PlotWindow::setMonitorYRange(bool autoRange, float ymin, float ymax) {
    if (m_monitorPlot) m_monitorPlot->setManualYRange(autoRange, ymin, ymax);
}

void PlotWindow::setMonitorSeriesVisible(const QString& name, bool on) {
    if (m_monitorPlot) m_monitorPlot->setSeriesVisible(name, on);
}

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
    else if (m_mode == 2) exportMonitorCsv();
    else exportHistogramCsv();
}

void PlotWindow::exportPng() {
    if (m_mode == 1) exportLinePng();
    else if (m_mode == 2) exportMonitorPng();
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
    QTextStream s(&f);
    // Long format: one row per (line, sample). Line id column distinguishes the
    // active curve, its compare placement, and any pinned lines.
    s << "line,distance,value\n";
    auto writeRow = [&](const QString& id, float d, float v) {
        s << QString(id).replace(',', ';') << "," << d << "," << v << "\n";
    };
    QString activeLabel = tr("active[%1]").arg(m_linePlacement ? "cell" : "vertex");
    for (size_t i = 0; i < m_lineDists.size() && i < m_lineVals.size(); ++i) {
        writeRow(activeLabel, m_lineDists[i], m_lineVals[i]);
        if (!m_lineVals2.empty() && i < m_lineVals2.size())
            writeRow(tr("active~%1").arg(m_lineLabel2.isEmpty()?"cmp":m_lineLabel2), m_lineDists[i], m_lineVals2[i]);
    }
    for (const auto& ps : m_lastPinned) {
        if (ps.hidden) continue; // CSV mirrors the canvas
        for (size_t i = 0; i < ps.dists.size() && i < ps.vals.size(); ++i)
            writeRow(ps.tipLabel.isEmpty() ? ps.label : ps.tipLabel, ps.dists[i], ps.vals[i]);
    }
    m_lineStatus->setText(tr("CSV saved: %1 lines").arg(m_lastPinned.size()+1));
    m_lineStatus->setText(tr("CSV saved: %1").arg(path));
}

void PlotWindow::exportLinePng() {
    QPixmap pm=m_linePlot->grab();
    QString path=QFileDialog::getSaveFileName(this,tr("Export Line PNG"),QString(),tr("PNG (*.png)"));
    if(path.isEmpty()) return;
    if(!pm.save(path,"PNG")) QMessageBox::warning(this,tr("Export PNG"),tr("Failed to save PNG."));
    else m_lineStatus->setText(tr("PNG saved: %1").arg(path));
}

bool PlotWindow::sampleLine(const glm::vec3& p0, const glm::vec3& p1, const QString& fieldQ,
                            int placement, int N, const FieldStore::Context& ctx,
                            std::vector<float>& dists, std::vector<float>& vals, QString* err){
    std::string fname=fieldQ.toStdString();
    N=std::clamp(N,32,2048);
    float len=glm::length(p1-p0);
    if (len<1e-9f){ if(err) *err=tr("P0==P1"); return false; }
    auto pl = placement ? FieldStore::Placement::CellCenter : FieldStore::Placement::Vertex;
    // Probe once so a missing field fails fast instead of sampling all-NaN.
    auto probe = FieldSampler::sample(ctx, fname, pl, p0 + 0.5f*(p1-p0));
    if (!probe) { if(err) *err=tr("Field not found"); return false; }
    dists.clear(); vals.clear();
    dists.reserve(N); vals.reserve(N);
    for(int i=0;i<N;++i){
        float t=float(i)/float(N-1);
        glm::vec3 pos = p0 + t*(p1-p0);
        float dist = t*len;
        auto opt = FieldSampler::sample(ctx, fname, pl, pos);
        float v = opt ? *opt : std::numeric_limits<float>::quiet_NaN();
        dists.push_back(dist);
        vals.push_back(v);
    }
    return true;
}

namespace {
// Compact endpoint text for pin identities, e.g. "(0.3, 0, 0)".
QString fmtLinePoint(const glm::vec3& p) {
    return QString("(%1)").arg(QStringList{QString::number(p.x,'g',3),
                                           QString::number(p.y,'g',3),
                                           QString::number(p.z,'g',3)}.join(", "));
}
}

void PlotWindow::pinCurrentLine(){
    if(!m_settings){ return; }
    QString field=m_settings->getLineField();
    if(field.isEmpty() || !m_settings->backend() || !m_settings->backend()->lastUploadedMeshForPlot()){
        m_lineStatus->setText(tr("Nothing to pin"));
        return;
    }
    int replaceIdx = (m_editingPin >= 0 && m_editingPin < int(m_pinned.size())) ? m_editingPin : -1;
    if (m_pinned.size() >= 8 && replaceIdx < 0) {
        m_lineStatus->setText(tr("Pin limit reached (%1) — clear first").arg(m_pinned.size()));
        return;
    }
    PinnedLineDef def;    def.p0 = glm::vec3(float(m_settings->getLineP0x()), float(m_settings->getLineP0y()), float(m_settings->getLineP0z()));
    def.p1 = glm::vec3(float(m_settings->getLineP1x()), float(m_settings->getLineP1y()), float(m_settings->getLineP1z()));
    def.field = field;
    def.placement = m_settings->getLinePlacement();
    def.samples = m_settings->getLineSamples();
    // Refuse exact duplicates against OTHER pins — they draw the same curve
    // twice and the copy hides invisibly under the original. The line being
    // replaced (if any) is skipped: editing it in place without moving is legal.
    for (size_t i = 0; i < m_pinned.size(); ++i) {
        if (int(i) == replaceIdx) continue;
        const auto& ex = m_pinned[i];
        if (ex.field == def.field && ex.placement == def.placement && ex.samples == def.samples &&
            glm::all(glm::lessThan(glm::abs(ex.p0 - def.p0), glm::vec3(1e-6f))) &&
            glm::all(glm::lessThan(glm::abs(ex.p1 - def.p1), glm::vec3(1e-6f)))) {
            m_lineStatus->setText(tr("Identical to %1 — move P0/P1 to pin a new line").arg(ex.label));
            return;
        }
    }
    QString label = (replaceIdx >= 0) ? m_pinned[replaceIdx].label : tr("L%1").arg(m_pinned.size()+1);
    def.label = label;
    def.tipLabel = tr("%1 %2 [%3] (%4) → (%5)").arg(def.label).arg(field)
                       .arg(def.placement?tr("Cell"):tr("Vertex"))
                       .arg(fmtLinePoint(def.p0)).arg(fmtLinePoint(def.p1));
    if (replaceIdx >= 0) m_pinned[replaceIdx] = std::move(def);
    else                 m_pinned.push_back(std::move(def));
    m_editingPin = -1;
    emit pinnedListChanged();
    scheduleRecompute();
}

void PlotWindow::clearPinnedLines(){
    if (m_pinned.empty()) return;
    m_pinned.clear();
    m_lastPinned.clear();
    m_editingPin = -1;
    emit pinnedListChanged();
    scheduleRecompute();
}

void PlotWindow::togglePinnedLine(int index){
    if (index < 0 || index >= int(m_pinned.size())) return;
    m_pinned[index].hidden = !m_pinned[index].hidden;
    emit pinnedListChanged();
    scheduleRecompute();
}

void PlotWindow::setPinnedHiddenAt(int index, bool hidden){
    if (index < 0 || index >= int(m_pinned.size())) return;
    if (m_pinned[index].hidden == hidden) return;
    m_pinned[index].hidden = hidden;
    emit pinnedListChanged();
    scheduleRecompute();
}

void PlotWindow::removePinnedLine(int index){
    if (index < 0 || index >= int(m_pinned.size())) return;
    m_pinned.erase(m_pinned.begin() + index);
    // Keep labels contiguous: L1..Ln re-assigned after any removal.
    for (size_t i = 0; i < m_pinned.size(); ++i) {
        m_pinned[i].label = tr("L%1").arg(i+1);
        m_pinned[i].tipLabel = tr("%1 %2 [%3] (%4) → (%5)").arg(m_pinned[i].label).arg(m_pinned[i].field)
                                   .arg(m_pinned[i].placement?tr("Cell"):tr("Vertex"))
                                   .arg(fmtLinePoint(m_pinned[i].p0)).arg(fmtLinePoint(m_pinned[i].p1));
    }
    if (m_editingPin == index) m_editingPin = -1;
    else if (m_editingPin > index) --m_editingPin;
    if (m_editingPin >= int(m_pinned.size())) m_editingPin = -1;
    emit pinnedListChanged();
    scheduleRecompute();
}

void PlotWindow::editPinnedLine(int index){
    if (index < 0 || index >= int(m_pinned.size()) || !m_settings) return;
    const PinnedLineDef& d = m_pinned[index];
    m_settings->setLineP0x(d.p0.x); m_settings->setLineP0y(d.p0.y); m_settings->setLineP0z(d.p0.z);
    m_settings->setLineP1x(d.p1.x); m_settings->setLineP1y(d.p1.y); m_settings->setLineP1z(d.p1.z);
    m_settings->setLineField(d.field);
    m_settings->setLinePlacement(d.placement);
    m_settings->setLineSamples(d.samples);
    m_editingPin = index;
    emit pinnedListChanged(); // sidebar marks this row with the edit marker
    scheduleRecompute();
}

std::vector<PlotWindow::PinInfo> PlotWindow::pinnedLineInfos() const {
    std::vector<PinInfo> out;
    out.reserve(m_pinned.size());
    for (const auto& d : m_pinned) {
        out.push_back({ tr("%1  %2→%3").arg(d.label).arg(fmtLinePoint(d.p0)).arg(fmtLinePoint(d.p1)),
                        d.tipLabel.isEmpty() ? d.label : d.tipLabel, d.hidden });
    }
    return out;
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
    // Active line
    std::vector<float> dists; std::vector<float> vals;
    QString err;
    if (!sampleLine(p0, p1, field, useCell?1:0, N, ctx, dists, vals, &err)) {
        m_lineStatus->setText(err);
        return;
    }
    // Compare mode: sample the same field along the SAME param points at the
    // other placement. Cell sampling stays flat per cell (piecewise constant);
    // vertex sampling is trilinear — their split is exactly the interpolation
    // error the user is probing for, hence the dual curve.
    std::vector<float> vals2;
    QString label2;
    if (m_settings->getComparePlacements()) {
        int other = useCell ? 0 : 1;
        std::vector<float> d2;
        if (sampleLine(p0, p1, field, other, N, ctx, d2, vals2)) {
            label2 = other ? tr("Cell") : tr("Vertex");
        } else {
            vals2.clear();
        }
    }
    // Pinned lines: re-sample their stored definitions against the CURRENT
    // mesh/snapshot so a mesh load or post-proc extract never leaves stale curves.
    std::vector<LineSeries> pinnedSeries;
    pinnedSeries.reserve(m_pinned.size());
    for (const auto& def : m_pinned) {
        std::vector<float> pd, pv;
        if (sampleLine(def.p0, def.p1, def.field, def.placement, def.samples, ctx, pd, pv))
            pinnedSeries.push_back({std::move(pd), std::move(pv), def.label, def.tipLabel, def.hidden});
    }
    m_lastPinned = pinnedSeries;
    // Y axis follows the sampled data, not the whole-field range. With compare
    // mode the range widens so both curves stay inside the frame.
    float sMin = std::numeric_limits<float>::max(), sMax = std::numeric_limits<float>::lowest();
    auto span = [&](const std::vector<float>& vv) {
        for (float v : vv) { if (!std::isfinite(v)) continue; sMin = std::min(sMin, v); sMax = std::max(sMax, v); }
    };
    span(vals);
    span(vals2);
    for (const auto& ps : pinnedSeries) span(ps.vals);
    if (sMin > sMax) { sMin = 0; sMax = 1; }
    float pad = (std::abs(sMax - sMin) < 1e-9f) ? std::max(1e-3f, std::abs(sMax) * 0.01f)
                                                : (sMax - sMin) * 0.05f;
    sMin -= pad; sMax += pad;
    m_lineDists=dists; m_lineVals=vals; m_lineMin=sMin; m_lineMax=sMax; m_lineField=field; m_linePlacement = useCell?1:0;
    m_lineVals2 = std::move(vals2); m_lineLabel2 = label2;
    m_linePlot->setData(dists, vals, sMin, sMax, field, m_linePlacement);
    m_linePlot->setCompareSeries(m_lineVals2, label2);
    m_linePlot->setPinnedLines(std::move(pinnedSeries));
    m_lineStats->setText(tr("Field %1 [%2]  %3 samples  range [%4 .. %5]").arg(field).arg(useCell?"Cell":"Vertex").arg(N).arg(sMin,0,'g',4).arg(sMax,0,'g',4));
    QString editHint;
    if (m_editingPin >= 0 && m_editingPin < int(m_pinned.size()))
        editHint = tr(" — editing %1: Pin replaces it").arg(m_pinned[m_editingPin].label);
    m_lineStatus->setText(
        (m_lastPinned.empty()
            ? (m_lineVals2.empty() ? tr("Sampled %1 points along line").arg(N)
                                   : tr("Sampled %1 points along line (dual: %2 vs %3)").arg(N).arg(useCell?"Cell":"Vertex").arg(m_lineLabel2))
            : tr("%1 pinned line(s) on canvas").arg(m_lastPinned.size()))
        + editHint);
}

