#include "plots_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include <functional>

#include <QStandardItemModel>

#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"
#include "ui/widgets/plot_window.h"

namespace sidebar {

PlotsPage::PlotsPage(RenderSettings* settings, PlotWindow* plotWindow,
                     bool viewportVisible, QWidget* parent)
    : SidebarPage(settings, parent), m_plotWindow(plotWindow) {
    auto* content = new QWidget;
    auto* lay = new QVBoxLayout(content);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(4);
    auto* s = S();

    // --- viewport toggle ---
    auto* openBtn = new QPushButton(tr("Open Plots Viewport"));
    m_openBtn = openBtn;
    openBtn->setCheckable(true);
    openBtn->blockSignals(true);
    openBtn->setChecked(viewportVisible);
    openBtn->blockSignals(false);
    openBtn->setMinimumHeight(32);
    lay->addWidget(openBtn);

    // --- mode switch (was the dock's tab bar) ---
    auto* modeHeader = new QLabel(tr("Mode"));
    modeHeader->setObjectName("modeHeader");
    lay->addWidget(modeHeader);

    auto* modeRow = new QHBoxLayout;
    modeRow->setSpacing(4);
    m_modeHistBtn = new QToolButton(content);
    m_modeHistBtn->setText(tr("Histogram"));
    m_modeHistBtn->setCheckable(true);
    m_modeHistBtn->setAutoExclusive(true);
    m_modeHistBtn->setFixedHeight(sb::kControlHeight + 2);
    m_modeHistBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_modeLineBtn = new QToolButton(content);
    m_modeLineBtn->setText(tr("Line Plot"));
    m_modeLineBtn->setCheckable(true);
    m_modeLineBtn->setAutoExclusive(true);
    m_modeLineBtn->setFixedHeight(sb::kControlHeight + 2);
    m_modeLineBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    modeRow->addWidget(m_modeHistBtn, 1);
    modeRow->addWidget(m_modeLineBtn, 1);
    lay->addLayout(modeRow);

    // --- parameter stack: one pane per mode ---
    // Row pattern mirrors probe_page.ui: one fixed 56px label column so every
    // value widget line up at the same x, spacing 4/6; the pane pins its rows
    // to the top and the stacked widget is capped at its size hint so the
    // panel's spare height stays at the bottom.
    auto* histPane = new QWidget;
    {
        auto* hl = new QVBoxLayout(histPane);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(4);
        auto* fRow = new QHBoxLayout;
        fRow->setSpacing(6);
        auto* fLabel = new QLabel(tr("Field"));
        fLabel->setObjectName("plotFieldLabel");
        fLabel->setFixedWidth(56);
        m_histFieldCombo = new QComboBox;
        m_histFieldCombo->setMinimumWidth(56);
        m_histFieldCombo->setFixedHeight(sb::kControlHeight);
        fRow->addWidget(fLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
        fRow->addWidget(m_histFieldCombo, 1);
        hl->addLayout(fRow);
        auto* pRow = new QHBoxLayout;
        pRow->setSpacing(6);
        auto* pLabel = new QLabel(tr("Node/Cell"));
        pLabel->setObjectName("plotPlacementLabel");
        pLabel->setFixedWidth(56);
        m_histPlacementCombo = new QComboBox;
        m_histPlacementCombo->addItems({tr("Vertex"), tr("Cell")});
        m_histPlacementCombo->setMinimumWidth(56);
        m_histPlacementCombo->setFixedHeight(sb::kControlHeight);
        m_histPlacementCombo->setToolTip(tr("Vertex = per-vertex (point) values, post-split, extrapolated if needed. Cell = raw per-cell values."));
        pRow->addWidget(pLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
        pRow->addWidget(m_histPlacementCombo, 1);
        hl->addLayout(pRow);
        auto* bRow = new QHBoxLayout;
        bRow->setSpacing(6);
        auto* bLabel = new QLabel(tr("Bins"));
        bLabel->setObjectName("plotBinsLabel");
        bLabel->setFixedWidth(56);
        m_histBinsSpin = new QSpinBox;
        m_histBinsSpin->setRange(8, 512);
        m_histBinsSpin->setValue(64);
        m_histBinsSpin->setSingleStep(8);
        m_histBinsSpin->setMinimumWidth(56);
        bRow->addWidget(bLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
        bRow->addWidget(m_histBinsSpin, 1);
        hl->addLayout(bRow);
        hl->addStretch(1);
    }

    auto* linePane = new QWidget;
    {
        auto* ll = new QVBoxLayout(linePane);
        ll->setContentsMargins(0, 0, 0, 0);
        ll->setSpacing(4);
        auto* fRow = new QHBoxLayout;
        fRow->setSpacing(6);
        auto* fLabel = new QLabel(tr("Field"));
        fLabel->setObjectName("plotLineFieldLabel");
        fLabel->setFixedWidth(56);
        m_lineFieldCombo = new QComboBox;
        m_lineFieldCombo->setMinimumWidth(56);
        m_lineFieldCombo->setFixedHeight(sb::kControlHeight);
        fRow->addWidget(fLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
        fRow->addWidget(m_lineFieldCombo, 1);
        ll->addLayout(fRow);
        auto* pRow = new QHBoxLayout;
        pRow->setSpacing(6);
        auto* pLabel = new QLabel(tr("Node/Cell"));
        pLabel->setObjectName("plotLinePlacementLabel");
        pLabel->setFixedWidth(56);
        m_linePlacementCombo = new QComboBox;
        m_linePlacementCombo->addItems({tr("Vertex"), tr("Cell")});
        m_linePlacementCombo->setMinimumWidth(56);
        m_linePlacementCombo->setFixedHeight(sb::kControlHeight);
        m_linePlacementCombo->setToolTip(tr("Vertex = trilinear point, Cell = nearest cell"));
        pRow->addWidget(pLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
        pRow->addWidget(m_linePlacementCombo, 1);
        ll->addLayout(pRow);
        auto* sRow = new QHBoxLayout;
        sRow->setSpacing(6);
        auto* sLabel = new QLabel(tr("Samples"));
        sLabel->setObjectName("plotSamplesLabel");
        sLabel->setFixedWidth(56);
        m_samplesSpin = new QSpinBox;
        m_samplesSpin->setRange(32, 2048);
        m_samplesSpin->setValue(256);
        m_samplesSpin->setSingleStep(32);
        m_samplesSpin->setMinimumWidth(56);
        sRow->addWidget(sLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
        sRow->addWidget(m_samplesSpin, 1);
        ll->addLayout(sRow);
        m_compareCb = new QCheckBox(tr("Compare vertex vs cell"));
        m_compareCb->setToolTip(tr("Draw the same field sampled at the other placement as a dashed curve: "
                                   "Cell = nearest cell (piecewise constant), Vertex = trilinear. Their split "
                                   "reveals interpolation/extrapolation error along the line."));
        ll->addWidget(m_compareCb);
        auto* pinRow = new QHBoxLayout;
        pinRow->setSpacing(4);
        auto* pinBtn = new QPushButton(tr("Pin line"));
        auto* clearPinBtn = new QPushButton(tr("Clear"));
        pinBtn->setToolTip(tr("Snapshot the current curve onto the plot, then move P0/P1 or change samples "
                              "to lay down more lines. Pinned lines re-sample when the mesh changes."));
        pinBtn->setFixedHeight(sb::kControlHeight + 2);
        clearPinBtn->setToolTip(tr("Remove all pinned lines"));
        clearPinBtn->setFixedHeight(sb::kControlHeight + 2);
        pinBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        clearPinBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        if (m_plotWindow) {
            connect(pinBtn, &QPushButton::clicked, m_plotWindow, &PlotWindow::pinCurrentLine);
            connect(clearPinBtn, &QPushButton::clicked, m_plotWindow, &PlotWindow::clearPinnedLines);
        }
        pinRow->addWidget(pinBtn, 1);
        pinRow->addWidget(clearPinBtn, 1);
        ll->addLayout(pinRow);
        // Pinned-lines manager (isosurface-contours idiom): checkable rows so
        // removal/hiding is discoverable in the panel, not buried in the legend.
        // The block is hidden entirely until the first pin exists.
        m_pinGroup = new QWidget(linePane);
        auto* pg = new QVBoxLayout(m_pinGroup);
        pg->setContentsMargins(0, 0, 0, 0);
        pg->setSpacing(4);
        auto* pinListHeader = new QLabel(tr("Pinned Lines"));
        pinListHeader->setObjectName("plotPinListHeader");
        pg->addWidget(pinListHeader);
        m_pinList = new QListWidget(m_pinGroup);
        m_pinList->setSelectionMode(QAbstractItemView::SingleSelection);
        m_pinList->setMinimumHeight(56);
        m_pinList->setMaximumHeight(150);   // 8 pins * ~18px + frame
        m_pinList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_pinList->setToolTip(tr("Check = visible on the plot. Double-click (or Edit) loads the line's P0/P1, "
                                 "field, placement and samples back into this panel — \"Pin line\" then "
                                 "overwrites that entry. Remove deletes it."));
        pg->addWidget(m_pinList);
        auto* pinManageRow = new QHBoxLayout;
        pinManageRow->setSpacing(4);
        auto* editPinBtn = new QPushButton(tr("Edit"));
        auto* removePinBtn = new QPushButton(tr("Remove"));
        editPinBtn->setToolTip(m_pinList->toolTip());
        removePinBtn->setToolTip(tr("Remove the selected pinned line"));
        for (auto* b : {editPinBtn, removePinBtn}) {
            b->setFixedHeight(sb::kControlHeight);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        }
        pinManageRow->addWidget(editPinBtn, 1);
        pinManageRow->addWidget(removePinBtn, 1);
        pg->addLayout(pinManageRow);
        m_pinGroup->setVisible(false);
        ll->addWidget(m_pinGroup);
        if (m_plotWindow) {
            connect(m_plotWindow, &PlotWindow::pinnedListChanged, this, [this]() { rebuildPinList(); });
            connect(m_pinList, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
                if (!m_plotWindow || !it) return;
                m_plotWindow->setPinnedHiddenAt(m_pinList->row(it), it->checkState() != Qt::Checked);
            });
            auto editSlot = [this]() {
                int r = m_pinList ? m_pinList->currentRow() : -1;
                if (r >= 0 && m_plotWindow) m_plotWindow->editPinnedLine(r);
            };
            connect(m_pinList, &QListWidget::itemDoubleClicked, this, [editSlot](QListWidgetItem*) { editSlot(); });
            connect(editPinBtn, &QPushButton::clicked, this, editSlot);
            connect(removePinBtn, &QPushButton::clicked, this, [this]() {
                int r = m_pinList ? m_pinList->currentRow() : -1;
                if (r >= 0 && m_plotWindow) m_plotWindow->removePinnedLine(r);
            });
        }
        // Line endpoints P0 (start) → P1 (end), one spinbox per axis.
        auto* lpHeader = new QLabel(tr("Line Points (P0 → P1)"));
        lpHeader->setObjectName("plotLinePointsHeader");
        ll->addWidget(lpHeader);
        auto makeEndpointRow = [ll](const QString& title, const QString& tip,
                                    QDoubleSpinBox** xs, QDoubleSpinBox** ys, QDoubleSpinBox** zs) {
            auto* row = new QHBoxLayout;
            row->setSpacing(2);
            auto* lbl = new QLabel(title);
            lbl->setObjectName("plotPointLabel");
            lbl->setFixedWidth(56);
            lbl->setToolTip(tip);
            lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            row->addWidget(lbl, 0, Qt::AlignLeft | Qt::AlignVCenter);
            *xs = new QDoubleSpinBox;
            *ys = new QDoubleSpinBox;
            *zs = new QDoubleSpinBox;
            for (QDoubleSpinBox* sb : {*xs, *ys, *zs}) {
                sb->setRange(-1e6, 1e6);
                sb->setDecimals(3);
                sb->setToolTip(tip);
                sb->setMinimumWidth(40);
                sb->setFixedHeight(sb::kControlHeight);
                sb->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            }
            row->addWidget(*xs, 1);
            row->addWidget(*ys, 1);
            row->addWidget(*zs, 1);
            ll->addLayout(row);
        };
        makeEndpointRow(tr("P0"), tr("Line start point (cyan marker in the 3D viewport)"),
                        &m_p0x, &m_p0y, &m_p0z);
        makeEndpointRow(tr("P1"), tr("Line end point (pink marker in the 3D viewport)"),
                        &m_p1x, &m_p1y, &m_p1z);
        m_showLineCb = new QCheckBox(tr("Show line"));
        m_showLineCb->setToolTip(tr("Draw the P0→P1 sampling line and its endpoints in the 3D viewport"));
        ll->addWidget(m_showLineCb);
        ll->addStretch(1);
    }

    m_paramStack = new QStackedWidget(content);
    m_paramStack->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    m_paramStack->addWidget(histPane);
    m_paramStack->addWidget(linePane);
    lay->addWidget(m_paramStack, 0);

    // --- export group ---
    auto* exportHeader = new QLabel(tr("Export"));
    exportHeader->setObjectName("exportHeader");
    lay->addWidget(exportHeader);
    auto* exportRow = new QHBoxLayout;
    exportRow->setSpacing(4);
    auto* csvBtn = new QPushButton(tr("CSV…"));
    auto* pngBtn = new QPushButton(tr("PNG…"));
    csvBtn->setToolTip(tr("Export the active plot as CSV"));
    pngBtn->setToolTip(tr("Export the active plot canvas as PNG"));
    csvBtn->setFixedHeight(sb::kControlHeight + 2);
    pngBtn->setFixedHeight(sb::kControlHeight + 2);
    csvBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    pngBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    exportRow->addWidget(csvBtn, 1);
    exportRow->addWidget(pngBtn, 1);
    lay->addLayout(exportRow);

    lay->addStretch();

    // Keep plot canvas mode in lockstep with the settings (single source of truth).
    connect(s, &RenderSettings::plotSettingsChanged, this, [this]() {
        int mode = S()->getPlotMode();
        if (m_paramStack) m_paramStack->setCurrentIndex(mode);
        if (m_plotWindow) m_plotWindow->setMode(mode);
        if (m_modeHistBtn) { m_modeHistBtn->blockSignals(true); m_modeHistBtn->setChecked(mode == 0); m_modeHistBtn->blockSignals(false); }
        if (m_modeLineBtn) { m_modeLineBtn->blockSignals(true); m_modeLineBtn->setChecked(mode == 1); m_modeLineBtn->blockSignals(false); }
    });

    connect(m_modeHistBtn, &QToolButton::toggled, this, [this](bool on) { if (on && S()->getPlotMode() != 0) S()->setPlotMode(0); });
    connect(m_modeLineBtn, &QToolButton::toggled, this, [this](bool on) { if (on && S()->getPlotMode() != 1) S()->setPlotMode(1); });

    connect(m_histFieldCombo, &QComboBox::currentTextChanged, this, [this](const QString& t) {
        if (t == tr("(no scalar fields)")) return;
        S()->setHistField(t);
        updatePlacementCombo(m_histPlacementCombo, false);
    });
    connect(m_lineFieldCombo, &QComboBox::currentTextChanged, this, [this](const QString& t) {
        if (t == tr("(no scalar fields)")) return;
        S()->setLineField(t);
        updatePlacementCombo(m_linePlacementCombo, true);
    });
    connect(m_histPlacementCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) { if (idx >= 0) S()->setHistPlacement(idx); });
    connect(m_linePlacementCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) { if (idx >= 0) S()->setLinePlacement(idx); });
    connect(m_histBinsSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { S()->setHistBins(v); });
    connect(m_samplesSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { S()->setLineSamples(v); });
    connect(m_compareCb, &QCheckBox::toggled, this, [this](bool v) { S()->setComparePlacements(v); });

    auto connectProbeSpin = [this](QDoubleSpinBox* sb, std::function<void(double)> setter) {
        connect(sb, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, setter](double v) {
            m_lineAutoSeeded = false; // user moved the line; mesh loads keep their endpoints
            setter(v);
            if (m_plotWindow) m_plotWindow->scheduleRecompute();
        });
    };
    connectProbeSpin(m_p0x, [this](double v) { S()->setLineP0x(v); });
    connectProbeSpin(m_p0y, [this](double v) { S()->setLineP0y(v); });
    connectProbeSpin(m_p0z, [this](double v) { S()->setLineP0z(v); });
    connectProbeSpin(m_p1x, [this](double v) { S()->setLineP1x(v); });
    connectProbeSpin(m_p1y, [this](double v) { S()->setLineP1y(v); });
    connectProbeSpin(m_p1z, [this](double v) { S()->setLineP1z(v); });

    connect(m_showLineCb, &QCheckBox::toggled, this, [this](bool v) { if (S()->getShowLineProbe() != v) S()->setShowLineProbe(v); });

    connect(csvBtn, &QPushButton::clicked, this, [this]() { if (m_plotWindow) m_plotWindow->exportCsv(); });
    connect(pngBtn, &QPushButton::clicked, this, [this]() { if (m_plotWindow) m_plotWindow->exportPng(); });

    // Viewport toggle -> MainWindow::setHistogramVisible
    connect(openBtn, &QPushButton::toggled, this, [this](bool checked) {
        applyViewportState();
        emit viewportToggleRequested(checked);
    });

    // Canvas may not yet hold data when the page is first built.
    connect(s, &RenderSettings::meshDataUpdated, this, [this]() {
        refreshFieldCombos();
        initLineProbeFromMeshBounds();
    });
    connect(s, &RenderSettings::meshLoadStateChanged, this, [this]() { refreshFieldCombos(); });
    connect(s, &RenderSettings::viewChanged, this, [this](ChangeFlags) { syncFromSettings(); });

    // Initial population
    refreshFieldCombos();
    initLineProbeFromMeshBounds();
    syncFromSettings();
    rebuildPinList();
    {
        int mode = s->getPlotMode();
        if (m_paramStack) m_paramStack->setCurrentIndex(mode);
        if (m_plotWindow) m_plotWindow->setMode(mode);
        if (m_modeHistBtn) m_modeHistBtn->setChecked(mode == 0);
        if (m_modeLineBtn) m_modeLineBtn->setChecked(mode == 1);
    }

    setContent(content);
}

void PlotsPage::setViewportVisible(bool visible) {
    if (!m_openBtn) return;
    m_openBtn->blockSignals(true);
    m_openBtn->setChecked(visible);
    m_openBtn->blockSignals(false);
    applyViewportState();
}

void PlotsPage::applyViewportState() {
    if (m_openBtn)
        m_openBtn->setText(m_openBtn->isChecked() ? tr("Hide Plot Viewport") : tr("Open Plots Viewport"));
}

// Populate the per-mode field combos from RenderSettings, preserving the
// user's previous selection and falling back to the active scalar.
void PlotsPage::refreshFieldCombos() {
    auto* s = S();
    const QStringList fields = s->getAvailableScalars();
    auto fill = [this, s, &fields](QComboBox* combo, const QString& current, bool lineMode) {        if (!combo) return;
        combo->blockSignals(true);
        combo->clear();
        if (fields.isEmpty()) {
            combo->addItem(tr("(no scalar fields)"));
            combo->setEnabled(false);
        } else {
            combo->addItems(fields);
            combo->setEnabled(true);
            int idx = combo->findText(current);
            if (idx < 0) idx = combo->findText(s->getActiveScalarNameQml());
            if (idx >= 0) combo->setCurrentIndex(idx);
        }
        combo->blockSignals(false);
        const QString selected = fields.isEmpty() ? QString() : combo->currentText();
        if (lineMode) s->setLineField(selected);
        else s->setHistField(selected);
    };
    fill(m_histFieldCombo, s->getHistField(), false);
    fill(m_lineFieldCombo, s->getLineField(), true);
    updatePlacementCombo(m_histPlacementCombo, false);
    updatePlacementCombo(m_linePlacementCombo, true);
    bool hasData = !fields.isEmpty();
    if (m_histBinsSpin) m_histBinsSpin->setEnabled(hasData);
    if (m_samplesSpin) m_samplesSpin->setEnabled(hasData);
}

// Enable/disable the Cell item of a placement combo depending on what the
// selected field actually provides (mirrors the old PlotWindow logic).
void PlotsPage::updatePlacementCombo(QComboBox* combo, bool lineMode) {
    if (!combo || !S()) return;
    auto* s = S();
    const QString field = lineMode ? s->getLineField() : s->getHistField();
    auto avail = plotPlacementAvailability(s, field);
    int cur = lineMode ? s->getLinePlacement() : s->getHistPlacement();
    if (cur == 1 && !avail.hasCell) cur = 0;
    if (cur == 0 && !avail.hasPoint && avail.hasCell) cur = 1;
    if (lineMode) s->setLinePlacement(cur);
    else s->setHistPlacement(cur);
    combo->blockSignals(true);
    combo->setCurrentIndex(cur);
    if (auto* model = qobject_cast<QStandardItemModel*>(combo->model())) {
        if (auto* item = model->item(1)) item->setEnabled(avail.hasCell);
    } else {
        combo->setItemData(1, QVariant(avail.hasCell), Qt::UserRole - 1);
    }
    combo->setToolTip(avail.hasCell ? tr("Raw cell data available") : tr("No raw cell data for this field — Vertex only"));
    combo->setEnabled(avail.hasPoint || avail.hasCell);
    combo->blockSignals(false);
}

// Seed the probe line along the current mesh bbox diagonal (mesh min/max,
// not the 0..1 defaults). Runs on every mesh load until the user edits the
// endpoints by hand; a manually-placed line is never stomped.
void PlotsPage::initLineProbeFromMeshBounds() {
    auto* s = S();
    if (!s || !s->getHasMeshLoaded()) return;
    if (!m_lineAutoSeeded) return;
    auto* r = s->backend();
    if (!r) return;
    auto* rm = r->lastUploadedMeshForPlot();
    if (!rm) return;
    s->setLineP0x(rm->bounds.minX); s->setLineP0y(rm->bounds.minY); s->setLineP0z(rm->bounds.minZ);
    s->setLineP1x(rm->bounds.maxX); s->setLineP1y(rm->bounds.maxY); s->setLineP1z(rm->bounds.maxZ);
    if (m_plotWindow) m_plotWindow->scheduleRecompute();
}

// Rebuild the pinned-lines list from PlotWindow's authoritative definitions.
// blockSignals during rebuild so the synthetic itemChanged ticks don't bounce
// hide flags back at PlotWindow.
void PlotsPage::rebuildPinList() {
    if (!m_pinList || !m_pinGroup) return;
    m_pinList->blockSignals(true);
    m_pinList->clear();
    int editing = m_plotWindow ? m_plotWindow->editingPin() : -1;
    if (m_plotWindow) {
        const auto infos = m_plotWindow->pinnedLineInfos();
        for (size_t i = 0; i < infos.size(); ++i) {
            // mark the entry loaded into the panel for in-place editing
            QString text = (int(i) == editing) ? tr("* ") + infos[i].label : infos[i].label;
            auto item = new QListWidgetItem(text, m_pinList);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            item->setCheckState(infos[i].hidden ? Qt::Unchecked : Qt::Checked);
            item->setToolTip(infos[i].tip);
            item->setData(Qt::UserRole, int(i));
        }
    }
    m_pinList->blockSignals(false);
    m_pinGroup->setVisible(m_pinList->count() > 0);
}

// Mirror RenderSettings back into the page (P0/P1 can be edited elsewhere,
// e.g. viewport overlays; wheel/arrow step = 1/10th of the bbox length).
void PlotsPage::syncFromSettings() {
    auto* s = S();
    if (!s) return;
    {
        double lenX = s->getWorldMaxX() - s->getWorldMinX();
        double lenY = s->getWorldMaxY() - s->getWorldMinY();
        double lenZ = s->getWorldMaxZ() - s->getWorldMinZ();
        double sx = lenX > 1e-12 ? lenX / 10.0 : 0.1;
        double sy = lenY > 1e-12 ? lenY / 10.0 : 0.1;
        double sz = lenZ > 1e-12 ? lenZ / 10.0 : 0.1;
        if (m_p0x) m_p0x->setSingleStep(sx); if (m_p1x) m_p1x->setSingleStep(sx);
        if (m_p0y) m_p0y->setSingleStep(sy); if (m_p1y) m_p1y->setSingleStep(sy);
        if (m_p0z) m_p0z->setSingleStep(sz); if (m_p1z) m_p1z->setSingleStep(sz);
    }
    auto syncSpin = [this](QDoubleSpinBox* sb, double v) {
        if (!sb) return;
        if (std::abs(sb->value() - v) < 1e-9) return;
        sb->blockSignals(true); sb->setValue(v); sb->blockSignals(false);
    };
    syncSpin(m_p0x, s->getLineP0x());
    syncSpin(m_p0y, s->getLineP0y());
    syncSpin(m_p0z, s->getLineP0z());
    syncSpin(m_p1x, s->getLineP1x());
    syncSpin(m_p1y, s->getLineP1y());
    syncSpin(m_p1z, s->getLineP1z());
    if (m_showLineCb) {
        m_showLineCb->blockSignals(true);
        m_showLineCb->setChecked(s->getShowLineProbe());
        m_showLineCb->blockSignals(false);
    }
    if (m_compareCb) {
        m_compareCb->blockSignals(true);
        m_compareCb->setChecked(s->getComparePlacements());
        m_compareCb->blockSignals(false);
    }
}

} // namespace sidebar
