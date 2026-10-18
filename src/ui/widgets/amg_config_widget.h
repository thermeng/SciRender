#pragma once

#include <QWidget>

#include "cfd/cfd_types.h"

class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QSpinBox;
class QVBoxLayout;

// Editor for one complete multigrid hierarchy configuration
// (fvm::AMGHierarchy::Config, mirrored by cfd::AmgSettings).
//
// Used twice by the CFD page: once for the pressure Poisson hierarchy and once
// for the momentum hierarchy. Both hierarchies expose the same knobs, so a
// single shared widget keeps the two from drifting apart and means a new
// AMGHierarchy::Config field only has to be added here once.
//
// The knobs are grouped the way they are reasoned about: how the hierarchy is
// built (coarsening), how a level is relaxed (cycle & smoother), how the
// standalone solver terminates, and the robustness guards.
class AmgConfigWidget : public QWidget {
    Q_OBJECT
public:
    // `defaults` is the preset the Reset button restores; see
    // cfd::AmgSettings::pressureDefaults()/momentumDefaults().
    explicit AmgConfigWidget(const cfd::AmgSettings& defaults, QWidget* parent = nullptr);

    void setConfig(const cfd::AmgSettings& settings);
    cfd::AmgSettings config() const;

    // Restore the preset passed to the constructor. Exposed as a method rather
    // than an in-widget button so the host dialog owns a single, unambiguous
    // "restore defaults" scope covering every tab.
    void resetToDefaults();

signals:
    // Emitted on any edit, so the host dialog can re-evaluate dependencies.
    void changed();

private:
    void buildUi();
    void syncDependencies();

    cfd::AmgSettings m_defaults;

    QComboBox* m_coarsening = nullptr;
    QComboBox* m_interpolation = nullptr;
    QComboBox* m_cycle = nullptr;
    QComboBox* m_smoother = nullptr;
    QDoubleSpinBox* m_smootherOmega = nullptr;
    QDoubleSpinBox* m_strength = nullptr;
    QDoubleSpinBox* m_truncation = nullptr;
    QDoubleSpinBox* m_galerkinDrop = nullptr;
    QSpinBox* m_maxLevels = nullptr;
    QSpinBox* m_minCoarse = nullptr;
    QSpinBox* m_preSweeps = nullptr;
    QSpinBox* m_postSweeps = nullptr;
    QSpinBox* m_maxIterations = nullptr;
    QDoubleSpinBox* m_tolerance = nullptr;
    QSpinBox* m_rngSeed = nullptr;
    QDoubleSpinBox* m_stagnationFactor = nullptr;
    QSpinBox* m_stagnationMinIter = nullptr;
    QDoubleSpinBox* m_stagnationLoose = nullptr;
};
