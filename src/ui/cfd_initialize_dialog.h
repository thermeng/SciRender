#pragma once

#include <QDialog>

#include "cfd/cfd_session.h"

class QDoubleSpinBox;

namespace sidebar {

// Initial field values for a fresh solve. Always opens at zero; k/omega are
// optional and 0 means "auto" (derived from the initial velocity), which is
// what the solver does when either is non-positive.
class CfdInitializeDialog : public QDialog {
    Q_OBJECT
public:
    CfdInitializeDialog(int dim, bool turbulence, QWidget* parent = nullptr);

    cfd::InitConditions conditions() const;

private:
    int m_dim = 3;
    QDoubleSpinBox* m_ux = nullptr;
    QDoubleSpinBox* m_uy = nullptr;
    QDoubleSpinBox* m_uz = nullptr;
    QDoubleSpinBox* m_pressure = nullptr;
    QDoubleSpinBox* m_k = nullptr;
    QDoubleSpinBox* m_omega = nullptr;
};

} // namespace sidebar