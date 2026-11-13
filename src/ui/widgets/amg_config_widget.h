#pragma once

#include <QWidget>

#include "cfd/cfd_types.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QSpinBox;

// -style AMG linear solver properties.
//
// The widget is a pure view over cfd::AmgSettings: every control maps to one
// field, so the dialog, the case file and the solver cannot drift apart.
class AmgConfigWidget : public QWidget {
    Q_OBJECT
public:
    explicit AmgConfigWidget(const cfd::AmgSettings& defaults, QWidget* parent = nullptr, bool lockNonsymmetric = false);

    void setConfig(const cfd::AmgSettings& settings);
    cfd::AmgSettings config() const;

    void resetToDefaults();

signals:
    void changed();

private:
    void buildUi();
    void syncDependencies();

    bool m_lockNonsymmetric = false;
    cfd::AmgSettings m_defaults;

    // AMG linear solver
    QSpinBox* m_maxCycles = nullptr;
    QComboBox* m_verbosity = nullptr;
    QDoubleSpinBox* m_tolerance = nullptr;
    QComboBox* m_cycleType = nullptr;
    QSpinBox* m_preSweeps = nullptr;
    QSpinBox* m_postSweeps = nullptr;
    QSpinBox* m_maxLevels = nullptr;

    // Coarse solve
    QCheckBox* m_directSolver = nullptr;
    QSpinBox* m_directMaxEquations = nullptr;
    QDoubleSpinBox* m_epsilon = nullptr;

    // Smoother / acceleration
    QComboBox* m_smoother = nullptr;
    QComboBox* m_acceleration = nullptr;
    QComboBox* m_scaling = nullptr;

    // Aggregation
    QComboBox* m_groupSizeControl = nullptr;
    QSpinBox* m_groupSize = nullptr;
    // Last value entered while Manual was active, so switching Automatic ->
    // Manual restores it instead of resetting to 1.
    int m_manualGroupSize = 1;

    // Coarsening (expert)
    QComboBox* m_coarsening = nullptr;
    QDoubleSpinBox* m_strength = nullptr;
    QDoubleSpinBox* m_truncation = nullptr;
    QSpinBox* m_minCoarse = nullptr;
    QCheckBox* m_nonsymmetric = nullptr;

    // Robustness (expert)
    QDoubleSpinBox* m_stagnationFactor = nullptr;
    QSpinBox* m_stagnationMinIter = nullptr;
    QDoubleSpinBox* m_stagnationLoose = nullptr;

    // Flexible cycle
    // Owned by the grid; hidden unless the cycle type is Flexible.
    QGroupBox* m_flexGroup = nullptr;
    QDoubleSpinBox* m_restrictionTolerance = nullptr;
    QDoubleSpinBox* m_prolongationTolerance = nullptr;
    QSpinBox* m_flexSweeps = nullptr;
    QSpinBox* m_maxFineRelaxations = nullptr;
    QSpinBox* m_maxCoarseRelaxations = nullptr;
};
