#include "main_window.h"
#include "ui_main_window.h"
#include "animation_export_dialog.h"
#include "ui_lighting_page.h"
#include "ui_slicing_page.h"
#include "ui_view_display_page.h"
#include "ui_scalar_page.h"
#include "ui_vectors_page.h"
#include "ui_streamlines_page.h"
#include "ui_screenshot_page.h"
#include "ui_mesh_info_page.h"
#include "ui_volume_page.h"
#include "ui_slice_plane_page.h"
#include "ui_isosurface_page.h"
#include "ui_animation_page.h"
#include "render/foundation/render_config.h"
#include "core/Colormaps.h"
#include <QApplication>
#include <QMenuBar>
#include <QMenu>
#include <QProgressDialog>
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
#include <algorithm>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QPainter>
#include <QFontMetrics>


// UI layout constants (mirrors MainWindow private members for use by free helpers)

static constexpr int kSidebarWidth = 220;
static constexpr int m_navWidth = 140;
static constexpr int kLabelWidth = 72;
static constexpr int kControlHeight = 24;

// Helper: Does a widget want typing/navigation keys (so viewport shortcuts
// must stay out of the way)?

static bool navFocusIsEditor(QWidget* w) {
    return w && (qobject_cast<QLineEdit*>(w) ||
                 qobject_cast<QComboBox*>(w) || qobject_cast<QSpinBox*>(w) ||
                 qobject_cast<QDoubleSpinBox*>(w));
}


// Helper: Create a labeled slider row (LightSlider equivalent)

struct SliderRow {
    QSlider* slider = nullptr;
    QLabel* valueLabel = nullptr;
    std::function<void(double)> callback;
};

static SliderRow createLightSlider(const QString& label, double value, double from, double to, double step, int decimals, std::function<void(double)> cb, const QString& labelObjName = {}) {
    SliderRow row;
    auto* widget = new QWidget;
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* lbl = new QLabel(label);
    if (!labelObjName.isEmpty()) lbl->setObjectName(labelObjName);
    lbl->setFixedWidth(kLabelWidth);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
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


// Helper: Fix layout overflow by making labels and value widgets flexible.
// .ui files have fixed 72px labels + 36-52px value widgets that overflow
// when sidebar width < 220px. This removes fixed max sizes and sets
// size policies so the layout can shrink gracefully.

static void fixLayoutOverflow(QWidget* root) {
    if (!root) return;
    for (QLineEdit* le : root->findChildren<QLineEdit*>()) {
        le->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        le->setMaximumWidth(QWIDGETSIZE_MAX);
        le->setMinimumWidth(32);
    }
    for (QSpinBox* sb : root->findChildren<QSpinBox*>()) {
        sb->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        sb->setMaximumWidth(QWIDGETSIZE_MAX);
        sb->setMinimumWidth(40);
    }
    for (QDoubleSpinBox* dsb : root->findChildren<QDoubleSpinBox*>()) {
        dsb->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        dsb->setMaximumWidth(QWIDGETSIZE_MAX);
        dsb->setMinimumWidth(40);
    }
}


// Helper: Apply panel styling (section headers, parameter labels, dividers)

static void applyPanelStyling(QWidget* root) {
    if (!root) return;

    // Establish a clear size distinction
    const int kHeaderFontSize = 11; // 11px Uppercase + Bold works well for headers
    const int kParamFontSize = 10;  // Slightly smaller muted text for parameters

    // Collect all child labels once
    const auto allLabels = root->findChildren<QLabel*>();

    // ── Phase 1: Style Parameter Labels ──────────────────────────────────────
    for (QLabel* lbl : allLabels) {
        const QString name = lbl->objectName();
        if (name.endsWith("Label") && !name.endsWith("Header")) {
            QFont f = lbl->font();
            f.setPixelSize(kParamFontSize);
            f.setBold(false);
            lbl->setFont(f);
            lbl->setStyleSheet("background: transparent;");
        }
    }

    // ── Phase 2: Identify Section Headers ────────────────────────────────────
    struct HeaderEntry {
        QVBoxLayout* layout = nullptr; // Strictly QVBoxLayout to prevent row divider bugs
        int index = -1;
        QLabel* label = nullptr;
    };
    QList<HeaderEntry> headers;

    for (QLabel* lbl : allLabels) {
        if (!lbl->objectName().endsWith("Header")) continue;

        // Find the actual layout containing this widget item (handles nested layouts)
        QLayoutItem* item = nullptr;
        QLayout* containingLayout = nullptr;
        
        // Search parent hierarchy if needed, or query parent widget's layout
        if (QWidget* parent = lbl->parentWidget()) {
            if (QLayout* lay = parent->layout()) {
                // Check if directly in parent layout or in one of its sub-layouts
                for (QObject* child : parent->children()) {
                    if (auto* box = qobject_cast<QVBoxLayout*>(child)) {
                        int idx = box->indexOf(lbl);
                        if (idx != -1) {
                            containingLayout = box;
                            break;
                        }
                    }
                }
                // Fallback to direct parent layout check
                if (!containingLayout) {
                    containingLayout = lay;
                }
            }
        }

        // Strictly check for Vertical Layouts so horizontal headers don't get dividers inserted
        auto* vBox = qobject_cast<QVBoxLayout*>(containingLayout);
        if (!vBox) continue;

        int idx = vBox->indexOf(lbl);
        if (idx != -1) {
            headers.append({vBox, idx, lbl});
        }
    }

    // ── Phase 3: Insert Dividers (Bottom-to-Top) ─────────────────────────────
    QMap<QVBoxLayout*, QList<int>> indicesByLayout;
    for (const auto& e : headers) {
        // Prevent duplicate dividers if a divider line already precedes this header
        bool hasDivider = false;
        if (e.index > 0) {
            if (QLayoutItem* prevItem = e.layout->itemAt(e.index - 1)) {
                if (QWidget* w = prevItem->widget()) {
                    if (w->property("isHeaderDivider").toBool()) {
                        hasDivider = true;
                    }
                }
            }
        }

        if (!hasDivider) {
            indicesByLayout[e.layout].append(e.index);
        }
    }

    // Insert separators descending by index
    for (auto it = indicesByLayout.begin(); it != indicesByLayout.end(); ++it) {
        QList<int> indices = it.value();
        std::sort(indices.begin(), indices.end(), std::greater<int>());

        for (int idx : indices) {
            auto* line = new QFrame;
            line->setProperty("isHeaderDivider", true); // Flag to prevent duplicate insertion on re-runs
            line->setFrameShape(QFrame::NoFrame);
            line->setFixedHeight(1);
            line->setStyleSheet("background-color: palette(mid); margin-top: 8px; margin-bottom: 4px;");
            it.key()->insertWidget(idx, line);
        }
    }

    // // ── Phase 4: Apply Subtle Header Typography ───────────────────────────────
    // for (const auto& e : headers) {
    //     QFont f = e.label->font();
    //     f.setBold(false);
    //     f.setPixelSize(10);
    //     f.setLetterSpacing(QFont::AbsoluteSpacing, 0.3);

    //     e.label->setFont(f);
    //     e.label->setStyleSheet("background: transparent; padding-top: 6px;");
    // }

    // ── Phase 4: Apply Subtle Header Typography ───────────────────────────────
    for (const auto& e : headers) {
        QFont f = e.label->font();
        f.setWeight(QFont::Medium);  // or f.setBold(true) with 10px
        f.setPixelSize(11);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 0.3);
        e.label->setFont(f);
        e.label->setStyleSheet("background: transparent; padding-top: 6px;");
    }
}


// Helper: Swatch button (color swatch + label)

static QPushButton* createSwatchButton(const QString& text, const QColor& color, std::function<void()> onClicked) {
    auto* btn = new QPushButton(text);
    btn->setFixedHeight(kControlHeight);
    btn->setMinimumWidth(kSidebarWidth - m_navWidth - 36);
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
    combo->setMinimumWidth(kSidebarWidth - m_navWidth - 20);

    int count = static_cast<int>(ColormapType::Count);
    for (int i = 0; i < count; ++i) {
        combo->addItem(QString::fromUtf8(Colormaps::getName(static_cast<ColormapType>(i))));
    }
    combo->setCurrentIndex(currentChoice);

    QObject::connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     [onChoose](int idx) { if (idx >= 0) onChoose(idx); });
    return combo;
}

static QPushButton* createColorButton(MainWindow* self, RenderSettings* settings, const QString& name, QColor initialColor, QColorDialog** dialogPtr, auto setterMember) {
    auto* btn = createSwatchButton(name, initialColor, nullptr);
    QObject::connect(btn, &QPushButton::clicked, self, [self, btn, name, initialColor, dialogPtr, settings, setterMember]() {
        if (!*dialogPtr) {
            *dialogPtr = new QColorDialog(initialColor, self);
            (*dialogPtr)->setOption(QColorDialog::ShowAlphaChannel, false);
            QObject::connect(*dialogPtr, &QColorDialog::colorSelected, settings, setterMember);
            QObject::connect(*dialogPtr, &QColorDialog::colorSelected, btn, [btn](const QColor& c) {
                QPixmap pix(14, 14); pix.fill(c); btn->setIcon(pix);
            });
        }
        (*dialogPtr)->open();
    });
    return btn;
}

static void addCtlRow(QVBoxLayout* lay, const QString& labelObjName,
                      const QString& text, QWidget* ctl, int ctlStretch = 0) {
    auto* row = new QWidget;
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);
    auto* lbl = new QLabel(text);
    lbl->setObjectName(labelObjName);
    lbl->setFixedWidth(kLabelWidth);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    rowLayout->addWidget(lbl);
    rowLayout->addWidget(ctl, ctlStretch);
    lay->addWidget(row);
}

static void addSliderRow(RenderSettings* settings, QVBoxLayout* lay, const QString& labelObjName,
                         const QString& text, double value, double from,
                         double to, int decimals,
                         void (RenderSettings::*setter)(double)) {
    auto row = createLightSlider(text, value, from, to, 0.001, decimals,
                                 [settings, setter](double v) { (settings->*setter)(v); },
                                 labelObjName);
    lay->addWidget(row.slider->parentWidget());
}


// MainWindow

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
    setupTopToolbar();
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
    viewMenu->addAction("&Slicing", this, [this]() { setSidebarSection(2); });
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

    const QString navItems[] = {
        "Mesh Info", "Lighting", "Slicing", "View & Display", "Scalar",
        "Vectors", "Streamlines", "Volume", "Slice Plane", "Isosurface",
        "Screenshot", "Animation"
    };

    // Calculate width dynamically based on longest text + padding
    QFontMetrics fm(m_navList->font());
    int maxTextWidth = 0;
    for (const auto& name : navItems) {
        maxTextWidth = qMax(maxTextWidth, fm.horizontalAdvance(name));
    }
    m_navWidth = maxTextWidth + 48; // 24px padding each side

    for (const auto& name : navItems) {
        auto* item = new QListWidgetItem(name, m_navList);
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

    auto* closeBtn = new QToolButton;
    closeBtn->setText("\u00D7");
    closeBtn->setFixedSize(24, 24);
    closeBtn->setToolTip("Close panel");
    connect(closeBtn, &QToolButton::clicked, this, [this]() { setSidebarSection(m_activeSection); });
    headerLayout->addWidget(closeBtn);

    rightLayout->addWidget(m_panelHeader);
    m_panelHeader->setVisible(false);

    // Stacked widget (section pages)
    m_sectionStack = new QStackedWidget;

    m_sectionStack->addWidget(buildMeshInfoPage());       // 0
    m_meshInfoPage = m_sectionStack->widget(0);
    m_sectionStack->addWidget(buildLightingPage());     // 1
    m_sectionStack->addWidget(buildSlicingPage());      // 2
    m_sectionStack->addWidget(buildViewDisplayPage());  // 3
    m_sectionStack->addWidget(buildScalarPage());     // 4
    m_sectionStack->addWidget(buildVectorsPage());      // 5
    m_sectionStack->addWidget(buildStreamlinesPage());  // 6
    m_sectionStack->addWidget(buildVolumePage());       // 7
    m_sectionStack->addWidget(buildSlicePlanePage());   // 8
    m_sectionStack->addWidget(buildIsosurfacePage());   // 9
    m_sectionStack->addWidget(buildScreenshotPage());   // 10
    m_sectionStack->addWidget(buildAnimationPage());    // 11

    rightLayout->addWidget(m_sectionStack, 1);
    m_sectionStack->setVisible(false);

    mainLayout->addWidget(m_navList);
    mainLayout->addWidget(m_rightPanel, 1);

    m_sidebarDock->setWidget(m_sidebarWidget);
    m_sidebarDock->setMinimumWidth(m_navWidth);
    m_sidebarDock->setMaximumWidth(m_navWidth + kSidebarWidth);
    addDockWidget(Qt::LeftDockWidgetArea, m_sidebarDock);
}


// Section: Lighting (0)

QWidget* MainWindow::buildLightingPage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget;
    Ui::LightingPage lightingUi;
    lightingUi.setupUi(content);
    fixLayoutOverflow(content);

    auto* resetBtn = lightingUi.resetBtn;
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

    applyPanelStyling(content);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}


// Section: Slicing (1)

QWidget* MainWindow::buildSlicingPage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* page = new QWidget;
    Ui::SlicingPage slicingUi;
    slicingUi.setupUi(page);
    fixLayoutOverflow(page);

    // -- Master enable --------------------------------------------------------
    auto* enableCb = slicingUi.enableCb;
    m_sliceEnableCb = enableCb;
    enableCb->setChecked(m_settings->getClipEnabled());
    connect(enableCb, &QCheckBox::toggled, m_settings, &RenderSettings::setClipEnabled);
    connect(enableCb, &QCheckBox::toggled, slicingUi.optionsGroup, &QWidget::setEnabled);
    slicingUi.optionsGroup->setEnabled(m_settings->getClipEnabled());

    // -- Crinkle Clip checkbox -----------------------------------------------
    m_sliceCrinkleCb = slicingUi.crinkleCb;
    m_sliceCrinkleCb->setEnabled(m_settings->getClipEnabled());
    m_sliceCrinkleCb->setChecked(m_settings->getCrinkleClipMode());
    connect(enableCb, &QCheckBox::toggled, m_sliceCrinkleCb, &QWidget::setEnabled);
    connect(m_sliceCrinkleCb, &QCheckBox::toggled, m_settings, &RenderSettings::setCrinkleClipMode);

    // -- Per-axis setup lambda -----------------------------------------------
    auto setupAxis = [&](QCheckBox* axisCb, QCheckBox* invertCb,
                         QSlider* slider, QLabel* valueLabel,
                         bool enabled, bool invert, double from, double to, double val,
                         auto enableSlot, auto setter, auto invertSlot) {
        axisCb->setChecked(enabled);
        connect(axisCb, &QCheckBox::toggled, m_settings, enableSlot);

        invertCb->setChecked(invert);
        connect(invertCb, &QCheckBox::toggled, m_settings, invertSlot);

        int minI = static_cast<int>(from * 1000);
        int maxI = static_cast<int>(to * 1000);
        slider->setRange(minI, maxI);
        slider->setValue(static_cast<int>(val * 1000));

        valueLabel->setText(QString::number(val, 'f', 3));

        // Slider -> valueLabel + setter
        connect(slider, &QSlider::valueChanged, this,
            [valueLabel, setter](int raw) {
                double v = raw / 1000.0;
                valueLabel->setText(QString::number(v, 'f', 3));
                setter(v);
            });
    };

    // -- X Axis ---------------------------------------------------------------
    m_sliceXSlider = slicingUi.sliceXSlider;
    m_sliceXValue = slicingUi.xValue;
    m_sliceAxisXCb = slicingUi.axisX;
    setupAxis(slicingUi.axisX, slicingUi.invX,
              m_sliceXSlider, m_sliceXValue,
              m_settings->getSliceEnabledX(), m_settings->getInvertX(),
              m_settings->getWorldMinX(), m_settings->getWorldMaxX(), m_settings->getSliceX(),
              &RenderSettings::setSliceEnabledX,
              [this](double v) { m_settings->setSliceX(v); },
              &RenderSettings::setInvertX);

    // -- Y Axis ---------------------------------------------------------------
    m_sliceYSlider = slicingUi.sliceYSlider;
    m_sliceYValue = slicingUi.yValue;
    m_sliceAxisYCb = slicingUi.axisY;
    setupAxis(slicingUi.axisY, slicingUi.invY,
              m_sliceYSlider, m_sliceYValue,
              m_settings->getSliceEnabledY(), m_settings->getInvertY(),
              m_settings->getWorldMinY(), m_settings->getWorldMaxY(), m_settings->getSliceY(),
              &RenderSettings::setSliceEnabledY,
              [this](double v) { m_settings->setSliceY(v); },
              &RenderSettings::setInvertY);

    // -- Z Axis ---------------------------------------------------------------
    m_sliceZSlider = slicingUi.sliceZSlider;
    m_sliceZValue = slicingUi.zValue;
    m_sliceAxisZCb = slicingUi.axisZ;
    setupAxis(slicingUi.axisZ, slicingUi.invZ,
              m_sliceZSlider, m_sliceZValue,
              m_settings->getSliceEnabledZ(), m_settings->getInvertZ(),
              m_settings->getWorldMinZ(), m_settings->getWorldMaxZ(), m_settings->getSliceZ(),
              &RenderSettings::setSliceEnabledZ,
              [this](double v) { m_settings->setSliceZ(v); },
              &RenderSettings::setInvertZ);

    qobject_cast<QVBoxLayout*>(page->layout())->addStretch();

    scroll->setWidget(page);

    applyPanelStyling(page);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}


// Section: View & Display (2)



QWidget* MainWindow::buildViewDisplayPage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget;
    Ui::ViewDisplayPage viewUi;
    viewUi.setupUi(content);
    fixLayoutOverflow(content);

    auto* orthoCb = viewUi.parallelCb;
    orthoCb->setChecked(m_settings->getOrthographic());
    connect(orthoCb, &QCheckBox::toggled, m_settings, &RenderSettings::setOrthographic);

    auto* rotateCb = viewUi.autoRotateCb;
    rotateCb->setChecked(m_settings->getAutoRotate());
    connect(rotateCb, &QCheckBox::toggled, m_settings, &RenderSettings::setAutoRotate);

    auto* resetCamBtn = viewUi.resetCamBtn;
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
        slider->setRange(static_cast<int>(0.0 * 1000), static_cast<int>(1 * 1000));
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
        slider->setRange(static_cast<int>(0.0 * 1000), static_cast<int>(1 * 1000));
        slider->setValue(static_cast<int>(m_settings->getPointOpacity() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            m_settings->setPointOpacity(v);
        });
    }

    {
        auto* spin = viewUi.peelLayersSpin;
        spin->setValue(m_settings->getMaxPeelLayers());
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), m_settings, &RenderSettings::setMaxPeelLayers);
    }

    auto* gizmoCb = viewUi.gizmoCb;
    gizmoCb->setChecked(m_settings->isGizmoVisible());
    connect(gizmoCb, &QCheckBox::toggled, m_settings, &RenderSettings::setGizmoVisible);
    m_vdGizmoCb = gizmoCb;

    auto* gizmoSizeCombo = viewUi.gizmoSizeCombo;
    gizmoSizeCombo->setCurrentIndex(m_settings->getGizmoSizeChoice());
    connect(gizmoSizeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            m_settings, &RenderSettings::setGizmoSizeChoice);
    m_vdGizmoSizeCombo = gizmoSizeCombo;

    auto* gizmoCornerCombo = viewUi.gizmoCornerCombo;
    gizmoCornerCombo->setCurrentIndex(m_settings->getGizmoCorner());
    connect(gizmoCornerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            m_settings, &RenderSettings::setGizmoCorner);
    m_vdGizmoCornerCombo = gizmoCornerCombo;

    auto* fpsCb = viewUi.fpsCb;
    fpsCb->setChecked(m_settings->getShowFps());
    connect(fpsCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowFps);
    m_vdFpsCb = fpsCb;

    auto* parallelCb = viewUi.parallelCb;
    parallelCb->setChecked(m_settings->getOrthographic());
    connect(parallelCb, &QCheckBox::toggled, m_settings, &RenderSettings::setOrthographic);
    m_vdParallelCb = parallelCb;

    content->layout()->replaceWidget(viewUi.wireframeColorBtn, createColorButton(this, m_settings, "Wireframe", m_settings->getMeshColorQml(), &m_meshColorDialog, &RenderSettings::setMeshColorQml));
    delete viewUi.wireframeColorBtn;
    content->layout()->replaceWidget(viewUi.surfaceColorBtn, createColorButton(this, m_settings, "Surface", m_settings->getSurfaceColorQml(), &m_surfaceColorDialog, &RenderSettings::setSurfaceColorQml));
    delete viewUi.surfaceColorBtn;
    content->layout()->replaceWidget(viewUi.bgColorBtn, createColorButton(this, m_settings, "Background", m_settings->getBgColorQml(), &m_bgColorDialog, &RenderSettings::setBgColorQml));
    delete viewUi.bgColorBtn;

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

    scroll->setWidget(content);

    applyPanelStyling(content);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}


// Section: Colormap (3)

QWidget* MainWindow::buildScalarPage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget;
    Ui::ScalarPage scalarUi;
    scalarUi.setupUi(content);
    fixLayoutOverflow(content);

    // Wire up static controls from .ui file
    auto* showCb = scalarUi.showCb;
    m_scalarShowCb = showCb;
    m_scalarOptionsGroup = scalarUi.optionsGroup;
    showCb->setChecked(m_settings->getMeshUseScalarColor());
    connect(showCb, &QCheckBox::toggled, m_settings, &RenderSettings::setMeshUseScalarColor);
    scalarUi.optionsGroup->setEnabled(m_settings->hasMeshScalars());

    m_scalarCombo = scalarUi.scalarCombo;
    m_scalarCombo->addItems(m_settings->getAvailableScalars());
    m_scalarCombo->setCurrentText(m_settings->getActiveScalarNameQml());
    m_scalarCombo->setEnabled(m_settings->hasMeshScalars());
    m_scalarCombo->setMinimumWidth(kSidebarWidth - m_navWidth - 20);
    connect(m_scalarCombo, &QComboBox::activated, m_settings, [this](int idx) {
        m_settings->setActiveScalarField(m_scalarCombo->itemText(idx));
    });

    auto* paletteCombo = buildColormapCombo(m_settings->getColormapChoice(),
        [this](int i) { m_settings->setColormapChoice(i); });
    paletteCombo->setMinimumWidth(kSidebarWidth - m_navWidth - 20);
    scalarUi.optionsGroup->layout()->replaceWidget(scalarUi.paletteCombo, paletteCombo);
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
        auto* filterEnabledCb = scalarUi.filterEnabledCb;
        m_filterEnabledCb = filterEnabledCb;
        filterEnabledCb->setChecked(m_settings->getFilterEnabled());
        connect(filterEnabledCb, &QCheckBox::toggled, m_settings, &RenderSettings::setFilterEnabled);
        connect(m_settings, &RenderSettings::viewChanged, this, [this](ChangeFlags) {
            if (m_filterEnabledCb) {
                bool en = m_settings->getFilterEnabled();
                m_filterEnabledCb->blockSignals(true);
                m_filterEnabledCb->setChecked(en);
                m_filterEnabledCb->blockSignals(false);
                m_filterMinSlider->setEnabled(en);
                m_filterMaxSlider->setEnabled(en);
                m_filterMinField->setEnabled(en);
                m_filterMaxField->setEnabled(en);
            }
        });
        // Initial enabled state
        bool enInit = m_settings->getFilterEnabled();
        scalarUi.minFilterSlider->setEnabled(enInit);
        scalarUi.maxFilterSlider->setEnabled(enInit);
        scalarUi.minFilterField->setEnabled(enInit);
        scalarUi.maxFilterField->setEnabled(enInit);
    }

    {
        auto* slider = scalarUi.minFilterSlider;
        auto* field = scalarUi.minFilterField;
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
            v = std::min(v, static_cast<double>(m_settings->getFilterMax()));
            slider->setValue(static_cast<int>(std::lround(v * 1000.0)));
            m_settings->setFilterMin(v);
        });
    }
    {
        auto* slider = scalarUi.maxFilterSlider;
        auto* field = scalarUi.maxFilterField;
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
            v = std::max(v, static_cast<double>(m_settings->getFilterMin()));
            slider->setValue(static_cast<int>(std::lround(v * 1000.0)));
            m_settings->setFilterMax(v);
        });
    }
    refreshScalarFilterRange();

    auto* resetFilterBtn = scalarUi.resetFilterBtn;
    connect(resetFilterBtn, &QPushButton::clicked, m_settings, [this]() {
        m_settings->setFilterMin(m_settings->getDataScalarMinQml());
        m_settings->setFilterMax(m_settings->getDataScalarMaxQml());
        refreshScalarFilterRange();
    });

    // ---- Fixed colormap range (RangeEditor) ----
    {
        auto* colorRangeCb = scalarUi.colorRangeCb;
        m_colorRangeCb = colorRangeCb;
        colorRangeCb->setChecked(m_settings->getColorRangeOverrideEnabled());
        connect(colorRangeCb, &QCheckBox::toggled, m_settings, &RenderSettings::setColorRangeOverrideEnabled);
        // create RangeEditor directly in the options layout
        auto* optionsLay = qobject_cast<QVBoxLayout*>(scalarUi.optionsGroup->layout());
        m_colorRangeEditor = new RangeEditor();
        if (optionsLay) optionsLay->addWidget(m_colorRangeEditor);
        m_colorRangeEditor->setEnabled(m_settings->getColorRangeOverrideEnabled());
        connect(m_colorRangeEditor, &RangeEditor::windowEdited, this, [this](double lo, double hi){
            m_settings->setColorRangeLo(static_cast<float>(lo));
            m_settings->setColorRangeHi(static_cast<float>(hi));
        });
        connect(m_settings, &RenderSettings::viewChanged, this, [this](ChangeFlags f){
            if (!m_colorRangeCb || !m_colorRangeEditor) return;
            if (f & ChangeFlag::Colormap) {
                const bool en = m_settings->getColorRangeOverrideEnabled();
                m_colorRangeCb->blockSignals(true); m_colorRangeCb->setChecked(en); m_colorRangeCb->blockSignals(false);
                m_colorRangeEditor->setEnabled(en);
                const double lo = m_settings->getColorRangeLo();
                const double hi = m_settings->getColorRangeHi();
                auto cur = m_colorRangeEditor->window();
                if (cur.first != lo || cur.second != hi) {
                    m_colorRangeEditor->blockSignals(true);
                    m_colorRangeEditor->setWindow(lo, hi);
                    m_colorRangeEditor->blockSignals(false);
                }
            }
        });
    }
    refreshColorRangeBounds();

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

    scroll->setWidget(content);

    applyPanelStyling(content);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}


// Section: Vectors (4)

QWidget* MainWindow::buildVectorsPage() {
    auto* page = new QWidget;
    Ui::VectorsPage vectorsUi;
    vectorsUi.setupUi(page);
    fixLayoutOverflow(page);

    auto* showCb = vectorsUi.showCb;
    m_vecShowCb = showCb;
    showCb->setChecked(m_settings->getShowVectors());
    connect(showCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowVectors);

    m_vectorCombo = vectorsUi.vectorCombo;
    m_vectorCombo->addItems(m_settings->getAvailableVectors());
    m_vectorCombo->setCurrentText(m_settings->getVectorField());
    m_vectorCombo->setMinimumWidth(kSidebarWidth - m_navWidth - 20);
    connect(m_vectorCombo, &QComboBox::activated, m_settings, [this](int) {
        m_settings->setActiveVectorField(m_vectorCombo->currentText());
    });

    m_vectorPlacementCombo = vectorsUi.placementCombo;
    m_vectorPlacementCombo->addItems(m_settings->getVectorPlacementOptions());
    m_vectorPlacementCombo->setCurrentIndex(m_settings->getVectorPlacement());
    connect(m_vectorPlacementCombo, &QComboBox::currentIndexChanged, m_settings, [this](int idx) {
        m_settings->setVectorPlacement(idx);
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
        connect(slider, &QSlider::valueChanged, this, [valueLabel](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 0));
        });
        connect(slider, &QSlider::sliderReleased, this, [slider, this]() {
            int v = static_cast<int>(slider->value() / 1000.0);
            if (v < 1) v = 1;
            m_settings->setVectorStride(v);
        });
    }

    auto* magCombo = vectorsUi.magCombo;
    magCombo->addItems({"Linear", "Square root", "Logarithmic"});
    magCombo->setCurrentIndex(m_settings->getVectorMagTransform());
    connect(magCombo, &QComboBox::activated, m_settings, [this](int idx) { m_settings->setVectorMagTransform(idx); });

    vectorsUi.optionsGroup->layout()->replaceWidget(vectorsUi.vectorColorBtn, createColorButton(this, m_settings, "Vector", m_settings->getVectorColorQml(), &m_vectorColorDialog, &RenderSettings::setVectorColorQml));
    delete vectorsUi.vectorColorBtn;

    auto* colorModeCombo = new QComboBox;
    colorModeCombo->addItems({"Solid Color", "Magnitude", "X Component", "Y Component", "Z Component"});
    colorModeCombo->setCurrentIndex(m_settings->getVectorColorMode());
    colorModeCombo->setMinimumWidth(kSidebarWidth - m_navWidth - 20);
    connect(colorModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            m_settings, &RenderSettings::setVectorColorMode);
    vectorsUi.optionsGroup->layout()->replaceWidget(vectorsUi.vectorColorModeCombo, colorModeCombo);
    delete vectorsUi.vectorColorModeCombo;

    auto* vCmapCombo = buildColormapCombo(m_settings->getVectorColormapChoice(),
        [this](int i) { m_settings->setVectorColormapChoice(i); });
    vCmapCombo->setMinimumWidth(kSidebarWidth - m_navWidth - 20);
    vectorsUi.optionsGroup->layout()->replaceWidget(vectorsUi.vCmapCombo, vCmapCombo);
    delete vectorsUi.vCmapCombo;

    auto* revCb = vectorsUi.revCb;
    revCb->setChecked(m_settings->getVectorColormapReversed());
    connect(revCb, &QCheckBox::toggled, m_settings, &RenderSettings::setVectorColormapReversed);

    // ---- Glyph Fixed Range (adaptive: magnitude or active component) ----
    {
        auto* lay = qobject_cast<QVBoxLayout*>(vectorsUi.optionsGroup->layout());
        auto* hdr = new QLabel("Fixed Range");
        hdr->setObjectName("glyphFixedHeader");
        lay->addWidget(hdr);
        m_glyphMagRangeCb = new QCheckBox("Fixed Range");
        m_glyphMagRangeCb->setToolTip("Map a fixed range to glyph palette for the active 'Color By' mode; values outside clamp to end colors");
        m_glyphMagRangeCb->setEnabled(m_settings->hasMeshVectors() || m_settings->hasMeshCellVectors());
        lay->addWidget(m_glyphMagRangeCb);
        m_glyphMagRangeEditor = new RangeEditor();
        lay->addWidget(m_glyphMagRangeEditor);

        auto bindToActiveMode = [this]() {
            int mode = m_settings->getVectorColorMode();
            int comp = (mode >= 2) ? mode - 2 : -1;
            bool solidOrNone = (mode == 0) || !(m_settings->hasMeshVectors() || m_settings->hasMeshCellVectors());
            m_glyphRangeBoundComp = comp;
            double bLo = 0, bHi = 1, lo, hi;
            bool en = false;
            if (solidOrNone) {
                m_glyphMagRangeEditor->setEnabled(false);
                m_glyphMagRangeCb->setEnabled(false);
                return;
            }
            if (comp < 0) {
                en = m_settings->getGlyphMagRangeOverrideEnabled();
                lo = m_settings->getGlyphMagRangeLo();
                hi = m_settings->getGlyphMagRangeHi();
                if (m_settings->backend()) { bLo = m_settings->backend()->vectorMagMin(); bHi = m_settings->backend()->vectorMagMax(); if (!(bHi > bLo)) bHi = bLo + 1.0; }
            } else {
                en = m_settings->getGlyphCompRangeOverrideEnabled(comp);
                lo = m_settings->getGlyphCompRangeLo(comp);
                hi = m_settings->getGlyphCompRangeHi(comp);
                if (m_settings->backend()) { bLo = m_settings->backend()->vectorCompMin(comp); bHi = m_settings->backend()->vectorCompMax(comp); if (!(bHi > bLo)) bHi = bLo + 1.0; }
            }
            m_glyphMagRangeCb->blockSignals(true); m_glyphMagRangeCb->setChecked(en); m_glyphMagRangeCb->blockSignals(false);
            m_glyphMagRangeCb->setEnabled(true);
            m_glyphMagRangeEditor->blockSignals(true);
            m_glyphMagRangeEditor->setBounds(bLo, bHi);
            if (hi > lo) m_glyphMagRangeEditor->setWindow(lo, hi); else m_glyphMagRangeEditor->setWindow(bLo, bHi);
            m_glyphMagRangeEditor->blockSignals(false);
            m_glyphMagRangeEditor->setEnabled(en && m_glyphMagRangeCb->isEnabled());
        };

        bindToActiveMode();

        connect(m_glyphMagRangeCb, &QCheckBox::toggled, this, [this](bool v){
            int comp = m_glyphRangeBoundComp;
            if (comp < 0) {
                m_settings->setGlyphMagRangeOverrideEnabled(v);
            } else {
                m_settings->setGlyphCompRangeOverrideEnabled(comp, v);
            }
            m_glyphMagRangeEditor->setEnabled(v && m_glyphMagRangeCb->isEnabled());
        });

        connect(m_glyphMagRangeEditor, &RangeEditor::windowEdited, this, [this](double lo, double hi){
            int comp = m_glyphRangeBoundComp;
            if (comp < 0) {
                m_settings->setGlyphMagRangeLo(static_cast<float>(lo));
                m_settings->setGlyphMagRangeHi(static_cast<float>(hi));
            } else {
                m_settings->setGlyphCompRangeLo(comp, static_cast<float>(lo));
                m_settings->setGlyphCompRangeHi(comp, static_cast<float>(hi));
            }
        });

        connect(m_settings, &RenderSettings::viewChanged, this, [this, bindToActiveMode](ChangeFlags flags){
            if (flags & ChangeFlag::Vectors) {
                bindToActiveMode();
            } else if ((flags & ChangeFlag::Colormap) && m_glyphMagRangeCb && m_glyphMagRangeEditor) {
                int comp = m_glyphRangeBoundComp;
                bool en = (comp < 0) ? m_settings->getGlyphMagRangeOverrideEnabled()
                                     : m_settings->getGlyphCompRangeOverrideEnabled(comp);
                m_glyphMagRangeCb->blockSignals(true); m_glyphMagRangeCb->setChecked(en); m_glyphMagRangeCb->blockSignals(false);
                m_glyphMagRangeEditor->setEnabled(en && m_glyphMagRangeCb->isEnabled());
                double lo = (comp < 0) ? m_settings->getGlyphMagRangeLo() : m_settings->getGlyphCompRangeLo(comp);
                double hi = (comp < 0) ? m_settings->getGlyphMagRangeHi() : m_settings->getGlyphCompRangeHi(comp);
                auto cur = m_glyphMagRangeEditor->window();
                if (hi > lo && (cur.first != lo || cur.second != hi)) {
                    m_glyphMagRangeEditor->blockSignals(true); m_glyphMagRangeEditor->setWindow(lo, hi); m_glyphMagRangeEditor->blockSignals(false);
                }
            }
        });

        connect(m_settings, &RenderSettings::meshLoadStateChanged, this, [this, bindToActiveMode](){
            bindToActiveMode();
        });
        connect(m_settings, &RenderSettings::meshDataUpdated, this, [this, bindToActiveMode](){
            bindToActiveMode();
        });
    }

    connect(showCb, &QCheckBox::toggled, vectorsUi.optionsGroup, &QWidget::setEnabled);
    vectorsUi.optionsGroup->setEnabled(m_settings->getShowVectors());

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    qobject_cast<QVBoxLayout*>(page->layout())->addStretch();

    scroll->setWidget(page);

    applyPanelStyling(page);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}


// Section: Streamlines (5)

QWidget* MainWindow::buildStreamlinesPage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget;
    Ui::StreamlinesPage slUi;
    slUi.setupUi(content);
    fixLayoutOverflow(content);

    auto* showCb = slUi.showCb;
    m_slShowCb = showCb;
    showCb->setChecked(m_settings->getShowStreamlines());
    connect(showCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowStreamlines);
    connect(showCb, &QCheckBox::toggled, slUi.optionsGroup, &QWidget::setEnabled);
    slUi.optionsGroup->setEnabled(m_settings->getShowStreamlines());

    auto* tabs = slUi.slTabs;

    // Tab: Flow

    auto* flowTab = new QWidget;
    auto* flowLay = new QVBoxLayout(flowTab);
    flowLay->setContentsMargins(4, 4, 4, 4);
    flowLay->setSpacing(4);

    m_streamlineCombo = new QComboBox;
    m_streamlineCombo->addItems(m_settings->getAvailableVectors());
    m_streamlineCombo->setCurrentText(m_settings->getStreamlineVectorField());
    m_streamlineCombo->setMinimumWidth(kSidebarWidth - m_navWidth - 20);
    connect(m_streamlineCombo, &QComboBox::activated, m_settings, [this](int) {
        m_settings->setStreamlineVectorField(m_streamlineCombo->currentText());
    });
    flowLay->addWidget(m_streamlineCombo);

    addSliderRow(m_settings, flowLay, "stepSizeLabel", "Step Size",
                 m_settings->getStreamlineStepSize(), 0.005, 0.1, 3,
                 &RenderSettings::setStreamlineStepSize);

    auto* maxStepsSpin = new QSpinBox;
    maxStepsSpin->setRange(10, 500);
    maxStepsSpin->setValue(m_settings->getStreamlineMaxSteps());
    connect(maxStepsSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setStreamlineMaxSteps);
    addCtlRow(flowLay, "maxStepsLabel", "Max Steps", maxStepsSpin, 1);

    auto* directionCombo = new QComboBox;
    directionCombo->addItems({"Forward", "Backward", "Both"});
    directionCombo->setCurrentText(m_settings->getStreamlineDirection());
    directionCombo->setFixedWidth(100);
    m_streamlineDirectionCombo = directionCombo;
    connect(directionCombo, &QComboBox::activated, m_settings, [this, directionCombo](int) {
        m_settings->setStreamlineDirection(directionCombo->currentText());
    });
    addCtlRow(flowLay, "directionLabel", "Direction", directionCombo);

    auto* arrowsCb = new QCheckBox("Show Direction");
    arrowsCb->setChecked(m_settings->getShowStreamlineArrows());
    connect(arrowsCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowStreamlineArrows);
    flowLay->addWidget(arrowsCb);

    auto* arrowSpacingSpin = new QDoubleSpinBox;
    arrowSpacingSpin->setToolTip("Distance between direction arrows as a fraction of the dataset extent");
    arrowSpacingSpin->setRange(0.02, 0.50);
    arrowSpacingSpin->setDecimals(2);
    arrowSpacingSpin->setSingleStep(0.01);
    arrowSpacingSpin->setValue(m_settings->getStreamlineArrowSpacingFrac());
    connect(arrowSpacingSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            m_settings, &RenderSettings::setStreamlineArrowSpacingFrac);
    addCtlRow(flowLay, "arrowSpacingLabel", "Arrow spacing", arrowSpacingSpin, 1);

    addSliderRow(m_settings, flowLay, "arrowSizeLabel", "Arrow Size",
                 m_settings->getStreamlineArrowSize(), 0.01, 0.2, 2,
                 &RenderSettings::setStreamlineArrowSize);

    auto* integrateBtn = new QPushButton("Integrate");
    connect(integrateBtn, &QPushButton::clicked, this, [this]() {
        m_settings->backend()->markStreamlineDirty();
        m_viewport->update();
    });
    flowLay->addWidget(integrateBtn);
    flowLay->addStretch();
    tabs->addTab(flowTab, tr("Flow"));

    // Tab: Seeds

    auto* seedsTab = new QWidget;
    auto* seedsLay = new QVBoxLayout(seedsTab);
    seedsLay->setContentsMargins(4, 4, 4, 4);
    seedsLay->setSpacing(4);

    auto* seedModeCombo = new QComboBox;
    seedModeCombo->addItems({"Volume", "Surface", "Plane XY", "Plane XZ", "Plane YZ"});
    const QStringList modeKeys = {"Volume", "Surface", "PlaneXY", "PlaneXZ", "PlaneYZ"};
    seedModeCombo->setCurrentIndex(modeKeys.indexOf(m_settings->getSeedMode()));
    seedModeCombo->setMinimumWidth(kSidebarWidth - m_navWidth - 20);
    connect(seedModeCombo, &QComboBox::activated, m_settings, [this, modeKeys](int idx) {
        m_settings->setSeedMode(modeKeys[idx]);
    });
    seedsLay->addWidget(seedModeCombo);

    addSliderRow(m_settings, seedsLay, "planePosLabel", "Plane Position",
                 m_settings->getSeedPlanePos(), 0.0, 1.0, 2,
                 &RenderSettings::setSeedPlanePos);

    auto* seedsUSpin = new QSpinBox;
    seedsUSpin->setRange(1, 200);
    seedsUSpin->setValue(m_settings->getSeedPlaneCountU());
    connect(seedsUSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setSeedPlaneCountU);
    addCtlRow(seedsLay, "seedsULabel", "Seeds U", seedsUSpin, 1);

    auto* seedsVSpin = new QSpinBox;
    seedsVSpin->setRange(1, 200);
    seedsVSpin->setValue(m_settings->getSeedPlaneCountV());
    connect(seedsVSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setSeedPlaneCountV);
    addCtlRow(seedsLay, "seedsVLabel", "Seeds V", seedsVSpin, 1);

    auto updateSeedGridRows = [seedModeCombo, seedsUSpin, seedsVSpin]() {
        const bool planeSeeds = seedModeCombo->currentText().startsWith("Plane");
        seedsUSpin->setEnabled(planeSeeds);
        seedsVSpin->setEnabled(planeSeeds);
    };
    connect(seedModeCombo, &QComboBox::activated, seedsUSpin, updateSeedGridRows);
    updateSeedGridRows();

    addSliderRow(m_settings, seedsLay, "jitterLabel", "Jitter",
                 m_settings->getSeedJitter(), 0.0, 1.0, 2,
                 &RenderSettings::setSeedJitter);

    auto* showSeedsCb = new QCheckBox("Show seeds");
    showSeedsCb->setChecked(m_settings->getShowSeeds());
    connect(showSeedsCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowSeeds);
    seedsLay->addWidget(showSeedsCb);

    addSliderRow(m_settings, seedsLay, "seedSizeLabel", "Seed Size",
                 m_settings->getSeedPointSize(), 1.0, 20.0, 1,
                 &RenderSettings::setSeedPointSize);

    auto* seedBtn = createSwatchButton("Seed Color", m_settings->getSeedPointColorQml(), nullptr);
    connect(seedBtn, &QPushButton::clicked, this, [this, seedBtn]() {
        if (!m_seedColorDialog) {
            m_seedColorDialog = new QColorDialog(m_settings->getSeedPointColorQml(), this);
            connect(m_seedColorDialog, &QColorDialog::colorSelected, m_settings, &RenderSettings::setSeedPointColorQml);
            connect(m_seedColorDialog, &QColorDialog::colorSelected, seedBtn, [seedBtn](const QColor& c) {
                QPixmap pix(14, 14); pix.fill(c); seedBtn->setIcon(pix);
            });
        }
        m_seedColorDialog->open();
    });
    seedsLay->addWidget(seedBtn);
    seedsLay->addStretch();
    tabs->addTab(seedsTab, tr("Seeds"));

    // Tab: Look

    auto* lookTab = new QWidget;
    auto* lookLay = new QVBoxLayout(lookTab);
    lookLay->setContentsMargins(4, 4, 4, 4);
    lookLay->setSpacing(4);

    auto* streamlineBtn = createSwatchButton("Line color", m_settings->getStreamlineColorQml(), nullptr);
    connect(streamlineBtn, &QPushButton::clicked, this, [this, streamlineBtn]() {
        if (!m_streamlineColorDialog) {
            m_streamlineColorDialog = new QColorDialog(m_settings->getStreamlineColorQml(), this);
            connect(m_streamlineColorDialog, &QColorDialog::colorSelected, m_settings, &RenderSettings::setStreamlineColorQml);
            connect(m_streamlineColorDialog, &QColorDialog::colorSelected, streamlineBtn, [streamlineBtn](const QColor& c) {
                QPixmap pix(14, 14); pix.fill(c); streamlineBtn->setIcon(pix);
            });
        }
        m_streamlineColorDialog->open();
    });
    lookLay->addWidget(streamlineBtn);

    // ---- Streamline Color By (combo) ----
    auto* slColorByLabel = new QLabel("Color By");
    slColorByLabel->setObjectName("slColorByLabel");
    lookLay->addWidget(slColorByLabel);
    auto* slColorModeCombo = new QComboBox;
    slColorModeCombo->setObjectName("streamlineColorModeCombo");
    slColorModeCombo->addItems({"Solid Color", "Magnitude", "X Component", "Y Component", "Z Component"});
    slColorModeCombo->setCurrentIndex(m_settings->getStreamlineColorMode());
    slColorModeCombo->setMinimumWidth(kSidebarWidth - m_navWidth - 20);
    connect(slColorModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            m_settings, &RenderSettings::setStreamlineColorMode);
    lookLay->addWidget(slColorModeCombo);

    auto* slCmapCombo = buildColormapCombo(m_settings->getStreamlineColormapChoice(),
        [this](int i) { m_settings->setStreamlineColormapChoice(i); });
    lookLay->addWidget(slCmapCombo);

    auto* slRevCb = new QCheckBox("Reverse Palette");
    slRevCb->setChecked(m_settings->getStreamlineColormapReversed());
    connect(slRevCb, &QCheckBox::toggled, m_settings, &RenderSettings::setStreamlineColormapReversed);
    lookLay->addWidget(slRevCb);

    // ---- Streamline Fixed Range (adaptive: magnitude or active component) ----
    {
        auto* hdr = new QLabel("Fixed Range");
        hdr->setObjectName("streamlineFixedHeader");
        lookLay->addWidget(hdr);
        m_streamlineMagRangeCb = new QCheckBox("Fixed Range");
        m_streamlineMagRangeCb->setToolTip("Map a fixed range to streamline palette for the active 'Color By' mode; values outside clamp to end colors");
        m_streamlineMagRangeCb->setEnabled(m_settings->hasMeshVectors());
        lookLay->addWidget(m_streamlineMagRangeCb);
        m_streamlineMagRangeEditor = new RangeEditor();
        lookLay->addWidget(m_streamlineMagRangeEditor);

        auto bindToActiveMode = [this]() {
            int mode = m_settings->getStreamlineColorMode();
            int comp = (mode >= 2) ? mode - 2 : -1;
            bool solidOrNone = (mode == 0) || !m_settings->hasMeshVectors();
            m_streamlineRangeBoundComp = comp;
            double bLo = 0, bHi = 1, lo, hi;
            bool en = false;
            if (solidOrNone) {
                m_streamlineMagRangeEditor->setEnabled(false);
                m_streamlineMagRangeCb->setEnabled(false);
                return;
            }
            if (comp < 0) {
                en = m_settings->getStreamlineMagRangeOverrideEnabled();
                lo = m_settings->getStreamlineMagRangeLo();
                hi = m_settings->getStreamlineMagRangeHi();
                if (m_settings->backend()) { bLo = m_settings->backend()->streamlineMagMin(); bHi = m_settings->backend()->streamlineMagMax(); if (!(bHi > bLo)) bHi = bLo + 1.0; }
            } else {
                en = m_settings->getStreamlineCompRangeOverrideEnabled(comp);
                lo = m_settings->getStreamlineCompRangeLo(comp);
                hi = m_settings->getStreamlineCompRangeHi(comp);
                if (m_settings->backend()) { bLo = m_settings->backend()->streamlineCompMin(comp); bHi = m_settings->backend()->streamlineCompMax(comp); if (!(bHi > bLo)) bHi = bLo + 1.0; }
            }
            m_streamlineMagRangeCb->blockSignals(true); m_streamlineMagRangeCb->setChecked(en); m_streamlineMagRangeCb->blockSignals(false);
            m_streamlineMagRangeCb->setEnabled(true);
            m_streamlineMagRangeEditor->blockSignals(true);
            m_streamlineMagRangeEditor->setBounds(bLo, bHi);
            if (hi > lo) m_streamlineMagRangeEditor->setWindow(lo, hi); else m_streamlineMagRangeEditor->setWindow(bLo, bHi);
            m_streamlineMagRangeEditor->blockSignals(false);
            m_streamlineMagRangeEditor->setEnabled(en && m_streamlineMagRangeCb->isEnabled());
        };

        bindToActiveMode();

        connect(m_streamlineMagRangeCb, &QCheckBox::toggled, this, [this](bool v){
            int comp = m_streamlineRangeBoundComp;
            if (comp < 0) {
                m_settings->setStreamlineMagRangeOverrideEnabled(v);
            } else {
                m_settings->setStreamlineCompRangeOverrideEnabled(comp, v);
            }
            m_streamlineMagRangeEditor->setEnabled(v && m_streamlineMagRangeCb->isEnabled());
        });

        connect(m_streamlineMagRangeEditor, &RangeEditor::windowEdited, this, [this](double lo, double hi){
            int comp = m_streamlineRangeBoundComp;
            if (comp < 0) {
                m_settings->setStreamlineMagRangeLo(static_cast<float>(lo));
                m_settings->setStreamlineMagRangeHi(static_cast<float>(hi));
            } else {
                m_settings->setStreamlineCompRangeLo(comp, static_cast<float>(lo));
                m_settings->setStreamlineCompRangeHi(comp, static_cast<float>(hi));
            }
        });

        connect(m_settings, &RenderSettings::viewChanged, this, [this, bindToActiveMode](ChangeFlags flags){
            if (flags & ChangeFlag::Display) {
                bindToActiveMode();
            } else if ((flags & ChangeFlag::Colormap) && m_streamlineMagRangeCb && m_streamlineMagRangeEditor) {
                int comp = m_streamlineRangeBoundComp;
                bool en = (comp < 0) ? m_settings->getStreamlineMagRangeOverrideEnabled()
                                     : m_settings->getStreamlineCompRangeOverrideEnabled(comp);
                m_streamlineMagRangeCb->blockSignals(true); m_streamlineMagRangeCb->setChecked(en); m_streamlineMagRangeCb->blockSignals(false);
                m_streamlineMagRangeEditor->setEnabled(en && m_streamlineMagRangeCb->isEnabled());
                double lo = (comp < 0) ? m_settings->getStreamlineMagRangeLo() : m_settings->getStreamlineCompRangeLo(comp);
                double hi = (comp < 0) ? m_settings->getStreamlineMagRangeHi() : m_settings->getStreamlineCompRangeHi(comp);
                auto cur = m_streamlineMagRangeEditor->window();
                if (hi > lo && (cur.first != lo || cur.second != hi)) {
                    m_streamlineMagRangeEditor->blockSignals(true); m_streamlineMagRangeEditor->setWindow(lo, hi); m_streamlineMagRangeEditor->blockSignals(false);
                }
            }
        });

        connect(m_settings, &RenderSettings::meshLoadStateChanged, this, [this, bindToActiveMode](){
            bindToActiveMode();
        });
        connect(m_settings, &RenderSettings::meshDataUpdated, this, [this, bindToActiveMode](){
            bindToActiveMode();
        });
    }

    addSliderRow(m_settings, lookLay, "opacityLabel", "Opacity",
                 m_settings->getStreamlineOpacity(), 0.0, 1.0, 2,
                 &RenderSettings::setStreamlineOpacity);
    addSliderRow(m_settings, lookLay, "ribbonWidthLabel", "Ribbon Width",
                 m_settings->getStreamlineRibbonWidth(), 0.001, 0.05, 3,
                 &RenderSettings::setStreamlineRibbonWidth);
    addSliderRow(m_settings, lookLay, "taperFactorLabel", "Taper Factor",
                 m_settings->getStreamlineTaperFactor(), 0.0, 0.8, 2,
                 &RenderSettings::setStreamlineTaperFactor);

    auto* shadingHeader = new QLabel("Shading");
    shadingHeader->setObjectName("slShadingHeader");
    lookLay->addWidget(shadingHeader);

    addSliderRow(m_settings, lookLay, "streamlineAmbientLabel", "Ambient",
                 m_settings->getStreamlineAmbient(), 0.0, 1.0, 2,
                 &RenderSettings::setStreamlineAmbient);
    addSliderRow(m_settings, lookLay, "streamlineDiffuseLabel", "Diffuse",
                 m_settings->getStreamlineDiffuse(), 0.0, 1.0, 2,
                 &RenderSettings::setStreamlineDiffuse);
    addSliderRow(m_settings, lookLay, "streamlineSpecularLabel", "Specular",
                 m_settings->getStreamlineSpecular(), 0.0, 1.0, 2,
                 &RenderSettings::setStreamlineSpecular);

    auto* specPowerSpin = new QSpinBox;
    specPowerSpin->setRange(2, 128);
    specPowerSpin->setValue(m_settings->getStreamlineSpecularPower());
    connect(specPowerSpin, &QSpinBox::valueChanged, m_settings, &RenderSettings::setStreamlineSpecularPower);
    addCtlRow(lookLay, "specPowerLabel", "Specular Power", specPowerSpin, 1);
    tabs->addTab(lookTab, tr("Look"));

    // Animate section (below the tabs so the tab bar stays arrow-free)

    auto* optionsLay = qobject_cast<QVBoxLayout*>(slUi.optionsGroup->layout());

    auto* particlesCb = new QCheckBox("Show Particles");
    particlesCb->setChecked(m_settings->getShowParticles());
    connect(particlesCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowParticles);
    optionsLay->addWidget(particlesCb);

    {
        auto row = createLightSlider("Particle Count", m_settings->getParticleCount(),
                                     10.0, 5000.0, 1.0, 0,
                                     [this](double v) { m_settings->setParticleCount(static_cast<int>(v)); },
                                     "particleCountLabel");
        optionsLay->addWidget(row.slider->parentWidget());
    }
    addSliderRow(m_settings, optionsLay, "particleSpeedLabel", "Particle Speed",
                 m_settings->getParticleSpeed(), 0.1, 100.0, 1,
                 &RenderSettings::setParticleSpeed);
    addSliderRow(m_settings, optionsLay, "particleSizeLabel", "Particle Size",
                 m_settings->getParticleSize(), 1.0, 20.0, 1,
                 &RenderSettings::setParticleSize);

    auto* particleAdditiveCb = new QCheckBox("Additive Glow");
    particleAdditiveCb->setChecked(m_settings->getParticleAdditive());
    connect(particleAdditiveCb, &QCheckBox::toggled, m_settings, &RenderSettings::setParticleAdditive);
    optionsLay->addWidget(particleAdditiveCb);

    tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    optionsLay->addStretch();
    scroll->setWidget(content);

    applyPanelStyling(content);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}


// Section: Volume Rendering (6)

QWidget* MainWindow::buildScreenshotPage() {
    auto* page = new QWidget;
    Ui::ScreenshotPage ssUi;
    ssUi.setupUi(page);
    fixLayoutOverflow(page);

    auto* saveBtn = ssUi.saveBtn;
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::saveScreenshot);

    auto* transCb = ssUi.transCb;
    transCb->setChecked(m_settings->getScreenshotTransparent());
    connect(transCb, &QCheckBox::toggled, m_settings, &RenderSettings::setScreenshotTransparent);

    m_ssResCombo = ssUi.resCombo;
    m_ssResCombo->setMinimumWidth(kSidebarWidth - m_navWidth - 20);
    m_ssResCombo->setCurrentIndex(m_settings->getScreenshotResolution());
    connect(m_ssResCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            m_settings, &RenderSettings::setScreenshotResolution);

    m_ssAaCombo = ssUi.aaCombo;
    m_ssAaCombo->setMinimumWidth(kSidebarWidth - m_navWidth - 20);
    m_ssAaCombo->setCurrentIndex(m_settings->getScreenshotAASamples() <= 0 ? 0 : m_settings->getScreenshotAASamples() <= 2 ? 1 : 2);
    connect(m_ssAaCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                const int samples = idx == 0 ? 0 : (idx == 1 ? 2 : 4);
                m_settings->setScreenshotAASamples(samples);
            });

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    qobject_cast<QVBoxLayout*>(page->layout())->addStretch();

    scroll->setWidget(page);

    applyPanelStyling(page);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}


// Section: Animation (9)

QWidget* MainWindow::buildAnimationPage() {
    auto* page = new QWidget;
    Ui::AnimationPage animUi;
    animUi.setupUi(page);
    fixLayoutOverflow(page);

    auto* ctrl = m_settings->anim();

    m_animPlayBtn      = animUi.playBtn;
    m_animStepBackBtn  = animUi.stepBackBtn;
    m_animStepFwdBtn   = animUi.stepFwdBtn;
    m_animSlider       = animUi.frameSlider;
    m_animTimeLabel    = animUi.timeLabel;
    m_animFrameLabel   = animUi.frameLabel;
    m_animStatusLabel  = animUi.statusLabel;
    m_animSequenceLabel = animUi.sequenceLabel;
    m_animLoopCb       = animUi.loopCb;
    m_animFpsSpin      = animUi.fpsSpin;
    m_animScaleCombo   = animUi.scaleCombo;
    m_animExportBtn    = animUi.exportBtn;

    // Transport
    connect(m_animPlayBtn, &QPushButton::clicked, ctrl, &AnimationController::togglePlay);
    connect(m_animStepBackBtn, &QToolButton::clicked, ctrl, &AnimationController::stepBackward);
    connect(m_animStepFwdBtn, &QToolButton::clicked, ctrl, &AnimationController::stepForward);
    connect(m_animLoopCb, &QCheckBox::toggled, ctrl, &AnimationController::setLoop);
    connect(m_animFpsSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            ctrl, &AnimationController::setFps);
    connect(m_animScaleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int idx) { m_settings->setAnimScaleGlobal(idx == 0); });

    // Timeline scrubbing: debounce dragging to avoid queue churn (see review 5.5).
    // sliderMoved fires only on user drag (not programmatic setValue), so we
    // coalesce rapid drags with a 50ms single-shot timer. Clicks on the track
    // (valueChanged without drag) seek immediately; release seeks to final.
    m_animSeekDebounce.setSingleShot(true);
    m_animSeekDebounce.setInterval(50);
    connect(&m_animSeekDebounce, &QTimer::timeout, this, [this]() {
        if (m_pendingSeekFrame >= 0) {
            m_settings->anim()->seek(m_pendingSeekFrame);
            m_pendingSeekFrame = -1;
        }
    });
    connect(m_animSlider, &QSlider::sliderMoved, this, [this](int v) {
        m_pendingSeekFrame = v;
        m_animSeekDebounce.start();
    });
    connect(m_animSlider, &QSlider::sliderReleased, this, [this]() {
        m_animSeekDebounce.stop();
        m_pendingSeekFrame = -1;
        m_settings->anim()->seek(m_animSlider->value());
    });
    connect(m_animSlider, &QSlider::valueChanged, this, [this](int v) {
        if (!m_animSlider->isSliderDown()) {
            // Track click (programmatic setValue is blocked in refreshAnimationPage)
            m_settings->anim()->seek(v);
        }
    });

    // Open a .pvd straight from the page.
    connect(animUi.openPvdBtn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "Load PVD Animation", QString(),
            "PVD collections (*.pvd);;All files (*)");
        if (!path.isEmpty()) m_settings->loadMesh(path);
    });

    // Export the loaded sequence (moved here from the File menu).
    connect(m_animExportBtn, &QPushButton::clicked,
            this, &MainWindow::exportAnimation);

    // Controller → UI sync (stateChanged covers play/pause/seek/load).
    connect(ctrl, &AnimationController::stateChanged,
            this, &MainWindow::refreshAnimationPage);
    QTimer::singleShot(0, this, &MainWindow::refreshAnimationPage);
    return page;
}

void MainWindow::refreshAnimationPage() {
    auto* ctrl = m_settings->anim();
    const bool has = ctrl->hasSequence();

    m_animPlayBtn->setEnabled(has);
    m_animStepBackBtn->setEnabled(has);
    m_animStepFwdBtn->setEnabled(has);
    m_animLoopCb->setEnabled(has);
    m_animFpsSpin->setEnabled(has);
    m_animScaleCombo->setEnabled(has);
    m_animSlider->setEnabled(has);
    m_animExportBtn->setEnabled(has);

    if (!has) {
        m_animSequenceLabel->setText("No sequence loaded");
        m_animStatusLabel->setText(QString());
        m_animFrameLabel->setText("Frame —");
        m_animTimeLabel->setText("t = —");
        m_animPlayBtn->setText("Play");
        return;
    }

    m_animPlayBtn->setText(ctrl->isPlaying() ? "Pause" : "Play");
    m_animLoopCb->blockSignals(true);
    m_animLoopCb->setChecked(ctrl->loop());
    m_animLoopCb->blockSignals(false);
    m_animFpsSpin->blockSignals(true);
    m_animFpsSpin->setValue(ctrl->fps());
    m_animFpsSpin->blockSignals(false);
    m_animScaleCombo->blockSignals(true);
    m_animScaleCombo->setCurrentIndex(m_settings->getAnimScaleGlobal() ? 0 : 1);
    m_animScaleCombo->blockSignals(false);

    const int n = ctrl->frameCount();
    m_animSlider->blockSignals(true);
    m_animSlider->setRange(0, std::max(0, n - 1));
    m_animSlider->setValue(std::clamp(ctrl->currentFrame(), 0, std::max(0, n - 1)));
    m_animSlider->blockSignals(false);

    const int cur = ctrl->currentFrame();
    m_animFrameLabel->setText((cur >= 0)
        ? QString("Frame %1 / %2").arg(cur + 1).arg(n)
        : QString("Frame — / %1").arg(n));
    m_animTimeLabel->setText(QString("t = %1 s").arg(ctrl->currentTime(), 0, 'f', 3));
    m_animSequenceLabel->setText(ctrl->sequenceName());

    if (ctrl->isBuffering()) {
        m_animStatusLabel->setText("Buffering…");
        m_animStatusLabel->setStyleSheet("color: #FFAA44;");
    } else {
        m_animStatusLabel->setText(QString());
        m_animStatusLabel->setStyleSheet(QString());
    }
}

QWidget* MainWindow::buildMeshInfoPage() {
    auto* page = new QWidget;
    Ui::MeshInfoPage meshUi;
    meshUi.setupUi(page);
    fixLayoutOverflow(page);
    m_meshInfoLabels.clear();

    auto addInfoRow = [this, &meshUi](const QString& label, QLabel* valueWidget, const QString& value) {
        valueWidget->setText(value);
        m_meshInfoLabels[label] = valueWidget;
    };

    addInfoRow("Type", meshUi.typeValue, m_settings->getMeshDataType());
    addInfoRow("Format", meshUi.formatValue, m_settings->getMeshFormat());

    addInfoRow("Triangles", meshUi.trianglesValue, QString::number(m_settings->getTriangleCount()));
    addInfoRow("Points", meshUi.pointsValue, QString::number(m_settings->getPointCount()));

    addInfoRow("Degenerate", meshUi.degenerateValue, QString::number(m_settings->getDegenerateFaces()));
    addInfoRow("Open Edges", meshUi.openEdgesValue, QString::number(m_settings->getOpenEdges()));
    addInfoRow("Non-manifold Edge", meshUi.nonManifoldEValue, QString::number(m_settings->getNonManifoldEdges()));
    addInfoRow("Non-manifold Vertex", meshUi.nonManifoldVValue, QString::number(m_settings->getNonManifoldVerts()));
    addInfoRow("Watertight", meshUi.watertightValue, m_settings->getWatertight() ? "Yes" : "No");

    // Quality overlay colors — match QualityOverlayRenderer defect colors
    if (m_settings->getDegenerateFaces() > 0)
        meshUi.degenerateValue->setStyleSheet("color: #FF6666; font-weight: bold;");
    if (m_settings->getOpenEdges() > 0)
        meshUi.openEdgesValue->setStyleSheet("color: #FFAA44; font-weight: bold;");
    if (m_settings->getNonManifoldEdges() > 0)
        meshUi.nonManifoldEValue->setStyleSheet("color: #FF44FF; font-weight: bold;");
    if (m_settings->getNonManifoldVerts() > 0)
        meshUi.nonManifoldVValue->setStyleSheet("color: #FF44FF; font-weight: bold;");
    meshUi.watertightValue->setStyleSheet(
        m_settings->getWatertight() ? "color: #4CAF50;" : "color: #FF6666;");

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
    qobject_cast<QVBoxLayout*>(page->layout())->addStretch();

    scroll->setWidget(page);

    applyPanelStyling(page);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}


// Section: Screenshot (8)

QWidget* MainWindow::buildVolumePage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget;
    Ui::VolumePage volumeUi;
    volumeUi.setupUi(content);
    fixLayoutOverflow(content);

    auto* showCb = volumeUi.showVolumeCb;
    m_volumeShowCb = showCb;
    m_volumeOptionsGroup = volumeUi.optionsGroup;
    showCb->setChecked(m_settings->getShowVolume());
    showCb->setEnabled(m_settings->hasVolumeData());
    if (!m_settings->hasVolumeData()) showCb->setChecked(false);
    connect(showCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowVolume);
    // Full-volume controls (step size, opacity) are individually disabled when showVolume is off.
    connect(showCb, &QCheckBox::toggled, this, [this](bool) { applyVolumeControlGating(); });
    applyVolumeControlGating();

    m_volumeFieldCombo = volumeUi.volumeFieldCombo;
    m_volumeFieldCombo->addItems(m_settings->getAvailableScalars());
    m_volumeFieldCombo->setCurrentText(m_settings->getActiveScalarNameQml());
    m_volumeFieldCombo->setEnabled(m_settings->hasMeshScalars());
    m_volumeFieldCombo->setMinimumWidth(kSidebarWidth - m_navWidth - 20);
    connect(m_volumeFieldCombo, &QComboBox::activated, m_settings, [this](int idx) {
        m_volumeFieldCombo->setCurrentIndex(idx);
        m_settings->setActiveScalarField(m_volumeFieldCombo->itemText(idx));
    });

    auto* paletteCombo = buildColormapCombo(m_settings->getVolumeColormapChoice(),
        [this](int i) { m_settings->setVolumeColormapChoice(i); });
    paletteCombo->setMinimumWidth(kSidebarWidth - m_navWidth - 20);
    volumeUi.optionsGroup->layout()->replaceWidget(volumeUi.volumePaletteCombo, paletteCombo);
    delete volumeUi.volumePaletteCombo;

    auto* reversedCb = volumeUi.volumeReverseCb;
    reversedCb->setChecked(m_settings->getVolumeColormapReversed());
    connect(reversedCb, &QCheckBox::toggled, m_settings, &RenderSettings::setVolumeColormapReversed);

    auto* useCmapCb = volumeUi.volumeUseCmapCb;
    useCmapCb->setChecked(m_settings->getVolumeUseColormap());
    connect(useCmapCb, &QCheckBox::toggled, m_settings, &RenderSettings::setVolumeUseColormap);

    // ---- Volume Fixed Range (RangeEditor) ----
    {
        auto* lay = qobject_cast<QVBoxLayout*>(volumeUi.optionsGroup->layout());
        auto* hdr = new QLabel("Volume Fixed Range");
        hdr->setObjectName("volumeFixedHeader");
        lay->addWidget(hdr);
        m_volumeRangeCb = new QCheckBox("Fixed Range");
        m_volumeRangeCb->setToolTip("Map a fixed value range to the volume palette; values outside clamp to the end colors");
        m_volumeRangeCb->setChecked(m_settings->getVolumeColorRangeOverrideEnabled());
        m_volumeRangeCb->setEnabled(m_settings->hasVolumeData());
        lay->addWidget(m_volumeRangeCb);
        m_volumeRangeEditor = new RangeEditor();
        lay->addWidget(m_volumeRangeEditor);
        m_volumeRangeEditor->setEnabled(m_volumeRangeCb->isChecked() && m_volumeRangeCb->isEnabled());
        // init bounds/window
        {
            double mn = m_settings->getDataScalarMinQml(), mx = m_settings->getDataScalarMaxQml();
            if (!(mx > mn)) { mn = 0; mx = 1; }
            m_volumeRangeEditor->setBounds(mn, mx);
            m_volumeRangeEditor->setWindow(m_settings->getVolumeColorRangeLo(), m_settings->getVolumeColorRangeHi());
        }
        connect(m_volumeRangeCb, &QCheckBox::toggled, this, [this](bool v){
            m_settings->setVolumeColorRangeOverrideEnabled(v);
            if (m_volumeRangeEditor) m_volumeRangeEditor->setEnabled(v && m_volumeRangeCb->isEnabled());
        });
        connect(m_volumeRangeEditor, &RangeEditor::windowEdited, this, [this](double lo, double hi){
            m_settings->setVolumeColorRangeLo(static_cast<float>(lo));
            m_settings->setVolumeColorRangeHi(static_cast<float>(hi));
        });
        connect(m_settings, &RenderSettings::viewChanged, this, [this](ChangeFlags f){
            if (!m_volumeRangeCb || !m_volumeRangeEditor) return;
            if (f & ChangeFlag::Colormap) {
                bool en = m_settings->getVolumeColorRangeOverrideEnabled();
                m_volumeRangeCb->blockSignals(true); m_volumeRangeCb->setChecked(en); m_volumeRangeCb->blockSignals(false);
                m_volumeRangeEditor->setEnabled(en && m_volumeRangeCb->isEnabled());
                auto cur = m_volumeRangeEditor->window();
                double lo = m_settings->getVolumeColorRangeLo(), hi = m_settings->getVolumeColorRangeHi();
                if (cur.first != lo || cur.second != hi) {
                    m_volumeRangeEditor->blockSignals(true); m_volumeRangeEditor->setWindow(lo,hi); m_volumeRangeEditor->blockSignals(false);
                }
            }
        });
        connect(m_settings, &RenderSettings::meshLoadStateChanged, this, [this](){
            if (!m_volumeRangeCb || !m_volumeRangeEditor) return;
            bool ok = m_settings->hasVolumeData();
            m_volumeRangeCb->setEnabled(ok);
            if (!ok) { m_volumeRangeCb->blockSignals(true); m_volumeRangeCb->setChecked(false); m_volumeRangeCb->blockSignals(false); }
            m_volumeRangeEditor->setEnabled(m_volumeRangeCb->isChecked() && ok);
        });
    }

    {
        auto* slider = volumeUi.stepSlider;
        auto* valueLabel = volumeUi.stepValue;
        slider->setRange(1, 1000);
        slider->setValue(static_cast<int>(std::log10(m_settings->getVolumeStepSize()) * 1000.0 + 4000.0));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = std::pow(10.0, (raw - 4000.0) / 1000.0);
            valueLabel->setText(QString::number(v, 'f', 4));
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

    // Full-volume-rendering-only controls: greyed when "Enable Volume Rendering" is off.
    m_volumeRenderCtrls << volumeUi.stepLabel << volumeUi.stepSlider << volumeUi.stepValue
                        << volumeUi.opacityLabel << volumeUi.opacitySlider << volumeUi.opacityValue;
    applyVolumeControlGating();

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

    scroll->setWidget(content);

    applyPanelStyling(content);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}

// Section: Slice Plane (7)

QWidget* MainWindow::buildSlicePlanePage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget;
    Ui::SlicePlanePage sliceUi;
    sliceUi.setupUi(content);
    fixLayoutOverflow(content);

    // -- Show slice checkbox --
    auto* showCb = sliceUi.showSliceCb;
    m_volumeSliceShowCb = showCb;
    showCb->setChecked(m_settings->getShowVolumeSlice());
    showCb->setEnabled(m_settings->hasVolumeData());
    if (!m_settings->hasVolumeData()) showCb->setChecked(false);
    connect(showCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowVolumeSlice);
    connect(showCb, &QCheckBox::toggled, sliceUi.optionsGroup, &QWidget::setEnabled);
    sliceUi.optionsGroup->setEnabled(m_settings->getShowVolumeSlice());

    // -- Slice axis radio buttons --
    auto* axisX = sliceUi.axisX;
    auto* axisY = sliceUi.axisY;
    auto* axisZ = sliceUi.axisZ;
    m_sliceAxisXRb = axisX;
    m_sliceAxisYRb = axisY;
    m_sliceAxisZRb = axisZ;
    int currentAxis = m_settings->getVolumeSliceAxis();
    axisX->setChecked(currentAxis == 0);
    axisY->setChecked(currentAxis == 1);
    axisZ->setChecked(currentAxis == 2);
    auto* axisGroup = new QButtonGroup(this);
    axisGroup->addButton(axisX, 0);
    axisGroup->addButton(axisY, 1);
    axisGroup->addButton(axisZ, 2);
    connect(axisGroup, &QButtonGroup::idToggled, m_settings, [this](int id, bool checked) {
        if (checked) m_settings->setVolumeSliceAxis(id);
    });

    // -- Slice position slider --
    {
        auto* slider = sliceUi.posSlider;
        auto* valueLabel = sliceUi.posValue;
        m_slicePosSlider = slider;
        m_slicePosValue = valueLabel;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(m_settings->getVolumeSlicePos() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v * 100.0, 'f', 0) + "%");
            m_settings->setVolumeSlicePos(v);
        });
    }

    // -- Slice opacity slider --
    {
        auto* slider = sliceUi.opacitySlider;
        auto* valueLabel = sliceUi.opacityValue;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(m_settings->getVolumeSliceOpacity() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, this](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v * 100.0, 'f', 0) + "%");
            m_settings->setVolumeSliceOpacity(v);
        });
    }

    // -- Slice Fixed Range (RangeEditor) --
    {
        auto* container = sliceUi.rangeContainer;
        auto* lay = qobject_cast<QHBoxLayout*>(container->layout());
        if (!lay) { lay = new QHBoxLayout(container); lay->setContentsMargins(0,0,0,0); }
        m_sliceRangeEditor = new RangeEditor(container);
        lay->addWidget(m_sliceRangeEditor);
        m_sliceRangeCb = sliceUi.rangeCb;
        m_sliceRangeCb->setToolTip("Map a fixed value range to the slice palette; values outside clamp to the end colors");
        m_sliceRangeCb->setChecked(m_settings->getSliceColorRangeOverrideEnabled());
        m_sliceRangeCb->setEnabled(m_settings->hasVolumeData());
        m_sliceRangeEditor->setEnabled(m_sliceRangeCb->isChecked() && m_sliceRangeCb->isEnabled());
        {
            double mn = m_settings->getDataScalarMinQml(), mx = m_settings->getDataScalarMaxQml();
            if (!(mx > mn)) { mn = 0; mx = 1; }
            m_sliceRangeEditor->setBounds(mn, mx);
            m_sliceRangeEditor->setWindow(m_settings->getSliceColorRangeLo(), m_settings->getSliceColorRangeHi());
        }
        connect(m_sliceRangeCb, &QCheckBox::toggled, this, [this](bool v){
            m_settings->setSliceColorRangeOverrideEnabled(v);
            if (m_sliceRangeEditor) m_sliceRangeEditor->setEnabled(v && m_sliceRangeCb->isEnabled());
        });
        connect(m_sliceRangeEditor, &RangeEditor::windowEdited, this, [this](double lo, double hi){
            m_settings->setSliceColorRangeLo(static_cast<float>(lo));
            m_settings->setSliceColorRangeHi(static_cast<float>(hi));
        });
        connect(m_settings, &RenderSettings::viewChanged, this, [this](ChangeFlags f){
            if (!m_sliceRangeCb || !m_sliceRangeEditor) return;
            if (f & ChangeFlag::Colormap) {
                bool en = m_settings->getSliceColorRangeOverrideEnabled();
                m_sliceRangeCb->blockSignals(true); m_sliceRangeCb->setChecked(en); m_sliceRangeCb->blockSignals(false);
                m_sliceRangeEditor->setEnabled(en && m_sliceRangeCb->isEnabled());
                auto cur = m_sliceRangeEditor->window();
                double lo = m_settings->getSliceColorRangeLo(), hi = m_settings->getSliceColorRangeHi();
                if (cur.first != lo || cur.second != hi) { m_sliceRangeEditor->blockSignals(true); m_sliceRangeEditor->setWindow(lo,hi); m_sliceRangeEditor->blockSignals(false); }
            }
        });
        connect(m_settings, &RenderSettings::meshLoadStateChanged, this, [this](){
            if (!m_sliceRangeCb || !m_sliceRangeEditor) return;
            bool ok = m_settings->hasVolumeData();
            m_sliceRangeCb->setEnabled(ok);
            if (!ok) { m_sliceRangeCb->blockSignals(true); m_sliceRangeCb->setChecked(false); m_sliceRangeCb->blockSignals(false); }
            m_sliceRangeEditor->setEnabled(m_sliceRangeCb->isChecked() && ok);
        });
    }

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

    scroll->setWidget(content);

    applyPanelStyling(content);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}

// Section: Isosurface (8)

QWidget* MainWindow::buildIsosurfacePage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget;
    Ui::IsosurfacePage isoUi;
    isoUi.setupUi(content);
    fixLayoutOverflow(content);

    // -- Isosurface enable checkbox --
    auto* enableCb = isoUi.enableCb;
    m_isoEnableCb = enableCb;
    enableCb->setChecked(m_settings->getShowIsosurface());
    enableCb->setEnabled(m_settings->getIsosurfaceAvailable() || m_settings->hasVolumeData());
    if (!m_settings->getIsosurfaceAvailable() && !m_settings->hasVolumeData()) enableCb->setChecked(false);
    connect(enableCb, &QCheckBox::toggled, m_settings, &RenderSettings::setShowIsosurface);
    connect(enableCb, &QCheckBox::toggled, isoUi.optionsGroup, &QWidget::setEnabled);
    isoUi.optionsGroup->setEnabled(enableCb->isChecked() && enableCb->isEnabled());

    // -- Isovalue slider --
    auto* slider = isoUi.valueSlider;
    auto* valueLabel = isoUi.valueLabel;
    m_isoValueSlider = slider;
    m_isoValueLabel = valueLabel;
    slider->setRange(0, 1000);
    refreshIsosurfaceSlider();
    connect(slider, &QSlider::valueChanged, this, [this, valueLabel](int raw) {
        double lo = m_settings->getDataScalarMinQml();
        double hi = m_settings->getDataScalarMaxQml();
        if (hi <= lo) hi = lo + 1.0;
        double v = lo + (raw / 1000.0) * (hi - lo);
        m_settings->setIsovalue(v);
        valueLabel->setText(QString::number(m_settings->getIsovalue(), 'f', 3));
    });

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

    scroll->setWidget(content);

    applyPanelStyling(content);

    auto* wrapper = new QWidget;
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scroll);
    return wrapper;
}

void MainWindow::refreshMeshInfoPage() {
    if (!m_meshInfoPage) return;

    auto setInfo = [&](const QString& label, const QString& value) {
        auto it = m_meshInfoLabels.find(label);
        if (it == m_meshInfoLabels.end()) return;
        it.value()->setText(value);
    };
    setInfo("Type",       m_settings->getMeshDataType());
    setInfo("Format",     m_settings->getMeshFormat());
    setInfo("Triangles",  QString::number(m_settings->getTriangleCount()));
    setInfo("Points",     QString::number(m_settings->getPointCount()));
    setInfo("Degenerate", QString::number(m_settings->getDegenerateFaces()));
    setInfo("Open Edges", QString::number(m_settings->getOpenEdges()));
    setInfo("Non-manifold Edge", QString::number(m_settings->getNonManifoldEdges()));
    setInfo("Non-manifold Vertex", QString::number(m_settings->getNonManifoldVerts()));
    setInfo("Watertight", m_settings->getWatertight() ? "Yes" : "No");

    // Quality overlay colors — match QualityOverlayRenderer defect colors
    auto colorQuality = [&](const QString& label, int count, const char* defectColor) {
        auto it = m_meshInfoLabels.find(label);
        if (it == m_meshInfoLabels.end()) return;
        it.value()->setStyleSheet(count > 0
            ? QString("color: %1; font-weight: bold;").arg(defectColor)
            : QString());
    };
    colorQuality("Degenerate",         m_settings->getDegenerateFaces(),   "#FF6666");
    colorQuality("Open Edges",         m_settings->getOpenEdges(),         "#FFAA44");
    colorQuality("Non-manifold Edge",  m_settings->getNonManifoldEdges(),  "#FF44FF");
    colorQuality("Non-manifold Vertex",m_settings->getNonManifoldVerts(),  "#FF44FF");
    auto wtIt = m_meshInfoLabels.find("Watertight");
    if (wtIt != m_meshInfoLabels.end())
        wtIt.value()->setStyleSheet(m_settings->getWatertight() ? "color: #4CAF50;" : "color: #FF6666;");

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

void MainWindow::refreshSlicingPageBounds() {
    if (m_sliceEnableCb) m_sliceEnableCb->setChecked(m_settings->getClipEnabled());
    if (m_sliceCrinkleCb) {
        m_sliceCrinkleCb->setEnabled(m_settings->getClipEnabled());
        m_sliceCrinkleCb->setChecked(m_settings->getCrinkleClipMode());
    }
    if (m_sliceAxisXCb)  m_sliceAxisXCb->setChecked(m_settings->getSliceEnabledX());
    if (m_sliceAxisYCb)  m_sliceAxisYCb->setChecked(m_settings->getSliceEnabledY());
    if (m_sliceAxisZCb)  m_sliceAxisZCb->setChecked(m_settings->getSliceEnabledZ());

    auto updateSlider = [](QSlider* slider, QLabel* valueLabel, double from, double to, double val) {
        if (!slider || !valueLabel) return;
        int minI = static_cast<int>(from * 1000);
        int maxI = static_cast<int>(to * 1000);
        slider->setRange(minI, maxI);
        double center = (from + to) * 0.5;
        slider->blockSignals(true);
        slider->setValue(static_cast<int>(center * 1000));
        slider->blockSignals(false);
        valueLabel->setText(QString::number(center, 'f', 3));
    };
    updateSlider(m_sliceXSlider, m_sliceXValue,
                 m_settings->getWorldMinX(), m_settings->getWorldMaxX(), m_settings->getSliceX());
    updateSlider(m_sliceYSlider, m_sliceYValue,
                 m_settings->getWorldMinY(), m_settings->getWorldMaxY(), m_settings->getSliceY());
    updateSlider(m_sliceZSlider, m_sliceZValue,
                  m_settings->getWorldMinZ(), m_settings->getWorldMaxZ(), m_settings->getSliceZ());
}

void MainWindow::refreshScalarFilterRange() {
    const double minVal = m_settings->getDataScalarMinQml();
    const double maxVal = m_settings->getDataScalarMaxQml();
    auto applyOne = [minVal, maxVal](QSlider* slider, QLineEdit* field, double filterVal) {
        if (!slider || !field) return;
        slider->setRange(static_cast<int>(std::lround(minVal * 1000.0)), static_cast<int>(std::lround(maxVal * 1000.0)));
        int vi = static_cast<int>(std::lround(filterVal * 1000.0));
        slider->blockSignals(true);
        slider->setValue(vi);
        slider->blockSignals(false);
        field->setText(QString::number(filterVal, 'f', 3));
    };
    applyOne(m_filterMinSlider, m_filterMinField, m_settings->getFilterMin());
    applyOne(m_filterMaxSlider, m_filterMaxField, m_settings->getFilterMax());
}

// Slider bounds + displayed values for the fixed colormap range; follows the
// data range like the filter controls so a new dataset rescales the widgets.
void MainWindow::refreshColorRangeBounds() {
    if (!m_colorRangeEditor) return;
    const double minVal = m_settings->getDataScalarMinQml();
    const double maxVal = m_settings->getDataScalarMaxQml();
    const double lo = m_settings->getColorRangeLo();
    const double hi = m_settings->getColorRangeHi();
    if (!(maxVal > minVal)) {
        m_colorRangeEditor->blockSignals(true);
        m_colorRangeEditor->setBounds(minVal, minVal + 1.0);
        m_colorRangeEditor->setWindow(lo, hi);
        m_colorRangeEditor->blockSignals(false);
        return;
    }
    m_colorRangeEditor->blockSignals(true);
    m_colorRangeEditor->setBounds(minVal, maxVal);
    m_colorRangeEditor->setWindow(lo, hi);
    m_colorRangeEditor->blockSignals(false);
}
void MainWindow::refreshVolumeRangeBounds() {
    if (!m_volumeRangeEditor) return;
    const double minVal = m_settings->getDataScalarMinQml();
    const double maxVal = m_settings->getDataScalarMaxQml();
    const double lo = m_settings->getVolumeColorRangeLo();
    const double hi = m_settings->getVolumeColorRangeHi();
    if (!(maxVal > minVal)) { m_volumeRangeEditor->blockSignals(true); m_volumeRangeEditor->setBounds(minVal, minVal+1.0); m_volumeRangeEditor->setWindow(lo,hi); m_volumeRangeEditor->blockSignals(false); return; }
    m_volumeRangeEditor->blockSignals(true); m_volumeRangeEditor->setBounds(minVal, maxVal); m_volumeRangeEditor->setWindow(lo,hi); m_volumeRangeEditor->blockSignals(false);
}
void MainWindow::refreshSliceRangeBounds() {
    if (!m_sliceRangeEditor) return;
    const double minVal = m_settings->getDataScalarMinQml();
    const double maxVal = m_settings->getDataScalarMaxQml();
    const double lo = m_settings->getSliceColorRangeLo();
    const double hi = m_settings->getSliceColorRangeHi();
    if (!(maxVal > minVal)) { m_sliceRangeEditor->blockSignals(true); m_sliceRangeEditor->setBounds(minVal, minVal+1.0); m_sliceRangeEditor->setWindow(lo,hi); m_sliceRangeEditor->blockSignals(false); return; }
    m_sliceRangeEditor->blockSignals(true); m_sliceRangeEditor->setBounds(minVal, maxVal); m_sliceRangeEditor->setWindow(lo,hi); m_sliceRangeEditor->blockSignals(false);
}
void MainWindow::refreshGlyphMagRangeBounds() {
    if (!m_glyphMagRangeEditor) return;
    int comp = m_glyphRangeBoundComp;
    double lo, hi, bLo = 0, bHi = 1;
    if (comp < 0) {
        lo = m_settings->getGlyphMagRangeLo();
        hi = m_settings->getGlyphMagRangeHi();
        if (m_settings->backend()) { bLo = m_settings->backend()->vectorMagMin(); bHi = m_settings->backend()->vectorMagMax(); if (!(bHi > bLo)) { bHi = bLo + 1.0; } }
    } else {
        lo = m_settings->getGlyphCompRangeLo(comp);
        hi = m_settings->getGlyphCompRangeHi(comp);
        if (m_settings->backend()) { bLo = m_settings->backend()->vectorCompMin(comp); bHi = m_settings->backend()->vectorCompMax(comp); if (!(bHi > bLo)) { bHi = bLo + 1.0; } }
    }
    m_glyphMagRangeEditor->blockSignals(true); m_glyphMagRangeEditor->setBounds(bLo, bHi); if (hi > lo) m_glyphMagRangeEditor->setWindow(lo, hi); else m_glyphMagRangeEditor->setWindow(bLo, bHi); m_glyphMagRangeEditor->blockSignals(false);
}
void MainWindow::refreshStreamlineMagRangeBounds() {
    if (!m_streamlineMagRangeEditor) return;
    double lo = m_settings->getStreamlineMagRangeLo();
    double hi = m_settings->getStreamlineMagRangeHi();
    double bLo = 0, bHi = 1;
    if (m_settings->backend()) { bLo = m_settings->backend()->streamlineMagMin(); bHi = m_settings->backend()->streamlineMagMax(); if (!(bHi > bLo)) { bHi = bLo + 1.0; } }
    else { bLo = std::min(lo,hi); bHi = std::max(lo,hi); if (!(bHi > bLo)) bHi = bLo + 1.0; }
    m_streamlineMagRangeEditor->blockSignals(true); m_streamlineMagRangeEditor->setBounds(bLo, bHi); if (hi > lo) m_streamlineMagRangeEditor->setWindow(lo,hi); else m_streamlineMagRangeEditor->setWindow(bLo,bHi); m_streamlineMagRangeEditor->blockSignals(false);
}


// Sidebar section switching

static const char* sectionNames[] = {
    "Mesh Info", "Lighting", "Slicing", "View & Display", "Scalar",
    "Vectors", "Streamlines", "Volume Rendering", "Slice Plane", "Isosurface",
    "Screenshot", "Animation"
};

void MainWindow::setSidebarSection(int section) {
    if (m_activeSection == section && m_sidebarExpanded) {
        // Collapse
        m_activeSection = -1;
        m_sidebarExpanded = false;
        m_sectionStack->setVisible(false);
        m_panelHeader->setVisible(false);
        m_navList->clearSelection();
    } else {
        // Expand
        m_activeSection = section;
        m_sidebarExpanded = true;
        m_sectionStack->setCurrentIndex(section);
        m_sectionStack->setVisible(true);
        m_panelHeader->setVisible(true);
        m_panelTitle->setText(QString::fromUtf8(sectionNames[section]));
        m_navList->setCurrentRow(section, QItemSelectionModel::ClearAndSelect);
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
        "QToolBar { border: none; padding: 4px; spacing: 4px; }"
        "QToolButton { background: transparent; border: 1px solid transparent; border-radius: 5px; padding: 4px 8px; }"
        "QToolButton:hover { background: palette(midlight); }"
        "QToolButton:checked { background: palette(highlight); border: 1px solid palette(highlight); }");
    m_topToolbar->setIconSize(QSize(20, 20));
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

    m_tbSlice = m_topToolbar->addAction(QIcon(":/src/resources/icons/slice.svg"), "Slice");
    m_tbSlice->setCheckable(true);
    m_tbSlice->setChecked(m_settings->getShowVolumeSlice());
    m_tbSlice->setEnabled(m_settings->hasVolumeData());
    connect(m_tbSlice, &QAction::toggled, m_settings, &RenderSettings::setShowVolumeSlice);

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

    addToolBar(Qt::TopToolBarArea, m_topToolbar);
}

void MainWindow::syncTopToolbar() {
    if (m_tbWireframe) m_tbWireframe->setChecked(m_settings->isWireframe());
    if (m_tbSurface)   m_tbSurface->setChecked(m_settings->isSurfaceVisible());
    if (m_tbVolume) {
        m_tbVolume->setEnabled(m_settings->hasVolumeData());
        m_tbVolume->setChecked(m_settings->getShowVolume());
    }
    if (m_tbSlice) {
        m_tbSlice->setEnabled(m_settings->hasVolumeData());
        m_tbSlice->setChecked(m_settings->getShowVolumeSlice());
    }
}

void MainWindow::syncVolumePage() {
    const bool hasVol = m_settings->hasVolumeData();
    if (m_volumeShowCb) {
        m_volumeShowCb->setEnabled(hasVol);
        m_volumeShowCb->setChecked(hasVol && m_settings->getShowVolume());
    }
    if (m_volumeSliceShowCb) {
        m_volumeSliceShowCb->setEnabled(hasVol);
        m_volumeSliceShowCb->setChecked(hasVol && m_settings->getShowVolumeSlice());
    }
    if (m_isoEnableCb) {
        bool isoAvail = m_settings->getIsosurfaceAvailable();
        m_isoEnableCb->setEnabled(isoAvail);
        if (!isoAvail) m_isoEnableCb->setChecked(false);
    }
    refreshIsosurfaceSlider();
    applyVolumeControlGating();
}

void MainWindow::refreshIsosurfaceSlider() {
    if (!m_isoValueSlider || !m_isoValueLabel) return;
    const double lo = m_settings->getDataScalarMinQml();
    const double hi = m_settings->getDataScalarMaxQml();
    double span = hi - lo;
    if (span <= 0.0) span = 1.0;
    double frac = (m_settings->getIsovalue() - lo) / span;
    frac = std::clamp(frac, 0.0, 1.0);
    m_isoValueSlider->blockSignals(true);
    m_isoValueSlider->setValue(static_cast<int>(frac * 1000));
    m_isoValueSlider->blockSignals(false);
    m_isoValueLabel->setText(QString::number(m_settings->getIsovalue(), 'f', 3));
}

// The Volume page places both full-volume-rendering controls (Step Size, Opacity,
// Wireframe) and slice-plane controls inside one `optionsGroup`. Only the
// full-volume controls should be greyed when "Enable Volume Rendering" is off; the
// slice controls must remain configurable so a slice plane can be viewed with no
// volume rendering. This re-applies that split whenever showVolume/data availability
// changes (checkbox toggle, quick-bar button, mesh load). The group itself is
// enabled purely on volume-data availability so slice options are always reachable.
void MainWindow::applyVolumeControlGating() {
    const bool hasVol = m_settings->hasVolumeData() || m_settings->getIsosurfaceAvailable();
    if (m_volumeOptionsGroup) m_volumeOptionsGroup->setEnabled(hasVol);
    const bool enableRenderControls = hasVol && m_settings->getShowVolume();
    for (QWidget* w : m_volumeRenderCtrls) {
        if (w) w->setEnabled(enableRenderControls);
    }
}

// Keep the View & Display checkboxes in sync when settings are changed from the
// quick-bar buttons or keyboard shortcuts, so both UIs reflect the same state.
void MainWindow::syncViewDisplayPage() {
    if (m_vdWireframeCb) m_vdWireframeCb->setChecked(m_settings->isWireframe());
    if (m_vdSurfaceCb)   m_vdSurfaceCb->setChecked(m_settings->isSurfaceVisible());
    if (m_vdPointsCb)    m_vdPointsCb->setChecked(m_settings->getShowPoints());
    if (m_vdBboxCb)      m_vdBboxCb->setChecked(m_settings->getShowBounds());
    if (m_vdDefectsCb)   m_vdDefectsCb->setChecked(m_settings->getShowQualityOverlay());
    if (m_vdScalarCb)    m_vdScalarCb->setChecked(m_settings->getPointUseScalar());
    if (m_vdGizmoCb)     m_vdGizmoCb->setChecked(m_settings->isGizmoVisible());
    if (m_vdGizmoSizeCombo)   m_vdGizmoSizeCombo->setCurrentIndex(m_settings->getGizmoSizeChoice());
    if (m_vdGizmoCornerCombo) m_vdGizmoCornerCombo->setCurrentIndex(m_settings->getGizmoCorner());
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


// Keyboard shortcuts

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
    connect(m_settings, &RenderSettings::meshLoadStateChanged, this, &MainWindow::refreshSlicingPageBounds);
    connect(m_settings, &RenderSettings::meshLoadStateChanged, this, &MainWindow::refreshScalarFilterRange);
    connect(m_settings, &RenderSettings::meshLoadStateChanged, this, &MainWindow::refreshColorRangeBounds);
    connect(m_settings, &RenderSettings::meshLoadStateChanged, this, &MainWindow::refreshVolumeRangeBounds);
    connect(m_settings, &RenderSettings::meshLoadStateChanged, this, &MainWindow::refreshSliceRangeBounds);
    connect(m_settings, &RenderSettings::meshLoadStateChanged, this, &MainWindow::refreshGlyphMagRangeBounds);
    connect(m_settings, &RenderSettings::meshLoadStateChanged, this, &MainWindow::refreshStreamlineMagRangeBounds);
    connect(m_settings, &RenderSettings::meshLoadStateChanged, this, [this]() {
        const bool hasVectors = m_settings->hasMeshVectors();
        const bool hasCellVectors = m_settings->hasMeshCellVectors();
        if (m_slShowCb) {
            m_slShowCb->setEnabled(hasVectors);
            if (!hasVectors) m_slShowCb->setChecked(false);
        }
        if (m_vecShowCb) {
            m_vecShowCb->setEnabled(hasVectors || hasCellVectors);
            if (!(hasVectors || hasCellVectors)) m_vecShowCb->setChecked(false);
        }
        if (m_scalarShowCb) {
            bool ok = m_settings->hasMeshScalars();
            m_scalarShowCb->setEnabled(ok);
            if (ok) {
                m_scalarShowCb->setChecked(m_settings->getMeshUseScalarColor());
            } else {
                m_scalarShowCb->setChecked(false);
            }
            if (m_scalarOptionsGroup) {
                m_scalarOptionsGroup->setEnabled(ok);
            }
        }
        if (m_volumeShowCb) {
            bool ok = m_settings->hasVolumeData();
            m_volumeShowCb->setEnabled(ok);
            if (!ok) m_volumeShowCb->setChecked(false);
        }
        if (m_volumeSliceShowCb) {
            bool ok = m_settings->hasVolumeData();
            m_volumeSliceShowCb->setEnabled(ok);
            if (!ok) m_volumeSliceShowCb->setChecked(false);
        }
        applyVolumeControlGating();
    });
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
        if (m_vectorPlacementCombo) {
            m_vectorPlacementCombo->blockSignals(true);
            m_vectorPlacementCombo->setCurrentIndex(m_settings->getVectorPlacement());
            m_vectorPlacementCombo->setEnabled(m_settings->hasMeshCellVectors());
            m_vectorPlacementCombo->blockSignals(false);
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

    // Mirror toolbar and View & Display toggles whenever the corresponding
    // settings change (keyboard shortcuts, toolbar buttons, side-panel checkboxes).
    // Sync runs on every view change, but the setChecked calls only emit when the
    // value actually differs.
    connect(m_settings, &RenderSettings::viewChanged, this, [this](ChangeFlags flags) {
        if (flags.testFlag(ChangeFlag::Lighting)) syncLightingPage();
        syncTopToolbar();
        syncViewDisplayPage();
        syncVolumePage();
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
    if (!path.isEmpty()) m_settings->loadMesh(path);
}

void MainWindow::openRecent(const QString& path) {
    m_settings->openRecent(path);
}

void MainWindow::clearRecentFiles() {
    m_settings->clearRecentFiles();
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
    m_settings->clearMeshes();
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
            "QToolBar { border: none; padding: 4px; spacing: 4px; }"
            "QToolButton { background: transparent; border: 1px solid transparent; border-radius: 5px; padding: 4px 8px; }"
            "QToolButton:hover { background: palette(midlight); }"
            "QToolButton:checked { background: palette(highlight); border: 1px solid palette(highlight); }");
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
        "<tr><td><b>S</b></td><td>Save screenshot</td></tr>"
        "<tr><td><b>Left/Right</b></td><td>Orbit (azimuth)</td></tr>"
        "<tr><td><b>Up/Down</b></td><td>Elevation</td></tr>"
        "<tr><td><b>Ctrl + =</b></td><td>Zoom in</td></tr>"
        "<tr><td><b>Ctrl + -</b></td><td>Zoom out</td></tr>"
        "</table>"
    );
    info.exec();
}


