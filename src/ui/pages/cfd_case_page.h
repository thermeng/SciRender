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

private:
    void buildAdvancedDialog();
    // Seed every control in the advanced dialog from the shipped defaults.
    // Used both at construction and by the dialog's Restore defaults button,
    // so the two can never disagree.
    void applyDialogDefaults();
    // Enable/disable controls whose meaning depends on a checkbox.
    void syncDialogDependencies();
    void rebuildPatchTable();
    void collectSettings();
    void setRunning(bool running);

    CfdController* m_ctrl = nullptr;
    std::vector<cfd::PatchBC> m_patchBCs;
    QString m_meshName;

    QDoubleSpinBox* m_rho = nullptr;
    QDoubleSpinBox* m_nu = nullptr;
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
    QCheckBox* m_useAMGMomentum = nullptr;
    QSpinBox* m_momentumAmgThreshold = nullptr;
    QSpinBox* m_momentumAmgCycles = nullptr;
    QDoubleSpinBox* m_momentumAmgMaxResidual = nullptr;
    QCheckBox* m_momentumSolverStats = nullptr;
    AmgConfigWidget* m_momentumAmgWidget = nullptr;

    bool m_running = false;
};

} // namespace sidebar
