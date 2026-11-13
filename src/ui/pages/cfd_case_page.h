#pragma once
#include "sidebar_page.h"
#include "ui/controllers/cfd_controller.h"
#include "cfd/cfd_types.h"
#include "ui/cfd_advanced_dialog.h"
#include "ui/widgets/stopping_criteria_dialog.h"

#include <QString>

class QPushButton;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QLabel;
class QDoubleSpinBox;
class QRadioButton;
class QLineEdit;
class QTimer;
class ScientificDoubleSpinBox;

namespace sidebar {

class CfdCasePage : public SidebarPage {
    Q_OBJECT
signals:
    void residualPoint(int iteration, double continuity, double momentum);
    void residualSeriesUpdated(int iteration, int dim, double continuity,
                               double momU, double momV, double momW,
                               double k, double omega, double maxYPlus, bool hasTurbulence);
    void residualsCleared();
    void monitorHistoryRestored();
public:
    explicit CfdCasePage(RenderSettings* settings, QWidget* parent = nullptr);
    CfdController* controller() const { return m_ctrl; }
    void loadMeshFromPath(const QString& path);
    void loadCaseFromPath(const QString& path);

public slots:
    void onLoadMesh();
    void onExport();
    void onSaveCase();
    void onLoadCase();

private slots:
    void onRun();
    void onCancel();
    void onPause();
    void onStep();
    void onInitialize();
    void onEditBCs();
    void onStoppingCriteria();
    void onProgress(int iteration, double continuity, double momentum);
    void onFinished(const QString& summary);
    void onFailed(const QString& error);
    void log(const QString& msg);
    void onMeshLoaded();
    void onCaseLoaded();
    void onTransientFrame(std::shared_ptr<const RenderMesh> mesh, int step, double time);
    void onPauseSettled();

private:
    // Enable/disable every control whose availability depends on the run state.
    // The pause/step/stopping state is also refreshed on each iteration, so this
    // is the only place that spells it out.
    void refreshRunControls();
    void syncTimeDependencies();
    void rebuildPatchTable();
    void collectSettings();
    void applySettings(const cfd::CaseSettings& s);

    void setRunning(bool running);

    CfdController* m_ctrl = nullptr;
    std::vector<cfd::PatchBC> m_patchBCs;
    QString m_meshName;

    QLineEdit* m_rho = nullptr;
    QLineEdit* m_nu = nullptr;
    QDoubleSpinBox* m_alphaU = nullptr;
    QDoubleSpinBox* m_alphaP = nullptr;
    QDoubleSpinBox* m_alphaK = nullptr;
    QDoubleSpinBox* m_alphaW = nullptr;
    QCheckBox* m_wallFuncCb = nullptr;
    QCheckBox* m_simplec = nullptr;
    QComboBox* m_scheme = nullptr;
    QComboBox* m_gradientScheme = nullptr;
    // Rhie-Chow is always on (no toggle).
    QCheckBox* m_turb = nullptr;

    QLabel* m_iterValue = nullptr;
    QLabel* m_contValue = nullptr;
    QLabel* m_momValue = nullptr;

    QPushButton* m_bcEditBtn = nullptr;
    QPushButton* m_advancedBtn = nullptr;
    QPushButton* m_initBtn = nullptr;
    QPushButton* m_runBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;
    QPushButton* m_pauseBtn = nullptr;
    QPushButton* m_stepBtn = nullptr;
    QPushButton* m_stoppingBtn = nullptr;

    // Advanced solver settings own their widgets and are the single reader of
    // those CaseSettings fields; the page only reads them through collectInto().
    // Built up front and kept hidden because a run reads them whether or not the
    // dialog was ever opened.
    CfdAdvancedDialog* m_advancedDlg = nullptr;
    cfd::StoppingCriteriaDialog* m_stoppingDlg = nullptr;

    // ── Time mode (steady / transient) ──────────────────────────────
    QRadioButton* m_steadyRadio = nullptr;
    QRadioButton* m_transientRadio = nullptr;
    QWidget* m_transientOptions = nullptr;
    QComboBox* m_timeSchemeCombo = nullptr;
    ScientificDoubleSpinBox* m_dtSpin = nullptr;
    ScientificDoubleSpinBox* m_tEndSpin = nullptr;
    QCheckBox* m_adaptiveCflCb = nullptr;
    QDoubleSpinBox* m_cflSpin = nullptr;
    ScientificDoubleSpinBox* m_minDtSpin = nullptr;
    ScientificDoubleSpinBox* m_maxDtSpin = nullptr;
    QSpinBox* m_writeIntervalSpin = nullptr;
    QSpinBox* m_outerCorrectorsSpin = nullptr;
    QSpinBox* m_nonOrthCorrectorsSpin = nullptr;
    QLineEdit* m_pvdFileEdit = nullptr;
    QPushButton* m_pvdBrowseBtn = nullptr;

    bool m_running = false;
    bool m_savedAnimGlobal = true;
    QTimer* m_frameTimer = nullptr;
    QTimer* m_stateTimer = nullptr;
    bool m_pauseSettledLogged = false;
    std::shared_ptr<const RenderMesh> m_lastFrame;
};

} // namespace sidebar