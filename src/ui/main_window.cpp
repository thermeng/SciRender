#include "main_window.h"
#include "theme.h"
#include "ui_main_window.h"
#include "ui_lighting_page.h"
#include "ui_slicing_page.h"
#include "ui_view_display_page.h"
#include "ui_scalar_page.h"
#include "ui_vectors_page.h"
#include "ui_streamlines_page.h"
#include "ui_screenshot_page.h"
#include "ui_mesh_info_page.h"
#include "ui_volume_page.h"
#include "render/render_config.h"
#include "core/Colormaps.h"
#include <QApplication>
#include <QMenuBar>
#include <QMenu>
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
#include <QStyledItemDelegate>
#include <QTimer>
#include <QPainter>

// ============================================================================
// UI layout constants (mirrors MainWindow private members for use by free helpers)
// ============================================================================
static constexpr int kSidebarWidth = 220;
static constexpr int kIconStripWidth = 48;
static constexpr int kLabelWidth = 72;
static constexpr int kControlHeight = 24;
static constexpr int kValueFieldWidth = 48;

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
    lbl->    setFixedWidth(kLabelWidth);
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
    lbl->    setFixedWidth(kLabelWidth);
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
    row.field->    setFixedWidth(kValueFieldWidth);
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
    lbl->setStyleSheet(QString("font-size: 11px; font-weight: bold; color: %1; padding: 4px 0 6px 0; border-bottom: 1px solid %2;").arg(currentThemeColors().accent.name(), currentThemeColors().borderLight.name()));
    return lbl;
}

// ============================================================================
// Helper: Swatch button (color swatch + label)
// ============================================================================
static QPushButton* createSwatchButton(const QString& text, const QColor& color, std::function<void()> onClicked) {
    auto* btn = new QPushButton(text);
    btn->setFixedHeight(kControlHeight);
    btn->setMinimumWidth(kSidebarWidth - kIconStripWidth - 36);
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

class PalettePreviewDelegate : public QStyledItemDelegate {
public:
    explicit PalettePreviewDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);

        int colormapIndex = index.row();
        ColormapType type = static_cast<ColormapType>(colormapIndex);

        int previewWidth = 56;
        int previewHeight = 14;
        int margin = 4;
        QRect previewRect(opt.rect.left() + margin,
                         opt.rect.top() + (opt.rect.height() - previewHeight) / 2,
                         previewWidth, previewHeight);

        QImage img(previewWidth, previewHeight, QImage::Format_RGB888);
        for (int x = 0; x < previewWidth; ++x) {
            float t = static_cast<float>(x) / static_cast<float>(previewWidth - 1);
            glm::vec3 c = Colormaps::evaluate(t, type);
            int r = static_cast<int>(glm::clamp(c.r, 0.0f, 1.0f) * 255.0f);
            int g = static_cast<int>(glm::clamp(c.g, 0.0f, 1.0f) * 255.0f);
            int b = static_cast<int>(glm::clamp(c.b, 0.0f, 1.0f) * 255.0f);
            for (int y = 0; y < previewHeight; ++y) img.setPixel(x, y, qRgb(r, g, b));
        }
        painter->drawImage(previewRect, img);

        QString name = QString::fromUtf8(Colormaps::getName(type));
        painter->setPen(opt.palette.color(QPalette::Text));
        painter->drawText(opt.rect.adjusted(previewRect.right() + 6, 0, -margin, 0),
                         Qt::AlignVCenter | Qt::AlignLeft, name);

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        Q_UNUSED(option);
        Q_UNUSED(index);
        return QSize(180, kControlHeight);
    }
};

static QComboBox* buildColormapCombo(int currentChoice, std::function<void(int)> onChoose) {
    auto* combo = new QComboBox;
    combo->setItemDelegate(new PalettePreviewDelegate(combo));
    combo->setFixedHeight(kControlHeight);
    combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    combo->setMinimumWidth(kSidebarWidth - kIconStripWidth - 20);

    int count = static_cast<int>(ColormapType::Count);
    for (int i = 0; i < count; ++i) {
        combo->addItem(QString::fromUtf8(Colormaps::getName(static_cast<ColormapType>(i))));
    }
    combo->setCurrentIndex(currentChoice);

    QObject::connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     [onChoose](int idx) { if (idx >= 0) onChoose(idx); });
    return combo;
}

// ============================================================================
// MainWindow
// ============================================================================
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow) {
    ui->setupUi(this);

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

MainWindow::~MainWindow() {
    delete ui;
}

// ============================================================================
// Menus
// ============================================================================
void MainWindow::setupMenus() {
    auto* menuBar = this->menuBar();

    // File menu
    auto* fileMenu = ui->menuFile;
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
    auto* viewMenu = ui->menuView;
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
    auto* helpMenu = ui->menuHelp;
    helpMenu->addAction("&Keyboard Shortcuts", this, &MainWindow::showShortcuts);
    helpMenu->addAction("&About SciRender", this, &MainWindow::showAbout);
    helpMenu->addSeparator();
    helpMenu->addAction("&Documentation", this, []() { QDesktopServices::openUrl(QUrl("https://github.com/thermeng/SciRender")); });
}

// ============================================================================
// Sidebar
// ============================================================================
void MainWindow::setupSidebar() {
    m_sidebarDock = ui->sidebarDock;
    m_sidebarDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    m_sidebarDock->setAllowedAreas(Qt::LeftDockWidgetArea);
    m_sidebarDock->setTitleBarWidget(new QWidget); // hide default title bar

    m_sidebarWidget = ui->sidebarContents;
    m_sidebarWidget->setStyleSheet(QString("background-color: %1;").arg(currentThemeColors().panelBg.name()));
    auto* mainLayout = qobject_cast<QHBoxLayout*>(m_sidebarWidget->layout());
    if (!mainLayout) {
        mainLayout = new QHBoxLayout(m_sidebarWidget);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(0);
    }

    // --- Icon strip (left edge, 48px) ---
    m_iconStrip = ui->iconStrip;
    m_iconStrip->setFixedWidth(kIconStripWidth);
    m_iconStrip->setStyleSheet(QString("background-color: %1;").arg(currentThemeColors().panelBg.name()));
    auto* iconLayout = new QVBoxLayout(m_iconStrip);
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
        {"\u{1F52C}", "Volume Rendering", 8},
    };

    m_iconButtons.clear();
    for (int i = 0; i < 10; ++i) {
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
    m_rightPanel = ui->rightPanel;
    m_rightPanel->setStyleSheet(QString("background-color: %1;").arg(currentThemeColors().panelBg.name()));
    auto* rightLayout = qobject_cast<QVBoxLayout*>(m_rightPanel->layout());
    if (!rightLayout) {
        rightLayout = new QVBoxLayout(m_rightPanel);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->setSpacing(0);
    }

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
    m_sectionStack->addWidget(buildScalarPage());     // 3
    m_sectionStack->addWidget(buildVectorsPage());      // 4
    m_sectionStack->addWidget(buildStreamlinesPage());  // 5
    m_sectionStack->addWidget(buildScreenshotPage());   // 6
    m_meshInfoPage = buildMeshInfoPage();
    m_sectionStack->addWidget(m_meshInfoPage);           // 7
    m_sectionStack->addWidget(buildVolumePage());        // 8

    rightLayout->addWidget(m_sectionStack, 1);
    m_sectionStack->setVisible(false);

    mainLayout->addWidget(m_iconStrip);
    mainLayout->addWidget(m_rightPanel, 1);

    m_sidebarDock->setWidget(m_sidebarWidget);
    m_sidebarDock->setMinimumWidth(kIconStripWidth);
    m_sidebarDock->setMaximumWidth(kIconStripWidth + kSidebarWidth);
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
    Ui::LightingPage lightingUi;
    lightingUi.setupUi(content);

    auto* resetBtn = lightingUi.resetBtn;
    resetBtn->setObjectName("secondaryButton");
    connect(resetBtn, &QPushButton::clicked, m_settings, &RenderSettings::resetLighting);

    auto* markersCb = lightingUi.markersCb;
    markersCb->setChecked(m_settings->getShowLightMarkers());
    connect(markersCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowLightMarkers);
    m_lightingMarkersCb = markersCb;

    auto* kitCb = lightingUi.kitCb;
    kitCb->setChecked(m_settings->getLightKitEnabled());
    connect(kitCb, &QCheckBox::toggled, m_settings, &RenderSettings::setLightKitEnabled);
    m_lightingKitCb = kitCb;


        {
        auto* slider = lightingUi.keyLightSlider;
        auto* valueLabel = lightingUi.keyLightValue;
        m_lightingKeySlider = slider;
        m_lightingKeyValue = valueLabel;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(m_settings->getLightKeyIntensity() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            m_settings->setLightKeyIntensity(v);
        });
    }
        {
        auto* slider = lightingUi.warmthSlider;
        auto* valueLabel = lightingUi.warmthValue;
        m_lightingWarmthSlider = slider;
        m_lightingWarmthValue = valueLabel;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(m_settings->getLightWarm() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            m_settings->setLightWarm(v);
        });
    }
        {
        auto* slider = lightingUi.fillKfSlider;
        auto* valueLabel = lightingUi.fillKFValue;
        m_lightingFillKfSlider = slider;
        m_lightingFillKfValue = valueLabel;
        slider->setRange(1000, 15000);
        slider->setValue(static_cast<int>(m_settings->getLightKF() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            m_settings->setLightKF(v);
        });
    }
        {
        auto* slider = lightingUi.backKbSlider;
        auto* valueLabel = lightingUi.backKbValue;
        m_lightingBackKbSlider = slider;
        m_lightingBackKbValue = valueLabel;
        slider->setRange(1000, 15000);
        slider->setValue(static_cast<int>(m_settings->getLightKB() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            m_settings->setLightKB(v);
        });
    }
        {
        auto* slider = lightingUi.headKhSlider;
        auto* valueLabel = lightingUi.headKHValue;
        m_lightingHeadKhSlider = slider;
        m_lightingHeadKhValue = valueLabel;
        slider->setRange(1000, 15000);
        slider->setValue(static_cast<int>(m_settings->getLightKH() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            m_settings->setLightKH(v);
        });
    }

    auto* tabWidget = lightingUi.directionTabs;
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

    m_lightDirTabs.resize(4);
    for (int i = 0; i < 4; ++i) {
        auto* tab = new QWidget;
        auto* tabLayout = new QVBoxLayout(tab);
        tabLayout->setContentsMargins(4, 4, 4, 4);
        {
            auto row = createLightSlider("Azimuth", (m_settings->*azGetters[i])(), -180, 180, 1, 0,
                [this, i, azSetters](double v) { (m_settings->*azSetters[i])(v); });
            m_lightDirTabs[i].azimuthSlider = row.slider;
            m_lightDirTabs[i].azimuthValue = row.valueLabel;
            tabLayout->addWidget(row.slider->parentWidget());
        }
        {
            auto row = createLightSlider("Elevation", (m_settings->*elGetters[i])(), -90, 90, 1, 0,
                [this, i, elSetters](double v) { (m_settings->*elSetters[i])(v); });
            m_lightDirTabs[i].elevationSlider = row.slider;
            m_lightDirTabs[i].elevationValue = row.valueLabel;
            tabLayout->addWidget(row.slider->parentWidget());
        }
        tabLayout->addStretch();
        tabWidget->addTab(tab, QString::fromUtf8(lightNames[i]));
    }
    m_lightingDirectionTabs = tabWidget;

        {
        auto* slider = lightingUi.ambientSlider;
        auto* valueLabel = lightingUi.ambientValue;
        m_lightingAmbientSlider = slider;
        m_lightingAmbientValue = valueLabel;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(m_settings->getMatAmbient() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            m_settings->setMatAmbient(v);
        });
    }
        {
        auto* slider = lightingUi.diffuseSlider;
        auto* valueLabel = lightingUi.diffuseValue;
        m_lightingDiffuseSlider = slider;
        m_lightingDiffuseValue = valueLabel;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(m_settings->getMatDiffuse() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            m_settings->setMatDiffuse(v);
        });
    }
        {
        auto* slider = lightingUi.specularSlider;
        auto* valueLabel = lightingUi.specularValue;
        m_lightingSpecularSlider = slider;
        m_lightingSpecularValue = valueLabel;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(m_settings->getMatSpecular() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            m_settings->setMatSpecular(v);
        });
    }
        {
        auto* slider = lightingUi.roughnessSlider;
        auto* valueLabel = lightingUi.roughnessValue;
        m_lightingRoughnessSlider = slider;
        m_lightingRoughnessValue = valueLabel;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(m_settings->getMatRoughness() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            m_settings->setMatRoughness(v);
        });
    }
        {
        auto* slider = lightingUi.metallicSlider;
        auto* valueLabel = lightingUi.metallicValue;
        m_lightingMetallicSlider = slider;
        m_lightingMetallicValue = valueLabel;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(m_settings->getMatMetallic() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            m_settings->setMatMetallic(v);
        });
    }

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

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
    Ui::SlicingPage slicingUi;
    slicingUi.setupUi(page);

    auto* enableCb = slicingUi.enableCb;
    enableCb->setChecked(m_settings->getClipEnabled());
    connect(enableCb, &QCheckBox::toggled, m_settings, &RenderSettings::setClipEnabled);

    auto* cbX = slicingUi.axisX;
    auto* cbY = slicingUi.axisY;
    auto* cbZ = slicingUi.axisZ;
    cbX->setChecked(m_settings->getSliceEnabledX());
    cbY->setChecked(m_settings->getSliceEnabledY());
    cbZ->setChecked(m_settings->getSliceEnabledZ());
    connect(cbX, &QCheckBox::toggled, m_settings, &RenderSettings::setSliceEnabledX);
    connect(cbY, &QCheckBox::toggled, m_settings, &RenderSettings::setSliceEnabledY);
    connect(cbZ, &QCheckBox::toggled, m_settings, &RenderSettings::setSliceEnabledZ);

    {
        auto* slider = slicingUi.sliceXSlider;
        slider->setRange(static_cast<int>(m_settings->getWorldMinX() * 1000), static_cast<int>(m_settings->getWorldMaxX() * 1000));
        slider->setValue(static_cast<int>(m_settings->getSliceX() * 1000));
        connect(slider, &QSlider::valueChanged, this, [this](int raw) {
            double v = raw / 1000.0;
            m_settings->setSliceX(v);
        });
    }
    {
        auto* slider = slicingUi.sliceYSlider;
        slider->setRange(static_cast<int>(m_settings->getWorldMinY() * 1000), static_cast<int>(m_settings->getWorldMaxY() * 1000));
        slider->setValue(static_cast<int>(m_settings->getSliceY() * 1000));
        connect(slider, &QSlider::valueChanged, this, [this](int raw) {
            double v = raw / 1000.0;
            m_settings->setSliceY(v);
        });
    }
    {
        auto* slider = slicingUi.sliceZSlider;
        slider->setRange(static_cast<int>(m_settings->getWorldMinZ() * 1000), static_cast<int>(m_settings->getWorldMaxZ() * 1000));
        slider->setValue(static_cast<int>(m_settings->getSliceZ() * 1000));
        connect(slider, &QSlider::valueChanged, this, [this](int raw) {
            double v = raw / 1000.0;
            m_settings->setSliceZ(v);
        });
    }

    auto* invX = slicingUi.invX;
    auto* invY = slicingUi.invY;
    auto* invZ = slicingUi.invZ;
    invX->setChecked(m_settings->getInvertX());
    invY->setChecked(m_settings->getInvertY());
    invZ->setChecked(m_settings->getInvertZ());
    connect(invX, &QCheckBox::toggled, m_settings, &RenderSettings::setInvertX);
    connect(invY, &QCheckBox::toggled, m_settings, &RenderSettings::setInvertY);
    connect(invZ, &QCheckBox::toggled, m_settings, &RenderSettings::setInvertZ);

    connect(enableCb, &QCheckBox::toggled, slicingUi.optionsGroup, &QWidget::setEnabled);
    slicingUi.optionsGroup->setEnabled(m_settings->getClipEnabled());

    qobject_cast<QVBoxLayout*>(page->layout())->addStretch();

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
    Ui::ViewDisplayPage viewUi;
    viewUi.setupUi(content);

    auto* orthoCb = viewUi.parallelCb;
    orthoCb->setChecked(m_settings->getOrthographic());
    connect(orthoCb, &QCheckBox::toggled, m_settings, &RenderSettings::setOrthographic);

    auto* rotateCb = viewUi.autoRotateCb;
    rotateCb->setChecked(m_settings->getAutoRotate());
    connect(rotateCb, &QCheckBox::toggled, m_settings, &RenderSettings::setAutoRotate);

    auto* resetCamBtn = viewUi.resetCamBtn;
    resetCamBtn->setObjectName("secondaryButton");
    connect(resetCamBtn, &QPushButton::clicked, m_settings, &RenderSettings::resetCamera);

    auto createModeRow = [this](const QString& text, bool checked, bool enabled,
                                double val, double min, double max,
                                auto toggleSlot, auto sliderSlot) -> QWidget* {
        auto* rowWidget = new QWidget;
        auto* rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(4);

        auto* cb = new QCheckBox(text);
        cb->setChecked(checked);
        cb->setEnabled(enabled);
        connect(cb, &QCheckBox::toggled, m_settings, toggleSlot);
        rowLayout->addWidget(cb);

        auto* slider = new QSlider(Qt::Horizontal);
        slider->setRange(static_cast<int>(min * 10), static_cast<int>(max * 10));
        slider->setValue(static_cast<int>(val * 10));
        slider->setEnabled(checked && enabled);
        slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(slider, &QSlider::valueChanged, m_settings, sliderSlot);
        connect(cb, &QCheckBox::toggled, slider, &QWidget::setEnabled);
        rowLayout->addWidget(slider, 1);

        rowWidget->setFixedHeight(kControlHeight);
        rowWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        return rowWidget;
    };

    {
        auto* cb = viewUi.wireframeCb;
        auto* slider = viewUi.wireframeSlider;
        cb->setChecked(m_settings->isWireframe());
        cb->setEnabled(true);
        connect(cb, &QCheckBox::toggled, m_settings, &RenderSettings::setWireframe);
        slider->setRange(10, 100);
        slider->setValue(static_cast<int>(m_settings->getLineWidth() * 10));
        slider->setEnabled(m_settings->isWireframe());
        slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(slider, &QSlider::valueChanged, m_settings, [this](int v) { m_settings->setLineWidth(v / 10.0); });
        connect(cb, &QCheckBox::toggled, slider, &QWidget::setEnabled);
        m_vdWireframeCb = cb;
    }
    auto* surfaceCb = viewUi.surfaceCb;
    auto* pointsCb = viewUi.pointsCb;
    surfaceCb->setChecked(m_settings->isSurfaceVisible());
    pointsCb->setChecked(m_settings->getShowPoints());
    connect(surfaceCb, &QCheckBox::toggled, m_settings, &RenderSettings::toggleSurface);
    connect(pointsCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowPoints);
    m_vdSurfaceCb = surfaceCb;
    m_vdPointsCb = pointsCb;
    viewUi.refGridCb->setChecked(m_settings->isGridVisible());
    connect(viewUi.refGridCb, &QCheckBox::toggled, m_settings, &RenderSettings::toggleGrid);
    m_vdGridCb = viewUi.refGridCb;
    viewUi.bboxCb->setChecked(m_settings->getShowBounds());
    connect(viewUi.bboxCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowBounds);
    m_vdBboxCb = viewUi.bboxCb;
    viewUi.defectsCb->setChecked(m_settings->getShowQualityOverlay());
    connect(viewUi.defectsCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowQualityOverlay);
    m_vdDefectsCb = viewUi.defectsCb;

    {
        auto* slider = viewUi.pointSizeSlider;
        auto* valueLabel = viewUi.pointSizeValue;
        slider->setRange(static_cast<int>(1 * 1000), static_cast<int>(20 * 1000));
        slider->setValue(static_cast<int>(m_settings->getPointSize() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            m_settings->setPointSize(v);
        });
    }

    auto* scalarCb = viewUi.scalarCb;
    scalarCb->setChecked(m_settings->getPointUseScalar());
    connect(scalarCb, &QCheckBox::toggled, m_settings, &RenderSettings::setPointUseScalar);
    m_vdScalarCb = scalarCb;

    connect(pointsCb, &QCheckBox::toggled, viewUi.pointGroup, &QWidget::setVisible);
    viewUi.pointGroup->setVisible(m_settings->getShowPoints());

    {
        auto* slider = viewUi.surfaceOpacitySlider;
        auto* valueLabel = viewUi.surfaceOpacityValue;
        slider->setRange(static_cast<int>(0.1 * 1000), static_cast<int>(1 * 1000));
        slider->setValue(static_cast<int>(m_settings->getSurfaceOpacity() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            m_settings->setSurfaceOpacity(v);
        });
    }
    {
        auto* slider = viewUi.pointOpacitySlider;
        auto* valueLabel = viewUi.pointOpacityValue;
        slider->setRange(static_cast<int>(0.1 * 1000), static_cast<int>(1 * 1000));
        slider->setValue(static_cast<int>(m_settings->getPointOpacity() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            m_settings->setPointOpacity(v);
        });
    }

    auto* gizmoCb = viewUi.gizmoCb;
    gizmoCb->setChecked(m_settings->isGizmoVisible());
    connect(gizmoCb, &QCheckBox::toggled, m_settings, &RenderSettings::setGizmoVisible);
    m_vdGizmoCb = gizmoCb;

    auto* fpsCb = viewUi.fpsCb;
    fpsCb->setChecked(m_settings->getShowFps());
    connect(fpsCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowFps);
    m_vdFpsCb = fpsCb;

    auto* parallelCb = viewUi.parallelCb;
    parallelCb->setChecked(m_settings->getOrthographic());
    connect(parallelCb, &QCheckBox::toggled, m_settings, &RenderSettings::setOrthographic);
    m_vdParallelCb = parallelCb;

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

    content->layout()->replaceWidget(viewUi.wireframeColorBtn, createColorButton("Wireframe", m_settings->getMeshColorQml(), &m_meshColorDialog, &RenderSettings::setMeshColorQml));
    delete viewUi.wireframeColorBtn;
    content->layout()->replaceWidget(viewUi.surfaceColorBtn, createColorButton("Surface", m_settings->getSurfaceColorQml(), &m_surfaceColorDialog, &RenderSettings::setSurfaceColorQml));
    delete viewUi.surfaceColorBtn;
    content->layout()->replaceWidget(viewUi.bgColorBtn, createColorButton("Background", m_settings->getBgColorQml(), &m_bgColorDialog, &RenderSettings::setBgColorQml));
    delete viewUi.bgColorBtn;

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

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
QWidget* MainWindow::buildScalarPage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");

    auto* content = new QWidget;
    Ui::ScalarPage scalarUi;
    scalarUi.setupUi(content);

    // Wire up static controls from .ui file
    auto* scalarColorCb = scalarUi.colorByScalar;
    scalarColorCb->setChecked(m_settings->getMeshUseScalarColor());
    connect(scalarColorCb, &QCheckBox::toggled, m_settings, &RenderSettings::setMeshUseScalarColor);

    m_scalarCombo = scalarUi.scalarCombo;
    m_scalarCombo->addItems(m_settings->getAvailableScalars());
    m_scalarCombo->setCurrentText(m_settings->getActiveScalarNameQml());
    m_scalarCombo->setEnabled(m_settings->hasMeshScalars());
    m_scalarCombo->setMinimumWidth(kSidebarWidth - kIconStripWidth - 20);
    connect(m_scalarCombo, &QComboBox::activated, m_settings, [this](int idx) {
        m_settings->setActiveScalarField(m_scalarCombo->itemText(idx));
    });

    auto* paletteCombo = buildColormapCombo(m_settings->getColormapChoice(),
        [this](int i) { m_settings->setColormapChoice(i); });
    paletteCombo->setMinimumWidth(kSidebarWidth - kIconStripWidth - 20);
    content->layout()->replaceWidget(scalarUi.paletteCombo, paletteCombo);
    delete scalarUi.paletteCombo;

    auto* reversedCb = scalarUi.reversePalette;
    reversedCb->setChecked(m_settings->getColormapReversed());
    connect(reversedCb, &QCheckBox::toggled, m_settings, &RenderSettings::setColormapReversed);

    auto* showBarCb = scalarUi.showColorbar;
    showBarCb->setChecked(m_settings->getShowScalarColorbar());
    connect(showBarCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowScalarColorbar);

    auto* ticksSpin = scalarUi.ticksSpin;
    ticksSpin->setValue(m_settings->getColorbarTicks());
    connect(ticksSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setColorbarTicks);

    {
        auto* slider = scalarUi.minFilterSlider;
        auto* field = scalarUi.minFilterField;
        double minVal = m_settings->getDataScalarMinQml();
        double maxVal = m_settings->getDataScalarMaxQml();
        slider->setRange(static_cast<int>(minVal * 1000), static_cast<int>(maxVal * 1000));
        slider->setValue(static_cast<int>(m_settings->getFilterMin() * 1000));
        field->setText(QString::number(m_settings->getFilterMin(), 'f', 3));
        m_filterMinSlider = slider;
        m_filterMinField = field;
        connect(slider, &QSlider::valueChanged, this, [field, this](int raw) {
            double v = raw / 1000.0;
            field->setText(QString::number(v, 'f', 3));
            m_settings->setFilterMin(v);
        });
        connect(field, &QLineEdit::editingFinished, this, [field, slider, this]() {
            bool ok = false;
            double v = field->text().toDouble(&ok);
            if (!ok) v = m_settings->getFilterMin();
            v = qBound(m_settings->getDataScalarMinQml(), v, m_settings->getDataScalarMaxQml());
            slider->setValue(static_cast<int>(v * 1000));
            m_settings->setFilterMin(v);
        });
    }
    {
        auto* slider = scalarUi.maxFilterSlider;
        auto* field = scalarUi.maxFilterField;
        double minVal = m_settings->getDataScalarMinQml();
        double maxVal = m_settings->getDataScalarMaxQml();
        slider->setRange(static_cast<int>(minVal * 1000), static_cast<int>(maxVal * 1000));
        slider->setValue(static_cast<int>(m_settings->getFilterMax() * 1000));
        field->setText(QString::number(m_settings->getFilterMax(), 'f', 3));
        m_filterMaxSlider = slider;
        m_filterMaxField = field;
        connect(slider, &QSlider::valueChanged, this, [field, this](int raw) {
            double v = raw / 1000.0;
            field->setText(QString::number(v, 'f', 3));
            m_settings->setFilterMax(v);
        });
        connect(field, &QLineEdit::editingFinished, this, [field, slider, this]() {
            bool ok = false;
            double v = field->text().toDouble(&ok);
            if (!ok) v = m_settings->getFilterMax();
            v = qBound(m_settings->getDataScalarMinQml(), v, m_settings->getDataScalarMaxQml());
            slider->setValue(static_cast<int>(v * 1000));
            m_settings->setFilterMax(v);
        });
    }

    auto* resetFilterBtn = scalarUi.resetFilterBtn;
    connect(resetFilterBtn, &QPushButton::clicked, m_settings, [this]() {
        double minVal = m_settings->getDataScalarMinQml();
        double maxVal = m_settings->getDataScalarMaxQml();
        m_settings->setFilterMin(minVal);
        m_settings->setFilterMax(maxVal);
        if (m_filterMinSlider) {
            m_filterMinSlider->blockSignals(true);
            m_filterMinSlider->setValue(static_cast<int>(minVal * 1000));
            m_filterMinSlider->blockSignals(false);
        }
        if (m_filterMinField) {
            m_filterMinField->setText(QString::number(minVal, 'f', 3));
        }
        if (m_filterMaxSlider) {
            m_filterMaxSlider->blockSignals(true);
            m_filterMaxSlider->setValue(static_cast<int>(maxVal * 1000));
            m_filterMaxSlider->blockSignals(false);
        }
        if (m_filterMaxField) {
            m_filterMaxField->setText(QString::number(maxVal, 'f', 3));
        }
    });

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

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
    Ui::VectorsPage vectorsUi;
    vectorsUi.setupUi(page);

    auto* showCb = vectorsUi.showCb;
    showCb->setChecked(m_settings->getShowVectors());
    connect(showCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowVectors);

    m_vectorCombo = vectorsUi.vectorCombo;
    m_vectorCombo->addItems(m_settings->getAvailableVectors());
    m_vectorCombo->setCurrentText(m_settings->getVectorField());
    m_vectorCombo->setMinimumWidth(kSidebarWidth - kIconStripWidth - 20);
    connect(m_vectorCombo, &QComboBox::activated, m_settings, [this](int) {
        m_settings->setActiveVectorField(m_vectorCombo->currentText());
    });

    {
        auto* slider = vectorsUi.scaleSlider;
        auto* valueLabel = vectorsUi.scaleValue;
        slider->setRange(static_cast<int>(0.01 * 1000), static_cast<int>(5.0 * 1000));
        slider->setValue(static_cast<int>(m_settings->getVectorScale() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            m_settings->setVectorScale(v);
        });
    }

    auto* scaleMagCb = vectorsUi.scaleMagCb;
    scaleMagCb->setChecked(m_settings->getVectorScaleByMagnitude());
    connect(scaleMagCb, &QCheckBox::toggled, m_settings, &RenderSettings::setVectorScaleByMagnitude);

    {
        auto* slider = vectorsUi.strideSlider;
        auto* valueLabel = vectorsUi.strideValue;
        slider->setRange(static_cast<int>(1 * 1000), static_cast<int>(20 * 1000));
        slider->setValue(static_cast<int>(m_settings->getVectorStride() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 0));
            m_settings->setVectorStride(static_cast<int>(v));
        });
    }

    auto* magCombo = vectorsUi.magCombo;
    magCombo->addItems({"Linear", "Square root", "Logarithmic"});
    magCombo->setCurrentIndex(m_settings->getVectorMagTransform());
    connect(magCombo, &QComboBox::activated, m_settings, [this](int idx) { m_settings->setVectorMagTransform(idx); });

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

    vectorsUi.optionsGroup->layout()->replaceWidget(vectorsUi.vectorColorBtn, createColorButton("Vector", m_settings->getVectorColorQml(), &m_vectorColorDialog, &RenderSettings::setVectorColorQml));
    delete vectorsUi.vectorColorBtn;

    auto* useCmapCb = vectorsUi.useCmapCb;
    useCmapCb->setChecked(m_settings->getVectorUseColormap());
    connect(useCmapCb, &QCheckBox::toggled, m_settings, &RenderSettings::setVectorUseColormap);

    auto* vCmapCombo = buildColormapCombo(m_settings->getVectorColormapChoice(),
        [this](int i) { m_settings->setVectorColormapChoice(i); });
    vCmapCombo->setMinimumWidth(kSidebarWidth - kIconStripWidth - 20);
    vectorsUi.optionsGroup->layout()->replaceWidget(vectorsUi.vCmapCombo, vCmapCombo);
    delete vectorsUi.vCmapCombo;

    auto* revCb = vectorsUi.revCb;
    revCb->setChecked(m_settings->getVectorColormapReversed());
    connect(revCb, &QCheckBox::toggled, m_settings, &RenderSettings::setVectorColormapReversed);

    connect(showCb, &QCheckBox::toggled, vectorsUi.optionsGroup, &QWidget::setEnabled);
    vectorsUi.optionsGroup->setEnabled(m_settings->getShowVectors());

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");
    qobject_cast<QVBoxLayout*>(page->layout())->addStretch();

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
    Ui::StreamlinesPage slUi;
    slUi.setupUi(content);

    auto* showCb = slUi.showCb;
    showCb->setChecked(m_settings->getShowStreamlines());
    connect(showCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowStreamlines);

    m_streamlineCombo = slUi.streamlineCombo;
    m_streamlineCombo->addItems(m_settings->getAvailableVectors());
    m_streamlineCombo->setCurrentText(m_settings->getStreamlineVectorField());
    m_streamlineCombo->setMinimumWidth(kSidebarWidth - kIconStripWidth - 20);
    connect(m_streamlineCombo, &QComboBox::activated, m_settings, [this](int) {
        m_settings->setStreamlineVectorField(m_streamlineCombo->currentText());
    });

    {
        auto* slider = slUi.stepSizeSlider;
        auto* valueLabel = slUi.stepSizeValue;
        slider->setRange(static_cast<int>(0.005 * 1000), static_cast<int>(0.1 * 1000));
        slider->setValue(static_cast<int>(m_settings->getStreamlineStepSize() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 3));
            m_settings->setStreamlineStepSize(v);
        });
    }

    auto* maxStepsSpin = slUi.maxStepsSpin;
    maxStepsSpin->setRange(10, 500);
    maxStepsSpin->setValue(m_settings->getStreamlineMaxSteps());
    connect(maxStepsSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setStreamlineMaxSteps);

    auto* integrateBtn = slUi.integrateBtn;
    integrateBtn->setObjectName("secondaryButton");
    connect(integrateBtn, &QPushButton::clicked, this, [this]() {
        m_settings->backend()->markStreamlineDirty();
        m_viewport->update();
    });

    auto* directionCombo = slUi.streamlineDirectionCombo;
    directionCombo->addItems({"Forward", "Backward", "Both"});
    directionCombo->setCurrentText(m_settings->getStreamlineDirection());
    directionCombo->setFixedWidth(100);
    m_streamlineDirectionCombo = directionCombo;
    connect(directionCombo, &QComboBox::activated, m_settings, [this, directionCombo](int) {
        m_settings->setStreamlineDirection(directionCombo->currentText());
    });

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
        content->layout()->replaceWidget(slUi.streamlineColorBtn, btn);
        delete slUi.streamlineColorBtn;
    }

    auto* slUseCmapCb = slUi.slUseCmapCb;
    slUseCmapCb->setChecked(m_settings->getStreamlineUseColormap());
    connect(slUseCmapCb, &QCheckBox::toggled, m_settings, &RenderSettings::setStreamlineUseColormap);

    auto* slCmapCombo = buildColormapCombo(m_settings->getStreamlineColormapChoice(),
        [this](int i) { m_settings->setStreamlineColormapChoice(i); });
    content->layout()->replaceWidget(slUi.slCmapCombo, slCmapCombo);
    delete slUi.slCmapCombo;

    auto* slRevCb = slUi.slRevCb;
    slRevCb->setChecked(m_settings->getStreamlineColormapReversed());
    connect(slRevCb, &QCheckBox::toggled, m_settings, &RenderSettings::setStreamlineColormapReversed);

    auto* seedModeCombo = slUi.seedModeCombo;
    seedModeCombo->addItems({"Volume", "Surface", "Plane XY", "Plane XZ", "Plane YZ"});
    const QStringList modeKeys = {"Volume", "Surface", "PlaneXY", "PlaneXZ", "PlaneYZ"};
    seedModeCombo->setCurrentIndex(modeKeys.indexOf(m_settings->getSeedMode()));
    seedModeCombo->setMinimumWidth(kSidebarWidth - kIconStripWidth - 20);
    connect(seedModeCombo, &QComboBox::activated, m_settings, [this, modeKeys](int idx) {
        m_settings->setSeedMode(modeKeys[idx]);
    });

    auto* planePosSlider = slUi.planePosSlider;
    planePosSlider->setRange(0, 1000);
    planePosSlider->setValue(static_cast<int>(m_settings->getSeedPlanePos() * 1000));
    connect(planePosSlider, &QSlider::valueChanged, m_settings, [this](int v) { m_settings->setSeedPlanePos(v / 1000.0); });

    auto* seedsUSpin = slUi.seedsUSpin;
    seedsUSpin->setRange(1, 200);
    seedsUSpin->setValue(m_settings->getSeedPlaneCountU());
    connect(seedsUSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setSeedPlaneCountU);

    auto* seedsVSpin = slUi.seedsVSpin;
    seedsVSpin->setRange(1, 200);
    seedsVSpin->setValue(m_settings->getSeedPlaneCountV());
    connect(seedsVSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setSeedPlaneCountV);

    {
        auto* slider = slUi.jitterSlider;
        auto* valueLabel = slUi.jitterValue;
        slider->setRange(static_cast<int>(0 * 1000), static_cast<int>(1 * 1000));
        slider->setValue(static_cast<int>(m_settings->getSeedJitter() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            m_settings->setSeedJitter(v);
        });
    }

    auto* showSeedsCb = slUi.showSeedsCb;
    showSeedsCb->setChecked(m_settings->getShowSeeds());
    connect(showSeedsCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowSeeds);

    {
        auto* slider = slUi.seedSizeSlider;
        auto* valueLabel = slUi.seedSizeValue;
        slider->setRange(static_cast<int>(1 * 1000), static_cast<int>(20 * 1000));
        slider->setValue(static_cast<int>(m_settings->getSeedPointSize() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            m_settings->setSeedPointSize(v);
        });
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
        content->layout()->replaceWidget(slUi.seedColorBtn, btn);
        delete slUi.seedColorBtn;
    }

    {
        auto* slider = slUi.opacitySlider;
        auto* valueLabel = slUi.opacityValue;
        slider->setRange(static_cast<int>(0 * 1000), static_cast<int>(1 * 1000));
        slider->setValue(static_cast<int>(m_settings->getStreamlineOpacity() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            m_settings->setStreamlineOpacity(v);
        });
    }
    {
        auto* slider = slUi.ribbonWidthSlider;
        auto* valueLabel = slUi.ribbonWidthValue;
        slider->setRange(static_cast<int>(0.001 * 1000), static_cast<int>(0.05 * 1000));
        slider->setValue(static_cast<int>(m_settings->getStreamlineRibbonWidth() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 3));
            m_settings->setStreamlineRibbonWidth(v);
        });
    }
    {
        auto* slider = slUi.taperFactorSlider;
        auto* valueLabel = slUi.taperFactorValue;
        slider->setRange(static_cast<int>(0 * 1000), static_cast<int>(0.8 * 1000));
        slider->setValue(static_cast<int>(m_settings->getStreamlineTaperFactor() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            m_settings->setStreamlineTaperFactor(v);
        });
    }

    {
        auto* slider = slUi.streamlineAmbientSlider;
        auto* valueLabel = slUi.streamlineAmbientValue;
        slider->setRange(static_cast<int>(0 * 1000), static_cast<int>(1 * 1000));
        slider->setValue(static_cast<int>(m_settings->getStreamlineAmbient() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            m_settings->setStreamlineAmbient(v);
        });
    }
    {
        auto* slider = slUi.streamlineDiffuseSlider;
        auto* valueLabel = slUi.streamlineDiffuseValue;
        slider->setRange(static_cast<int>(0 * 1000), static_cast<int>(1 * 1000));
        slider->setValue(static_cast<int>(m_settings->getStreamlineDiffuse() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            m_settings->setStreamlineDiffuse(v);
        });
    }
    {
        auto* slider = slUi.streamlineSpecularSlider;
        auto* valueLabel = slUi.streamlineSpecularValue;
        slider->setRange(static_cast<int>(0 * 1000), static_cast<int>(1 * 1000));
        slider->setValue(static_cast<int>(m_settings->getStreamlineSpecular() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            m_settings->setStreamlineSpecular(v);
        });
    }

    auto* specPowerSpin = slUi.specPowerSpin;
    specPowerSpin->setRange(2, 128);
    specPowerSpin->setValue(m_settings->getStreamlineSpecularPower());
    connect(specPowerSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setStreamlineSpecularPower);

    auto* arrowsCb = slUi.arrowsCb;
    arrowsCb->setChecked(m_settings->getShowStreamlineArrows());
    connect(arrowsCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowStreamlineArrows);

    auto* arrowSpacingSpin = slUi.arrowSpacingSpin;
    arrowSpacingSpin->setRange(2, 20);
    arrowSpacingSpin->setValue(m_settings->getStreamlineArrowSpacing());
    connect(arrowSpacingSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setStreamlineArrowSpacing);

    {
        auto* slider = slUi.arrowSizeSlider;
        auto* valueLabel = slUi.arrowSizeValue;
        slider->setRange(static_cast<int>(0.01 * 1000), static_cast<int>(0.2 * 1000));
        slider->setValue(static_cast<int>(m_settings->getStreamlineArrowSize() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            m_settings->setStreamlineArrowSize(v);
        });
    }

    auto* particlesCb = slUi.particlesCb;
    particlesCb->setChecked(m_settings->getShowParticles());
    connect(particlesCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowParticles);

    {
        auto* slider = slUi.particleCountSlider;
        auto* valueLabel = slUi.particleCountValue;
        slider->setRange(static_cast<int>(10 * 1000), static_cast<int>(5000 * 1000));
        slider->setValue(static_cast<int>(m_settings->getParticleCount() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 0));
            m_settings->setParticleCount(static_cast<int>(v));
        });
    }
    {
        auto* slider = slUi.particleSpeedSlider;
        auto* valueLabel = slUi.particleSpeedValue;
        slider->setRange(static_cast<int>(0.1 * 1000), static_cast<int>(100 * 1000));
        slider->setValue(static_cast<int>(m_settings->getParticleSpeed() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            m_settings->setParticleSpeed(v);
        });
    }
    {
        auto* slider = slUi.particleSizeSlider;
        auto* valueLabel = slUi.particleSizeValue;
        slider->setRange(static_cast<int>(1 * 1000), static_cast<int>(20 * 1000));
        slider->setValue(static_cast<int>(m_settings->getParticleSize() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            m_settings->setParticleSize(v);
        });
    }

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

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
    Ui::ScreenshotPage ssUi;
    ssUi.setupUi(page);

    auto* saveBtn = ssUi.saveBtn;
    saveBtn->setObjectName("secondaryButton");
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::saveScreenshot);

    auto* transCb = ssUi.transCb;
    transCb->setChecked(m_settings->getScreenshotTransparent());
    connect(transCb, &QCheckBox::toggled, m_settings, &RenderSettings::setScreenshotTransparent);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");
    qobject_cast<QVBoxLayout*>(page->layout())->addStretch();

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
    Ui::MeshInfoPage meshUi;
    meshUi.setupUi(page);
    m_meshInfoLabels.clear();

    auto addInfoRow = [this, &meshUi](const QString& label, QLabel* valueWidget, const QString& value, const QString& color = currentThemeColors().textPrimary.name()) {
        valueWidget->setText(value);
        valueWidget->setStyleSheet(QString("font-size: 11px; color: %1;").arg(color));
        m_meshInfoLabels[label] = valueWidget;
    };

    addInfoRow("Type", meshUi.typeValue, m_settings->getMeshDataType());
    addInfoRow("Format", meshUi.formatValue, m_settings->getMeshFormat());

    addInfoRow("Triangles", meshUi.trianglesValue, QString::number(m_settings->getTriangleCount()));
    addInfoRow("Points", meshUi.pointsValue, QString::number(m_settings->getPointCount()));

    addInfoRow("Degenerate", meshUi.degenerateValue, QString::number(m_settings->getDegenerateFaces()), "#ff6666");
    addInfoRow("Open Edges", meshUi.openEdgesValue, QString::number(m_settings->getOpenEdges()), "#ffaa44");
    addInfoRow("Non-manifold Edge", meshUi.nonManifoldEValue, QString::number(m_settings->getNonManifoldEdges()), "#ff44ff");
    addInfoRow("Non-manifold Vertex", meshUi.nonManifoldVValue, QString::number(m_settings->getNonManifoldVerts()), "#ff44ff");
    addInfoRow("Watertight", meshUi.watertightValue, m_settings->getWatertight() ? "Yes" : "No", m_settings->getWatertight() ? "#66dd66" : "#ff6666");

    auto addBB = [&](const QString& row, int axis, double v) {
        QString key = QString("BB:%1:%2").arg(row).arg(axis);
        QLabel* widget = nullptr;
        if (row == "Min") widget = axis == 0 ? meshUi.bbMinX : axis == 1 ? meshUi.bbMinY : meshUi.bbMinZ;
        else if (row == "Max") widget = axis == 0 ? meshUi.bbMaxX : axis == 1 ? meshUi.bbMaxY : meshUi.bbMaxZ;
        else widget = axis == 0 ? meshUi.bbDeltaX : axis == 1 ? meshUi.bbDeltaY : meshUi.bbDeltaZ;
        if (widget) {
            widget->setText(QString::number(v, 'f', 3));
            m_meshInfoLabels[key] = widget;
        }
    };

    addBB("Min", 0, m_settings->getWorldMinX());
    addBB("Min", 1, m_settings->getWorldMinY());
    addBB("Min", 2, m_settings->getWorldMinZ());
    addBB("Max", 0, m_settings->getWorldMaxX());
    addBB("Max", 1, m_settings->getWorldMaxY());
    addBB("Max", 2, m_settings->getWorldMaxZ());
    addBB("Delta", 0, m_settings->getWorldMaxX() - m_settings->getWorldMinX());
    addBB("Delta", 1, m_settings->getWorldMaxY() - m_settings->getWorldMinY());
    addBB("Delta", 2, m_settings->getWorldMaxZ() - m_settings->getWorldMinZ());

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");
    qobject_cast<QVBoxLayout*>(page->layout())->addStretch();

    scroll->setWidget(page);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}

// ============================================================================
// Section: Volume Rendering (8)
// ============================================================================
QWidget* MainWindow::buildVolumePage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { border: none; }");

    auto* content = new QWidget;
    Ui::VolumePage volumeUi;
    volumeUi.setupUi(content);

    auto* showCb = volumeUi.showVolumeCb;
    showCb->setChecked(m_settings->getShowVolume());
    connect(showCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowVolume);

    m_volumeFieldCombo = volumeUi.volumeFieldCombo;
    m_volumeFieldCombo->addItems(m_settings->getAvailableScalars());
    m_volumeFieldCombo->setCurrentText(m_settings->getActiveScalarNameQml());
    m_volumeFieldCombo->setEnabled(m_settings->hasMeshScalars());
    m_volumeFieldCombo->setMinimumWidth(kSidebarWidth - kIconStripWidth - 20);
    connect(m_volumeFieldCombo, &QComboBox::activated, m_settings, [this](int idx) {
        m_volumeFieldCombo->setCurrentIndex(idx);
        m_settings->setActiveScalarField(m_volumeFieldCombo->itemText(idx));
    });

    auto* paletteCombo = buildColormapCombo(m_settings->getVolumeColormapChoice(),
        [this](int i) { m_settings->setVolumeColormapChoice(i); });
    paletteCombo->setMinimumWidth(kSidebarWidth - kIconStripWidth - 20);
    content->layout()->replaceWidget(volumeUi.volumePaletteCombo, paletteCombo);
    delete volumeUi.volumePaletteCombo;

    auto* reversedCb = volumeUi.volumeReverseCb;
    reversedCb->setChecked(m_settings->getVolumeColormapReversed());
    connect(reversedCb, &QCheckBox::toggled, m_settings, &RenderSettings::setVolumeColormapReversed);

    {
        auto* slider = volumeUi.stepSlider;
        auto* valueLabel = volumeUi.stepValue;
        slider->setRange(1, 1000);
        slider->setValue(static_cast<int>(m_settings->getVolumeStepSize() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 3));
            m_settings->setVolumeStepSize(v);
        });
    }

    {
        auto* slider = volumeUi.opacitySlider;
        auto* valueLabel = volumeUi.opacityValue;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(m_settings->getVolumeOpacity() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 3));
            m_settings->setVolumeOpacity(v);
        });
    }

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

    scroll->setWidget(content);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}

void MainWindow::refreshMeshInfoPage() {
    if (!m_meshInfoPage) return;

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
    "Lighting", "Slicing", "View & Display", "Scalar",
    "Vectors", "Streamlines", "Screenshot", "Mesh Info", "Volume Rendering"
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
    m_sidebarDock->setFixedWidth(m_sidebarExpanded ? kIconStripWidth + kSidebarWidth : kIconStripWidth);
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
    m_qbVolume = addQBButton("V", "Volume", m_settings->getShowVolume(), true, [this]() {
        m_settings->setShowVolume(!m_settings->getShowVolume());
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
    if (m_qbVolume)    m_qbVolume->setChecked(m_settings->getShowVolume());
}

// Keep the View & Display checkboxes in sync when settings are changed from the
// quick-bar buttons or keyboard shortcuts, so both UIs reflect the same state.
void MainWindow::syncViewDisplayPage() {
    if (m_vdWireframeCb) m_vdWireframeCb->setChecked(m_settings->isWireframe());
    if (m_vdGridCb)      m_vdGridCb->setChecked(m_settings->isGridVisible());
    if (m_vdSurfaceCb)   m_vdSurfaceCb->setChecked(m_settings->isSurfaceVisible());
    if (m_vdPointsCb)    m_vdPointsCb->setChecked(m_settings->getShowPoints());
    if (m_vdBboxCb)      m_vdBboxCb->setChecked(m_settings->getShowBounds());
    if (m_vdDefectsCb)   m_vdDefectsCb->setChecked(m_settings->getShowQualityOverlay());
    if (m_vdScalarCb)    m_vdScalarCb->setChecked(m_settings->getPointUseScalar());
    if (m_vdGizmoCb)     m_vdGizmoCb->setChecked(m_settings->isGizmoVisible());
    if (m_vdFpsCb)       m_vdFpsCb->setChecked(m_settings->getShowFps());
    if (m_vdParallelCb)  m_vdParallelCb->setChecked(m_settings->getOrthographic());
}

void MainWindow::syncLightingPage() {
    if (m_lightingMarkersCb) m_lightingMarkersCb->setChecked(m_settings->getShowLightMarkers());
    if (m_lightingKitCb)    m_lightingKitCb->setChecked(m_settings->getLightKitEnabled());

    auto syncSlider = [this](QSlider* slider, QLabel* label, double value, int decimals) {
        if (!slider) return;
        slider->blockSignals(true);
        slider->setValue(static_cast<int>(value * 1000));
        slider->blockSignals(false);
        if (label) label->setText(QString::number(value, 'f', decimals));
    };

    syncSlider(m_lightingKeySlider, m_lightingKeyValue, m_settings->getLightKeyIntensity(), 1);
    syncSlider(m_lightingWarmthSlider, m_lightingWarmthValue, m_settings->getLightWarm(), 1);
    syncSlider(m_lightingFillKfSlider, m_lightingFillKfValue, m_settings->getLightKF(), 1);
    syncSlider(m_lightingBackKbSlider, m_lightingBackKbValue, m_settings->getLightKB(), 1);
    syncSlider(m_lightingHeadKhSlider, m_lightingHeadKhValue, m_settings->getLightKH(), 1);

    using GetterFn = float (RenderSettings::*)() const;
    static const GetterFn azGetters[] = { &RenderSettings::getLightKeyAzimuth, &RenderSettings::getLightFillAzimuth, &RenderSettings::getLightBackAzimuth, &RenderSettings::getLightHeadAzimuth };
    static const GetterFn elGetters[] = { &RenderSettings::getLightKeyElevation, &RenderSettings::getLightFillElevation, &RenderSettings::getLightBackElevation, &RenderSettings::getLightHeadElevation };

    for (int i = 0; i < 4 && i < m_lightDirTabs.size(); ++i) {
        const auto& tab = m_lightDirTabs[i];
        syncSlider(tab.azimuthSlider, tab.azimuthValue, (m_settings->*azGetters[i])(), 0);
        syncSlider(tab.elevationSlider, tab.elevationValue, (m_settings->*elGetters[i])(), 0);
    }

    syncSlider(m_lightingAmbientSlider, m_lightingAmbientValue, m_settings->getMatAmbient(), 1);
    syncSlider(m_lightingDiffuseSlider, m_lightingDiffuseValue, m_settings->getMatDiffuse(), 1);
    syncSlider(m_lightingSpecularSlider, m_lightingSpecularValue, m_settings->getMatSpecular(), 1);
    syncSlider(m_lightingRoughnessSlider, m_lightingRoughnessValue, m_settings->getMatRoughness(), 2);
    syncSlider(m_lightingMetallicSlider, m_lightingMetallicValue, m_settings->getMatMetallic(), 2);
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
        if (m_streamlineDirectionCombo) {
            m_streamlineDirectionCombo->blockSignals(true);
            m_streamlineDirectionCombo->setCurrentText(m_settings->getStreamlineDirection());
            m_streamlineDirectionCombo->blockSignals(false);
        }
        if (m_volumeFieldCombo) {
            m_volumeFieldCombo->blockSignals(true);
            m_volumeFieldCombo->clear();
            m_volumeFieldCombo->addItems(scalars);
            m_volumeFieldCombo->setCurrentText(m_settings->getActiveScalarNameQml());
            m_volumeFieldCombo->setEnabled(m_settings->hasMeshScalars());
            m_volumeFieldCombo->blockSignals(false);
        }
    });

    // Theme changes
    connect(m_settings, &RenderSettings::themeChanged, this, [this]() {
        applyTheme(m_settings->getTheme());
    });

    // Mirror quick-bar and View & Display toggles whenever the corresponding
    // settings change (keyboard shortcuts, quick-bar buttons, side-panel checkboxes).
    // Sync runs on every view change, but the setChecked calls only emit when the
    // value actually differs.
    connect(m_settings, &RenderSettings::viewChanged, this, [this](ChangeFlags flags) {
        if (flags.testFlag(ChangeFlag::Lighting)) syncLightingPage();
        syncQuickBar();
        syncViewDisplayPage();
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
        sb->showMessage("No mesh loaded | drag a .stl / .vtk / .obj / .vtu / .vts / .vti / .vtp / .vtr file, or use File > Open Mesh");
    }
}

// ============================================================================
// File dialogs / actions
// ============================================================================
void MainWindow::openMesh() {
    QString path = QFileDialog::getOpenFileName(this, "Load Mesh", QString(),
        "Mesh files (*.stl *.vtk *.obj *.vtu *.vts *.vti *.vtp *.vtr);;All files (*)");
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
    m_sectionStack->addWidget(buildScalarPage());     // 3
    m_sectionStack->addWidget(buildVectorsPage());      // 4
    m_sectionStack->addWidget(buildStreamlinesPage());  // 5
    m_sectionStack->addWidget(buildScreenshotPage());   // 6
    m_meshInfoPage = buildMeshInfoPage();
    m_sectionStack->addWidget(m_meshInfoPage);  
    m_sectionStack->addWidget(buildVolumePage());        // 8

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
