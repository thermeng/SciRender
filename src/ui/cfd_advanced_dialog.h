#pragma once

#include <QDialog>

#include "cfd/cfd_types.h"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTableWidget;

namespace sidebar {

// Every solver knob that does not belong on the CFD page itself: pressure
// correction, the three AMG hierarchies, slope limiting and diagnostics.
//
// The dialog owns these widgets and is the single reader/writer of these
// CaseSettings fields, so the page keeps no second copy. It is built up front
// and stays hidden: collectInto() is consulted before every run, so the fields
// must exist whether or not the dialog has ever been opened.
class CfdAdvancedDialog : public QDialog {
    Q_OBJECT
public:
    explicit CfdAdvancedDialog(QWidget* parent = nullptr);

    // Load the current settings and remember them as the Cancel snapshot.
    void snapshot(const cfd::CaseSettings& s);
    // Push a full settings object into the widgets (case load, defaults).
    void setSettings(const cfd::CaseSettings& s);
    // Write the widgets back into `s` (before a run or save).
    void collectInto(cfd::CaseSettings& s);
    // Kept for callers: the simplified Multigrid tab has no dimension-dependent
    // control, so the mesh dimension no longer changes anything here.
    void setDimension(int dim);

private:
    void buildUi();
    void resetToDefaults();
    void syncDependencies();
    void readSharedAmg(const cfd::AmgSettings& s);
    void writeSharedAmgInto(cfd::AmgSettings& s) const;
    void commitSharedToSelected();
    void loadSharedFromSelected();
    void syncAmgRows();

    enum { kAmgRows = 3 };
    cfd::CaseSettings m_snapshot;
    // Each table row owns one backend AMG config, including the shared-panel
    // fields: momentum/turbulence default to 2/30/50 flexible relaxations while
    // pressure defaults to 1/1/1, so a single set of panels cannot represent all.
    cfd::AmgSettings m_amgRows[kAmgRows];
    int m_amgSelectedRow = 0;

    // Pressure correction
    QSpinBox* m_pressureCorrectors = nullptr;
    QCheckBox* m_limitPressureCorrection = nullptr;
    QDoubleSpinBox* m_pressureCorrectionLimit = nullptr;
    QSpinBox* m_pressureRefCell = nullptr;
    QCheckBox* m_pressureSolverStats = nullptr;
    QCheckBox* m_momentumSolverStats = nullptr;

    // Multigrid: one row per real AMG config (pressure / momentum / turbulence).
    QTableWidget* m_amgTable = nullptr;

    // Shared AMG controls (edited for the selected row).
    QSpinBox* m_amgPreSweeps = nullptr;
    QSpinBox* m_amgPostSweeps = nullptr;
    QSpinBox* m_amgMaxCycles = nullptr;
    QSpinBox* m_amgMaxLevels = nullptr;
    QRadioButton* m_amgGaussSeidel = nullptr;
    QRadioButton* m_amgILU = nullptr;
    QSpinBox* m_amgSweeps = nullptr;
    QSpinBox* m_amgMaxFineRelaxations = nullptr;
    QSpinBox* m_amgMaxCoarseRelaxations = nullptr;
    QSpinBox* m_amgVerbosity = nullptr;

    // Slope limiter
    QComboBox* m_limiterKind = nullptr;
    QWidget* m_limiterKRow = nullptr;
    QWidget* m_limiterCubicRow = nullptr;
    QRadioButton* m_limiterFaceCentre = nullptr;
    QRadioButton* m_limiterCentroidLine = nullptr;
    QButtonGroup* m_limiterSpan = nullptr;
    QDoubleSpinBox* m_limiterK = nullptr;
    QRadioButton* m_limiterCellWide = nullptr;
    QRadioButton* m_limiterComponentWise = nullptr;
    QButtonGroup* m_limiterAggregation = nullptr;
    QDoubleSpinBox* m_limiterCubicTransition = nullptr;

    // Diagnostics
    QSpinBox* m_printFrequency = nullptr;
    QSpinBox* m_residualHistoryWindow = nullptr;
    QDoubleSpinBox* m_velocityCorrectionMaxSpeed = nullptr;
    QDoubleSpinBox* m_velocityCorrectionMaxGrowth = nullptr;
    QSpinBox* m_numThreads = nullptr;
    QCheckBox* m_printResidentSet = nullptr;
    QCheckBox* m_printIterationTime = nullptr;
    QCheckBox* m_printTotalTime = nullptr;
};

} // namespace sidebar