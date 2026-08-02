#include "main_window.h"
#include "theme.h"
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
// Helper: Does a widget want typing/navigation keys (so viewport shortcuts
// must stay out of the way)?
// ============================================================================
static bool navFocusIsEditor(QWidget* w) {
    return w && (qobject_cast<QLineEdit*>(w) ||
                 qobject_cast<QComboBox*>(w) || qobject_cast<QSpinBox*>(w) ||
                 qobject_cast<QDoubleSpinBox*>(w));
}

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
    lbl->setFixedWidth(72);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    lbl->setStyleSheet(QString("font-size: 11px; color: %1;").arg(currentThemeColors().textPrimary.name()));
    lbl->setWordWrap(false);
    layout->addWidget(lbl);

    row.slider = new QSlider(Qt::Horizontal);
    row.slider->setMinimum(static_cast<int>(from * 1000));
    row.slider->setMaximum(static_cast<int>(to * 1000));
    row.slider->setSingleStep(static_cast<int>(step * 1000));
    row.slider->setValue(static_cast<int>(value * 1000));
    layout->addWidget(row.slider, 1, Qt::AlignVCenter);

    row.valueLabel = new QLabel(QString::number(value, 'f', decimals));
    row.valueLabel->setFixedWidth(36);
    row.valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row.valueLabel->setStyleSheet(QString("font-size: 10px; color: %1;").arg(currentThemeColors().textMuted.name()));
    layout->addWidget(row.valueLabel, 0, Qt::AlignVCenter);

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
    lbl->setFixedWidth(72);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    lbl->setStyleSheet(QString("font-size: 11px; color: %1;").arg(currentThemeColors().textPrimary.name()));
    lbl->setWordWrap(false);
    layout->addWidget(lbl);

    row.slider = new QSlider(Qt::Horizontal);
    row.slider->setMinimum(static_cast<int>(from * 1000));
    row.slider->setMaximum(static_cast<int>(to * 1000));
    row.slider->setValue(static_cast<int>(value * 1000));
    layout->addWidget(row.slider, 1, Qt::AlignVCenter);

    row.field = new QLineEdit(QString::number(value, 'f', 3));
    row.field->setFixedWidth(48);
    row.field->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row.field->setStyleSheet(QString("font-size: 11px; background: %1; color: %2; border: 1px solid %1; border-radius: 2px; padding: 1px 4px;")
        .arg(currentThemeColors().inputBg.name(), currentThemeColors().textPrimary.name()));
    auto* validator = new QDoubleValidator(from, to, 3);
    row.field->setValidator(validator);
    layout->addWidget(row.field, 0, Qt::AlignVCenter);

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
    lbl->setStyleSheet(QString("font-size: 11px; font-weight: bold; color: %1; padding-top: 2px;").arg(currentThemeColors().accent.name()));
    return lbl;
}

// ============================================================================
// Helper: Swatch button (color swatch + label)
// ============================================================================
static QPushButton* createSwatchButton(const QString& text, const QColor& color, std::function<void()> onClicked) {
    auto* btn = new QPushButton(text);
    btn->setFixedHeight(24);
    btn->setObjectName("swatchButton");
    QPixmap pix(14, 14);
    pix.fill(color);
    btn->setIcon(pix);
    btn->setIconSize(QSize(14, 14));
    // Only connect when a handler was provided. Callers (e.g. createColorButton)
    // attach their own clicked() slot separately and pass nullptr here; connecting
    // an empty std::function throws std::bad_function_call when the button is clicked.
    if (onClicked)
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
        QString(
        "QToolButton {"
        "  text-align: left; font-size: 12px; font-weight: bold;"
        "  color: %1; padding: 6px 8px; border: none;"
        "  background: %2; border-radius: 4px 4px 0 0;"
        "}"
        "QToolButton:hover { background: %3; }"
        ).arg(
            currentThemeColors().textPrimary.name(),
            currentThemeColors().surfaceBg.name(),
            currentThemeColors().hoverBg.name()
        )
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
        btn->setFixedSize(96, 24);
        btn->setIcon(generateColormapPreview(i, 92, 20));
        btn->setIconSize(QSize(92, 20));
        btn->setCheckable(true);
        btn->setChecked(i == currentChoice);
        btn->setStyleSheet(
            QString(
            "QPushButton { border: 2px solid transparent; border-radius: 2px; padding: 0px; }"
            "QPushButton:checked { border: 2px solid %1; }"
            "QPushButton:hover { border: 2px solid %2; }"
            ).arg(
                currentThemeColors().accent.name(),
                currentThemeColors().accentHover.name()
            )
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
    m_viewport = new ViewportWidget(m_settings->getMsaaSamples(), this);
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
    applyTheme(m_settings->getTheme());
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

    viewMenu->addSeparator();
    auto* themeMenu = viewMenu->addMenu("&Theme");
    m_themeGroup = new QActionGroup(this);
    m_themeGroup->setExclusive(true);

    auto* darkAction = m_themeGroup->addAction("Dark");
    darkAction->setCheckable(true);
    darkAction->setChecked(m_settings->getTheme() == AppTheme::Dark);
    themeMenu->addAction(darkAction);
    connect(darkAction, &QAction::triggered, this, [this]() { m_settings->setTheme(AppTheme::Dark); });

    auto* lightAction = m_themeGroup->addAction("Light");
    lightAction->setCheckable(true);
    lightAction->setChecked(m_settings->getTheme() == AppTheme::Light);
    themeMenu->addAction(lightAction);
    connect(lightAction, &QAction::triggered, this, [this]() { m_settings->setTheme(AppTheme::Light); });

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
    m_sidebarWidget->setStyleSheet(QString("background-color: %1;").arg(currentThemeColors().panelBg.name()));
    auto* mainLayout = new QHBoxLayout(m_sidebarWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // --- Icon strip (left edge, 48px) ---
    auto* iconStrip = new QWidget;
    m_iconStrip = iconStrip;
    iconStrip->setFixedWidth(kIconStripWidth);
    iconStrip->setStyleSheet(QString("background-color: %1;").arg(currentThemeColors().panelBg.name()));
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
            QString(
            "QToolButton {"
            "  font-size: 18px; border: none;"
            "  background: transparent; color: %1;"
            "  border-left: 3px solid transparent;"
            "}"
            "QToolButton:hover {"
            "  background: %2; color: %3;"
            "}"
            "QToolButton:checked {"
            "  background: %4; color: %5;"
            "  border-left: 3px solid %6;"
            "}"
            ).arg(
                currentThemeColors().textMuted.name(),
                currentThemeColors().surfaceBg.name(),
                currentThemeColors().textPrimary.name(),
                currentThemeColors().checkedBg.name(),
                currentThemeColors().textBright.name(),
                currentThemeColors().accent.name()
            )
        );

        iconLayout->addWidget(btn);
        m_iconButtons.append(btn);

        if (i == 0) {
            // Open button — no checkable, just opens file
            btn->setStyleSheet(
                QString(
                "QToolButton {"
                "  font-size: 18px; border: none;"
                "  background: transparent; color: %1;"
                "  border-left: 3px solid transparent;"
                "}"
                "QToolButton:hover {"
                "  background: %2; color: %3;"
                "}"
                ).arg(
                    currentThemeColors().textMuted.name(),
                    currentThemeColors().surfaceBg.name(),
                    currentThemeColors().textPrimary.name()
                )
            );
            connect(btn, &QToolButton::clicked, this, &MainWindow::openMesh);
        } else {
            int section = icons[i].section;
            connect(btn, &QToolButton::clicked, this, [this, section]() { setSidebarSection(section); });
        }
    }
    iconLayout->addStretch();

    // --- Section stack with panel header ---
    m_rightPanel = new QWidget;
    m_rightPanel->setStyleSheet(QString("background-color: %1;").arg(currentThemeColors().panelBg.name()));
    auto* rightLayout = new QVBoxLayout(m_rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    // Panel header (title bar for the expanded panel)
    m_panelHeader = new QWidget;
    m_panelHeader->setFixedHeight(32);
    m_panelHeader->setStyleSheet(QString("background-color: %1; border-bottom: 1px solid %2;").arg(currentThemeColors().panelBg.name(), currentThemeColors().border.name()));
    auto* headerLayout = new QHBoxLayout(m_panelHeader);
    headerLayout->setContentsMargins(10, 0, 4, 0);
    headerLayout->setSpacing(4);

    m_panelTitle = new QLabel;
    m_panelTitle->setStyleSheet(QString("font-size: 12px; font-weight: bold; color: %1; background: transparent;").arg(currentThemeColors().textPrimary.name()));
    m_panelTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    headerLayout->addWidget(m_panelTitle, 1);

    auto* closeBtn = new QToolButton;
    m_closeBtn = closeBtn;
    closeBtn->setText("\u00D7");
    closeBtn->setFixedSize(24, 24);
    closeBtn->setToolTip("Close panel");
    closeBtn->setStyleSheet(
        QString(
        "QToolButton { font-size: 16px; border: none; background: transparent; color: %1; border-radius: 4px; }"
        "QToolButton:hover { background: %2; color: %3; }"
        ).arg(
            currentThemeColors().textMuted.name(),
            currentThemeColors().border.name(),
            currentThemeColors().textBright.name()
        )
    );
    connect(closeBtn, &QToolButton::clicked, this, [this]() { setSidebarSection(m_activeSection); });
    headerLayout->addWidget(closeBtn);

    rightLayout->addWidget(m_panelHeader);
    m_panelHeader->setVisible(false);

    // Stacked widget (section pages)
    m_sectionStack = new QStackedWidget;
    m_sectionStack->setStyleSheet(QString("QStackedWidget { background: %1; }").arg(currentThemeColors().panelBg.name()));

    m_sectionStack->addWidget(buildLightingPage());     // 0
    m_sectionStack->addWidget(buildSlicingPage());      // 1
    m_sectionStack->addWidget(buildViewDisplayPage());  // 2
    m_sectionStack->addWidget(buildColormapPage());     // 3
    m_sectionStack->addWidget(buildVectorsPage());      // 4
    m_sectionStack->addWidget(buildStreamlinesPage());  // 5
    m_sectionStack->addWidget(buildScreenshotPage());   // 6
    m_meshInfoPage = buildMeshInfoPage();
    m_sectionStack->addWidget(m_meshInfoPage);           // 7

    rightLayout->addWidget(m_sectionStack, 1);
    m_sectionStack->setVisible(false);

    mainLayout->addWidget(iconStrip);
    mainLayout->addWidget(m_rightPanel, 1);

    m_sidebarDock->setWidget(m_sidebarWidget);
    addDockWidget(Qt::LeftDockWidgetArea, m_sidebarDock);
}

// ============================================================================
// Section: Lighting (0)
// ============================================================================
QWidget* MainWindow::buildLightingPage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(3);

    auto* resetBtn = new QPushButton("Reset");
    resetBtn->setObjectName("secondaryButton");
    connect(resetBtn, &QPushButton::clicked, m_settings, &RenderSettings::resetLighting);
    layout->addWidget(resetBtn);

    auto* markersCb = new QCheckBox("Light Markers");
    markersCb->setChecked(m_settings->getShowLightMarkers());
    connect(markersCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowLightMarkers);
    layout->addWidget(markersCb);

    auto* kitCb = new QCheckBox("Light Kit");
    kitCb->setChecked(m_settings->getLightKitEnabled());
    connect(kitCb, &QCheckBox::toggled, m_settings, &RenderSettings::setLightKitEnabled);
    layout->addWidget(kitCb);

    layout->addWidget(sectionHeader("Intensity & Tone"));
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
        QString(
        "QTabBar::tab {"
        "  padding: 4px 12px; background: %1; color: %2;"
        "  border: 1px solid %3; border-bottom: none;"
        "}"
        "QTabBar::tab:selected {"
        "  background: %4; color: %5; border-top: 2px solid %6;"
        "}"
        "QTabBar::tab:hover:!selected { background: %7; color: %8; }"
        "QTabWidget::pane { border: 1px solid %3; background: %4; }"
        ).arg(
            currentThemeColors().surfaceBg.name(),
            currentThemeColors().textMuted.name(),
            currentThemeColors().border.name(),
            currentThemeColors().windowBg.name(),
            currentThemeColors().textBright.name(),
            currentThemeColors().accent.name(),
            currentThemeColors().hoverBg.name(),
            currentThemeColors().textPrimary.name()
        )
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
    {
        auto row = createLightSlider("Roughness", m_settings->getMatRoughness(), 0, 1, 0.01, 2, [this](double v) { m_settings->setMatRoughness(v); });
        layout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Metallic", m_settings->getMatMetallic(), 0, 1, 0.01, 2, [this](double v) { m_settings->setMatMetallic(v); });
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
    layout->setSpacing(3);

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

    auto* cutLabel = new QLabel("Cut planes (world units)");
    cutLabel->setStyleSheet(QString("font-size: 11px; color: %1;").arg(currentThemeColors().textMuted.name()));
    optLayout->addWidget(cutLabel);

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
    keepLabel->setStyleSheet(QString("font-size: 11px; color: %1;").arg(currentThemeColors().textMuted.name()));
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

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");
    scroll->setWidget(page);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}

// ============================================================================
// Section: View & Display (2)
// ============================================================================


QWidget* MainWindow::buildViewDisplayPage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6); // Section breathing room

    // ==========================================
    // 1. CAMERA SECTION
    // ==========================================
    layout->addWidget(sectionHeader("Camera"));

    // Compact Checkbox Row (Parallel View + Auto-Rotate)
    auto* camOptionsLayout = new QHBoxLayout;
    auto* orthoCb = new QCheckBox("Parallel");
    orthoCb->setChecked(m_settings->getOrthographic());
    connect(orthoCb, &QCheckBox::toggled, m_settings, &RenderSettings::setOrthographic);
    camOptionsLayout->addWidget(orthoCb);

    auto* rotateCb = new QCheckBox("Auto-Rotate");
    rotateCb->setChecked(m_settings->getAutoRotate());
    connect(rotateCb, &QCheckBox::toggled, m_settings, &RenderSettings::setAutoRotate);
    camOptionsLayout->addWidget(rotateCb);
    layout->addLayout(camOptionsLayout);

    // Roll slider with integrated reset
    {
        auto row = createLightSlider("Roll", m_settings->getRoll(), -180, 180, 1, 0, [this](double v) {
            double delta = v - m_settings->getRoll();
            if (delta != 0.0) m_settings->roll(delta);
        });
        auto* resetBtn = new QPushButton("0");
        resetBtn->setObjectName("secondaryButton");
        resetBtn->setFixedWidth(24);
        resetBtn->setFixedHeight(20);
        connect(resetBtn, &QPushButton::clicked, m_settings, &RenderSettings::resetCamera);
        auto* rowWidget = row.slider->parentWidget();
        if (auto* hlay = qobject_cast<QHBoxLayout*>(rowWidget->layout())) {
            hlay->addWidget(resetBtn);
        }
        layout->addWidget(rowWidget);
    }

    auto* resetCamBtn = new QPushButton("Reset All Camera Settings");
    resetCamBtn->setObjectName("secondaryButton");
    resetCamBtn->setFixedHeight(24);
    connect(resetCamBtn, &QPushButton::clicked, m_settings, &RenderSettings::resetCamera);
    layout->addWidget(resetCamBtn);

    // ==========================================
    // 2. DISPLAY & RENDERING SECTION
    // ==========================================
    layout->addWidget(sectionHeader("Display Modes"));

    auto createModeRow = [this](const QString& text, bool checked, bool enabled,
                                double val, double min, double max,
                                auto toggleSlot, auto sliderSlot) {
        auto* rowLayout = new QHBoxLayout;
        rowLayout->setContentsMargins(0, 0, 0, 0);

        auto* cb = new QCheckBox(text);
        cb->setChecked(checked);
        cb->setEnabled(enabled);
        connect(cb, &QCheckBox::toggled, m_settings, toggleSlot);
        rowLayout->addWidget(cb);

        auto* slider = new QSlider(Qt::Horizontal);
        slider->setRange(static_cast<int>(min * 10), static_cast<int>(max * 10));
        slider->setValue(static_cast<int>(val * 10));
        slider->setEnabled(checked && enabled);
        connect(slider, &QSlider::valueChanged, m_settings, sliderSlot);
        connect(cb, &QCheckBox::toggled, slider, &QWidget::setEnabled);
        rowLayout->addWidget(slider);

        return rowLayout;
    };

    // Wireframe
    layout->addLayout(createModeRow("Wireframe", m_settings->isWireframe(), true,
                                    m_settings->getLineWidth(), 1.0, 10.0,
                                    &RenderSettings::setWireframe, [this](int v) { m_settings->setLineWidth(v / 10.0); }));

    // Cell Edge
    layout->addLayout(createModeRow("Cell Edge", m_settings->getShowCellEdges(), m_settings->getSupportsCellGrid(),
                                    m_settings->getCellEdgeLineWidth(), 1.0, 10.0,
                                    &RenderSettings::setShowCellEdges, [this](int v) { m_settings->setCellEdgeLineWidth(v / 10.0); }));

    // Simple checkboxes grid or vertical flow for standard toggles
    auto* flagsLayout = new QGridLayout;
    int flagIdx = 0;

    auto addFlagCb = [&](const QString& text, bool checked, auto slot) {
        auto* cb = new QCheckBox(text);
        cb->setChecked(checked);
        connect(cb, &QCheckBox::toggled, m_settings, slot);
        flagsLayout->addWidget(cb, flagIdx / 2, flagIdx % 2);
        flagIdx++;
        return cb;
    };

    auto* surfaceCb = addFlagCb("Surface", m_settings->isSurfaceVisible(), &RenderSettings::toggleSurface);
    auto* pointsCb  = addFlagCb("Points", m_settings->getShowPoints(), &RenderSettings::setShowPoints);
    addFlagCb("Reference Grid", m_settings->isGridVisible(), &RenderSettings::toggleGrid);
    addFlagCb("Bounding Box", m_settings->getShowBounds(), &RenderSettings::setShowBounds);
    addFlagCb("Defects", m_settings->getShowQualityOverlay(), &RenderSettings::setShowQualityOverlay);

    layout->addLayout(flagsLayout);

    // Point controls (conditional)
    auto* pointGroup = new QWidget;
    auto* pointLayout = new QVBoxLayout(pointGroup);
    pointLayout->setContentsMargins(12, 2, 0, 2); // Indented for hierarchy
    {
        auto row = createLightSlider("Size", m_settings->getPointSize(), 1, 20, 0.5, 1, [this](double v) { m_settings->setPointSize(v); });
        pointLayout->addWidget(row.slider->parentWidget());
    }
    auto* scalarCb = new QCheckBox("Color by scalar");
    scalarCb->setChecked(m_settings->getPointUseScalar());
    connect(scalarCb, &QCheckBox::toggled, m_settings, &RenderSettings::setPointUseScalar);
    pointLayout->addWidget(scalarCb);
    layout->addWidget(pointGroup);

    connect(pointsCb, &QCheckBox::toggled, pointGroup, &QWidget::setVisible);
    pointGroup->setVisible(m_settings->getShowPoints());

    // ==========================================
    // 3. TRANSPARENCY & APPEARANCE
    // ==========================================
    layout->addWidget(sectionHeader("Transparency & Quality"));
    {
        auto row = createLightSlider("Surface Opacity", m_settings->getSurfaceOpacity(), 0.1, 1, 0.05, 2, [this](double v) { m_settings->setSurfaceOpacity(v); });
        layout->addWidget(row.slider->parentWidget());
    }
    {
        auto row = createLightSlider("Point Opacity", m_settings->getPointOpacity(), 0.1, 1, 0.05, 2, [this](double v) { m_settings->setPointOpacity(v); });
        layout->addWidget(row.slider->parentWidget());
    }

    // HUD / Gizmo + FPS compact row
    auto* subRowLayout = new QHBoxLayout;
    auto* gizmoCb = new QCheckBox("Gizmo");
    gizmoCb->setChecked(m_settings->isGizmoVisible());
    connect(gizmoCb, &QCheckBox::toggled, m_settings, &RenderSettings::setGizmoVisible);
    subRowLayout->addWidget(gizmoCb);

    auto* fpsCb = new QCheckBox("FPS HUD");
    fpsCb->setChecked(m_settings->getShowFps());
    connect(fpsCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowFps);
    subRowLayout->addWidget(fpsCb);

    layout->addLayout(subRowLayout);

    // ==========================================
    // 4. COLORS SECTION (Swatch Grid)
    // ==========================================
    layout->addWidget(sectionHeader("Colors"));
    auto* colorLayout = new QHBoxLayout;

    auto createColorButton = [this](const QString& name, QColor initialColor, QColorDialog** dialogPtr, auto setterMember) {
        auto* btn = createSwatchButton(name, initialColor, nullptr);
        connect(btn, &QPushButton::clicked, this, [this, btn, name, initialColor, dialogPtr, setterMember]() {
            if (!*dialogPtr) {
                *dialogPtr = new QColorDialog(initialColor, this);
                (*dialogPtr)->setOption(QColorDialog::ShowAlphaChannel, false);
                connect(*dialogPtr, &QColorDialog::colorSelected, m_settings, setterMember);
                connect(*dialogPtr, &QColorDialog::colorSelected, btn, [btn](const QColor& c) {
                    QPixmap pix(14, 14); pix.fill(c); btn->setIcon(pix);
                });
            }
            (*dialogPtr)->open();
        });
        return btn;
    };

    colorLayout->addWidget(createColorButton("Wireframe", m_settings->getMeshColorQml(), &m_meshColorDialog, &RenderSettings::setMeshColorQml));
    colorLayout->addWidget(createColorButton("Surface", m_settings->getSurfaceColorQml(), &m_surfaceColorDialog, &RenderSettings::setSurfaceColorQml));
    colorLayout->addWidget(createColorButton("Background", m_settings->getBgColorQml(), &m_bgColorDialog, &RenderSettings::setBgColorQml));

    layout->addLayout(colorLayout);

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
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(3);

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
    ticksLabel->setFixedWidth(72);
    ticksLabel->setStyleSheet(QString("font-size: 11px; color: %1;").arg(currentThemeColors().textPrimary.name()));
    ticksLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ticksRow->addWidget(ticksLabel);
    auto* ticksSpin = new QSpinBox;
    ticksSpin->setRange(2, 20);
    ticksSpin->setValue(m_settings->getColorbarTicks());
    connect(ticksSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setColorbarTicks);
    ticksRow->addWidget(ticksSpin, 1);
    layout->addLayout(ticksRow);

    layout->addWidget(sectionHeader("Filter"));
    auto* resetFilterBtn = new QPushButton("Reset");
    resetFilterBtn->setObjectName("secondaryButton");
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
    layout->setSpacing(3);

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
    {
        auto* btn = createSwatchButton("Vector", m_settings->getVectorColorQml(), nullptr);
        connect(btn, &QPushButton::clicked, this, [this, btn]() {
            if (!m_vectorColorDialog) {
                m_vectorColorDialog = new QColorDialog(m_settings->getVectorColorQml(), this);
                connect(m_vectorColorDialog, &QColorDialog::colorSelected, m_settings, &RenderSettings::setVectorColorQml);
                connect(m_vectorColorDialog, &QColorDialog::colorSelected, btn, [btn](const QColor& c) {
                    QPixmap pix(14, 14); pix.fill(c); btn->setIcon(pix);
                });
            }
            m_vectorColorDialog->open();
        });
        optLayout->addWidget(btn);
    }

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

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");
    scroll->setWidget(page);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}

// ============================================================================
// Section: Streamlines (5)
// ============================================================================
QWidget* MainWindow::buildStreamlinesPage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(3);

    auto* showCb = new QCheckBox("Show streamlines");
    showCb->setChecked(m_settings->getShowStreamlines());
    connect(showCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowStreamlines);
    layout->addWidget(showCb);

    auto* optionsGroup = new QWidget;
    auto* optLayout = new QVBoxLayout(optionsGroup);
    optLayout->setContentsMargins(0, 0, 0, 0);
    optLayout->setSpacing(6);

    // Field
    auto* fieldGroup = new QWidget;
    fieldGroup->setStyleSheet(QString("background-color: %1; border-radius: 0 0 4px 4px;").arg(currentThemeColors().surfaceBg.name()));
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
    seedCountLabel->setFixedWidth(72);
    seedCountLabel->setStyleSheet(QString("font-size: 11px; color: %1;").arg(currentThemeColors().textPrimary.name()));
    seedCountLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
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
    maxStepsLabel->setFixedWidth(72);
    maxStepsLabel->setStyleSheet(QString("font-size: 11px; color: %1;").arg(currentThemeColors().textPrimary.name()));
    maxStepsLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    maxStepsRow->addWidget(maxStepsLabel);
    auto* maxStepsSpin = new QSpinBox;
    maxStepsSpin->setRange(10, 500);
    maxStepsSpin->setValue(m_settings->getStreamlineMaxSteps());
    connect(maxStepsSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setStreamlineMaxSteps);
    maxStepsRow->addWidget(maxStepsSpin, 1);
    fieldLayout->addLayout(maxStepsRow);

    auto* integrateBtn = new QPushButton("Integrate");
    integrateBtn->setMinimumHeight(24);
    connect(integrateBtn, &QPushButton::clicked, this, [this]() {
        m_settings->backend()->markStreamlineDirty();
        m_viewport->update();
    });
    fieldLayout->addWidget(integrateBtn);

    auto* fieldHeader = createCollapsibleHeader("Field", true, [fieldGroup](bool expanded) {
        fieldGroup->setVisible(expanded);
    });
    optLayout->addWidget(fieldHeader);
    optLayout->addWidget(fieldGroup);

    // Color
    auto* colorGroup = new QWidget;
    colorGroup->setStyleSheet(QString("background-color: %1; border-radius: 0 0 4px 4px;").arg(currentThemeColors().surfaceBg.name()));
    auto* colorLayout = new QVBoxLayout(colorGroup);
    colorLayout->setContentsMargins(12, 4, 8, 8);
    {
        auto* btn = createSwatchButton("Streamline", m_settings->getStreamlineColorQml(), nullptr);
        connect(btn, &QPushButton::clicked, this, [this, btn]() {
            if (!m_streamlineColorDialog) {
                m_streamlineColorDialog = new QColorDialog(m_settings->getStreamlineColorQml(), this);
                connect(m_streamlineColorDialog, &QColorDialog::colorSelected, m_settings, &RenderSettings::setStreamlineColorQml);
                connect(m_streamlineColorDialog, &QColorDialog::colorSelected, btn, [btn](const QColor& c) {
                    QPixmap pix(14, 14); pix.fill(c); btn->setIcon(pix);
                });
            }
            m_streamlineColorDialog->open();
        });
        colorLayout->addWidget(btn);
    }
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
    seedGroup->setStyleSheet(QString("background-color: %1; border-radius: 0 0 4px 4px;").arg(currentThemeColors().surfaceBg.name()));
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
    planePosLabel->setFixedWidth(72);
    planePosLabel->setStyleSheet(QString("font-size: 11px; color: %1;").arg(currentThemeColors().textPrimary.name()));
    planePosLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    planePosRow->addWidget(planePosLabel);    auto* planePosSlider = new QSlider(Qt::Horizontal);
    planePosSlider->setRange(0, 1000);
    planePosSlider->setValue(static_cast<int>(m_settings->getSeedPlanePos() * 1000));
    connect(planePosSlider, &QSlider::valueChanged, m_settings, [this](int v) { m_settings->setSeedPlanePos(v / 1000.0); });
    planePosRow->addWidget(planePosSlider, 1);
    seedLayout->addLayout(planePosRow);

    auto* seedsURow = new QHBoxLayout;
    auto* seedsULabel = new QLabel("Seeds U");
    seedsULabel->setFixedWidth(72);
    seedsULabel->setStyleSheet(QString("font-size: 11px; color: %1;").arg(currentThemeColors().textPrimary.name()));
    seedsULabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    seedsURow->addWidget(seedsULabel);
    auto* seedsUSpin = new QSpinBox;
    seedsUSpin->setRange(1, 200);
    seedsUSpin->setValue(m_settings->getSeedPlaneCountU());
    connect(seedsUSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setSeedPlaneCountU);
    seedsURow->addWidget(seedsUSpin, 1);
    seedLayout->addLayout(seedsURow);

    auto* seedsVRow = new QHBoxLayout;
    auto* seedsVLabel = new QLabel("Seeds V");
    seedsVLabel->setFixedWidth(72);
    seedsVLabel->setStyleSheet(QString("font-size: 11px; color: %1;").arg(currentThemeColors().textPrimary.name()));
    seedsVLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
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

    {
        auto* btn = createSwatchButton("Seed color", m_settings->getSeedPointColorQml(), nullptr);
        connect(btn, &QPushButton::clicked, this, [this, btn]() {
            if (!m_seedColorDialog) {
                m_seedColorDialog = new QColorDialog(m_settings->getSeedPointColorQml(), this);
                connect(m_seedColorDialog, &QColorDialog::colorSelected, m_settings, &RenderSettings::setSeedPointColorQml);
                connect(m_seedColorDialog, &QColorDialog::colorSelected, btn, [btn](const QColor& c) {
                    QPixmap pix(14, 14); pix.fill(c); btn->setIcon(pix);
                });
            }
            m_seedColorDialog->open();
        });
        seedLayout->addWidget(btn);
    }

    auto* seedHeader = createCollapsibleHeader("Seeding", true, [seedGroup](bool expanded) {
        seedGroup->setVisible(expanded);
    });
    optLayout->addWidget(seedHeader);
    optLayout->addWidget(seedGroup);

    // Appearance
    auto* appGroup = new QWidget;
    appGroup->setStyleSheet(QString("background-color: %1; border-radius: 0 0 4px 4px;").arg(currentThemeColors().surfaceBg.name()));
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
    lightGroup->setStyleSheet(QString("background-color: %1; border-radius: 0 0 4px 4px;").arg(currentThemeColors().surfaceBg.name()));
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
    layout->setSpacing(3);

    auto* saveBtn = new QPushButton("Save Screenshot");
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::saveScreenshot);
    layout->addWidget(saveBtn);

    auto* transCb = new QCheckBox("Transparent (PNG)");
    transCb->setChecked(m_settings->getScreenshotTransparent());
    connect(transCb, &QCheckBox::toggled, m_settings, &RenderSettings::setScreenshotTransparent);
    layout->addWidget(transCb);

    layout->addStretch();

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");
    scroll->setWidget(page);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}

// ============================================================================
// Section: Mesh Info (7)
// ============================================================================
QWidget* MainWindow::buildMeshInfoPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(3);
    m_meshInfoLabels.clear();

    auto addInfoRow = [this, layout](const QString& label, const QString& value, const QString& color = currentThemeColors().textPrimary.name()) {
        auto* row = new QHBoxLayout;
        auto* lbl = new QLabel(label);
        lbl->setFixedWidth(72);
        lbl->setStyleSheet(QString("font-size: 11px; color: %1;").arg(currentThemeColors().textMuted.name()));
        auto* val = new QLabel(value);
        val->setObjectName("mvp:" + label);
        val->setStyleSheet(QString("font-size: 11px; color: %1;").arg(color));
        row->addWidget(lbl);
        row->addWidget(val);
        layout->addLayout(row);
        m_meshInfoLabels[label] = val;
    };

    layout->addWidget(sectionHeader("Source"));
    addInfoRow("Type", m_settings->getMeshDataType());
    addInfoRow("Format", m_settings->getMeshFormat());

    layout->addWidget(sectionHeader("Geometry"));
    addInfoRow("Triangles", QString::number(m_settings->getTriangleCount()));
    addInfoRow("Points", QString::number(m_settings->getPointCount()));

    layout->addWidget(sectionHeader("Quality"));
    addInfoRow("Degenerate", QString::number(m_settings->getDegenerateFaces()), "#ff6666");
    addInfoRow("Open edges", QString::number(m_settings->getOpenEdges()), "#ffaa44");
    addInfoRow("Non-manifold E", QString::number(m_settings->getNonManifoldEdges()), "#ff44ff");
    addInfoRow("Non-manifold V", QString::number(m_settings->getNonManifoldVerts()), "#ff44ff");
    addInfoRow("Watertight", m_settings->getWatertight() ? "yes" : "no", m_settings->getWatertight() ? "#66dd66" : "#ff6666");

    layout->addWidget(sectionHeader("Bounding box"));
    auto* bbGrid = new QGridLayout;
    bbGrid->setHorizontalSpacing(4);
    const char* xyz[] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
        auto* h = new QLabel(QString::fromUtf8(xyz[i]));
        h->setStyleSheet(QString("font-size: 10px; color: %1;").arg(currentThemeColors().textMuted.name()));
        bbGrid->addWidget(h, 0, i + 1);
    }
    auto addBBRow = [this, &bbGrid](int row, const QString& lbl, auto getter) {
        auto* l = new QLabel(lbl);
        l->setStyleSheet(QString("font-size: 11px; color: %1;").arg(currentThemeColors().textMuted.name()));
        bbGrid->addWidget(l, row, 0);
        for (int i = 0; i < 3; ++i) {
            auto* v = new QLabel(QString::number(getter(i), 'f', 3));
            v->setObjectName(QString("mvp:BB:%1:%2").arg(lbl).arg(i));
            v->setStyleSheet(QString("font-size: 11px; color: %1;").arg(currentThemeColors().textPrimary.name()));
            bbGrid->addWidget(v, row, i + 1);
            m_meshInfoLabels[QString("BB:%1:%2").arg(lbl).arg(i)] = v;
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

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");
    scroll->setWidget(page);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}

void MainWindow::refreshMeshInfoPage() {
    if (!m_meshInfoPage) return;

    // Update existing value labels in place instead of tearing the page down
    // (preserves scroll position and avoids re-layout churn on every mesh change).
    auto setInfo = [&](const QString& label, const QString& value, const QString& color) {
        auto it = m_meshInfoLabels.find(label);
        if (it == m_meshInfoLabels.end()) return;
        it.value()->setText(value);
        it.value()->setStyleSheet(QString("font-size: 11px; color: %1;").arg(color));
    };
    const auto& tc = currentThemeColors();
    const QString ok   = tc.textPrimary.name();
    const QString bad  = "#ff6666";
    setInfo("Type",       m_settings->getMeshDataType(),  ok);
    setInfo("Format",     m_settings->getMeshFormat(),    ok);
    setInfo("Triangles",  QString::number(m_settings->getTriangleCount()), ok);
    setInfo("Points",     QString::number(m_settings->getPointCount()),    ok);
    setInfo("Degenerate", QString::number(m_settings->getDegenerateFaces()), "#ff6666");
    setInfo("Open edges", QString::number(m_settings->getOpenEdges()),       "#ffaa44");
    setInfo("Non-manifold E", QString::number(m_settings->getNonManifoldEdges()), "#ff44ff");
    setInfo("Non-manifold V", QString::number(m_settings->getNonManifoldVerts()), "#ff44ff");
    setInfo("Watertight", m_settings->getWatertight() ? "yes" : "no",
            m_settings->getWatertight() ? "#66dd66" : bad);

    auto setBB = [&](const QString& row, int axis, double v) {
        auto it = m_meshInfoLabels.find(QString("BB:%1:%2").arg(row).arg(axis));
        if (it != m_meshInfoLabels.end()) it.value()->setText(QString::number(v, 'f', 3));
    };
    const double min[3] = { m_settings->getWorldMinX(), m_settings->getWorldMinY(), m_settings->getWorldMinZ() };
    const double max[3] = { m_settings->getWorldMaxX(), m_settings->getWorldMaxY(), m_settings->getWorldMaxZ() };
    for (int i = 0; i < 3; ++i) {
        setBB("Min",   i, min[i]);
        setBB("Max",   i, max[i]);
        setBB("Delta", i, max[i] - min[i]);
    }
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
        QString("QWidget { background-color: %1; border-radius: 6px; }").arg(currentThemeColors().panelBg.name())
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
            QString(
            "QToolButton {"
            "  font-size: 12px; border-radius: 4px;"
            "  background: transparent; color: %1; border: none;"
            "}"
            "QToolButton:hover { background: %2; color: %3; }"
            "QToolButton:checked { background: %4; color: %5; }"
             ).arg(
                 currentThemeColors().textPrimary.name(),
                 currentThemeColors().border.name(),
                 currentThemeColors().textPrimary.name(),
                 currentThemeColors().accent.name(),
                 currentThemeColors().textOnAccent.name()
             )
        );
        connect(btn, &QToolButton::clicked, onClicked);
        m_quickBarLayout->addWidget(btn);
        return btn;
    };

    auto addSeparator = [this]() {
        auto* sep = new QFrame;
        sep->setFrameShape(QFrame::VLine);
        sep->setFixedSize(1, 22);
        sep->setStyleSheet(QString("color: %1;").arg(currentThemeColors().border.name()));
        m_quickBarLayout->addWidget(sep);
    };

    // Display toggles
    m_qbWireframe = addQBButton("W", "Wireframe", m_settings->isWireframe(), true, [this]() {
        m_settings->setWireframe(!m_settings->isWireframe());
    });
    m_qbGrid = addQBButton("G", "Ground", m_settings->isGridVisible(), true, [this]() {
        m_settings->toggleGrid(!m_settings->isGridVisible());
    });
    m_qbSurface = addQBButton("S", "Surface", m_settings->isSurfaceVisible(), true, [this]() {
        m_settings->toggleSurface(!m_settings->isSurfaceVisible());
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
        QString(
        "QToolButton {"
        "  border-radius: 6px; font-size: 15px;"
        "  background-color: %1; color: %2; border: 1px solid %3;"
        "}"
        "QToolButton:hover { background-color: %3; color: %4; }"
        ).arg(
            currentThemeColors().panelBg.name(),
            currentThemeColors().textMuted.name(),
            currentThemeColors().border.name(),
            currentThemeColors().textPrimary.name()
        )
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

// Keep the quick-bar display toggles in sync with settings that are changed via
// keyboard shortcuts / View & Display checkboxes, not the quick-bar buttons themselves.
void MainWindow::syncQuickBar() {
    if (m_qbWireframe) m_qbWireframe->setChecked(m_settings->isWireframe());
    if (m_qbGrid)      m_qbGrid->setChecked(m_settings->isGridVisible());
    if (m_qbSurface)   m_qbSurface->setChecked(m_settings->isSurfaceVisible());
}

// ============================================================================
// Keyboard shortcuts
// ============================================================================
void MainWindow::setupKeyboardShortcuts() {
    auto addNav = [this](const QKeySequence& ks, std::function<void()> fn) {
        auto* sc = new QShortcut(ks, this);
        sc->setContext(Qt::WindowShortcut);
        m_navShortcuts.append(sc);
        connect(sc, &QShortcut::activated, this, fn);
    };
    addNav(QKeySequence("R"),            [this]() { m_settings->resetCamera(); });
    addNav(QKeySequence("W"),            [this]() { m_settings->setWireframe(!m_settings->isWireframe()); });
    addNav(QKeySequence("G"),            [this]() { m_settings->toggleGrid(!m_settings->isGridVisible()); });
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

// ============================================================================
// Connect settings signals
// ============================================================================
void MainWindow::connectSettings() {
    connect(m_settings, &RenderSettings::meshLoadStateChanged, this, &MainWindow::updateStatusBar);
    connect(m_settings, &RenderSettings::meshLoadStateChanged, this, &MainWindow::updateQuickBarVisibility);
    connect(m_settings, &RenderSettings::meshDataUpdated, this, &MainWindow::refreshMeshInfoPage);

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

    // Theme changes
    connect(m_settings, &RenderSettings::themeChanged, this, [this]() {
        applyTheme(m_settings->getTheme());
    });

    // Mirror quick-bar display toggles whenever the corresponding settings change
    // (keyboard shortcuts, View & Display checkboxes). Sync runs on every view change,
    // but the setChecked calls only emit when the value actually differs.
    connect(m_settings, &RenderSettings::viewChanged, this, [this](ChangeFlags) {
        syncQuickBar();
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

void MainWindow::recreateViewport() {
    auto* old = m_viewport;
    m_viewport = new ViewportWidget(m_settings->getMsaaSamples(), this);
    m_viewport->setSettings(m_settings);
    setCentralWidget(m_viewport);

    // Reconnect timers
    connect(&m_fpsTimer, &QTimer::timeout, m_viewport, QOverload<>::of(&QWidget::update));
    connect(&m_dashTimer, &QTimer::timeout, m_viewport, QOverload<>::of(&QWidget::update));
    connect(&m_particleTimer, &QTimer::timeout, m_viewport, QOverload<>::of(&QWidget::update));

    // Re-parent quick bar widgets
    if (m_quickBar) { m_quickBar->setParent(m_viewport); m_quickBar->show(); }
    if (m_quickBarHandle) { m_quickBarHandle->setParent(m_viewport); m_quickBarHandle->show(); }

    old->deleteLater();
    m_viewport->update();
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

// ============================================================================
// Theme
// ============================================================================
ThemeColors MainWindow::currentColors() const {
    return getThemeColors(m_settings ? m_settings->getTheme() : AppTheme::Dark);
}

void MainWindow::applyTheme(AppTheme theme) {
    setThemeColors(theme);

    auto pal = buildPalette(currentThemeColors());
    qApp->setPalette(pal);
    qApp->setStyleSheet(buildGlobalStylesheet(currentThemeColors()));

    rebuildSidebarStyles();
    rebuildQuickBarStyles();

    if (m_themeGroup) {
        auto actions = m_themeGroup->actions();
        for (auto* a : actions) {
            a->setChecked(a->text() == (theme == AppTheme::Dark ? "Dark" : "Light"));
        }
    }
}

void MainWindow::rebuildSidebarStyles() {
    auto c = currentColors();
    m_sidebarWidget->setStyleSheet(QString("background-color: %1;").arg(c.panelBg.name()));
    if (m_rightPanel)
        m_rightPanel->setStyleSheet(QString("background-color: %1;").arg(c.panelBg.name()));
    m_panelHeader->setStyleSheet(QString("background-color: %1; border-bottom: 1px solid %2;").arg(c.panelBg.name(), c.border.name()));
    m_panelTitle->setStyleSheet(QString("font-size: 12px; font-weight: bold; color: %1; background: transparent;").arg(c.textPrimary.name()));
    m_sectionStack->setStyleSheet(QString("QStackedWidget { background: %1; }").arg(c.panelBg.name()));

    // Rebuild icon strip background
    if (m_iconStrip) {
        m_iconStrip->setStyleSheet(QString("background-color: %1;").arg(c.panelBg.name()));
    }

    // Rebuild icon button styles
    for (int i = 0; i < m_iconButtons.size(); ++i) {
        auto* btn = m_iconButtons[i];
        if (i == 0) {
            btn->setStyleSheet(
                QString(
                "QToolButton {"
                "  font-size: 18px; border: none;"
                "  background: transparent; color: %1;"
                "  border-left: 3px solid transparent;"
                "}"
                "QToolButton:hover {"
                "  background: %2; color: %3;"
                "}"
                ).arg(c.textMuted.name(), c.surfaceBg.name(), c.textPrimary.name())
            );
        } else {
            btn->setStyleSheet(
                QString(
                "QToolButton {"
                "  font-size: 18px; border: none;"
                "  background: transparent; color: %1;"
                "  border-left: 3px solid transparent;"
                "}"
                "QToolButton:hover {"
                "  background: %2; color: %3;"
                "}"
                "QToolButton:checked {"
                "  background: %4; color: %5;"
                "  border-left: 3px solid %6;"
                "}"
                ).arg(
                    c.textMuted.name(),
                    c.surfaceBg.name(),
                    c.textPrimary.name(),
                    c.checkedBg.name(),
                    c.textBright.name(),
                    c.accent.name()
                )
            );
        }
    }

    // Rebuild close button style
    if (m_closeBtn) {
        m_closeBtn->setStyleSheet(
            QString(
            "QToolButton { font-size: 16px; border: none; background: transparent; color: %1; border-radius: 4px; }"
            "QToolButton:hover { background: %2; color: %3; }"
            ).arg(c.textMuted.name(), c.border.name(), c.textBright.name())
        );
    }

    // Rebuild all section pages with fresh theme colors
    int active = m_activeSection;
    m_activeSection = -1;

    // Remove all pages from stack (they will be deleted by Qt parent-child)
    while (m_sectionStack->count() > 0) {
        auto* w = m_sectionStack->widget(0);
        m_sectionStack->removeWidget(w);
        w->deleteLater();
    }

    // Rebuild all pages
    m_sectionStack->addWidget(buildLightingPage());     // 0
    m_sectionStack->addWidget(buildSlicingPage());      // 1
    m_sectionStack->addWidget(buildViewDisplayPage());  // 2
    m_sectionStack->addWidget(buildColormapPage());     // 3
    m_sectionStack->addWidget(buildVectorsPage());      // 4
    m_sectionStack->addWidget(buildStreamlinesPage());  // 5
    m_sectionStack->addWidget(buildScreenshotPage());   // 6
    m_meshInfoPage = buildMeshInfoPage();
    m_sectionStack->addWidget(m_meshInfoPage);  

    // Give each page a solid theme background so labels (whose colors come from
    // the palette / stylesheet) stay readable in both themes. Prevents the
    // transparent-scroll-area path from showing the parent's stale background.
    for (int i = 0; i < m_sectionStack->count(); ++i) {
        if (auto* pg = m_sectionStack->widget(i))
            pg->setStyleSheet(QString("background: %1;").arg(c.panelBg.name()));
    }

    // Restore active section
    if (active >= 0) setSidebarSection(active);
}

void MainWindow::rebuildQuickBarStyles() {
    auto c = currentColors();
    if (m_quickBar) {
        // Clear individual button stylesheets so parent cascade works
        for (int i = 0; i < m_quickBarLayout->count(); ++i) {
            if (auto* item = m_quickBarLayout->itemAt(i)) {
                if (auto* w = item->widget()) {
                    if (qobject_cast<QToolButton*>(w) || qobject_cast<QFrame*>(w)) {
                        w->setStyleSheet("");
                    }
                }
            }
        }

        m_quickBar->setStyleSheet(
            QString("QWidget { background-color: %1; border-radius: 6px; }"
            "QToolButton {"
            "  font-size: 12px; border-radius: 4px;"
            "  background: transparent; color: %2; border: none;"
            "}"
            "QToolButton:hover { background: %3; color: %4; }"
            "QToolButton:checked { background: %5; color: %6; }"
            "QFrame { color: %7; }")
            .arg(c.panelBg.name(), c.textPrimary.name(), c.border.name(),
                 c.textPrimary.name(), c.accent.name(), c.textOnAccent.name(),
                 c.border.name()));
    }
    if (m_quickBarHandle) {
        m_quickBarHandle->setStyleSheet(QString(
            "QToolButton { background: %1; border-radius: 6px; }"
            "QToolButton:hover { background: %2; }")
            .arg(c.panelBg.name(), c.border.name()));
    }
}
