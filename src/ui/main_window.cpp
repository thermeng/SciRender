#include "main_window.h"
#include "ui_main_window.h"
#include "animation_export_dialog.h"
#include "ui/widgets/plot_window.h"
#include "ui/panel_helpers.h"
#include <cmath>
#include <QApplication>
#include <QMenuBar>
#include <QMenu>
#include <QProgressDialog>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QLabel>
#include <QToolButton>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QFrame>
#include <QAbstractItemView>
#include <QFile>
#include <QTextStream>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QUrl>
#include <QResizeEvent>
#include <QFileDialog>
#include <QMessageBox>
#include <QShortcut>
#include <QActionGroup>
#include <QAction>
#include <QStyle>
#include <algorithm>
#include <QTimer>
#include <QFontMetrics>
// Helper: Does a widget want typing/navigation keys (so viewport shortcuts
// must stay out of the way)?

static bool navFocusIsEditor(QWidget* w) {
    return w && (qobject_cast<QLineEdit*>(w) ||
                 qobject_cast<QComboBox*>(w) || qobject_cast<QSpinBox*>(w) ||
                 qobject_cast<QDoubleSpinBox*>(w));
}


// Shared panel helpers live in ui/panel_helpers (used by every session page).
using sb::kSidebarWidth;


// MainWindow

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow) {
    ui->setupUi(this);

    m_settings = new RenderSettings(this);
    m_settings->restoreStateFromSettings();

    // Viewport + toolbar + histogram split (histogram is now docked, not floating)
    auto* viewportContainer = new QWidget;
    auto* vbox = new QVBoxLayout(viewportContainer);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    m_viewport = new ViewportWidget(m_settings->getMsaaSamples(), this);
    m_viewport->setSettings(m_settings);
    vbox->addWidget(m_viewport, 1);

    // Plot: dockable viewport constrained to original viewport location.
    // Inner QMainWindow hosts viewport + plot so plot can only dock around the
    // viewport (Left/Right/Top/Bottom of central area) â€” never before the sidebar.
    m_centralHost = new QMainWindow(this);
    m_centralHost->setObjectName("CentralHost");
    m_centralHost->setWindowFlags(Qt::Widget);
    m_centralHost->setDockNestingEnabled(true);
    m_centralHost->setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks);
    m_centralHost->setCentralWidget(viewportContainer);

    m_histContainer = new QWidget;
    m_histContainer->setMinimumSize(320, 180);
    auto* histLay = new QVBoxLayout(m_histContainer);
    histLay->setContentsMargins(0, 0, 0, 0);
    histLay->setSpacing(0);
    m_plotWindow = new PlotWindow(m_settings, m_histContainer);
    histLay->addWidget(m_plotWindow, 1);

    m_plotDock = new QDockWidget(tr("Plots"), m_centralHost);
    m_plotDock->setObjectName("PlotDock");
    m_plotDock->setWidget(m_histContainer);
    m_plotDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    // No Floatable â€” plot must stay docked inside central host (original viewport location)
    m_plotDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    m_centralHost->addDockWidget(Qt::RightDockWidgetArea, m_plotDock); // default: horizontally beside scene
    m_plotDock->hide();
    connect(m_plotWindow, &PlotWindow::requestCloseHistogram, this, [this](){ setHistogramVisible(false); });
    connect(m_plotWindow, &PlotWindow::requestOpenControls, this, [this](){ setSidebarSection(14); }); // 14 = Plots nav section
    connect(m_plotDock, &QDockWidget::visibilityChanged, this, [this](bool v){ m_histogramVisible = v; });

    setCentralWidget(m_centralHost);
    setDockNestingEnabled(true);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks);

    // Accept drops
    setAcceptDrops(true);

    setupMenus();
    setupTopToolbar();
    if (m_topToolbar) {
        vbox->insertWidget(0, m_topToolbar);
    }
    setupSidebar();
    setupTimers();
    setupKeyboardShortcuts();
    connectSettings();
    updateStatusBar();

    // Save state on quit
    connect(qApp, &QCoreApplication::aboutToQuit, m_settings, &RenderSettings::saveStateToSettings);
}

MainWindow::~MainWindow() {
    delete ui;
}


// Menus

void MainWindow::setupMenus() {
    auto* menuBar = this->menuBar();

    // File menu
    auto* fileMenu = ui->menuFile;
    fileMenu->addAction("&Open Mesh...", this, &MainWindow::openMesh);
    fileMenu->addAction("&Reload", this, &MainWindow::reloadMesh);
    fileMenu->addSeparator();

    auto* recentMenu = fileMenu->addMenu("Open &Recent");
    connect(recentMenu, &QMenu::aboutToShow, this, [recentMenu, this]() {
        recentMenu->clear();
        auto files = m_settings->getRecentFiles();
        if (files.isEmpty()) {
            auto* emptyAction = recentMenu->addAction("(No recent files)");
            emptyAction->setEnabled(false);
            return;
        }
        for (const auto& f : files) {
            recentMenu->addAction(f, this, [this, f]() { openRecent(f); });
        }
        recentMenu->addSeparator();
        recentMenu->addAction("Clear Recent Files", this, &MainWindow::clearRecentFiles);
    });

    fileMenu->addAction("&Clear", this, &MainWindow::clearMeshes);
    fileMenu->addSeparator();
    fileMenu->addAction("E&xit", qApp, &QApplication::quit);

    // View menu
    auto* viewMenu = ui->menuView;
    viewMenu->addAction("&Mesh Info", this, [this]() { setSidebarSection(0); });
    viewMenu->addAction("&Lighting", this, [this]() { setSidebarSection(1); });
    viewMenu->addAction("&Clipping", this, [this]() { setSidebarSection(2); });
    viewMenu->addSeparator();

    auto* cullGroup = new QActionGroup(this);
    cullGroup->setExclusive(true);
    auto* cullOff = cullGroup->addAction("Culling: Off");
    cullOff->setCheckable(true);
    cullOff->setChecked(m_settings->getCullMode() == 0);
    connect(cullOff, &QAction::triggered, m_settings, [this]() { m_settings->setCullMode(0); });

    auto* cullBack = cullGroup->addAction("Culling: Back faces");
    cullBack->setCheckable(true);
    cullBack->setChecked(m_settings->getCullMode() == 1);
    connect(cullBack, &QAction::triggered, m_settings, [this]() { m_settings->setCullMode(1); });

    auto* cullFront = cullGroup->addAction("Culling: Front faces");
    cullFront->setCheckable(true);
    cullFront->setChecked(m_settings->getCullMode() == 2);
    connect(cullFront, &QAction::triggered, m_settings, [this]() { m_settings->setCullMode(2); });

    viewMenu->addActions(cullGroup->actions());
    viewMenu->addSeparator();

    auto* shadingGroup = new QActionGroup(this);
    shadingGroup->setExclusive(true);

    auto* flatAction = shadingGroup->addAction("Flat Shading");
    flatAction->setCheckable(true);
    flatAction->setChecked(m_settings->getFlatShading());
    connect(flatAction, &QAction::triggered, m_settings, [this]() { m_settings->setFlatShading(true); });

    auto* smoothAction = shadingGroup->addAction("Smooth Shading");
    smoothAction->setCheckable(true);
    smoothAction->setChecked(!m_settings->getFlatShading());
    connect(smoothAction, &QAction::triggered, m_settings, [this]() { m_settings->setFlatShading(false); });

    m_shadingGroup = shadingGroup;
    viewMenu->addActions(shadingGroup->actions());
    viewMenu->addSeparator();

    viewMenu->addAction("&Reset Camera", m_settings, &RenderSettings::resetCamera);

    auto* lodAction = viewMenu->addAction("&Level of detail (LOD)");
    lodAction->setCheckable(true);
    lodAction->setChecked(m_settings->getUseLod());
    connect(lodAction, &QAction::triggered, m_settings, [this, lodAction]() { m_settings->setUseLod(lodAction->isChecked()); });

    viewMenu->addSeparator();
    auto* msaaMenu = viewMenu->addMenu("&MSAA");
    auto* msaaGroup = new QActionGroup(this);
    msaaGroup->setExclusive(true);
    const char* msaaLabels[] = {"Off", "2x", "4x"};
    int msaaSamples[] = {0, 2, 4};
    for (int i = 0; i < 3; ++i) {
        auto* a = msaaGroup->addAction(QString::fromUtf8(msaaLabels[i]));
        a->setCheckable(true);
        a->setChecked(m_settings->getMsaaSamples() == msaaSamples[i]);
        msaaMenu->addAction(a);
        connect(a, &QAction::triggered, this, [this, s = msaaSamples[i]]() { m_settings->setMsaaSamples(s); recreateViewport(); });
    }

    // Help menu
    auto* helpMenu = ui->menuHelp;
    helpMenu->addAction("&Keyboard Shortcuts", this, &MainWindow::showShortcuts);
    helpMenu->addAction("&About SciRender", this, &MainWindow::showAbout);
    helpMenu->addSeparator();
    helpMenu->addAction("&Documentation", this, []() { QDesktopServices::openUrl(QUrl("https://github.com/thermeng/SciRender")); });
}


// Sidebar

// One table drives the nav labels, the panel-header title, and the section
// stack order. The labels used to live in two parallel arrays (navItems in
// setupSidebar, sectionNames in setSidebarSection) that had to be kept in sync
// by hand; a single table makes drift impossible and gives each section a
// place to declare what data it needs.
//
// Capability::Always sections are usable with whatever is loaded. Every other
// section is a visualizer that needs specific dataset contents: when those are
// absent the nav label gains a warning glyph and the page shows a reason banner.
//
// The capability drives the predicate; `unavailableReason` is deliberately
// per-section rather than per-capability. Two sections can need the same data
// and still describe it in their own terms â€” Volume and Slice Plane both need a
// 3D grid with a scalar field, but only one of them is volume rendering, and
// showing the wrong feature name on a page reads as a bug.
enum class NavCapability {
    Always,
    ScalarData,      // surface coloring by a scalar field
    VectorData,      // arrow glyphs / surface LIC
    StreamlineData,  // point vectors only (integration walks the grid)
    VolumeData,      // 3D grid + scalar field
    IsosurfaceData,  // 3D grid + scalar field, marching cubes
    PlotData,        // a mesh with scalar data to histogram
    SequenceData,    // a loaded .pvd time-series
};

struct NavSection {
    const char* name;
    NavCapability capability;
    const char* unavailableReason; // null when always available
};

static const NavSection navSections[] = {
    { "Mesh Info",       NavCapability::Always,        nullptr },
    { "Lighting",        NavCapability::Always,        nullptr },
    { "Clipping",        NavCapability::Always,        nullptr },
    { "View & Display",  NavCapability::Always,        nullptr },
    { "Scalar",          NavCapability::ScalarData,
      QT_TRANSLATE_NOOP("MainWindow", "Coloring by scalar needs a scalar field. "
                                      "Load a mesh with point or cell scalars.") },
    { "Probe",           NavCapability::Always,        nullptr },
    { "Vectors",         NavCapability::VectorData,
      QT_TRANSLATE_NOOP("MainWindow", "Arrow glyphs and Surface LIC need a vector field. "
                                      "Load a mesh with point or cell vectors.") },
    { "Streamlines",     NavCapability::StreamlineData,
      QT_TRANSLATE_NOOP("MainWindow", "Streamlines integrate through point vectors. "
                                      "Load a mesh with node-based vectors, "
                                      "for example a .vtu with a velocity field.") },
    { "Volume",          NavCapability::VolumeData,
      QT_TRANSLATE_NOOP("MainWindow", "Volume rendering needs a 3D grid with a scalar field. "
                                      "Load a .vtu, .vts or .vti volume dataset.") },
    { "Slice Plane",     NavCapability::VolumeData,
      QT_TRANSLATE_NOOP("MainWindow", "Slice planes need a 3D grid with a scalar field to "
                                      "texture. Load a .vtu, .vts or .vti volume dataset.") },
    { "Isosurface",      NavCapability::IsosurfaceData,
      QT_TRANSLATE_NOOP("MainWindow", "Isosurface extraction needs a 3D grid with a scalar "
                                      "field to contour. Load a .vtu, .vts or .vti "
                                      "volume dataset.") },
    { "Post Processing", NavCapability::Always,        nullptr },
    { "Screenshot",      NavCapability::Always,        nullptr },
    { "Animation",       NavCapability::SequenceData,
      QT_TRANSLATE_NOOP("MainWindow", "No time-series sequence loaded. "
                                      "Open a .pvd collection to animate frames.") },
    { "Plots",           NavCapability::PlotData,
      QT_TRANSLATE_NOOP("MainWindow", "Histograms need a mesh with scalar data. "
                                      "Load a mesh with scalars, for example a .vtu.") },
};
static constexpr int kNavSectionCount = int(sizeof(navSections) / sizeof(navSections[0]));

// U+26A0 WARNING SIGN, appended to a nav label whose data is missing.
static const char* const kUnavailableSuffix = " \u26A0";

void MainWindow::setupSidebar() {
    m_sidebarDock = ui->sidebarDock;
    m_sidebarDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    m_sidebarDock->setAllowedAreas(Qt::LeftDockWidgetArea);
    m_sidebarDock->setTitleBarWidget(new QWidget); // hide default title bar

    m_sidebarWidget = ui->sidebarContents;
    auto* mainLayout = qobject_cast<QHBoxLayout*>(m_sidebarWidget->layout());
    if (!mainLayout) {
        mainLayout = new QHBoxLayout(m_sidebarWidget);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(0);
    }

    // --- Navigation list (left edge, text-based) ---
    m_navList = new QListWidget;
    // m_navList->setStyleSheet(
    //     "QListWidget { border: none; outline: none; background: transparent; }"
    //     "QListWidget::item { padding: 10px 12px; font-size: 13px; }"
    //     "QListWidget::item:selected { background: palette(highlight); color: palette(highlightedText); }"
    //     "QListWidget::item:hover:!selected { background: palette(midlight); }");

    m_navList->setStyleSheet(
    "QListWidget { border: none; outline: none; }"
    "QListWidget::item { padding: 10px 12px; font-size: 13px; }");

    m_navList->setFrameShape(QFrame::NoFrame);
    m_navList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_navList->setFocusPolicy(Qt::NoFocus);

    // Calculate width dynamically based on longest text + padding. The warning
    // suffix is always reserved, so a badge appearing later never re-measures
    // (which would reflow the dock and jump the user's sidebar under the cursor).
    QFontMetrics fm(m_navList->font());
    int maxTextWidth = 0;
    for (const auto& section : navSections) {
        maxTextWidth = qMax(maxTextWidth, fm.horizontalAdvance(QString::fromUtf8(section.name)));
    }
    const int badgeWidth = fm.horizontalAdvance(QString::fromUtf8(kUnavailableSuffix));
    m_navWidth = maxTextWidth + badgeWidth + 48; // 24px padding each side

    for (const auto& section : navSections) {
        const QString name = QString::fromUtf8(section.name);
        auto* item = new QListWidgetItem(name, m_navList);
        item->setData(Qt::UserRole, name); // pristine label, for badge toggling
        item->setSizeHint(QSize(m_navWidth, 36));
    }
    m_navList->setFixedWidth(m_navWidth);

    connect(m_navList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0) setSidebarSection(row);
    });

    // --- Section stack with panel header ---
    m_rightPanel = ui->rightPanel;
    auto* rightLayout = qobject_cast<QVBoxLayout*>(m_rightPanel->layout());
    if (!rightLayout) {
        rightLayout = new QVBoxLayout(m_rightPanel);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->setSpacing(0);
    }

    // Panel header (title bar for the expanded panel)
    m_panelHeader = new QWidget;
    m_panelHeader->setFixedHeight(32);
    auto* headerLayout = new QHBoxLayout(m_panelHeader);
    headerLayout->setContentsMargins(10, 0, 4, 0);
    headerLayout->setSpacing(4);

    m_panelTitle = new QLabel;
    QFont titleFont = m_panelTitle->font();
    titleFont.setBold(true);
    m_panelTitle->setFont(titleFont);
    m_panelTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    headerLayout->addWidget(m_panelTitle, 1);

    m_closeBtn = new QToolButton;
    m_closeBtn->setText("\u00D7");
    m_closeBtn->setFixedSize(24, 24);
    m_closeBtn->setToolTip("Close panel");
    connect(m_closeBtn, &QToolButton::clicked, this, [this]() {
        if (m_sidebarExpanded) {
            setSidebarSection(m_activeSection);
        }
    });
    headerLayout->addWidget(m_closeBtn);

    rightLayout->addWidget(m_panelHeader);
    m_panelHeader->setVisible(false);

    // Stacked widget (section pages). Every page goes through addSectionPage so
    // each one gets the availability banner prepended in a single place, rather
    // than each build*Page() repeating the wrapper it already builds internally.
    m_sectionStack = new QStackedWidget;
    m_sectionBanners.resize(kNavSectionCount);

    addSectionPage(0, new sidebar::MeshInfoPage(m_settings));     // Mesh Info
    addSectionPage(1, new sidebar::LightingPage(m_settings));     // Lighting
    addSectionPage(2, new sidebar::ClippingPage(m_settings));     // Clipping
    addSectionPage(3, new sidebar::ViewDisplayPage(m_settings));  // View & Display
    addSectionPage(4, new sidebar::ScalarPage(m_settings));       // Scalar
    addSectionPage(5, new sidebar::ProbePage(m_settings));        // Probe
    addSectionPage(6, new sidebar::VectorsPage(m_settings));      // Vectors
    {
        auto* slPage = new sidebar::StreamlinesPage(m_settings);  // Streamlines
        // The page marks the backend dirty itself; MainWindow only wakes the viewport.
        connect(slPage, &sidebar::StreamlinesPage::viewportUpdateRequested,
                this, [this] { if (m_viewport) m_viewport->update(); });
        addSectionPage(7, slPage);
    }
    addSectionPage(8, new sidebar::VolumePage(m_settings));       // Volume
    addSectionPage(9, new sidebar::SlicePlanePage(m_settings));   // Slice Plane
    addSectionPage(10, new sidebar::IsosurfacePage(m_settings));  // Isosurface
    addSectionPage(11, new sidebar::PostProcPage(m_settings));    // Post Processing
    {
        auto* ssPage = new sidebar::ScreenshotPage(m_settings);
        // The save dialog, shortcuts and capture plumbing stay on MainWindow.
        connect(ssPage, &sidebar::ScreenshotPage::saveRequested,
                this, &MainWindow::saveScreenshot);
        addSectionPage(12, ssPage);  // Screenshot
    }
    {
        auto* animPage = new sidebar::AnimationPage(m_settings);
        // Export dialog/exporter capture plumbing stays on MainWindow.
        connect(animPage, &sidebar::AnimationPage::exportRequested,
                this, &MainWindow::exportAnimation);
        // Section 13 (Animation) is usable only once a .pvd loads, so the nav
        // badge re-evaluates on stateChanged, exactly like the old builder.
        connect(m_settings->anim(), &AnimationController::stateChanged,
                this, &MainWindow::refreshSidebarCapabilityBadges);
        addSectionPage(13, animPage);
    }
    {
        auto* plotsPage = new sidebar::PlotsPage(m_settings, m_plotWindow, m_histogramVisible);
        connect(plotsPage, &sidebar::PlotsPage::viewportToggleRequested,
                this, &MainWindow::setHistogramVisible);
        if (m_plotDock)
            connect(m_plotDock, &QDockWidget::visibilityChanged, plotsPage,
                    [plotsPage](bool v) { plotsPage->setViewportVisible(v); });
        addSectionPage(14, plotsPage);  // Plots
    }

    rightLayout->addWidget(m_sectionStack, 1);
    m_sectionStack->setVisible(false);

    if (qEnvironmentVariableIsSet("SCIRENDER_UI_DUMP")) {
        QTimer::singleShot(2500, this, [this] {
            QFile dump(qEnvironmentVariable("SCIRENDER_UI_DUMP"));
            if (!dump.open(QIODevice::WriteOnly | QIODevice::Text)) return;
            QTextStream ts(&dump);
            ts << "navWidth=" << m_navWidth << " sidebarWidth=" << kSidebarWidth
               << " usableContent=" << (kSidebarWidth - 20) << "\n";
            const char* names[] = {"MeshInfo","Lighting","Clipping","ViewDisplay","Scalar",
                                   "Probe","Vectors","Streamlines","Volume","SlicePlane",
                                   "Isosurface","PostProc","Screenshot","Animation","Plots"};
            for (int i = 0; i < m_sectionStack->count(); ++i) {
                QWidget* page = m_sectionStack->widget(i);
                QScrollArea* sa = page->findChild<QScrollArea*>();
                if (!sa) { ts << names[i] << ": no scrollarea\n"; continue; }
                QWidget* c = sa->widget();
                int need = c->minimumSizeHint().width();
                ts << QString("%1 %2 need=%3 over=%4\n")
                      .arg(names[i], -13).arg(need > (kSidebarWidth - 20) ? "OVERFLOW" : "ok     ")
                      .arg(need, 4).arg(qMax(0, need - (kSidebarWidth - 20)), 4);
                if (need > (kSidebarWidth - 40)) {
                    ts << "   -- breakdown --\n";
                    for (QWidget* ch : c->findChildren<QWidget*>()) {
                        if (!ch->objectName().isEmpty() || ch->layout())
                            ts << QString("      %1 %2 min=%3 minHint=%4\n")
                                  .arg(ch->objectName().isEmpty() ? QString("<anon>") : ch->objectName(), -28)
                                  .arg(ch->metaObject()->className(), -20)
                                  .arg(ch->minimumWidth()).arg(ch->minimumSizeHint().width());
                    }
                }
            }
        });
    }

    mainLayout->addWidget(m_navList);
    mainLayout->addWidget(m_rightPanel, 1);

    m_sidebarDock->setWidget(m_sidebarWidget);
    m_sidebarDock->setMinimumWidth(m_navWidth);
    m_sidebarDock->setMaximumWidth(m_navWidth + kSidebarWidth);
    addDockWidget(Qt::LeftDockWidgetArea, m_sidebarDock);

    setSidebarSection(0);
    refreshSidebarCapabilityBadges();
}


// Prepend the "why is this unavailable" banner to a section page and register
// it. Done here rather than inside each build*Page() so the pages stay unaware
// of the badge feature and every section gets one wrapper by construction.

void MainWindow::addSectionPage(int section, QWidget* page) {
    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->setSpacing(0);

    auto* banner = new QLabel;
    banner->setObjectName("sectionUnavailableBanner");
    banner->setWordWrap(true);
    // Match the page header scale from applyPanelStyling (11px, Medium weight)
    // so the banner reads as part of the panel rather than a foreign overlay.
    QFont bannerFont = banner->font();
    bannerFont.setPixelSize(11);
    bannerFont.setWeight(QFont::Medium);
    banner->setFont(bannerFont);
    // Full-bleed strip: it spans the panel and carries the accent bar, while
    // padding is tuned so the text lands on the pages' content column. Every
    // badged page uses leftMargin/rightMargin 10, so border-left (3) +
    // padding-left (7) puts the first glyph at exactly 10px, level with the
    // first control below. An inset plate would instead need
    // margin 10 + border 3 + padding 8 = 21px and hang out to the right.
    //
    // Set here rather than in applyThemeAwareStylesheets(): that only runs on a
    // QEvent::PaletteChange, which never fires at startup, so anything styled
    // only there is invisible until the user switches the OS theme. The
    // palette(mid) reference is resolved by Qt at paint time, so a one-shot
    // stylesheet still follows light/dark correctly.
    banner->setStyleSheet(
        "QLabel { background: palette(mid); color: #b26a00;"
        "border-left: 3px solid #d08700;"
        "padding: 6px 10px 6px 7px; margin: 8px 0 0 0; }");
    // Hidden by default; refreshSidebarCapabilityBadges() decides per dataset.
    banner->setVisible(false);
    wrapperLayout->addWidget(banner);

    wrapperLayout->addWidget(page, 1);

    m_sectionBanners[section] = banner;
    m_sectionStack->addWidget(wrapper);
}


// Is this section's backing data present? Empty reason means supported.

QString MainWindow::sectionUnavailableReason(int section) const {
    const NavSection& s = navSections[section];

    // The predicate and the wording are separate concerns: the capability says
    // what data is needed, the table says how this page describes it.
    bool available = false;
    switch (s.capability) {
        case NavCapability::Always:
            return {};
        case NavCapability::ScalarData:
            available = m_settings->hasMeshScalars();
            break;
        case NavCapability::VectorData:
            available = m_settings->hasMeshVectors() || m_settings->hasMeshCellVectors();
            break;
        case NavCapability::StreamlineData:
            // Streamlines integrate through node vectors; cell-only vectors
            // cannot seed an integration, matching the auto-disable in
            // RenderSettings::loadMesh.
            available = m_settings->hasMeshVectors();
            break;
        case NavCapability::VolumeData:
            available = m_settings->hasVolumeData();
            break;
        case NavCapability::IsosurfaceData:
            available = m_settings->getIsosurfaceAvailable();
            break;
        case NavCapability::PlotData:
            available = m_settings->getHasMeshLoaded() && m_settings->hasMeshScalars();
            break;
        case NavCapability::SequenceData:
            available = m_settings->anim()->hasSequence();
            break;
    }

    if (available || !s.unavailableReason) return {};
    return tr(s.unavailableReason);
}


// Re-evaluate every section against the current dataset: append or strip the
// warning glyph on the nav label, set the generic tooltip, and refresh the
// banner text on all pages (including hidden ones, so switching sections never
// shows a stale reason).

void MainWindow::refreshSidebarCapabilityBadges() {
    if (!m_navList) return;

    const QString suffix = QString::fromUtf8(kUnavailableSuffix);
    for (int section = 0; section < kNavSectionCount; ++section) {
        QListWidgetItem* item = m_navList->item(section);
        if (!item) continue;

        const QString base = item->data(Qt::UserRole).toString();
        const QString reason = sectionUnavailableReason(section);
        const bool available = reason.isEmpty();

        item->setText(available ? base : base + suffix);
        item->setToolTip(available ? QString()
                                   : tr("Not available for the current dataset"));

        if (section < m_sectionBanners.size()) {
            QLabel* banner = m_sectionBanners[section];
            if (banner) {
                banner->setText(reason);
                banner->setVisible(!available);
            }
        }
    }
}








// Sidebar section switching

void MainWindow::setSidebarSection(int section) {
    if (section < 0 || section >= kNavSectionCount) return;
    if (m_activeSection == section && m_sidebarExpanded) {
        m_activeSection = -1;
        m_sidebarExpanded = false;
        m_sectionStack->setVisible(false);
        m_panelHeader->setVisible(false);
        m_navList->clearSelection();
        m_navList->setCurrentRow(-1);
    } else if (section >= 0) {
        m_sidebarExpanded = true;
        m_sectionStack->setCurrentIndex(section);
        m_sectionStack->setVisible(true);
        m_panelHeader->setVisible(true);
        m_panelTitle->setText(QString::fromUtf8(navSections[section].name));
        m_navList->blockSignals(true);
        m_navList->setCurrentRow(section, QItemSelectionModel::ClearAndSelect);
        m_navList->blockSignals(false);
        m_activeSection = section;
    }
    m_settings->setSidebarWidth(m_sidebarExpanded ? m_navWidth + kSidebarWidth : m_navWidth);
    m_sidebarDock->setFixedWidth(m_sidebarExpanded ? m_navWidth + kSidebarWidth : m_navWidth);
}


// Timers

void MainWindow::setupTimers() {
    // Auto-rotate (30fps azimuth nudge)
    connect(&m_autoRotateTimer, &QTimer::timeout, m_settings, [this]() { m_settings->azimuth(0.6); });
    m_autoRotateTimer.setInterval(33);
    connect(m_settings, &RenderSettings::viewChanged, this, [this](ChangeFlags) {
        bool shouldRun = m_settings->getAutoRotate();
        if (shouldRun && !m_autoRotateTimer.isActive()) m_autoRotateTimer.start();
        else if (!shouldRun && m_autoRotateTimer.isActive()) m_autoRotateTimer.stop();
    });

    // FPS HUD continuous repaint
    connect(&m_fpsTimer, &QTimer::timeout, m_viewport, QOverload<>::of(&QWidget::update));
    m_fpsTimer.setInterval(16);
    connect(m_settings, &RenderSettings::viewChanged, this, [this](ChangeFlags) {
        bool shouldRun = m_settings->getShowFps();
        if (shouldRun && !m_fpsTimer.isActive()) m_fpsTimer.start();
        else if (!shouldRun && m_fpsTimer.isActive()) m_fpsTimer.stop();
    });

    // Particle animation
    connect(&m_particleTimer, &QTimer::timeout, m_viewport, QOverload<>::of(&QWidget::update));
    m_particleTimer.setInterval(16);
    connect(m_settings, &RenderSettings::viewChanged, this, [this](ChangeFlags) {
        bool shouldRun = m_settings->getShowParticles() && m_settings->getShowStreamlines();
        if (shouldRun && !m_particleTimer.isActive()) m_particleTimer.start();
        else if (!shouldRun && m_particleTimer.isActive()) m_particleTimer.stop();
    });
}


// Top toolbar (display toggles)

void MainWindow::setupTopToolbar() {
    m_topToolbar = new QToolBar(this);
    m_topToolbar->setMovable(false);
    m_topToolbar->setStyleSheet(
    "QToolBar {"
    "    border: none;"
    "    padding: 2px;"
    "    spacing: 2px;"
    "    background: palette(window);"
    "}"
    "QToolButton {"
    "    background: transparent;"
    "    border: 1px solid transparent;" 
    "    border-radius: 4px;"
    "    padding: 2px 2px;" 
    "    margin: 1px;"
    "    color: palette(button-text);"
    "    min-width: 28px;"
    "}"
    "QToolButton:hover {"
    "    background: palette(midlight);"
    "    border: 1px solid palette(mid);"
    "}"
    "QToolButton:pressed {"
    "    background: transparent;"
    "    border: 1px solid palette(dark);" // Gives a clean sunken/bordered click effect
    "}"
    "QToolButton:checked {"
    "    background: palette(highlight);"
    "    border: 1px solid palette(dark);"
    "    color: palette(highlighted-text);"
    "}"
    "QToolButton:disabled {"
    "    color: palette(shadow);"
    "}"
    "QToolButton::menu-indicator {"
    "    image: none;"
    "}"
);

    m_topToolbar->setIconSize(QSize(32, 32));
    m_topToolbar->setFixedHeight(40);

    m_tbWireframe = m_topToolbar->addAction(QIcon(":/src/resources/icons/wireframe.svg"), "Wireframe");
    m_tbWireframe->setCheckable(true);
    m_tbWireframe->setChecked(m_settings->isWireframe());
    connect(m_tbWireframe, &QAction::toggled, m_settings, &RenderSettings::setWireframe);

    m_tbSurface = m_topToolbar->addAction(QIcon(":/src/resources/icons/surface.svg"), "Surface");
    m_tbSurface->setCheckable(true);
    m_tbSurface->setChecked(m_settings->isSurfaceVisible());
    connect(m_tbSurface, &QAction::toggled, m_settings, &RenderSettings::toggleSurface);

    m_tbVolume = m_topToolbar->addAction(QIcon(":/src/resources/icons/volume.svg"), "Volume");
    m_tbVolume->setCheckable(true);
    m_tbVolume->setChecked(m_settings->getShowVolume());
    m_tbVolume->setEnabled(m_settings->hasVolumeData());
    connect(m_tbVolume, &QAction::toggled, m_settings, &RenderSettings::setShowVolume);

    m_topToolbar->addSeparator();

    // Ortho snaps
    const char* orthoIcons[] = {
        ":/src/resources/icons/ortho_px.svg",
        ":/src/resources/icons/ortho_nx.svg",
        ":/src/resources/icons/ortho_py.svg",
        ":/src/resources/icons/ortho_ny.svg",
        ":/src/resources/icons/ortho_pz.svg",
        ":/src/resources/icons/ortho_nz.svg"
    };
    const char* orthoTips[] = {"Ortho +X", "Ortho -X", "Ortho +Y", "Ortho -Y", "Ortho +Z", "Ortho -Z"};
    for (int i = 0; i < 6; ++i) {
        auto* act = m_topToolbar->addAction(QIcon(orthoIcons[i]), orthoTips[i]);
        connect(act, &QAction::triggered, this, [this, i]() { m_settings->snapToOrthoView(i); });
    }

    m_topToolbar->addSeparator();

    // Reset camera
    auto* resetAct = m_topToolbar->addAction(QIcon(":/src/resources/icons/reset.svg"), "Reset Camera");
    connect(resetAct, &QAction::triggered, m_settings, &RenderSettings::resetCamera);
}

void MainWindow::syncTopToolbar() {
    if (m_tbWireframe) m_tbWireframe->setChecked(m_settings->isWireframe());
    if (m_tbSurface)   m_tbSurface->setChecked(m_settings->isSurfaceVisible());
    if (m_tbVolume) {
        m_tbVolume->setEnabled(m_settings->hasVolumeData());
        m_tbVolume->setChecked(m_settings->getShowVolume());
    }
}

void MainWindow::setupKeyboardShortcuts() {
    auto addNav = [this](const QKeySequence& ks, std::function<void()> fn) {
        auto* sc = new QShortcut(ks, this);
        sc->setContext(Qt::WindowShortcut);
        m_navShortcuts.append(sc);
        connect(sc, &QShortcut::activated, this, fn);
    };
    addNav(QKeySequence("R"),            [this]() { m_settings->resetCamera(); });
    addNav(QKeySequence("W"),            [this]() { m_settings->setWireframe(!m_settings->isWireframe()); });
    addNav(QKeySequence("S"),            [this]() { saveScreenshot(); });
    addNav(QKeySequence("Left"),         [this]() { m_settings->azimuth(-5); });
    addNav(QKeySequence("Right"),        [this]() { m_settings->azimuth(5); });
    addNav(QKeySequence("Up"),           [this]() { m_settings->elevation(5); });
    addNav(QKeySequence("Down"),         [this]() { m_settings->elevation(-5); });
    addNav(QKeySequence("Ctrl+="),       [this]() { m_settings->dolly(1.1); });
    addNav(QKeySequence("Ctrl+-"),       [this]() { m_settings->dolly(0.9); });

    // Disable navigation shortcuts while an editor widget (filter boxes, combos,
    // spin boxes) has focus so arrow keys / letters reach the editor instead of
    // orbiting or toggling the viewport. Survives viewport recreation because the
    // shortcuts live on the (never-recreated) MainWindow.
    auto refreshNavState = [this]() {
        const bool editing = navFocusIsEditor(QApplication::focusWidget());
        for (auto* sc : m_navShortcuts) sc->setEnabled(!editing);
    };
    connect(qApp, &QApplication::focusChanged, this, refreshNavState);
    refreshNavState();
}


// Connect settings signals

void MainWindow::connectSettings() {
    connect(m_settings, &RenderSettings::meshLoadStateChanged, this, &MainWindow::updateStatusBar);
    connect(m_settings, &RenderSettings::meshLoadStateChanged, this, [this]() {
        refreshSidebarCapabilityBadges();
    });
    connect(m_settings, &RenderSettings::meshDataUpdated, this, [this]() {
        refreshSidebarCapabilityBadges();
    });

    // Mirror toolbar and View & Display toggles whenever the corresponding
    // settings change (keyboard shortcuts, toolbar buttons, side-panel checkboxes).
    connect(m_settings, &RenderSettings::viewChanged, this, [this](ChangeFlags flags) {
        syncTopToolbar();
    });

    connect(m_settings, &RenderSettings::screenshotCaptured, this, &MainWindow::onScreenshotCaptured);
}


// Status bar

void MainWindow::updateStatusBar() {
    auto* sb = statusBar();
    if (m_settings->getHasMeshLoaded()) {
        sb->showMessage(QString("Mesh: %1   |   Type: %2   |   Points: %3   |   Triangles: %4")
            .arg(m_settings->getCurrentMeshNameQStr())
            .arg(m_settings->getMeshDataType())
            .arg(m_settings->getPointCount())
            .arg(m_settings->getTriangleCount()));
    } else {
        sb->showMessage("No mesh loaded | drag a .stl / .vtk / .obj / .vtu / .vts / .vti / .vtp / .vtr / .pvd file, or use File > Open Mesh");
    }
}


// File dialogs / actions

void MainWindow::openMesh() {
    QString path = QFileDialog::getOpenFileName(this, "Load Mesh", QString(),
        "Mesh files (*.stl *.vtk *.obj *.vtu *.vts *.vti *.vtp *.vtr *.pvd);;All files (*)");
    if (!path.isEmpty()) {
        m_currentFile = path;
        m_settings->loadMesh(path);
    }
}

void MainWindow::openRecent(const QString& path) {
    m_currentFile = path;
    m_settings->openRecent(path);
}

void MainWindow::clearRecentFiles() {
    m_settings->clearRecentFiles();
}

void MainWindow::reloadMesh() {
    if (m_currentFile.isEmpty()) return;
    if (!m_settings->getHasMeshLoaded()) return;
    m_settings->loadMesh(m_currentFile);
}

void MainWindow::saveScreenshot() {
    QString filename = m_settings->generateScreenshotFilename();
    QString path = QFileDialog::getSaveFileName(this, "Save Screenshot", filename,
        "PNG Images (*.png);;JPEG Images (*.jpg *.jpeg);;BMP Images (*.bmp);;All files (*)");
    if (path.isEmpty()) return;
    if (QFileInfo::exists(path)) {
        auto reply = QMessageBox::question(this, "Overwrite file?",
            QString("The file \"%1\" already exists. Overwrite?").arg(path),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
    }
    m_settings->requestScreenshot(path);
}

void MainWindow::exportAnimation() {
    AnimationController* ctrl = m_settings->anim();
    if (!ctrl || !ctrl->hasSequence()) {
        statusBar()->showMessage("Load a .pvd sequence before exporting an animation", 8000);
        return;
    }
    AnimationExportDialog dlg(ctrl, m_viewport->size(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    const AnimationExportConfig cfg = dlg.config();

    auto* exporter = m_settings->animationExporter();
    exporter->setCaptureFn([this](int w, int h, int s, bool t) {
        return m_viewport->captureFrameImage(w, h, s, t);
    });

    QProgressDialog progress(QStringLiteral("Exporting animation..."),
                             QStringLiteral("Cancel"), 0,
                             cfg.lastFrame - cfg.firstFrame + 1, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    connect(&progress, &QProgressDialog::canceled, exporter, &AnimationExporter::cancel);
    connect(exporter, &AnimationExporter::progress, &progress,
            [&progress](int done, int total) {
                progress.setMaximum(total);
                progress.setValue(done);
            });
    QString resultMessage;
    connect(exporter, &AnimationExporter::finished, &progress,
            [&](bool, const QString& msg) { resultMessage = msg; });

    exporter->start(cfg, m_settings->backend());
    statusBar()->showMessage(resultMessage, 8000);
}

void MainWindow::onScreenshotCaptured(const QString& savedPath) {    QTimer::singleShot(0, this, [this, savedPath]() {
        if (savedPath.isEmpty()) {
            QMessageBox::warning(this, "Screenshot failed",
                "Could not capture the screenshot. The OpenGL framebuffer readback failed.\n"
                "Try a smaller window or a different graphics driver.");
        } else {
            QMessageBox::information(this, "Screenshot saved",
                QString("Screenshot saved to:\n%1").arg(savedPath));
        }
    });
}

void MainWindow::clearMeshes() {
    if (m_viewport) {
        m_viewport->makeCurrent();
        if (!QOpenGLContext::currentContext()) {
            qWarning() << "clearMeshes: failed to make GL context current";
        }
    }
    m_settings->clearMeshes();
    if (m_viewport) m_viewport->doneCurrent();
    updateStatusBar();
}


// Drag and drop

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    auto urls = event->mimeData()->urls();
    for (const auto& url : urls) {
        QString path = url.toLocalFile();
        if (!path.isEmpty()) m_settings->loadMesh(path);
    }
    event->acceptProposedAction();
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
}

void MainWindow::changeEvent(QEvent* ev) {
    QMainWindow::changeEvent(ev);
    if (!m_viewport) return;

    if (ev->type() == QEvent::PaletteChange) {
        applyThemeAwareStylesheets();
        return;
    }

    if (ev->type() == QEvent::Show
        || (ev->type() == QEvent::WindowStateChange && !isMinimized() && isVisible())) {
        m_viewport->forceRepaint();
    }
}

void MainWindow::applyThemeAwareStylesheets() {
    if (m_navList) {
        m_navList->setStyleSheet(
            "QListWidget { border: none; outline: none; }"
            "QListWidget::item { padding: 10px 12px; font-size: 13px; }"
            "QListWidget::item:selected { background: palette(highlight); color: palette(highlightedText); }"
            "QListWidget::item:hover:!selected { background: palette(midlight); }");
    }
    if (m_topToolbar) {
        m_topToolbar->setStyleSheet(
            "QToolBar { border: none; padding: 6px 8px; spacing: 2px; background: palette(button); }"
            "QToolButton { background: transparent; border: none; border-radius: 4px; padding: 4px 10px; color: palette(text); min-width: 28px; }"
            "QToolButton:hover { background: palette(midlight); }"
            "QToolButton:checked { background: palette(highlight); color: palette(highlighted-text); }"
            "QToolButton::menu-indicator { image: none; }");
    }

    // Section headers (object name ends with "Header") and their dividers
    for (QLabel* lbl : findChildren<QLabel*>()) {
        if (lbl->objectName().endsWith("Header")) {
            lbl->setStyleSheet("background: transparent; padding-top: 6px;");
        }
    }
    // Dividers created by createGroup and applyPanelStyling
    for (QFrame* frame : findChildren<QFrame*>()) {
        if (frame->property("isHeaderDivider").toBool()
            || frame->frameShape() == QFrame::HLine) {
            frame->setStyleSheet("background-color: palette(mid); max-height: 1px;");
        }
    }

    // Availability banners are styled in addSectionPage() at construction, not
    // here: this function only runs on QEvent::PaletteChange, which never fires
    // at startup.
}

void MainWindow::recreateViewport() {
    auto* old = m_viewport;
    m_viewport = new ViewportWidget(m_settings->getMsaaSamples(), this);
    m_viewport->setSettings(m_settings);
    setCentralWidget(m_viewport);

    // Reconnect timers
    connect(&m_fpsTimer, &QTimer::timeout, m_viewport, QOverload<>::of(&QWidget::update));
    connect(&m_particleTimer, &QTimer::timeout, m_viewport, QOverload<>::of(&QWidget::update));

    old->deleteLater();
    m_viewport->update();
}


// Dialogs

void MainWindow::showAbout() {
    QMessageBox::about(this, "About SciRender",
        "<h2>SciRender</h2>"
        "<p>A Qt 6 + OpenGL scientific mesh renderer for VTK XML, STL and OBJ datasets.</p>"
        "<p>GPU shaders, instanced vector glyphs, lighting, slicing, colormaps & more.</p>"
        "<p>Build: MinGW 64-bit &middot; Qt 6.11</p>");
}

void MainWindow::showShortcuts() {
    QMessageBox info(this);
    info.setWindowTitle("Keyboard Shortcuts");
    info.setText(
        "<table style='font-size:12px;'>"
        "<tr><td style='padding-right:16px;'><b>R</b></td><td>Reset camera</td></tr>"
        "<tr><td><b>W</b></td><td>Toggle wireframe</td></tr>"
        "<tr><td><b>S</b></td><td>Save screenshot</td></tr>"
        "<tr><td><b>Left/Right</b></td><td>Orbit (azimuth)</td></tr>"
        "<tr><td><b>Up/Down</b></td><td>Elevation</td></tr>"
        "<tr><td><b>Ctrl + =</b></td><td>Zoom in</td></tr>"
        "<tr><td><b>Ctrl + -</b></td><td>Zoom out</td></tr>"
        "</table>"
    );
    info.exec();
}

// Plots page (12) â€” dockable viewport launcher (constrained to central host)
void MainWindow::setHistogramVisible(bool visible) {
    m_histogramVisible = visible;
    if (m_plotDock) {
        m_plotDock->setVisible(visible);
        if (visible && m_centralHost) {
            // Ensure default area is Right (horizontal beside scene) on first show
            if (m_centralHost->dockWidgetArea(m_plotDock) == Qt::NoDockWidgetArea)
                m_centralHost->addDockWidget(Qt::RightDockWidgetArea, m_plotDock);
        }
    }
    if (visible && m_plotWindow) m_plotWindow->refreshData();
}




