#pragma once

#include <QMainWindow>
#include <QDockWidget>
#include <QStackedWidget>
#include <QTimer>
#include <QLabel>
#include <QToolButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QSlider>
#include <QSpinBox>
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
#include <QHash>
#include <QList>
#include <QVector>

#include "viewport_widget.h"
#include "render/render_settings.h"

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
}

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void openMesh();
    void openRecent(const QString& path);
    void saveScreenshot();
    void clearMeshes();
    void showAbout();
    void showShortcuts();
    void setSidebarSection(int section);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void setupMenus();
    void setupSidebar();
    void setupQuickBar();
    void setupTimers();
    void setupKeyboardShortcuts();
    void connectSettings();
    void updateStatusBar();
    void updateQuickBarVisibility();
    void syncQuickBar();
    void syncViewDisplayPage();
    void syncLightingPage();
    void applyTheme(AppTheme theme);
    void rebuildSidebarStyles();
    void rebuildQuickBarStyles();
    void recreateViewport();
    ThemeColors currentColors() const;

    ::RenderSettings* m_settings = nullptr;
    ViewportWidget* m_viewport = nullptr;

    Ui::MainWindow* ui = nullptr;

    // Sidebar
    QDockWidget* m_sidebarDock = nullptr;
    QWidget* m_sidebarWidget = nullptr;
    QWidget* m_rightPanel = nullptr;
    QStackedWidget* m_sectionStack = nullptr;
    int m_activeSection = -1;
    bool m_sidebarExpanded = false;
    static constexpr int kSidebarWidth = 220;
    static constexpr int kIconStripWidth = 48;
    static constexpr int kLabelWidth = 72;
    static constexpr int kControlHeight = 24;
    static constexpr int kValueFieldWidth = 48;

    // Panel header
    QWidget* m_panelHeader = nullptr;
    QLabel* m_panelTitle = nullptr;

    // Icon strip buttons (indices 1-8 are section toggles)
    QVector<QToolButton*> m_iconButtons;
    QWidget* m_iconStrip = nullptr;
    QToolButton* m_closeBtn = nullptr;

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
    void refreshMeshInfoPage();
    QHash<QString, QLabel*> m_meshInfoLabels;

    // Quick bar
    QWidget* m_quickBar = nullptr;
    QToolButton* m_quickBarHandle = nullptr;
    QHBoxLayout* m_quickBarLayout = nullptr;
    QToolButton* m_qbWireframe = nullptr;
    QToolButton* m_qbGrid = nullptr;
    QToolButton* m_qbSurface = nullptr;
    QToolButton* m_qbVolume = nullptr;

    // View & Display page checkboxes (synced with quick bar / keyboard shortcuts)
    QCheckBox* m_vdWireframeCb = nullptr;
    QCheckBox* m_vdGridCb = nullptr;
    QCheckBox* m_vdSurfaceCb = nullptr;
    QCheckBox* m_vdPointsCb = nullptr;
    QCheckBox* m_vdBboxCb = nullptr;
    QCheckBox* m_vdDefectsCb = nullptr;
    QCheckBox* m_vdCellEdgeCb = nullptr;
    QCheckBox* m_vdScalarCb = nullptr;
    QCheckBox* m_vdGizmoCb = nullptr;
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

    // Navigation shortcuts (lives on MainWindow so it survives viewport rebuilds);
    // disabled while an editor widget has focus (see setupKeyboardShortcuts).
    QList<QShortcut*> m_navShortcuts;

    // Field selection combos (populated on mesh load)
    QComboBox* m_scalarCombo = nullptr;
    QComboBox* m_vectorCombo = nullptr;
    QComboBox* m_streamlineCombo = nullptr;
    QComboBox* m_streamlineDirectionCombo = nullptr;
    QComboBox* m_volumeFieldCombo = nullptr;

    // Filter sliders (Colormap page)
    QSlider* m_filterMinSlider = nullptr;
    QLineEdit* m_filterMinField = nullptr;
    QSlider* m_filterMaxSlider = nullptr;
    QLineEdit* m_filterMaxField = nullptr;

    // Mesh info page (rebuilt on mesh load)
    QWidget* m_meshInfoPage = nullptr;

    // Timers
    QTimer m_autoRotateTimer;
    QTimer m_fpsTimer;
    QTimer m_particleTimer;

    // Dialogs
    QFileDialog* m_openDialog = nullptr;
    QFileDialog* m_screenshotDialog = nullptr;
    QColorDialog* m_bgColorDialog = nullptr;
    QColorDialog* m_meshColorDialog = nullptr;
    QColorDialog* m_surfaceColorDialog = nullptr;
    QColorDialog* m_vectorColorDialog = nullptr;
    QColorDialog* m_streamlineColorDialog = nullptr;
    QColorDialog* m_seedColorDialog = nullptr;

    // Theme
    QActionGroup* m_themeGroup = nullptr;
};
