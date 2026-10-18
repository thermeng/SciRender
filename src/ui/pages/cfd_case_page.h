#pragma once
#include "sidebar_page.h"
#include "ui/controllers/cfd_controller.h"
#include "ui/widgets/amg_config_widget.h"
#include "cfd/cfd_types.h"

#include <QString>

class QPushButton;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QLabel;
class QDialog;
class QDoubleSpinBox;
class QRadioButton;
class QLineEdit;
class ScientificDoubleSpinBox;

namespace sidebar {

class CfdCasePage : public SidebarPage {
    Q_OBJECT
public:
    explicit CfdCasePage(RenderSettings* settings, QWidget* parent = nullptr);

private slots:
    void onLoadMesh();
    void onRun();
    void onCancel();
    void onInitialize();
    void onExport();
    void onEditBCs();
    void onProgress(int iteration, double continuity, double momentum);
    void onFinished(const QString& summary);
    void onFailed(const QString& error);
    void onMeshLoaded();
    void onTransientFrame(std::shared_ptr<const RenderMesh> mesh, int step, double time);

private:
    void buildAdvancedDialog();
    // Seed every control in the advanced dialog from the shipped defaults.
    // Used both at construction and by the dialog's Restore defaults button,
    // so the two can never disagree.
    void applyDialogDefaults();
    // Enable/disable controls whose meaning depends on a checkbox.
    void syncDialogDependencies();
    void syncTimeDependencies();
    void rebuildPatchTable();
    void collectSettings();
    void setRunning(bool running);

    CfdController* m_ctrl = nullptr;
    std::vector<cfd::PatchBC> m_patchBCs;
    QString m_meshName;

    QLineEdit* m_rho = nullptr;
    QLineEdit* m_nu = nullptr;
    QDoubleSpinBox* m_tol = nullptr;
    QSpinBox* m_maxIter = nullptr;
    QDoubleSpinBox* m_alphaU = nullptr;
    QDoubleSpinBox* m_alphaP = nullptr;
    QCheckBox* m_simplec = nullptr;
    QComboBox* m_scheme = nullptr;
    QCheckBox* m_rhie = nullptr;
    QCheckBox* m_turb = nullptr;
    QDoubleSpinBox* m_kInf = nullptr;
    QDoubleSpinBox* m_wInf = nullptr;

    QLabel* m_status = nullptr;
    QLabel* m_iterValue = nullptr;
    QLabel* m_contValue = nullptr;
    QLabel* m_momValue = nullptr;

    QPushButton* m_loadBtn = nullptr;
    QPushButton* m_bcEditBtn = nullptr;
    QPushButton* m_advancedBtn = nullptr;
    QPushButton* m_initBtn = nullptr;
    QPushButton* m_runBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;
    QPushButton* m_exportBtn = nullptr;

    // Advanced solver settings live in one dialog, built up front and hidden.
    QDialog* m_advancedDlg = nullptr;
    QSpinBox* m_pressureCorrectors = nullptr;
    QCheckBox* m_limitPressureCorrection = nullptr;
    QDoubleSpinBox* m_pressureCorrectionLimit = nullptr;
    QSpinBox* m_pressureRefCell = nullptr;
    QCheckBox* m_pressureUseAMG = nullptr;
    QCheckBox* m_pressureUseAMGPreconditioner = nullptr;
    QSpinBox* m_pressureAmgThreshold = nullptr;
    AmgConfigWidget* m_pressureAmgWidget = nullptr;
    QSpinBox* m_printFrequency = nullptr;
    QSpinBox* m_residualHistoryWindow = nullptr;
    QSpinBox* m_numThreads = nullptr;
    QCheckBox* m_useAMGMomentum = nullptr;
    QSpinBox* m_momentumAmgThreshold = nullptr;
    QSpinBox* m_momentumAmgCycles = nullptr;
    QDoubleSpinBox* m_momentumAmgMaxResidual = nullptr;
    QCheckBox* m_momentumSolverStats = nullptr;
    AmgConfigWidget* m_momentumAmgWidget = nullptr;

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
    QLineEdit* m_pvdFileEdit = nullptr;
    QPushButton* m_pvdBrowseBtn = nullptr;

    bool m_running = false;
    bool m_savedAnimGlobal = true;
};

} // namespace sidebar
