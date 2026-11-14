#pragma once

#include <QDialog>

#include "cfd/cfd_types.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QSpinBox;
class QRadioButton;
class QButtonGroup;
class AmgConfigWidget;

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

    // Push a full settings object into the widgets (case load, defaults).
    void setSettings(const cfd::CaseSettings& s);
    // Write the widgets back into `s` (before a run or save).
    void collectInto(cfd::CaseSettings& s) const;

private:
    void buildUi();
    void resetToDefaults();
    void syncDependencies();

    // Pressure correction
    QSpinBox* m_pressureCorrectors = nullptr;
    QCheckBox* m_limitPressureCorrection = nullptr;
    QDoubleSpinBox* m_pressureCorrectionLimit = nullptr;
    QSpinBox* m_pressureRefCell = nullptr;
    QCheckBox* m_pressureSolverStats = nullptr;
    AmgConfigWidget* m_pressureAmg = nullptr;

    // Momentum / turbulence linear solvers
    QCheckBox* m_momentumSolverStats = nullptr;
    AmgConfigWidget* m_momentumAmg = nullptr;
    AmgConfigWidget* m_turbulenceAmg = nullptr;

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
};

} // namespace sidebar