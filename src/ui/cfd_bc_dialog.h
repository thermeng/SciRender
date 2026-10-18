#pragma once

#include <QDialog>

#include <cstddef>
#include <vector>

#include "cfd/cfd_types.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QTableWidget;
class QWidget;

// Patch-by-patch boundary-condition editor.
//
// The old editor put the condition type and its parameters into editable table
// cells, so every row had a different height and the parameter labels shrank to
// cryptic tokens ("bu", "bv", "k"). This dialog splits the two jobs: the left
// table lists every patch and its assigned condition for a one-glance overview,
// the right pane edits the selected patch with properly labelled fields. Each
// row is uniform, and only the fields that matter for the chosen condition are
// shown.
class CfdBoundaryConditionsDialog : public QDialog {
    Q_OBJECT
public:
    CfdBoundaryConditionsDialog(std::vector<cfd::PatchBC> patchBCs,
                                std::vector<std::size_t> faceCounts,
                                QWidget* parent = nullptr);

    // Resolved per-patch conditions after the dialog is accepted.
    const std::vector<cfd::PatchBC>& patchBCs() const { return m_bcs; }

private:
    void buildTable();
    void buildDetail(QWidget* detail);

    void selectRow(int row);
    void loadCurrent();
    void commitCurrent();
    void refreshConditionCell(int row);
    void updateConditionVisibility();
    void applyToAll(cfd::BCType type);

    std::vector<cfd::PatchBC> m_bcs;
    std::vector<std::size_t> m_faceCounts;

    int m_current = -1;
    bool m_syncing = false; // guards widget signals while loading a patch

    QTableWidget* m_table = nullptr;
    QLabel* m_patchTitle = nullptr;
    QLabel* m_patchMeta = nullptr;

    QComboBox* m_typeCombo = nullptr;

    QWidget* m_velocitySection = nullptr;
    QDoubleSpinBox* m_u = nullptr;
    QDoubleSpinBox* m_v = nullptr;
    QDoubleSpinBox* m_w = nullptr;

    QWidget* m_pressureSection = nullptr;
    QDoubleSpinBox* m_pressure = nullptr;
    QCheckBox* m_suppressBackflow = nullptr;
    QWidget* m_backflowBox = nullptr;
    QDoubleSpinBox* m_backflowU = nullptr;
    QDoubleSpinBox* m_backflowV = nullptr;
    QDoubleSpinBox* m_backflowW = nullptr;
    QDoubleSpinBox* m_backflowK = nullptr;
    QDoubleSpinBox* m_backflowOmega = nullptr;

    QLabel* m_hint = nullptr;
};
