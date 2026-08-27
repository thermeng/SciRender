#pragma once

#include <QMainWindow>
#include <QDockWidget>
#include <QStackedWidget>
#include <QToolBar>
#include <QListWidget>
#include <QTimer>
#include <QLabel>
#include <QToolButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>
#include <QDoubleValidator>
#include <QShortcut>
#include <QFileDialog>
#include <QColorDialog>
#include <QMessageBox>
#include <QMenuBar>
#include <QStatusBar>
#include <QApplication>
#include <QActionGroup>
#include <QList>
#include <QVector>
#include <QTimeLine>

#include "viewport_widget.h"
#include "render/settings/render_settings.h"
#include "ui/range_editor.h"

namespace Ui {
    class MainWindow;
    class LightingPage;
    class SlicingPage;
    class ViewDisplayPage;
    class ScalarPage;
    class VectorsPage;
    class StreamlinesPage;
    class ScreenshotPage;
    class MeshInfoPage;
    class VolumePage;
    class SlicePlanePage;
    class IsosurfacePage;
    class AnimationPage;
}

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void openMesh();
    void openRecent(const QString& path);
    void clearRecentFiles();
    void saveScreenshot();
    void exportAnimation();
    void clearMeshes();
    void showAbout();
    void showShortcuts();
    void setSidebarSection(int section);
    void onScreenshotCaptured(const QString& path);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void setupMenus();
    void setupTopToolbar();
    void setupSidebar();
    void setupNavList();
    void setupTimers();
    void setupKeyboardShortcuts();
    void connectSettings();
    void updateStatusBar();
    void syncTopToolbar();
    void syncViewDisplayPage();
    void syncLightingPage();
    void syncVolumePage();
    void applyVolumeControlGating();
    void recreateViewport();
    void applyThemeAwareStylesheets();

    ::RenderSettings* m_settings = nullptr;
    ViewportWidget* m_viewport = nullptr;

    Ui::MainWindow* ui = nullptr;

    // Top toolbar (display toggles)
    QToolBar* m_topToolbar = nullptr;
    QAction* m_tbWireframe = nullptr;
    QAction* m_tbSurface = nullptr;
    QAction* m_tbVolume = nullptr;
    QAction* m_tbSlice = nullptr;

    // Sidebar
    QDockWidget* m_sidebarDock = nullptr;
    QWidget* m_sidebarWidget = nullptr;
    QWidget* m_rightPanel = nullptr;
    QStackedWidget* m_sectionStack = nullptr;
    int m_activeSection = -1;
    bool m_sidebarExpanded = false;
    static constexpr int kSidebarWidth = 240;
    static constexpr int kNavWidth = 140;
    static constexpr int kLabelWidth = 72;
    static constexpr int kControlHeight = 24;
    static constexpr int kValueFieldWidth = 48;

    // Navigation list (replaces icon strip)
    QListWidget* m_navList = nullptr;
    int m_navWidth = 140; // Dynamic width calculated from text content

    // Panel header
    QWidget* m_panelHeader = nullptr;
    QLabel* m_panelTitle = nullptr;

    // Section pages
    QWidget* buildLightingPage();
    QWidget* buildSlicingPage();
    QWidget* buildViewDisplayPage();
    QWidget* buildScalarPage();
    QWidget* buildVectorsPage();
    QWidget* buildStreamlinesPage();
    QWidget* buildScreenshotPage();
    QWidget* buildMeshInfoPage();
    QWidget* buildVolumePage();
    QWidget* buildSlicePlanePage();
    QWidget* buildIsosurfacePage();
    QWidget* buildAnimationPage();
    void refreshMeshInfoPage();
    QHash<QString, QLabel*> m_meshInfoLabels;

    // Animation page widgets (synced from AnimationController::stateChanged)
    QPushButton* m_animPlayBtn = nullptr;
    QToolButton* m_animStepBackBtn = nullptr;
    QToolButton* m_animStepFwdBtn = nullptr;
    QSlider* m_animSlider = nullptr;
    QLabel* m_animTimeLabel = nullptr;
    QLabel* m_animFrameLabel = nullptr;
    QLabel* m_animStatusLabel = nullptr;
    QLabel* m_animSequenceLabel = nullptr;
    QCheckBox* m_animLoopCb = nullptr;
    QDoubleSpinBox* m_animFpsSpin = nullptr;
    QComboBox* m_animScaleCombo = nullptr;
    QPushButton* m_animExportBtn = nullptr;
    void refreshAnimationPage();

    // View & Display page checkboxes (synced with toolbar / keyboard shortcuts)
    QCheckBox* m_vdWireframeCb = nullptr;
    QCheckBox* m_vdSurfaceCb = nullptr;
    QCheckBox* m_vdPointsCb = nullptr;
    QCheckBox* m_vdBboxCb = nullptr;
    QCheckBox* m_vdDefectsCb = nullptr;
    QCheckBox* m_vdScalarCb = nullptr;
    QCheckBox* m_vdGizmoCb = nullptr;
    QComboBox* m_vdGizmoSizeCombo = nullptr;
    QComboBox* m_vdGizmoCornerCombo = nullptr;
    QCheckBox* m_vdFpsCb = nullptr;
    QCheckBox* m_vdParallelCb = nullptr;

    // Lighting page widgets (for backend→UI sync)
    struct LightDirTabData {
        QSlider* azimuthSlider = nullptr;
        QLabel* azimuthValue = nullptr;
        QSlider* elevationSlider = nullptr;
        QLabel* elevationValue = nullptr;
    };
    QCheckBox* m_lightingMarkersCb = nullptr;
    QCheckBox* m_lightingKitCb = nullptr;
    QSlider* m_lightingKeySlider = nullptr;
    QLabel* m_lightingKeyValue = nullptr;
    QSlider* m_lightingWarmthSlider = nullptr;
    QLabel* m_lightingWarmthValue = nullptr;
    QSlider* m_lightingFillKfSlider = nullptr;
    QLabel* m_lightingFillKfValue = nullptr;
    QSlider* m_lightingBackKbSlider = nullptr;
    QLabel* m_lightingBackKbValue = nullptr;
    QSlider* m_lightingHeadKhSlider = nullptr;
    QLabel* m_lightingHeadKhValue = nullptr;
    QTabWidget* m_lightingDirectionTabs = nullptr;
    QVector<LightDirTabData> m_lightDirTabs;
    QSlider* m_lightingAmbientSlider = nullptr;
    QLabel* m_lightingAmbientValue = nullptr;
    QSlider* m_lightingDiffuseSlider = nullptr;
    QLabel* m_lightingDiffuseValue = nullptr;
    QSlider* m_lightingSpecularSlider = nullptr;
    QLabel* m_lightingSpecularValue = nullptr;
    QSlider* m_lightingRoughnessSlider = nullptr;
    QLabel* m_lightingRoughnessValue = nullptr;
    QSlider* m_lightingMetallicSlider = nullptr;
    QLabel* m_lightingMetallicValue = nullptr;

    // Screenshot page controls
    QComboBox* m_ssResCombo = nullptr;
    QComboBox* m_ssAaCombo = nullptr;

    // Navigation shortcuts (lives on MainWindow so it survives viewport rebuilds);
    // disabled while an editor widget has focus (see setupKeyboardShortcuts).
    QList<QShortcut*> m_navShortcuts;

    // Field selection combos (populated on mesh load)
    QComboBox* m_scalarCombo = nullptr;
    QComboBox* m_vectorCombo = nullptr;
    QComboBox* m_vectorPlacementCombo = nullptr;
    QComboBox* m_streamlineCombo = nullptr;
    QComboBox* m_streamlineDirectionCombo = nullptr;
    QComboBox* m_volumeFieldCombo = nullptr;

    // "Show" checkboxes for vector/streamline/volume/scalar pages (gated on data availability)
    QCheckBox* m_scalarShowCb = nullptr;
    QWidget* m_scalarOptionsGroup = nullptr;
    QCheckBox* m_slShowCb = nullptr;
    QCheckBox* m_vecShowCb = nullptr;
    QCheckBox* m_volumeShowCb = nullptr;
    QCheckBox* m_volumeSliceShowCb = nullptr;
    QWidget* m_volumeOptionsGroup = nullptr;          // gated on data availability only
    QList<QWidget*> m_volumeRenderCtrls;              // volume-rendering-only controls (gated on showVolume)
    QRadioButton* m_sliceAxisXRb = nullptr;
    QRadioButton* m_sliceAxisYRb = nullptr;
    QRadioButton* m_sliceAxisZRb = nullptr;
    QSlider* m_slicePosSlider = nullptr;
    QLabel* m_slicePosValue = nullptr;

    // Isosurface controls (on the Volume page, gated on structured volume data)
    QCheckBox* m_isoEnableCb = nullptr;
    QSlider* m_isoValueSlider = nullptr;
    QLabel* m_isoValueLabel = nullptr;

    // Filter sliders (Colormap page)
    QSlider* m_filterMinSlider = nullptr;
    QLineEdit* m_filterMinField = nullptr;
    QSlider* m_filterMaxSlider = nullptr;
    QLineEdit* m_filterMaxField = nullptr;
    QCheckBox* m_filterEnabledCb = nullptr;

    // Fixed colormap range (Colormap page)
    QSlider* m_colorLoSlider = nullptr;
    QLineEdit* m_colorLoField = nullptr;
    QSlider* m_colorHiSlider = nullptr;
    QLineEdit* m_colorHiField = nullptr;
    QCheckBox* m_colorRangeCb = nullptr;
    RangeEditor* m_colorRangeEditor = nullptr;

    // Per-pass Fixed Range editors
    RangeEditor* m_volumeRangeEditor = nullptr;
    QCheckBox* m_volumeRangeCb = nullptr;
    RangeEditor* m_sliceRangeEditor = nullptr;
    QCheckBox* m_sliceRangeCb = nullptr;
    RangeEditor* m_glyphMagRangeEditor = nullptr;
    QCheckBox* m_glyphMagRangeCb = nullptr;
    int m_glyphRangeBoundComp = -1; // -1=magnitude, 0/1/2=X/Y/Z based on vectorColorMode
    RangeEditor* m_streamlineMagRangeEditor = nullptr;
    QCheckBox* m_streamlineMagRangeCb = nullptr;
    int m_streamlineRangeBoundComp = -1; // -1=magnitude, 0/1/2=X/Y/Z based on streamlineColorMode

    // Mesh info page (rebuilt on mesh load)
    QWidget* m_meshInfoPage = nullptr;

    // Slicing page widgets (for bounds refresh on mesh load)
    QSlider* m_sliceXSlider = nullptr;
    QDoubleSpinBox* m_sliceXSpinBox = nullptr;
    QSlider* m_sliceYSlider = nullptr;
    QDoubleSpinBox* m_sliceYSpinBox = nullptr;
    QSlider* m_sliceZSlider = nullptr;
    QDoubleSpinBox* m_sliceZSpinBox = nullptr;
    QCheckBox* m_sliceEnableCb = nullptr;
    QCheckBox* m_sliceCrinkleCb = nullptr;
    QCheckBox* m_sliceAxisXCb = nullptr;
    QCheckBox* m_sliceAxisYCb = nullptr;
    QCheckBox* m_sliceAxisZCb = nullptr;

    void refreshSlicingPageBounds();
    void refreshScalarFilterRange();
    void refreshColorRangeBounds();
    void refreshVolumeRangeBounds();
    void refreshSliceRangeBounds();
    void refreshGlyphMagRangeBounds();
    void refreshStreamlineMagRangeBounds();
    void refreshIsosurfaceSlider();

    // Timers
    QTimer m_autoRotateTimer;
    QTimer m_fpsTimer;
    QTimer m_particleTimer;
    QTimer m_animSeekDebounce;
    int m_pendingSeekFrame = -1;

    // Dialogs
    QFileDialog* m_openDialog = nullptr;
    QColorDialog* m_bgColorDialog = nullptr;
    QColorDialog* m_meshColorDialog = nullptr;
    QColorDialog* m_surfaceColorDialog = nullptr;
    QColorDialog* m_vectorColorDialog = nullptr;
    QColorDialog* m_streamlineColorDialog = nullptr;
    QColorDialog* m_seedColorDialog = nullptr;

    // Theme
    QActionGroup* m_shadingGroup = nullptr;
};


