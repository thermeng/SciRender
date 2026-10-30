#pragma once

#include <QMainWindow>
#include <QDockWidget>
#include <QStackedWidget>
#include <QToolBar>
#include <QListWidget>
#include <QTimer>
#include <QLabel>
#include <QToolButton>
#include <QShortcut>
#include <QActionGroup>
#include <QList>
#include <QVector>

#include "solver_output_sink.h"
#include <QTextEdit>
#include <QCheckBox>
#include "viewport_widget.h"
#include "solver_output_sink.h"
#include <QTextEdit>
#include <QCheckBox>
#include "render/settings/render_settings.h"
#include "ui/pages/lighting_page.h"
#include "ui/pages/clipping_page.h"
#include "ui/pages/view_display_page.h"
#include "ui/pages/scalar_page.h"
#include "ui/pages/vectors_page.h"
#include "ui/pages/streamlines_page.h"
#include "ui/pages/slice_plane_page.h"
#include "ui/pages/postproc_page.h"
#include "ui/pages/volume_page.h"
#include "ui/pages/isosurface_page.h"
#include "ui/pages/probe_page.h"
#include "ui/pages/mesh_info_page.h"
#include "ui/pages/screenshot_page.h"
#include "ui/pages/animation_page.h"
#include "ui/pages/plots_page.h"
#include "ui/pages/cfd_case_page.h"

class PlotWindow;

namespace Ui {
    class MainWindow;
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
    void reloadMesh();
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
    QAction* m_clearAction = nullptr;

    // Sidebar
    QDockWidget* m_sidebarDock = nullptr;
    QWidget* m_sidebarWidget = nullptr;
    QWidget* m_rightPanel = nullptr;
    QStackedWidget* m_sectionStack = nullptr;
    sidebar::CfdCasePage* m_cfdPage = nullptr;
    int m_activeSection = -1;
    bool m_sidebarExpanded = false;
    static constexpr int kSidebarWidth = 240;
    static constexpr int kNavWidth = 140;

    // Navigation list (replaces icon strip)
    QListWidget* m_navList = nullptr;
    QVector<int> m_navRowToSection;   // maps list row -> nav section index (-1 for group headers)
    QVector<int> m_navSectionToRow;   // maps nav section index -> list row
    int m_navWidth = 140; // Dynamic width calculated from text content

    // Availability badges: a nav section whose backing data is missing gets a
    // warning glyph shown on the right edge of its nav label. `m_sectionBanners` holds the
    // inline reason banner injected at the top of each visualizer page.
    void refreshSidebarCapabilityBadges();
    QString sectionUnavailableReason(int section) const;
    void addSectionPage(int section, QWidget* page);
    QVector<QLabel*> m_sectionBanners;                  // indexed by section

    // Panel header
    QWidget* m_panelHeader = nullptr;
    QLabel* m_panelTitle = nullptr;
    QToolButton* m_closeBtn = nullptr;

    // Navigation shortcuts (lives on MainWindow so it survives viewport rebuilds);
    // disabled while an editor widget has focus (see setupKeyboardShortcuts).
    QList<QShortcut*> m_navShortcuts;

    // Timers
    QTimer m_autoRotateTimer;
    QTimer m_fpsTimer;
    QTimer m_particleTimer;

    QString m_currentFile;

    // Solver output — docked text panel at bottom of viewport area
    SolverOutputSink* m_outputSink = nullptr;
    QDockWidget* m_solverOutputDock = nullptr;
    QWidget* m_solverOutputContainer = nullptr;
    QTextEdit* m_solverOutputText = nullptr;
    QCheckBox* m_solverWrapCheck = nullptr;

    // Plots — dockable viewport constrained to original viewport location (draggable inside central host only)
    PlotWindow* m_plotWindow = nullptr;
    QDockWidget* m_plotDock = nullptr;
    QMainWindow* m_centralHost = nullptr; // inner host that owns viewport + plot dock, keeps plot from docking before sidebar
    QWidget* m_histContainer = nullptr;
    bool m_histogramVisible = false;
    void setHistogramVisible(bool visible);

    // Theme
    QActionGroup* m_shadingGroup = nullptr;
};


