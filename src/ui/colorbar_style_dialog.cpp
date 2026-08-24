#include "ui/colorbar_style_dialog.h"

#include "render/settings/render_settings.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

ColorbarStyleDialog::ColorbarStyleDialog(RenderSettings* settings, QWidget* parent)
    : QDialog(parent), m_settings(settings) {
    setWindowTitle("Colorbar Legend Style");
    setModal(true);

    m_initialFontScale = m_settings->getColorbarFontScale();
    m_initialTickFontScale = m_settings->getColorbarTickFontScale();
    m_initialLengthScale = m_settings->getColorbarLengthScale();
    m_initialThicknessScale = m_settings->getColorbarThicknessScale();
    m_initialPanelEnabled = m_settings->getColorbarPanelEnabled();
    m_initialPanelOpacity = m_settings->getColorbarPanelOpacity();
    m_initialShowAnnotation = m_settings->getColorbarShowAnnotation();

    auto* form = new QFormLayout;

    m_fontScale = new QDoubleSpinBox(this);
    m_fontScale->setRange(0.6, 2.5);
    m_fontScale->setSingleStep(0.05);
    m_fontScale->setDecimals(2);
    m_fontScale->setValue(m_initialFontScale);
    form->addRow("Title Font Scale", m_fontScale);

    m_tickFontScale = new QDoubleSpinBox(this);
    m_tickFontScale->setRange(0.6, 2.5);
    m_tickFontScale->setSingleStep(0.05);
    m_tickFontScale->setDecimals(2);
    m_tickFontScale->setValue(m_initialTickFontScale);
    form->addRow("Tick Font Scale", m_tickFontScale);

    m_lengthScale = new QDoubleSpinBox(this);
    m_lengthScale->setRange(0.5, 2.0);
    m_lengthScale->setSingleStep(0.05);
    m_lengthScale->setDecimals(2);
    m_lengthScale->setValue(m_initialLengthScale);
    form->addRow("Bar Length", m_lengthScale);

    m_thicknessScale = new QDoubleSpinBox(this);
    m_thicknessScale->setRange(0.5, 3.0);
    m_thicknessScale->setSingleStep(0.05);
    m_thicknessScale->setDecimals(2);
    m_thicknessScale->setValue(m_initialThicknessScale);
    form->addRow("Bar Thickness", m_thicknessScale);

    m_showAnnotation = new QCheckBox("Show annotation", this);
    m_showAnnotation->setChecked(m_initialShowAnnotation);
    form->addRow(QString(), m_showAnnotation);

    m_panelEnabled = new QCheckBox("Background panel", this);
    m_panelEnabled->setChecked(m_initialPanelEnabled);
    form->addRow(QString(), m_panelEnabled);

    m_panelOpacity = new QSlider(Qt::Horizontal, this);
    m_panelOpacity->setRange(0, 90);
    m_panelOpacity->setValue(static_cast<int>(m_initialPanelOpacity * 100.0f));
    form->addRow("Panel Opacity %", m_panelOpacity);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
    auto* defaultsBtn = buttons->button(QDialogButtonBox::RestoreDefaults);
    defaultsBtn->setText("Reset Defaults");

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(buttons);

    connect(m_fontScale, &QDoubleSpinBox::valueChanged,
            m_settings, &RenderSettings::setColorbarFontScale);
    connect(m_tickFontScale, &QDoubleSpinBox::valueChanged,
            m_settings, &RenderSettings::setColorbarTickFontScale);
    connect(m_lengthScale, &QDoubleSpinBox::valueChanged,
            m_settings, &RenderSettings::setColorbarLengthScale);
    connect(m_thicknessScale, &QDoubleSpinBox::valueChanged,
            m_settings, &RenderSettings::setColorbarThicknessScale);
    connect(m_showAnnotation, &QCheckBox::toggled,
            m_settings, &RenderSettings::setColorbarShowAnnotation);
    connect(m_panelEnabled, &QCheckBox::toggled,
            m_settings, &RenderSettings::setColorbarPanelEnabled);
    connect(m_panelOpacity, &QSlider::valueChanged, this, [this](int v) {
        m_settings->setColorbarPanelOpacity(v / 100.0f);
    });
    connect(defaultsBtn, &QPushButton::clicked, this, [this]() {
        m_fontScale->setValue(1.0);
        m_tickFontScale->setValue(1.0);
        m_lengthScale->setValue(1.0);
        m_thicknessScale->setValue(1.0);
        m_showAnnotation->setChecked(true);
        m_panelEnabled->setChecked(false);
        m_panelOpacity->setValue(static_cast<int>(0.55f * 100.0f));
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void ColorbarStyleDialog::reject() {
    m_settings->setColorbarFontScale(m_initialFontScale);
    m_settings->setColorbarTickFontScale(m_initialTickFontScale);
    m_settings->setColorbarLengthScale(m_initialLengthScale);
    m_settings->setColorbarThicknessScale(m_initialThicknessScale);
    m_settings->setColorbarPanelEnabled(m_initialPanelEnabled);
    m_settings->setColorbarPanelOpacity(m_initialPanelOpacity);
    m_settings->setColorbarShowAnnotation(m_initialShowAnnotation);
    QDialog::reject();
}
