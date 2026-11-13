#include "ui/cfd_initialize_dialog.h"

#include "ui/uikit.h"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QVBoxLayout>

namespace sidebar {

CfdInitializeDialog::CfdInitializeDialog(int dim, bool turbulence, QWidget* parent)
    : QDialog(parent), m_dim(dim) {
    setWindowTitle("Initialize Solution");

    auto* lay = new QVBoxLayout(this);
    auto* form = uikit::form(nullptr);
    lay->addLayout(form);

    auto addField = [form](const QString& label, QDoubleSpinBox* spin) {
        form->addRow(label, spin);
    };

    m_ux = uikit::numeric(new QDoubleSpinBox(this));
    m_ux->setRange(-1e6, 1e6);
    m_ux->setDecimals(4);
    addField("Velocity X", m_ux);

    m_uy = uikit::numeric(new QDoubleSpinBox(this));
    m_uy->setRange(-1e6, 1e6);
    m_uy->setDecimals(4);
    addField("Velocity Y", m_uy);

    if (m_dim >= 3) {
        m_uz = uikit::numeric(new QDoubleSpinBox(this));
        m_uz->setRange(-1e6, 1e6);
        m_uz->setDecimals(4);
        addField("Velocity Z", m_uz);
    }

    m_pressure = uikit::numeric(new QDoubleSpinBox(this));
    m_pressure->setRange(-1e6, 1e6);
    m_pressure->setDecimals(4);
    addField("Pressure", m_pressure);

    if (turbulence) {
        m_k = uikit::numeric(new QDoubleSpinBox(this));
        m_k->setRange(0.0, 1e3);
        m_k->setDecimals(4);
        m_k->setSpecialValueText("Auto");
        m_k->setToolTip("0 = derive from the initial velocity (5% turbulence intensity).");
        addField("Turbulent Kinetic Energy (k)", m_k);

        m_omega = uikit::numeric(new QDoubleSpinBox(this));
        m_omega->setRange(0.0, 1e6);
        m_omega->setDecimals(4);
        m_omega->setSpecialValueText("Auto");
        m_omega->setToolTip("0 = derive from k and a 7% characteristic length.");
        addField("Specific Dissipation Rate (Omega)", m_omega);
    }

    auto* buttons = uikit::buttons(this, false);
    lay->addWidget(buttons);

    uikit::prepareDialog(this, 420);
}

cfd::InitConditions CfdInitializeDialog::conditions() const {
    cfd::InitConditions ic;
    ic.ux = m_ux->value();
    ic.uy = m_uy->value();
    ic.uz = m_uz ? m_uz->value() : 0.0;
    ic.pressure = m_pressure->value();
    ic.k = m_k ? m_k->value() : 0.0;
    ic.omega = m_omega ? m_omega->value() : 0.0;
    return ic;
}

} // namespace sidebar