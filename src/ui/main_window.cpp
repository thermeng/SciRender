#include "main_window.h"
#include "render/render_config.h"
#include "core/Colormaps.h"
#include <QApplication>
#include <QMenuBar>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QLabel>
#include <QToolButton>
#include <QPushButton>
#include <QCheckBox>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QFrame>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QResizeEvent>
#include <QFileDialog>
#include <QColorDialog>
#include <QMessageBox>
#include <QShortcut>
#include <QActionGroup>
#include <QAction>
#include <QStyle>
#include <QTimer>
#include <QPainter>
#include <QFile>
#include <QTextStream>

// ============================================================================
// Helper: Create a labeled slider row (LightSlider equivalent)
// ============================================================================
struct SliderRow {
    QSlider* slider = nullptr;
    QLabel* valueLabel = nullptr;
    std::function<void(double)> callback;
};

static SliderRow createLightSlider(const QString& label, double value, double from, double to, double step, int decimals, std::function<void(double)> cb) {
    SliderRow row;
    auto* widget = new QWidget;
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* lbl = new QLabel(label);
    lbl->setFixedWidth(64);
    lbl->setStyleSheet("font-size: 11px; color: #cccccc;");
    lbl->setWordWrap(false);
    layout->addWidget(lbl);

    row.slider = new QSlider(Qt::Horizontal);
    row.slider->setMinimum(static_cast<int>(from * 1000));
    row.slider->setMaximum(static_cast<int>(to * 1000));
    row.slider->setSingleStep(static_cast<int>(step * 1000));
    row.slider->setValue(static_cast<int>(value * 1000));
    layout->addWidget(row.slider, 1);

    row.valueLabel = new QLabel(QString::number(value, 'f', decimals));
    row.valueLabel->setFixedWidth(36);
    row.valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row.valueLabel->setStyleSheet("font-size: 10px; color: #858585;");
    layout->addWidget(row.valueLabel);

    row.callback = cb;
    auto updateLabel = [row, decimals](int raw) {
        double v = raw / 1000.0;
        row.valueLabel->setText(QString::number(v, 'f', decimals));
    };
    QObject::connect(row.slider, &QSlider::valueChanged, updateLabel);
    if (cb) {
        QObject::connect(row.slider, &QSlider::valueChanged, [cb](int raw) { cb(raw / 1000.0); });
    }
    return row;
}

// ============================================================================
// Helper: Create a clip slider with editable text field
// ============================================================================
struct ClipSliderRow {
    QSlider* slider = nullptr;
    QLineEdit* field = nullptr;
    std::function<void(double)> callback;
};

static ClipSliderRow createClipSlider(const QString& label, double value, double from, double to, std::function<void(double)> cb) {
    ClipSliderRow row;
    auto* widget = new QWidget;
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* lbl = new QLabel(label);
    lbl->setFixedWidth(64);
    lbl->setStyleSheet("font-size: 11px; color: #cccccc;");
    lbl->setWordWrap(false);
    layout->addWidget(lbl);

    row.slider = new QSlider(Qt::Horizontal);
    row.slider->setMinimum(static_cast<int>(from * 1000));
    row.slider->setMaximum(static_cast<int>(to * 1000));
    row.slider->setValue(static_cast<int>(value * 1000));
    layout->addWidget(row.slider, 1);

    row.field = new QLineEdit(QString::number(value, 'f', 3));
    row.field->setFixedWidth(48);
    row.field->setAlignment(Qt::AlignRight);
    row.field->setStyleSheet("font-size: 11px; background: #3c3c3c; color: #cccccc; border: 1px solid #3c3c3c; border-radius: 2px; padding: 1px 4px;");
    auto* validator = new QDoubleValidator(from, to, 3);
    row.field->setValidator(validator);
    layout->addWidget(row.field);

    row.callback = cb;
    auto syncFromSlider = [row](int raw) {
        double v = raw / 1000.0;
        row.field->setText(QString::number(v, 'f', 3));
    };
    auto commitFromField = [row, from, to]() {
        double v = row.field->text().toDouble();
        v = qBound(from, v, to);
        row.slider->setValue(static_cast<int>(v * 1000));
        if (row.callback) row.callback(v);
    };
    QObject::connect(row.slider, &QSlider::valueChanged, [syncFromSlider, row](int raw) {
        syncFromSlider(raw);
        if (row.callback) row.callback(raw / 1000.0);
    });
    QObject::connect(row.field, &QLineEdit::editingFinished, commitFromField);
    return row;
}

// ============================================================================
// Helper: Section header label (VS Code blue accent)
// ============================================================================
static QLabel* sectionHeader(const QString& text) {
    auto* lbl = new QLabel(text);
    lbl->setStyleSheet("font-size: 11px; font-weight: bold; color: #007acc; padding-top: 4px;");
    return lbl;
}

// ============================================================================
// Helper: Swatch button (color swatch + label)
// ============================================================================
static QPushButton* createSwatchButton(const QString& text, const QColor& color, std::function<void()> onClicked) {
    auto* btn = new QPushButton(text);
    btn->setFixedHeight(24);
    btn->setStyleSheet(
        "QPushButton {"
        "  text-align: left; font-size: 11px;"
        "  background: #3c3c3c; color: #cccccc;"
        "  border: 1px solid #505050; border-radius: 2px;"
        "  padding: 2px 6px;"
        "}"
        "QPushButton:hover { border: 1px solid #007acc; }"
    );
    QPixmap pix(14, 14);
    pix.fill(color);
    btn->setIcon(pix);
    btn->setIconSize(QSize(14, 14));
    QObject::connect(btn, &QPushButton::clicked, onClicked);
    return btn;
}

// ============================================================================
// Helper: collapsible section header
// ============================================================================
static QToolButton* createCollapsibleHeader(const QString& title, bool expanded, std::function<void(bool)> toggle) {
    auto* btn = new QToolButton;
    btn->setText(QString("%1 %2").arg(expanded ? "\u25BC" : "\u25B6", title));
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    btn->setStyleSheet(
        "QToolButton {"
        "  text-align: left; font-size: 12px; font-weight: bold;"
        "  color: #cccccc; padding: 6px 8px; border: none;"
        "  background: #2a2d2e; border-radius: 4px 4px 0 0;"
        "}"
        "QToolButton:hover { background: #333334; }"
    );
    btn->setCursor(Qt::PointingHandCursor);
    auto state = std::make_shared<bool>(expanded);
    QObject::connect(btn, &QToolButton::clicked, [btn, state, toggle]() {
        *state = !*state;
        btn->setText(QString("%1 %2").arg(*state ? "\u25BC" : "\u25B6", btn->text().mid(2)));
        toggle(*state);
    });
    return btn;
}

// ============================================================================
// Helper: Colormap preview pixmap (gradient + name label)
// ============================================================================
static QPixmap generateColormapPreview(int index, int w = 100, int h = 24) {
    QImage img(w, h, QImage::Format_RGB888);
    ColormapType type = static_cast<ColormapType>(index);
    for (int x = 0; x < w; ++x) {
        float t = static_cast<float>(x) / static_cast<float>(w - 1);
        glm::vec3 c = Colormaps::evaluate(t, type);
        int r = static_cast<int>(glm::clamp(c.r, 0.0f, 1.0f) * 255.0f);
        int g = static_cast<int>(glm::clamp(c.g, 0.0f, 1.0f) * 255.0f);
        int b = static_cast<int>(glm::clamp(c.b, 0.0f, 1.0f) * 255.0f);
        for (int y = 0; y < h; ++y) img.setPixel(x, y, qRgb(r, g, b));
    }
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        QFont f("Sans", 10, QFont::Bold);
        f.setStretch(QFont::Condensed);
        p.setFont(f);
        QRect r(0, 0, w, h);
        QString name = QString::fromUtf8(Colormaps::getName(type));
        p.setPen(Qt::black);
        p.drawText(r.translated(1, 1), Qt::AlignCenter, name);
        p.setPen(Qt::white);
        p.drawText(r, Qt::AlignCenter, name);
    }
    return QPixmap::fromImage(img);
}

// Helper: Build a 2-column colormap swatch grid wired to a callback
static QGridLayout* buildColormapGrid(int currentChoice, std::function<void(int)> onChoose) {
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(4);
    int count = static_cast<int>(ColormapType::Count);
    for (int i = 0; i < count; ++i) {
        auto* btn = new QPushButton;
        btn->setFixedSize(100, 24);
        btn->setIcon(generateColormapPreview(i));
        btn->setIconSize(QSize(100, 24));
        btn->setCheckable(true);
        btn->setChecked(i == currentChoice);
        btn->setStyleSheet(
            "QPushButton { border: 2px solid transparent; border-radius: 2px; padding: 0px; }"
            "QPushButton:checked { border: 2px solid #007acc; }"
            "QPushButton:hover { border: 2px solid #264f78; }"
        );
        QObject::connect(btn, &QPushButton::clicked, [onChoose, i]() { onChoose(i); });
        grid->addWidget(btn, i / 2, i % 2);
    }
    return grid;
}

// ============================================================================
// MainWindow
// ============================================================================
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("SciRender");
    resize(1024, 768);
    setMinimumSize(640, 480);

    m_settings = new RenderSettings(this);
    m_settings->restoreStateFromSettings();

    // Viewport (central widget)
    m_viewport = new ViewportWidget(this);
    m_viewport->setSettings(m_settings);
    setCentralWidget(m_viewport);

    // Accept drops
    setAcceptDrops(true);

    setupMenus();
    setupSidebar();
    setupQuickBar();
    setupTimers();
    setupKeyboardShortcuts();
    connectSettings();
    updateStatusBar();

    // Save state on quit
    connect(qApp, &QCoreApplication::aboutToQuit, m_settings, &RenderSettings::saveStateToSettings);
}

MainWindow::~MainWindow() = default;

// ============================================================================
// Menus
// ============================================================================
void MainWindow::setupMenus() {
    auto* menuBar = this->menuBar();

    // File menu
    auto* fileMenu = menuBar->addMenu("&File");
    fileMenu->addAction("&Open Mesh...", this, &MainWindow::openMesh);

    auto* recentMenu = fileMenu->addMenu("Open &Recent");
    connect(recentMenu, &QMenu::aboutToShow, this, [recentMenu, this]() {
        recentMenu->clear();
        auto files = m_settings->getRecentFiles();
        if (files.isEmpty()) return;
        for (const auto& f : files) {
            recentMenu->addAction(f, this, [this, f]() { openRecent(f); });
        }
    });

    fileMenu->addAction("&Clear", this, &MainWindow::clearMeshes);
    fileMenu->addSeparator();
    fileMenu->addAction("E&xit", qApp, &QApplication::quit);

    // View menu
    auto* viewMenu = menuBar->addMenu("&View");
    viewMenu->addAction("&Lighting", this, [this]() { setSidebarSection(0); });
    viewMenu->addAction("&Slicing", this, [this]() { setSidebarSection(1); });
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

    viewMenu->addSeparator();
    viewMenu->addAction("&Reset Camera", m_settings, &RenderSettings::resetCamera);

    auto* lodAction = viewMenu->addAction("&Level of detail (LOD)");
    lodAction->setCheckable(true);
    lodAction->setChecked(m_settings->getUseLod());
    connect(lodAction, &QAction::triggered, m_settings, [this, lodAction]() { m_settings->setUseLod(lodAction->isChecked()); });

    // Help menu
    auto* helpMenu = menuBar->addMenu("&Help");
    helpMenu->addAction("&Keyboard Shortcuts", this, &MainWindow::showShortcuts);
    helpMenu->addAction("&About SciRender", this, &MainWindow::showAbout);
    helpMenu->addSeparator();
    helpMenu->addAction("&Documentation", this, []() { QDesktopServices::openUrl(QUrl("https://github.com/thermeng/SciRender")); });
}

// ============================================================================
// Sidebar
// ============================================================================
void MainWindow::setupSidebar() {
    m_sidebarDock = new QDockWidget(this);
    m_sidebarDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    m_sidebarDock->setAllowedAreas(Qt::LeftDockWidgetArea);
    m_sidebarDock->setTitleBarWidget(new QWidget); // hide default title bar

    m_sidebarWidget = new QWidget;
    m_sidebarWidget->setStyleSheet("background-color: #252526;");
    auto* mainLayout = new QHBoxLayout(m_sidebarWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // --- Icon strip (left edge, 48px) ---
    auto* iconStrip = new QWidget;
    iconStrip->setFixedWidth(kIconStripWidth);
    iconStrip->setStyleSheet("background-color: #252526;");
    auto* iconLayout = new QVBoxLayout(iconStrip);
    iconLayout->setContentsMargins(0, 4, 0, 4);
    iconLayout->setSpacing(0);

    struct IconEntry {
        const char* icon;
        const char* tooltip;
        int section;
    };
    const IconEntry icons[] = {
        {"\u{1F4C2}", "Open Mesh", -1},
        {"\u{1F4A1}", "Lighting", 0},
        {"\u{2702}", "Slicing", 1},
        {"\u{1F441}", "View & Display", 2},
        {"\u{1F3A8}", "Colormap", 3},
        {"\u{27A1}", "Vectors", 4},
        {"\u{1F30D}", "Streamlines", 5},
        {"\u{1F4F7}", "Screenshot", 6},
        {"\u{1F4CA}", "Mesh Info", 7},
    };

    m_iconButtons.clear();
    for (int i = 0; i < 9; ++i) {
        auto* btn = new QToolButton;
        btn->setText(QString::fromUtf8(icons[i].icon));
        btn->setToolTip(QString::fromUtf8(icons[i].tooltip));
        btn->setFixedSize(48, 44);
        btn->setCheckable(i > 0);
        btn->setCursor(Qt::PointingHandCursor);

        // VS Code-style icon strip: no border, clean look
        btn->setStyleSheet(
            "QToolButton {"
            "  font-size: 18px; border: none;"
            "  background: transparent; color: #858585;"
            "  border-left: 3px solid transparent;"
            "}"
            "QToolButton:hover {"
            "  background: #2a2d2e; color: #cccccc;"
            "}"
            "QToolButton:checked {"
            "  background: #37373d; color: #ffffff;"
            "  border-left: 3px solid #007acc;"
            "}"
        );

        iconLayout->addWidget(btn);
        m_iconButtons.append(btn);

        if (i == 0) {
            // Open button — no checkable, just opens file
            btn->setStyleSheet(
                "QToolButton {"
                "  font-size: 18px; border: none;"
                "  background: transparent; color: #858585;"
                "  border-left: 3px solid transparent;"
                "}"
                "QToolButton:hover {"
                "  background: #2a2d2e; color: #cccccc;"
                "}"
            );
            connect(btn, &QToolButton::clicked, this, &MainWindow::openMesh);
        } else {
            int section = icons[i].section;
            connect(btn, &QToolButton::clicked, this, [this, section]() { setSidebarSection(section); });
        }
    }
    iconLayout->addStretch();

    // --- Section stack with panel header ---
    auto* rightPanel = new QWidget;
    rightPanel->setStyleSheet("background-color: #252526;");
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    // Panel header (title bar for the expanded panel)
    m_panelHeader = new QWidget;
    m_panelHeader->setFixedHeight(32);
    m_panelHeader->setStyleSheet("background-color: #252526; border-bottom: 1px solid #3c3c3c;");
    auto* headerLayout = new QHBoxLayout(m_panelHeader);
    headerLayout->setContentsMargins(10, 0, 4, 0);
    headerLayout->setSpacing(4);

    m_panelTitle = new QLabel;
    m_panelTitle->setStyleSheet("font-size: 12px; font-weight: bold; color: #cccccc; background: transparent;");
    m_panelTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    headerLayout->addWidget(m_panelTitle, 1);

    auto* closeBtn = new QToolButton;
    closeBtn->setText("\u00D7");
    closeBtn->setFixedSize(24, 24);
    closeBtn->setToolTip("Close panel");
    closeBtn->setStyleSheet(
        "QToolButton { font-size: 16px; border: none; background: transparent; color: #858585; border-radius: 4px; }"
        "QToolButton:hover { background: #3c3c3c; color: #ffffff; }"
    );
    connect(closeBtn, &QToolButton::clicked, this, [this]() { setSidebarSection(m_activeSection); });
    headerLayout->addWidget(closeBtn);

    rightLayout->addWidget(m_panelHeader);
    m_panelHeader->setVisible(false);

    // Stacked widget (section pages)
    m_sectionStack = new QStackedWidget;
    m_sectionStack->setStyleSheet("QStackedWidget { background: #252526; }");

    m_sectionStack->addWidget(buildLightingPage());     // 0
    m_sectionStack->addWidget(buildSlicingPage());      // 1
    m_sectionStack->addWidget(buildViewDisplayPage());  // 2
    m_sectionStack->addWidget(buildColormapPage());     // 3
    m_sectionStack->addWidget(buildVectorsPage());      // 4
    m_sectionStack->addWidget(buildStreamlinesPage());  // 5
    m_sectionStack->addWidget(buildScreenshotPage());   // 6
    m_sectionStack->addWidget(buildMeshInfoPage());     // 7

    rightLayout->addWidget(m_sectionStack, 1);
    m_sectionStack->setVisible(false);

    mainLayout->addWidget(iconStrip);
    mainLayout->addWidget(rightPanel, 1);

    m_sidebarDock->setWidget(m_sidebarWidget);
    addDockWidget(Qt::LeftDockWidgetArea, m_sidebarDock);
}

// ============================================================================
// Section: Lighting (0)
// ============================================================================
QWidget* MainWindow::buildLightingPage() {
    auto* page = new QWidget;
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(4);

    auto* resetBtn = new QPushButton("Reset");
    connect(resetBtn, &QPushButton::clicked, m_settings, &RenderSettings::resetLighting);
    layout->addWidget(resetBtn);

    auto* markersCb = new QCheckBox("Light Markers");
    markersCb->setChecked(m_settings->getShowLightMarkers());
    connect(markersCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowLightMarkers);
    layout->addWidget(markersCb);

    layout->addWidget(sectionHeader("Intensity & Tone"));
    createLightSlider("Key Light", m_settings->getLightKeyIntensity(), 0, 1, 0.01, 1, [this](double v) { m_settings->setLightKeyIntensity(v); });
    layout->addWidget(layout->itemAt(layout->count()-1)->widget() ? nullptr : nullptr);
    // Build sliders directly
    {
        auto row = createLightSlider("Key Light", m_settings->getLightKeyIntensity(), 0, 1, 0.01, 1, [this](double v) { m_settings->setLightKeyIntensity(v); });
        layout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Warmth", m_settings->getLightWarm(), 0, 1, 0.01, 1, [this](double v) { m_settings->setLightWarm(v); });
        layout->addWidget(row.slider->parentWidget());
    }

    layout->addWidget(sectionHeader("Light Ratios"));
    {
        auto row = createLightSlider("Fill K/F", m_settings->getLightKF(), 1, 15, 0.1, 1, [this](double v) { m_settings->setLightKF(v); });
        layout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Back K/B", m_settings->getLightKB(), 1, 15, 0.1, 1, [this](double v) { m_settings->setLightKB(v); });
        layout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Head K/H", m_settings->getLightKH(), 1, 15, 0.1, 1, [this](double v) { m_settings->setLightKH(v); });
        layout->addWidget(row.slider->parentWidget());
    }

    layout->addWidget(sectionHeader("Direction"));
    auto* tabWidget = new QTabWidget;
    tabWidget->setStyleSheet(
        "QTabBar::tab {"
        "  padding: 4px 12px; background: #2d2d2d; color: #969696;"
        "  border: 1px solid #3c3c3c; border-bottom: none;"
        "}"
        "QTabBar::tab:selected {"
        "  background: #1e1e1e; color: #ffffff; border-top: 2px solid #007acc;"
        "}"
        "QTabBar::tab:hover:!selected { background: #353535; color: #cccccc; }"
        "QTabWidget::pane { border: 1px solid #3c3c3c; background: #1e1e1e; }"
    );

    const char* lightNames[] = {"Key", "Fill", "Back", "Head"};
    using GetterFn = float (RenderSettings::*)() const;
    GetterFn azGetters[] = { &RenderSettings::getLightKeyAzimuth, &RenderSettings::getLightFillAzimuth, &RenderSettings::getLightBackAzimuth, &RenderSettings::getLightHeadAzimuth };
    GetterFn elGetters[] = { &RenderSettings::getLightKeyElevation, &RenderSettings::getLightFillElevation, &RenderSettings::getLightBackElevation, &RenderSettings::getLightHeadElevation };
    void (RenderSettings::*azSetters[])(float) = { &RenderSettings::setLightKeyAzimuth, &RenderSettings::setLightFillAzimuth, &RenderSettings::setLightBackAzimuth, &RenderSettings::setLightHeadAzimuth };
    void (RenderSettings::*elSetters[])(float) = { &RenderSettings::setLightKeyElevation, &RenderSettings::setLightFillElevation, &RenderSettings::setLightBackElevation, &RenderSettings::setLightHeadElevation };

    for (int i = 0; i < 4; ++i) {
        auto* tab = new QWidget;
        auto* tabLayout = new QVBoxLayout(tab);
        tabLayout->setContentsMargins(4, 4, 4, 4);
        {
            auto row = createLightSlider("Azimuth", (m_settings->*azGetters[i])(), -180, 180, 1, 0,
                [this, i, azSetters](double v) { (m_settings->*azSetters[i])(v); });
            tabLayout->addWidget(row.slider->parentWidget());
        }
        {
            auto row = createLightSlider("Elevation", (m_settings->*elGetters[i])(), -90, 90, 1, 0,
                [this, i, elSetters](double v) { (m_settings->*elSetters[i])(v); });
            tabLayout->addWidget(row.slider->parentWidget());
        }
        tabLayout->addStretch();
        tabWidget->addTab(tab, QString::fromUtf8(lightNames[i]));
    }
    layout->addWidget(tabWidget);

    layout->addWidget(sectionHeader("Material"));
    {
        auto row = createLightSlider("Ambient", m_settings->getMatAmbient(), 0, 1, 0.01, 1, [this](double v) { m_settings->setMatAmbient(v); });
        layout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Diffuse", m_settings->getMatDiffuse(), 0, 1, 0.01, 1, [this](double v) { m_settings->setMatDiffuse(v); });
        layout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Specular", m_settings->getMatSpecular(), 0, 1, 0.01, 1, [this](double v) { m_settings->setMatSpecular(v); });
        layout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Shininess", m_settings->getMatShininess(), 1, 100, 1, 0, [this](double v) { m_settings->setMatShininess(v); });
        layout->addWidget(row.slider->parentWidget());
    }

    layout->addStretch();
    scroll->setWidget(content);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}

// ============================================================================
// Section: Slicing (1)
// ============================================================================
QWidget* MainWindow::buildSlicingPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(4);

    layout->addWidget(sectionHeader("Slicing"));

    auto* enableCb = new QCheckBox("Enable slicing");
    enableCb->setChecked(m_settings->getClipEnabled());
    connect(enableCb, &QCheckBox::toggled, m_settings, &RenderSettings::setClipEnabled);
    layout->addWidget(enableCb);

    auto* optionsGroup = new QWidget;
    auto* optLayout = new QVBoxLayout(optionsGroup);
    optLayout->setContentsMargins(0, 0, 0, 0);

    auto* axisRow = new QHBoxLayout;
    for (const char* axis : {"X", "Y", "Z"}) {
        auto* cb = new QCheckBox(QString::fromUtf8(axis));
        axisRow->addWidget(cb);
    }
    auto* cbX = qobject_cast<QCheckBox*>(axisRow->itemAt(0)->widget());
    auto* cbY = qobject_cast<QCheckBox*>(axisRow->itemAt(1)->widget());
    auto* cbZ = qobject_cast<QCheckBox*>(axisRow->itemAt(2)->widget());
    cbX->setChecked(m_settings->getSliceEnabledX());
    cbY->setChecked(m_settings->getSliceEnabledY());
    cbZ->setChecked(m_settings->getSliceEnabledZ());
    connect(cbX, &QCheckBox::toggled, m_settings, &RenderSettings::setSliceEnabledX);
    connect(cbY, &QCheckBox::toggled, m_settings, &RenderSettings::setSliceEnabledY);
    connect(cbZ, &QCheckBox::toggled, m_settings, &RenderSettings::setSliceEnabledZ);
    optLayout->addLayout(axisRow);

    optLayout->addWidget(new QLabel("Cut planes (world units)"));
    auto* cutLabel = qobject_cast<QLabel*>(optLayout->itemAt(optLayout->count()-1)->widget());
    if (cutLabel) cutLabel->setStyleSheet("font-size: 11px; color: #858585;");
    {
        auto row = createClipSlider("Slice X", m_settings->getSliceX(), m_settings->getWorldMinX(), m_settings->getWorldMaxX(),
            [this](double v) { m_settings->setSliceX(v); });
        optLayout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createClipSlider("Slice Y", m_settings->getSliceY(), m_settings->getWorldMinY(), m_settings->getWorldMaxY(),
            [this](double v) { m_settings->setSliceY(v); });
        optLayout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createClipSlider("Slice Z", m_settings->getSliceZ(), m_settings->getWorldMinZ(), m_settings->getWorldMaxZ(),
            [this](double v) { m_settings->setSliceZ(v); });
        optLayout->addWidget(row.slider->parentWidget());
    }

    auto* keepLabel = new QLabel("Keep side");
    keepLabel->setStyleSheet("font-size: 11px; color: #858585;");
    optLayout->addWidget(keepLabel);
    auto* invRow = new QHBoxLayout;
    auto* invX = new QCheckBox("Inv X");
    auto* invY = new QCheckBox("Inv Y");
    auto* invZ = new QCheckBox("Inv Z");
    invX->setChecked(m_settings->getInvertX());
    invY->setChecked(m_settings->getInvertY());
    invZ->setChecked(m_settings->getInvertZ());
    connect(invX, &QCheckBox::toggled, m_settings, &RenderSettings::setInvertX);
    connect(invY, &QCheckBox::toggled, m_settings, &RenderSettings::setInvertY);
    connect(invZ, &QCheckBox::toggled, m_settings, &RenderSettings::setInvertZ);
    invRow->addWidget(invX); invRow->addWidget(invY); invRow->addWidget(invZ);
    optLayout->addLayout(invRow);

    layout->addWidget(optionsGroup);
    connect(enableCb, &QCheckBox::toggled, optionsGroup, &QWidget::setEnabled);
    optionsGroup->setEnabled(m_settings->getClipEnabled());

    layout->addStretch();
    return page;
}

// ============================================================================
// Section: View & Display (2)
// ============================================================================
QWidget* MainWindow::buildViewDisplayPage() {
    auto* page = new QWidget;
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);

    // Camera
    layout->addWidget(sectionHeader("Camera"));
    auto* cameraGrid = new QGridLayout;
    const char* axisLabels[] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
    for (int i = 0; i < 6; ++i) {
        auto* btn = new QPushButton(QString::fromUtf8(axisLabels[i]));
        cameraGrid->addWidget(btn, i / 3, i % 3);
        connect(btn, &QPushButton::clicked, m_settings, [this, i]() { m_settings->snapToOrthoView(i); });
    }
    layout->addLayout(cameraGrid);

    auto* orthoCb = new QCheckBox("Parallel View");
    orthoCb->setChecked(m_settings->getOrthographic());
    connect(orthoCb, &QCheckBox::toggled, m_settings, &RenderSettings::setOrthographic);
    layout->addWidget(orthoCb);

    auto* rotateCb = new QCheckBox("Auto-Rotate");
    rotateCb->setChecked(m_settings->getAutoRotate());
    connect(rotateCb, &QCheckBox::toggled, m_settings, &RenderSettings::setAutoRotate);
    layout->addWidget(rotateCb);

    // Roll slider
    {
        auto row = createLightSlider("Roll", 0, -180, 180, 1, 0, [this](double v) { m_settings->roll(v); });
        auto* resetBtn = new QPushButton("0");
        resetBtn->setFixedWidth(28);
        connect(resetBtn, &QPushButton::clicked, m_settings, &RenderSettings::resetCamera);
        auto* rowWidget = row.slider->parentWidget();
        auto* hlay = qobject_cast<QHBoxLayout*>(rowWidget->layout());
        if (hlay) hlay->addWidget(resetBtn);
        layout->addWidget(rowWidget);
    }

    auto* resetCamBtn = new QPushButton("Reset Camera");
    connect(resetCamBtn, &QPushButton::clicked, m_settings, &RenderSettings::resetCamera);
    layout->addWidget(resetCamBtn);

    // Display
    layout->addWidget(sectionHeader("Display"));

    auto* wfRow = new QHBoxLayout;
    auto* wfCb = new QCheckBox("Wireframe");
    wfCb->setChecked(m_settings->isWireframe());
    connect(wfCb, &QCheckBox::toggled, m_settings, &RenderSettings::setWireframe);
    wfRow->addWidget(wfCb);
    auto* wfSlider = new QSlider(Qt::Horizontal);
    wfSlider->setRange(10, 100);
    wfSlider->setValue(static_cast<int>(m_settings->getLineWidth() * 10));
    wfSlider->setEnabled(m_settings->isWireframe());
    connect(wfSlider, &QSlider::valueChanged, m_settings, [this](int v) { m_settings->setLineWidth(v / 10.0); });
    connect(wfCb, &QCheckBox::toggled, wfSlider, &QWidget::setEnabled);
    wfRow->addWidget(wfSlider);
    layout->addLayout(wfRow);

    auto* ceRow = new QHBoxLayout;
    auto* ceCb = new QCheckBox("Cell Edge");
    ceCb->setChecked(m_settings->getShowCellEdges());
    ceCb->setEnabled(m_settings->getSupportsCellGrid());
    connect(ceCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowCellEdges);
    ceRow->addWidget(ceCb);
    auto* ceSlider = new QSlider(Qt::Horizontal);
    ceSlider->setRange(10, 100);
    ceSlider->setValue(static_cast<int>(m_settings->getCellEdgeLineWidth() * 10));
    ceSlider->setEnabled(m_settings->getShowCellEdges());
    connect(ceSlider, &QSlider::valueChanged, m_settings, [this](int v) { m_settings->setCellEdgeLineWidth(v / 10.0); });
    connect(ceCb, &QCheckBox::toggled, ceSlider, &QWidget::setEnabled);
    ceRow->addWidget(ceSlider);
    layout->addLayout(ceRow);

    auto* surfaceCb = new QCheckBox("Surface");
    surfaceCb->setChecked(m_settings->isSurfaceVisible());
    connect(surfaceCb, &QCheckBox::toggled, m_settings, &RenderSettings::toggleSurface);
    layout->addWidget(surfaceCb);

    auto* pointsCb = new QCheckBox("Points");
    pointsCb->setChecked(m_settings->getShowPoints());
    connect(pointsCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowPoints);
    layout->addWidget(pointsCb);

    auto* gridCb = new QCheckBox("Reference");
    gridCb->setChecked(m_settings->isGridVisible());
    connect(gridCb, &QCheckBox::toggled, m_settings, &RenderSettings::toggleGrid);
    layout->addWidget(gridCb);

    auto* boundsCb = new QCheckBox("Bounding Box");
    boundsCb->setChecked(m_settings->getShowBounds());
    connect(boundsCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowBounds);
    layout->addWidget(boundsCb);

    auto* defectsCb = new QCheckBox("Defects");
    defectsCb->setChecked(m_settings->getShowQualityOverlay());
    connect(defectsCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowQualityOverlay);
    layout->addWidget(defectsCb);

    // Point controls
    auto* pointGroup = new QWidget;
    auto* pointLayout = new QVBoxLayout(pointGroup);
    pointLayout->setContentsMargins(0, 0, 0, 0);
    {
        auto row = createLightSlider("Size", m_settings->getPointSize(), 1, 20, 0.5, 1, [this](double v) { m_settings->setPointSize(v); });
        pointLayout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Opac", m_settings->getPointOpacity(), 0.1, 1, 0.05, 2, [this](double v) { m_settings->setPointOpacity(v); });
        pointLayout->addWidget(row.slider->parentWidget());
    }
    auto* scalarCb = new QCheckBox("Color by scalar");
    scalarCb->setChecked(m_settings->getPointUseScalar());
    connect(scalarCb, &QCheckBox::toggled, m_settings, &RenderSettings::setPointUseScalar);
    pointLayout->addWidget(scalarCb);
    layout->addWidget(pointGroup);
    connect(pointsCb, &QCheckBox::toggled, pointGroup, &QWidget::setVisible);
    pointGroup->setVisible(m_settings->getShowPoints());

    // Transparency
    layout->addWidget(sectionHeader("Transparency"));
    {
        auto row = createLightSlider("Surface", m_settings->getSurfaceOpacity(), 0.1, 1, 0.05, 2, [this](double v) { m_settings->setSurfaceOpacity(v); });
        layout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Point", m_settings->getPointOpacity(), 0.1, 1, 0.05, 2, [this](double v) { m_settings->setPointOpacity(v); });
        layout->addWidget(row.slider->parentWidget());
    }

    // Appearance
    layout->addWidget(sectionHeader("Appearance"));
    auto* msaaRow = new QHBoxLayout;
    auto* msaaLabel = new QLabel("MSAA");
    msaaLabel->setStyleSheet("font-size: 11px; color: #cccccc;");
    msaaRow->addWidget(msaaLabel);
    auto* msaaCombo = new QComboBox;
    msaaCombo->addItems({"Off", "2x", "4x"});
    msaaCombo->setCurrentIndex(m_settings->getMsaaSamples() / 2);
    connect(msaaCombo, &QComboBox::activated, m_settings, [this](int idx) { m_settings->setMsaaSamples(idx * 2); });
    msaaRow->addWidget(msaaCombo, 1);
    layout->addLayout(msaaRow);

    // Overlays
    layout->addWidget(sectionHeader("Overlays"));
    auto* gizmoCb = new QCheckBox("Gizmo");
    gizmoCb->setChecked(m_settings->isGizmoVisible());
    connect(gizmoCb, &QCheckBox::toggled, m_settings, &RenderSettings::setGizmoVisible);
    layout->addWidget(gizmoCb);

    auto* fpsCb = new QCheckBox("FPS HUD");
    fpsCb->setChecked(m_settings->getShowFps());
    connect(fpsCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowFps);
    layout->addWidget(fpsCb);

    // Colors
    layout->addWidget(sectionHeader("Colors"));
    layout->addWidget(createSwatchButton("Wireframe", m_settings->getMeshColorQml(), [this]() {
        if (!m_meshColorDialog) {
            m_meshColorDialog = new QColorDialog(m_settings->getMeshColorQml(), this);
            m_meshColorDialog->setOption(QColorDialog::ShowAlphaChannel, false);
            connect(m_meshColorDialog, &QColorDialog::colorSelected, m_settings, &RenderSettings::setMeshColorQml);
        }
        m_meshColorDialog->open();
    }));
    layout->addWidget(createSwatchButton("Surface", m_settings->getSurfaceColorQml(), [this]() {
        if (!m_surfaceColorDialog) {
            m_surfaceColorDialog = new QColorDialog(m_settings->getSurfaceColorQml(), this);
            connect(m_surfaceColorDialog, &QColorDialog::colorSelected, m_settings, &RenderSettings::setSurfaceColorQml);
        }
        m_surfaceColorDialog->open();
    }));
    layout->addWidget(createSwatchButton("Background", m_settings->getBgColorQml(), [this]() {
        if (!m_bgColorDialog) {
            m_bgColorDialog = new QColorDialog(m_settings->getBgColorQml(), this);
            connect(m_bgColorDialog, &QColorDialog::colorSelected, m_settings, &RenderSettings::setBgColorQml);
        }
        m_bgColorDialog->open();
    }));

    layout->addStretch();
    scroll->setWidget(content);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}

// ============================================================================
// Section: Colormap (3)
// ============================================================================
QWidget* MainWindow::buildColormapPage() {
    auto* page = new QWidget;
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(4);

    layout->addWidget(sectionHeader("Field"));
    m_scalarCombo = new QComboBox;
    m_scalarCombo->addItems(m_settings->getAvailableScalars());
    m_scalarCombo->setCurrentText(m_settings->getActiveScalarNameQml());
    m_scalarCombo->setEnabled(m_settings->hasMeshScalars());
    connect(m_scalarCombo, &QComboBox::activated, m_settings, [this](int idx) {
        m_settings->setActiveScalarField(m_scalarCombo->itemText(idx));
    });
    layout->addWidget(m_scalarCombo);

    layout->addWidget(sectionHeader("Palette"));
    auto* cmapGrid = buildColormapGrid(m_settings->getColormapChoice(),
        [this](int i) { m_settings->setColormapChoice(i); });
    layout->addLayout(cmapGrid);

    auto* scalarColorCb = new QCheckBox("Color by scalar");
    scalarColorCb->setChecked(m_settings->getMeshUseScalarColor());
    connect(scalarColorCb, &QCheckBox::toggled, m_settings, &RenderSettings::setMeshUseScalarColor);
    layout->addWidget(scalarColorCb);

    auto* reversedCb = new QCheckBox("Reverse palette");
    reversedCb->setChecked(m_settings->getColormapReversed());
    connect(reversedCb, &QCheckBox::toggled, m_settings, &RenderSettings::setColormapReversed);
    layout->addWidget(reversedCb);

    auto* showBarCb = new QCheckBox("Show colorbar");
    showBarCb->setChecked(m_settings->getShowScalarColorbar());
    connect(showBarCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowScalarColorbar);
    layout->addWidget(showBarCb);

    layout->addWidget(sectionHeader("Colorbar"));
    auto* ticksRow = new QHBoxLayout;
    auto* ticksLabel = new QLabel("Ticks");
    ticksLabel->setStyleSheet("font-size: 11px; color: #cccccc;");
    ticksRow->addWidget(ticksLabel);
    auto* ticksSpin = new QSpinBox;
    ticksSpin->setRange(2, 20);
    ticksSpin->setValue(m_settings->getColorbarTicks());
    connect(ticksSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setColorbarTicks);
    ticksRow->addWidget(ticksSpin, 1);
    layout->addLayout(ticksRow);

    layout->addWidget(sectionHeader("Filter"));
    auto* resetFilterBtn = new QPushButton("Reset");
    resetFilterBtn->setFixedWidth(50);
    connect(resetFilterBtn, &QPushButton::clicked, m_settings, [this]() {
        m_settings->setFilterMin(m_settings->getDataScalarMinQml());
        m_settings->setFilterMax(m_settings->getDataScalarMaxQml());
    });
    layout->addWidget(resetFilterBtn);

    {
        auto row = createClipSlider("Min", m_settings->getFilterMin(), m_settings->getDataScalarMinQml(), m_settings->getDataScalarMaxQml(),
            [this](double v) { m_settings->setFilterMin(v); });
        layout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createClipSlider("Max", m_settings->getFilterMax(), m_settings->getDataScalarMinQml(), m_settings->getDataScalarMaxQml(),
            [this](double v) { m_settings->setFilterMax(v); });
        layout->addWidget(row.slider->parentWidget());
    }

    layout->addStretch();
    scroll->setWidget(content);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}

// ============================================================================
// Section: Vectors (4)
// ============================================================================
QWidget* MainWindow::buildVectorsPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(4);

    auto* showCb = new QCheckBox("Show vectors");
    showCb->setChecked(m_settings->getShowVectors());
    connect(showCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowVectors);
    layout->addWidget(showCb);

    auto* optionsGroup = new QWidget;
    auto* optLayout = new QVBoxLayout(optionsGroup);
    optLayout->setContentsMargins(0, 0, 0, 0);

    optLayout->addWidget(sectionHeader("Field & Scale"));
    m_vectorCombo = new QComboBox;
    m_vectorCombo->addItems(m_settings->getAvailableVectors());
    m_vectorCombo->setCurrentText(m_settings->getVectorField());
    connect(m_vectorCombo, &QComboBox::activated, m_settings, [this](int) {
        m_settings->setActiveVectorField(m_vectorCombo->currentText());
    });
    optLayout->addWidget(m_vectorCombo);

    {
        auto row = createLightSlider("Scale", m_settings->getVectorScale(), 0.01, 5.0, 0.01, 2, [this](double v) { m_settings->setVectorScale(v); });
        optLayout->addWidget(row.slider->parentWidget());
    }

    auto* scaleMagCb = new QCheckBox("Scale by magnitude");
    scaleMagCb->setChecked(m_settings->getVectorScaleByMagnitude());
    connect(scaleMagCb, &QCheckBox::toggled, m_settings, &RenderSettings::setVectorScaleByMagnitude);
    optLayout->addWidget(scaleMagCb);

    {
        auto row = createLightSlider("Stride", m_settings->getVectorStride(), 1, 20, 1, 0, [this](double v) { m_settings->setVectorStride(static_cast<int>(v)); });
        optLayout->addWidget(row.slider->parentWidget());
    }

    optLayout->addWidget(sectionHeader("Magnitude"));
    auto* magCombo = new QComboBox;
    magCombo->addItems({"Linear", "Square root", "Logarithmic"});
    magCombo->setCurrentIndex(m_settings->getVectorMagTransform());
    connect(magCombo, &QComboBox::activated, m_settings, [this](int idx) { m_settings->setVectorMagTransform(idx); });
    optLayout->addWidget(magCombo);

    optLayout->addWidget(sectionHeader("Color"));
    optLayout->addWidget(createSwatchButton("Vector", m_settings->getVectorColorQml(), [this]() {
        if (!m_vectorColorDialog) {
            m_vectorColorDialog = new QColorDialog(m_settings->getVectorColorQml(), this);
            connect(m_vectorColorDialog, &QColorDialog::colorSelected, m_settings, &RenderSettings::setVectorColorQml);
        }
        m_vectorColorDialog->open();
    }));

    auto* useCmapCb = new QCheckBox("Color by magnitude");
    useCmapCb->setChecked(m_settings->getVectorUseColormap());
    connect(useCmapCb, &QCheckBox::toggled, m_settings, &RenderSettings::setVectorUseColormap);
    optLayout->addWidget(useCmapCb);

    auto* vCmapGrid = buildColormapGrid(m_settings->getVectorColormapChoice(),
        [this](int i) { m_settings->setVectorColormapChoice(i); });
    optLayout->addLayout(vCmapGrid);

    // Reverse palette
    auto* revCb = new QCheckBox("Reverse palette");
    revCb->setChecked(m_settings->getVectorColormapReversed());
    connect(revCb, &QCheckBox::toggled, m_settings, &RenderSettings::setVectorColormapReversed);
    optLayout->addWidget(revCb);

    layout->addWidget(optionsGroup);
    connect(showCb, &QCheckBox::toggled, optionsGroup, &QWidget::setEnabled);
    optionsGroup->setEnabled(m_settings->getShowVectors());

    layout->addStretch();
    return page;
}

// ============================================================================
// Section: Streamlines (5)
// ============================================================================
QWidget* MainWindow::buildStreamlinesPage() {
    auto* page = new QWidget;
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(4);

    auto* showCb = new QCheckBox("Show streamlines");
    showCb->setChecked(m_settings->getShowStreamlines());
    connect(showCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowStreamlines);
    layout->addWidget(showCb);

    auto* optionsGroup = new QWidget;
    auto* optLayout = new QVBoxLayout(optionsGroup);
    optLayout->setContentsMargins(0, 0, 0, 0);
    optLayout->setSpacing(8);

    // Field
    auto* fieldGroup = new QWidget;
    fieldGroup->setStyleSheet("QWidget { background: #222222; border-radius: 0 0 4px 4px; }");
    auto* fieldLayout = new QVBoxLayout(fieldGroup);
    fieldLayout->setContentsMargins(12, 4, 8, 8);

    m_streamlineCombo = new QComboBox;
    m_streamlineCombo->addItems(m_settings->getAvailableVectors());
    m_streamlineCombo->setCurrentText(m_settings->getStreamlineVectorField());
    connect(m_streamlineCombo, &QComboBox::activated, m_settings, [this](int) {
        m_settings->setStreamlineVectorField(m_streamlineCombo->currentText());
    });
    fieldLayout->addWidget(m_streamlineCombo);

    auto* seedCountRow = new QHBoxLayout;
    auto* seedCountLabel = new QLabel("Seed count");
    seedCountLabel->setStyleSheet("font-size: 11px; color: #cccccc;");
    seedCountRow->addWidget(seedCountLabel);
    auto* seedSpin = new QSpinBox;
    seedSpin->setRange(1, 500);
    seedSpin->setValue(m_settings->getStreamlineSeedCount());
    connect(seedSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setStreamlineSeedCount);
    seedCountRow->addWidget(seedSpin, 1);
    fieldLayout->addLayout(seedCountRow);

    {
        auto row = createLightSlider("Step size", m_settings->getStreamlineStepSize(), 0.005, 0.1, 0.001, 3, [this](double v) { m_settings->setStreamlineStepSize(v); });
        fieldLayout->addWidget(row.slider->parentWidget());
    }

    auto* maxStepsRow = new QHBoxLayout;
    auto* maxStepsLabel = new QLabel("Max steps");
    maxStepsLabel->setStyleSheet("font-size: 11px; color: #cccccc;");
    maxStepsRow->addWidget(maxStepsLabel);
    auto* maxStepsSpin = new QSpinBox;
    maxStepsSpin->setRange(10, 500);
    maxStepsSpin->setValue(m_settings->getStreamlineMaxSteps());
    connect(maxStepsSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setStreamlineMaxSteps);
    maxStepsRow->addWidget(maxStepsSpin, 1);
    fieldLayout->addLayout(maxStepsRow);

    auto* fieldHeader = createCollapsibleHeader("Field", true, [fieldGroup](bool expanded) {
        fieldGroup->setVisible(expanded);
    });
    optLayout->addWidget(fieldHeader);
    optLayout->addWidget(fieldGroup);

    // Color
    auto* colorGroup = new QWidget;
    colorGroup->setStyleSheet("QWidget { background: #222222; border-radius: 0 0 4px 4px; }");
    auto* colorLayout = new QVBoxLayout(colorGroup);
    colorLayout->setContentsMargins(12, 4, 8, 8);
    colorLayout->addWidget(createSwatchButton("Streamline", m_settings->getStreamlineColorQml(), [this]() {
        if (!m_streamlineColorDialog) {
            m_streamlineColorDialog = new QColorDialog(m_settings->getStreamlineColorQml(), this);
            connect(m_streamlineColorDialog, &QColorDialog::colorSelected, m_settings, &RenderSettings::setStreamlineColorQml);
        }
        m_streamlineColorDialog->open();
    }));
    auto* slUseCmapCb = new QCheckBox("Color by magnitude");
    slUseCmapCb->setChecked(m_settings->getStreamlineUseColormap());
    connect(slUseCmapCb, &QCheckBox::toggled, m_settings, &RenderSettings::setStreamlineUseColormap);
    colorLayout->addWidget(slUseCmapCb);

    auto* slCmapGrid = buildColormapGrid(m_settings->getStreamlineColormapChoice(),
        [this](int i) { m_settings->setStreamlineColormapChoice(i); });
    colorLayout->addLayout(slCmapGrid);

    auto* slRevCb = new QCheckBox("Reverse palette");
    slRevCb->setChecked(m_settings->getStreamlineColormapReversed());
    connect(slRevCb, &QCheckBox::toggled, m_settings, &RenderSettings::setStreamlineColormapReversed);
    colorLayout->addWidget(slRevCb);

    auto* colorHeader = createCollapsibleHeader("Color", true, [colorGroup](bool expanded) {
        colorGroup->setVisible(expanded);
    });
    optLayout->addWidget(colorHeader);
    optLayout->addWidget(colorGroup);

    // Seeding
    auto* seedGroup = new QWidget;
    seedGroup->setStyleSheet("QWidget { background: #222222; border-radius: 0 0 4px 4px; }");
    auto* seedLayout = new QVBoxLayout(seedGroup);
    seedLayout->setContentsMargins(12, 4, 8, 8);
    auto* seedModeCombo = new QComboBox;
    seedModeCombo->addItems({"Volume", "Surface", "Plane XY", "Plane XZ", "Plane YZ"});
    const QStringList modeKeys = {"Volume", "Surface", "PlaneXY", "PlaneXZ", "PlaneYZ"};
    seedModeCombo->setCurrentIndex(modeKeys.indexOf(m_settings->getSeedMode()));
    connect(seedModeCombo, &QComboBox::activated, m_settings, [this, modeKeys](int idx) {
        m_settings->setSeedMode(modeKeys[idx]);
    });
    seedLayout->addWidget(seedModeCombo);

    auto* planePosRow = new QHBoxLayout;
    auto* planePosLabel = new QLabel("Plane pos");
    planePosLabel->setStyleSheet("font-size: 11px; color: #cccccc;");
    planePosRow->addWidget(planePosLabel);
    auto* planePosSlider = new QSlider(Qt::Horizontal);
    planePosSlider->setRange(0, 1000);
    planePosSlider->setValue(static_cast<int>(m_settings->getSeedPlanePos() * 1000));
    connect(planePosSlider, &QSlider::valueChanged, m_settings, [this](int v) { m_settings->setSeedPlanePos(v / 1000.0); });
    planePosRow->addWidget(planePosSlider, 1);
    seedLayout->addLayout(planePosRow);

    auto* seedsURow = new QHBoxLayout;
    auto* seedsULabel = new QLabel("Seeds U");
    seedsULabel->setStyleSheet("font-size: 11px; color: #cccccc;");
    seedsURow->addWidget(seedsULabel);
    auto* seedsUSpin = new QSpinBox;
    seedsUSpin->setRange(1, 200);
    seedsUSpin->setValue(m_settings->getSeedPlaneCountU());
    connect(seedsUSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setSeedPlaneCountU);
    seedsURow->addWidget(seedsUSpin, 1);
    seedLayout->addLayout(seedsURow);

    auto* seedsVRow = new QHBoxLayout;
    auto* seedsVLabel = new QLabel("Seeds V");
    seedsVLabel->setStyleSheet("font-size: 11px; color: #cccccc;");
    seedsVRow->addWidget(seedsVLabel);
    auto* seedsVSpin = new QSpinBox;
    seedsVSpin->setRange(1, 200);
    seedsVSpin->setValue(m_settings->getSeedPlaneCountV());
    connect(seedsVSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setSeedPlaneCountV);
    seedsVRow->addWidget(seedsVSpin, 1);
    seedLayout->addLayout(seedsVRow);

    {
        auto row = createLightSlider("Jitter", m_settings->getSeedJitter(), 0, 1, 0.01, 2, [this](double v) { m_settings->setSeedJitter(v); });
        seedLayout->addWidget(row.slider->parentWidget());
    }

    auto* showSeedsCb = new QCheckBox("Show seeds");
    showSeedsCb->setChecked(m_settings->getShowSeeds());
    connect(showSeedsCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowSeeds);
    seedLayout->addWidget(showSeedsCb);

    {
        auto row = createLightSlider("Seed size", m_settings->getSeedPointSize(), 1, 20, 0.5, 1, [this](double v) { m_settings->setSeedPointSize(v); });
        seedLayout->addWidget(row.slider->parentWidget());
    }

    seedLayout->addWidget(createSwatchButton("Seed color", m_settings->getSeedPointColorQml(), [this]() {
        if (!m_seedColorDialog) {
            m_seedColorDialog = new QColorDialog(m_settings->getSeedPointColorQml(), this);
            connect(m_seedColorDialog, &QColorDialog::colorSelected, m_settings, &RenderSettings::setSeedPointColorQml);
        }
        m_seedColorDialog->open();
    }));

    auto* seedHeader = createCollapsibleHeader("Seeding", true, [seedGroup](bool expanded) {
        seedGroup->setVisible(expanded);
    });
    optLayout->addWidget(seedHeader);
    optLayout->addWidget(seedGroup);

    // Appearance
    auto* appGroup = new QWidget;
    appGroup->setStyleSheet("QWidget { background: #222222; border-radius: 0 0 4px 4px; }");
    auto* appLayout = new QVBoxLayout(appGroup);
    appLayout->setContentsMargins(12, 4, 8, 8);
    {
        auto row = createLightSlider("Opacity", m_settings->getStreamlineOpacity(), 0, 1, 0.01, 2, [this](double v) { m_settings->setStreamlineOpacity(v); });
        appLayout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Ribbon width", m_settings->getStreamlineRibbonWidth(), 0.001, 0.05, 0.001, 3, [this](double v) { m_settings->setStreamlineRibbonWidth(v); });
        appLayout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Taper factor", m_settings->getStreamlineTaperFactor(), 0, 0.8, 0.01, 2, [this](double v) { m_settings->setStreamlineTaperFactor(v); });
        appLayout->addWidget(row.slider->parentWidget());
    }
    auto* dashCb = new QCheckBox("Dash");
    dashCb->setChecked(m_settings->getStreamlineDashEnabled());
    connect(dashCb, &QCheckBox::toggled, m_settings, &RenderSettings::setStreamlineDashEnabled);
    appLayout->addWidget(dashCb);
    {
        auto row = createLightSlider("Dash speed", m_settings->getStreamlineDashSpeed(), 0.1, 5.0, 0.1, 1, [this](double v) { m_settings->setStreamlineDashSpeed(v); });
        appLayout->addWidget(row.slider->parentWidget());
    }
    auto* appHeader = createCollapsibleHeader("Appearance", false, [appGroup](bool expanded) {
        appGroup->setVisible(expanded);
    });
    optLayout->addWidget(appHeader);
    appGroup->setVisible(false);
    optLayout->addWidget(appGroup);

    // Lighting
    auto* lightGroup = new QWidget;
    lightGroup->setStyleSheet("QWidget { background: #222222; border-radius: 0 0 4px 4px; }");
    auto* lightLayout = new QVBoxLayout(lightGroup);
    lightLayout->setContentsMargins(12, 4, 8, 8);
    {
        auto row = createLightSlider("Ambient", m_settings->getStreamlineAmbient(), 0, 1, 0.01, 2, [this](double v) { m_settings->setStreamlineAmbient(v); });
        lightLayout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Diffuse", m_settings->getStreamlineDiffuse(), 0, 1, 0.01, 2, [this](double v) { m_settings->setStreamlineDiffuse(v); });
        lightLayout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Specular", m_settings->getStreamlineSpecular(), 0, 1, 0.01, 2, [this](double v) { m_settings->setStreamlineSpecular(v); });
        lightLayout->addWidget(row.slider->parentWidget());
    }
    auto* specPowerSpin = new QSpinBox;
    specPowerSpin->setRange(2, 128);
    specPowerSpin->setValue(m_settings->getStreamlineSpecularPower());
    connect(specPowerSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setStreamlineSpecularPower);
    lightLayout->addWidget(specPowerSpin);
    auto* lightHeader = createCollapsibleHeader("Lighting", false, [lightGroup](bool expanded) {
        lightGroup->setVisible(expanded);
    });
    optLayout->addWidget(lightHeader);
    lightGroup->setVisible(false);
    optLayout->addWidget(lightGroup);

    // Arrows
    auto* arrowsCb = new QCheckBox("Show direction");
    arrowsCb->setChecked(m_settings->getShowStreamlineArrows());
    connect(arrowsCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowStreamlineArrows);
    optLayout->addWidget(arrowsCb);

    auto* arrowsGroup = new QWidget;
    auto* arrowsLayout = new QVBoxLayout(arrowsGroup);
    arrowsLayout->setContentsMargins(0, 0, 0, 0);
    auto* arrowSpacingSpin = new QSpinBox;
    arrowSpacingSpin->setRange(2, 20);
    arrowSpacingSpin->setValue(m_settings->getStreamlineArrowSpacing());
    connect(arrowSpacingSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setStreamlineArrowSpacing);
    arrowsLayout->addWidget(arrowSpacingSpin);
    {
        auto row = createLightSlider("Arrow size", m_settings->getStreamlineArrowSize(), 0.01, 0.2, 0.01, 2, [this](double v) { m_settings->setStreamlineArrowSize(v); });
        arrowsLayout->addWidget(row.slider->parentWidget());
    }
    optLayout->addWidget(arrowsGroup);
    connect(arrowsCb, &QCheckBox::toggled, arrowsGroup, &QWidget::setVisible);
    arrowsGroup->setVisible(m_settings->getShowStreamlineArrows());

    // Particles
    auto* particlesCb = new QCheckBox("Show Particles");
    particlesCb->setChecked(m_settings->getShowParticles());
    connect(particlesCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowParticles);
    optLayout->addWidget(particlesCb);

    auto* particlesGroup = new QWidget;
    auto* particlesLayout = new QVBoxLayout(particlesGroup);
    particlesLayout->setContentsMargins(0, 0, 0, 0);
    {
        auto row = createLightSlider("Particle count", m_settings->getParticleCount(), 10, 5000, 10, 0, [this](double v) { m_settings->setParticleCount(static_cast<int>(v)); });
        particlesLayout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Particle speed", m_settings->getParticleSpeed(), 0.1, 100, 0.1, 1, [this](double v) { m_settings->setParticleSpeed(v); });
        particlesLayout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Particle size", m_settings->getParticleSize(), 1, 20, 0.5, 1, [this](double v) { m_settings->setParticleSize(v); });
        particlesLayout->addWidget(row.slider->parentWidget());
    }
    optLayout->addWidget(particlesGroup);
    connect(particlesCb, &QCheckBox::toggled, particlesGroup, &QWidget::setVisible);
    particlesGroup->setVisible(m_settings->getShowParticles());

    layout->addWidget(optionsGroup);
    connect(showCb, &QCheckBox::toggled, optionsGroup, &QWidget::setEnabled);
    optionsGroup->setEnabled(m_settings->getShowStreamlines());

    layout->addStretch();
    scroll->setWidget(content);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}

// ============================================================================
// Section: Screenshot (6)
// ============================================================================
QWidget* MainWindow::buildScreenshotPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(4);

    auto* saveBtn = new QPushButton("Save Screenshot");
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::saveScreenshot);
    layout->addWidget(saveBtn);

    auto* transCb = new QCheckBox("Transparent (PNG)");
    transCb->setChecked(m_settings->getScreenshotTransparent());
    connect(transCb, &QCheckBox::toggled, m_settings, &RenderSettings::setScreenshotTransparent);
    layout->addWidget(transCb);

    layout->addStretch();
    return page;
}

// ============================================================================
// Section: Mesh Info (7)
// ============================================================================
QWidget* MainWindow::buildMeshInfoPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(4);

    auto addInfoRow = [layout](const QString& label, const QString& value, const QString& color = "#ddd") {
        auto* row = new QHBoxLayout;
        auto* lbl = new QLabel(label);
        lbl->setFixedWidth(96);
        lbl->setStyleSheet("font-size: 11px; color: #858585;");
        auto* val = new QLabel(value);
        val->setStyleSheet(QString("font-size: 11px; color: %1;").arg(color));
        row->addWidget(lbl);
        row->addWidget(val);
        layout->addLayout(row);
    };

    layout->addWidget(sectionHeader("Source"));
    addInfoRow("Type", m_settings->getMeshDataType());
    addInfoRow("Format", m_settings->getMeshFormat());

    layout->addWidget(sectionHeader("Geometry"));
    addInfoRow("Triangles", QString::number(m_settings->getTriangleCount()));
    addInfoRow("Points", QString::number(m_settings->getPointCount()));

    layout->addWidget(sectionHeader("Quality"));
    addInfoRow("Degenerate", QString::number(m_settings->getDegenerateFaces()), m_settings->getDegenerateFaces() > 0 ? "#ff6666" : "#ddd");
    addInfoRow("Open edges", QString::number(m_settings->getOpenEdges()), m_settings->getOpenEdges() > 0 ? "#ffaa44" : "#ddd");
    addInfoRow("Non-manifold E", QString::number(m_settings->getNonManifoldEdges()), m_settings->getNonManifoldEdges() > 0 ? "#ff44ff" : "#ddd");
    addInfoRow("Non-manifold V", QString::number(m_settings->getNonManifoldVerts()), m_settings->getNonManifoldVerts() > 0 ? "#ff44ff" : "#ddd");
    addInfoRow("Watertight", m_settings->getWatertight() ? "yes" : "no", m_settings->getWatertight() ? "#66dd66" : "#ff6666");

    layout->addWidget(sectionHeader("Bounding box"));
    auto* bbGrid = new QGridLayout;
    bbGrid->setHorizontalSpacing(4);
    const char* xyz[] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
        auto* h = new QLabel(QString::fromUtf8(xyz[i]));
        h->setStyleSheet("font-size: 10px; color: #858585;");
        bbGrid->addWidget(h, 0, i + 1);
    }
    auto addBBRow = [&](int row, const QString& lbl, auto getter) {
        auto* l = new QLabel(lbl);
        l->setStyleSheet("font-size: 11px; color: #858585;");
        bbGrid->addWidget(l, row, 0);
        for (int i = 0; i < 3; ++i) {
            auto* v = new QLabel(QString::number(getter(i), 'f', 3));
            v->setStyleSheet("font-size: 11px; color: #cccccc;");
            bbGrid->addWidget(v, row, i + 1);
        }
    };
    addBBRow(1, "Min", [&](int i) { return i == 0 ? m_settings->getWorldMinX() : i == 1 ? m_settings->getWorldMinY() : m_settings->getWorldMinZ(); });
    addBBRow(2, "Max", [&](int i) { return i == 0 ? m_settings->getWorldMaxX() : i == 1 ? m_settings->getWorldMaxY() : m_settings->getWorldMaxZ(); });
    addBBRow(3, "Delta", [&](int i) {
        return i == 0 ? m_settings->getWorldMaxX() - m_settings->getWorldMinX()
             : i == 1 ? m_settings->getWorldMaxY() - m_settings->getWorldMinY()
                      : m_settings->getWorldMaxZ() - m_settings->getWorldMinZ();
    });
    layout->addLayout(bbGrid);

    layout->addStretch();
    return page;
}

// ============================================================================
// Sidebar section switching
// ============================================================================
static const char* sectionNames[] = {
    "Lighting", "Slicing", "View & Display", "Colormap",
    "Vectors", "Streamlines", "Screenshot", "Mesh Info"
};

void MainWindow::setSidebarSection(int section) {
    if (m_activeSection == section && m_sidebarExpanded) {
        // Collapse
        m_activeSection = -1;
        m_sidebarExpanded = false;
        m_sectionStack->setVisible(false);
        m_panelHeader->setVisible(false);
        // Uncheck all icon buttons
        for (auto* btn : m_iconButtons) btn->setChecked(false);
    } else {
        // Expand
        m_activeSection = section;
        m_sidebarExpanded = true;
        m_sectionStack->setCurrentIndex(section);
        m_sectionStack->setVisible(true);
        m_panelHeader->setVisible(true);
        m_panelTitle->setText(QString::fromUtf8(sectionNames[section]));
        // Update icon strip check state
        for (int i = 0; i < m_iconButtons.size(); ++i) {
            m_iconButtons[i]->setChecked(i == section + 1);
        }
    }
    m_settings->setSidebarWidth(m_sidebarExpanded ? kIconStripWidth + kSidebarWidth : kIconStripWidth);
}

// ============================================================================
// Timers
// ============================================================================
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

    // Dash animation
    connect(&m_dashTimer, &QTimer::timeout, m_viewport, QOverload<>::of(&QWidget::update));
    m_dashTimer.setInterval(16);
    connect(m_settings, &RenderSettings::viewChanged, this, [this](ChangeFlags) {
        bool shouldRun = m_settings->getStreamlineDashEnabled() && m_settings->getShowStreamlines();
        if (shouldRun && !m_dashTimer.isActive()) m_dashTimer.start();
        else if (!shouldRun && m_dashTimer.isActive()) m_dashTimer.stop();
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

// ============================================================================
// Quick bar (floating display toggles, top-left of viewport)
// ============================================================================
void MainWindow::setupQuickBar() {
    m_quickBar = new QWidget(m_viewport);
    m_quickBar->setStyleSheet(
        "QWidget { background-color: #252526; border-radius: 6px; }"
    );
    m_quickBarLayout = new QHBoxLayout(m_quickBar);
    m_quickBarLayout->setContentsMargins(6, 4, 6, 4);
    m_quickBarLayout->setSpacing(4);

    auto addQBButton = [this](const QString& text, const QString& tooltip, bool active, bool checkable, std::function<void()> onClicked) -> QToolButton* {
        auto* btn = new QToolButton;
        btn->setText(text);
        btn->setToolTip(tooltip);
        btn->setFixedSize(30, 28);
        btn->setCheckable(checkable);
        btn->setChecked(active);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(
            "QToolButton {"
            "  font-size: 12px; border-radius: 4px;"
            "  background: transparent; color: #858585; border: none;"
            "}"
            "QToolButton:hover { background: #3c3c3c; color: #cccccc; }"
            "QToolButton:checked { background: #007acc; color: #ffffff; }"
        );
        connect(btn, &QToolButton::clicked, onClicked);
        m_quickBarLayout->addWidget(btn);
        return btn;
    };

    auto addSeparator = [this]() {
        auto* sep = new QFrame;
        sep->setFrameShape(QFrame::VLine);
        sep->setFixedSize(1, 22);
        sep->setStyleSheet("color: #3c3c3c;");
        m_quickBarLayout->addWidget(sep);
    };

    // Display toggles
    addQBButton("W", "Wireframe", m_settings->isWireframe(), true, [this]() {
        m_settings->setWireframe(!m_settings->isWireframe());
        updateQuickBarVisibility();
    });
    addQBButton("G", "Ground", m_settings->isGridVisible(), true, [this]() {
        m_settings->toggleGrid(!m_settings->isGridVisible());
        updateQuickBarVisibility();
    });
    addQBButton("S", "Surface", m_settings->isSurfaceVisible(), true, [this]() {
        m_settings->toggleSurface(!m_settings->isSurfaceVisible());
        updateQuickBarVisibility();
    });

    addSeparator();

    // Ortho snaps
    for (int i = 0; i < 6; ++i) {
        const char* labels[] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
        const char* tips[] = {"Ortho +X", "Ortho -X", "Ortho +Y", "Ortho -Y", "Ortho +Z", "Ortho -Z"};
        addQBButton(labels[i], tips[i], false, false, [this, i]() { m_settings->snapToOrthoView(i); });
    }

    addSeparator();

    // Reset camera
    addQBButton("\u21BB", "Reset Camera", false, false, [this]() { m_settings->resetCamera(); });
    // Collapse
    addQBButton("\u00D7", "Collapse quick-bar", false, false, [this]() {
        m_settings->setQuickBarCollapsed(true);
        updateQuickBarVisibility();
    });

    // Handle (shown when collapsed)
    m_quickBarHandle = new QToolButton(m_viewport);
    m_quickBarHandle->setText("\u{25A6}");
    m_quickBarHandle->setToolTip("Show display quick-bar");
    m_quickBarHandle->setFixedSize(30, 30);
    m_quickBarHandle->setCursor(Qt::PointingHandCursor);
    m_quickBarHandle->setStyleSheet(
        "QToolButton {"
        "  border-radius: 6px; font-size: 15px;"
        "  background-color: #252526; color: #858585; border: 1px solid #3c3c3c;"
        "}"
        "QToolButton:hover { background-color: #3c3c3c; color: #cccccc; }"
    );
    connect(m_quickBarHandle, &QToolButton::clicked, this, [this]() {
        m_settings->setQuickBarCollapsed(false);
        updateQuickBarVisibility();
    });

    updateQuickBarVisibility();
}

void MainWindow::updateQuickBarVisibility() {
    bool hasMesh = m_settings->getHasMeshLoaded();
    bool collapsed = m_settings->getQuickBarCollapsed();
    m_quickBar->setVisible(hasMesh && !collapsed);
    m_quickBarHandle->setVisible(hasMesh && collapsed);
}

// ============================================================================
// Keyboard shortcuts
// ============================================================================
void MainWindow::setupKeyboardShortcuts() {
    new QShortcut(QKeySequence("R"), this, m_settings, &RenderSettings::resetCamera);
    new QShortcut(QKeySequence("W"), this, [this]() { m_settings->setWireframe(!m_settings->isWireframe()); });
    new QShortcut(QKeySequence("G"), this, [this]() { m_settings->toggleGrid(!m_settings->isGridVisible()); });
    new QShortcut(QKeySequence("S"), this, this, &MainWindow::saveScreenshot);
    new QShortcut(QKeySequence("Left"), this, [this]() { m_settings->azimuth(-5); });
    new QShortcut(QKeySequence("Right"), this, [this]() { m_settings->azimuth(5); });
    new QShortcut(QKeySequence("Up"), this, [this]() { m_settings->elevation(5); });
    new QShortcut(QKeySequence("Down"), this, [this]() { m_settings->elevation(-5); });
    new QShortcut(QKeySequence("Ctrl+="), this, [this]() { m_settings->dolly(1.1); });
    new QShortcut(QKeySequence("Ctrl+-"), this, [this]() { m_settings->dolly(0.9); });
}

// ============================================================================
// Connect settings signals
// ============================================================================
void MainWindow::connectSettings() {
    connect(m_settings, &RenderSettings::meshLoadStateChanged, this, &MainWindow::updateStatusBar);
    connect(m_settings, &RenderSettings::meshLoadStateChanged, this, &MainWindow::updateQuickBarVisibility);

    // Repopulate field combos when mesh data changes
    connect(m_settings, &RenderSettings::meshDataUpdated, this, [this]() {
        auto scalars = m_settings->getAvailableScalars();
        auto vectors = m_settings->getAvailableVectors();
        if (m_scalarCombo) {
            m_scalarCombo->blockSignals(true);
            m_scalarCombo->clear();
            m_scalarCombo->addItems(scalars);
            m_scalarCombo->setCurrentText(m_settings->getActiveScalarNameQml());
            m_scalarCombo->setEnabled(m_settings->hasMeshScalars());
            m_scalarCombo->blockSignals(false);
        }
        if (m_vectorCombo) {
            m_vectorCombo->blockSignals(true);
            m_vectorCombo->clear();
            m_vectorCombo->addItems(vectors);
            m_vectorCombo->setCurrentText(m_settings->getVectorField());
            m_vectorCombo->blockSignals(false);
        }
        if (m_streamlineCombo) {
            m_streamlineCombo->blockSignals(true);
            m_streamlineCombo->clear();
            m_streamlineCombo->addItems(vectors);
            m_streamlineCombo->setCurrentText(m_settings->getStreamlineVectorField());
            m_streamlineCombo->blockSignals(false);
        }
    });
}

// ============================================================================
// Status bar
// ============================================================================
void MainWindow::updateStatusBar() {
    auto* sb = statusBar();
    if (m_settings->getHasMeshLoaded()) {
        sb->showMessage(QString("Mesh: %1   |   Type: %2   |   Points: %3   |   Triangles: %4")
            .arg(m_settings->getCurrentMeshNameQStr())
            .arg(m_settings->getMeshDataType())
            .arg(m_settings->getPointCount())
            .arg(m_settings->getTriangleCount()));
    } else {
        sb->showMessage("No mesh loaded | drag a .stl / .vtk / .obj file, or use File > Open Mesh");
    }
}

// ============================================================================
// File dialogs / actions
// ============================================================================
void MainWindow::openMesh() {
    QString path = QFileDialog::getOpenFileName(this, "Load Mesh", QString(),
        "Mesh files (*.stl *.vtk *.obj);;All files (*)");
    if (!path.isEmpty()) m_settings->loadMesh(path);
}

void MainWindow::openRecent(const QString& path) {
    m_settings->openRecent(path);
}

void MainWindow::saveScreenshot() {
    QString filename = m_settings->generateScreenshotFilename();
    QString path = QFileDialog::getSaveFileName(this, "Save Screenshot", filename,
        "PNG Images (*.png);;JPEG Images (*.jpg *.jpeg);;BMP Images (*.bmp);;All files (*)");
    if (!path.isEmpty()) m_settings->requestScreenshot(path);
}

void MainWindow::clearMeshes() {
    m_settings->clearMeshes();
    updateStatusBar();
}

// ============================================================================
// Drag and drop
// ============================================================================
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
    if (!m_viewport) return;

    // Reposition quick bar and handle within the viewport
    if (m_quickBar && m_viewport) {
        int margin = 8;
        m_quickBar->move(margin, margin);
        m_quickBarHandle->move(margin, margin);
    }
}

// ============================================================================
// Dialogs
// ============================================================================
void MainWindow::showAbout() {
    QMessageBox::about(this, "About SciRender",
        "<h2>SciRender</h2>"
        "<p>A Qt 6 + OpenGL scientific mesh renderer for VTK/STL/OBJ datasets.</p>"
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
        "<tr><td><b>G</b></td><td>Toggle grid</td></tr>"
        "<tr><td><b>S</b></td><td>Save screenshot</td></tr>"
        "<tr><td><b>Left/Right</b></td><td>Orbit (azimuth)</td></tr>"
        "<tr><td><b>Up/Down</b></td><td>Elevation</td></tr>"
        "<tr><td><b>Ctrl + =</b></td><td>Zoom in</td></tr>"
        "<tr><td><b>Ctrl + -</b></td><td>Zoom out</td></tr>"
        "</table>"
    );
    info.exec();
}
