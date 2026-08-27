#include "ui/colorbar_style_dialog.h"

#include "render/settings/render_settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <functional>

ColorbarStyleDialog::ColorbarStyleDialog(RenderSettings* settings, const QString& barSubtitle,
                                           int initialBandCount, QWidget* parent)
    : QDialog(parent), m_settings(settings), m_barSubtitle(barSubtitle) {
    setWindowTitle("Colorbar Style");
    setModal(true);

    m_initialFontFamily = m_settings->getColorbarFontFamily();
    m_initialFontBold = m_settings->getColorbarFontBold();
    m_initialFontItalic = m_settings->getColorbarFontItalic();
    m_initialFontScale = m_settings->getColorbarFontScale();
    m_initialTickFontScale = m_settings->getColorbarTickFontScale();
    m_initialLengthScale = m_settings->getColorbarLengthScale();
    m_initialThicknessScale = m_settings->getColorbarThicknessScale();
    m_initialPanelEnabled = m_settings->getColorbarPanelEnabled();
    m_initialPanelOpacity = m_settings->getColorbarPanelOpacity();
    m_initialShowAnnotation = m_settings->getColorbarShowAnnotation();

    auto* form = new QFormLayout;

    m_fontFamily = new QFontComboBox(this);
    m_fontFamily->setCurrentText(m_initialFontFamily);
    form->addRow("Font", m_fontFamily);

    auto* styleRow = new QWidget(this);
    auto* styleLayout = new QHBoxLayout(styleRow);
    styleLayout->setContentsMargins(0, 0, 0, 0);
    m_fontBold = new QCheckBox("Bold", styleRow);
    m_fontBold->setChecked(m_initialFontBold);
    m_fontItalic = new QCheckBox("Italic", styleRow);
    m_fontItalic->setChecked(m_initialFontItalic);
    styleLayout->addWidget(m_fontBold);
    styleLayout->addWidget(m_fontItalic);
    form->addRow(QString(), styleRow);

    m_fontScale = new QDoubleSpinBox(this);
    m_fontScale->setRange(0.6, 2.5);
    m_fontScale->setSingleStep(0.05);
    m_fontScale->setDecimals(2);
    m_fontScale->setValue(m_initialFontScale);
    form->addRow("Title font scale", m_fontScale);

    m_tickFontScale = new QDoubleSpinBox(this);
    m_tickFontScale->setRange(0.6, 2.5);
    m_tickFontScale->setSingleStep(0.05);
    m_tickFontScale->setDecimals(2);
    m_tickFontScale->setValue(m_initialTickFontScale);
    form->addRow("Tick font scale", m_tickFontScale);

    m_lengthScale = new QDoubleSpinBox(this);
    m_lengthScale->setRange(0.5, 2.0);
    m_lengthScale->setSingleStep(0.05);
    m_lengthScale->setDecimals(2);
    m_lengthScale->setValue(m_initialLengthScale);
    form->addRow("Bar length", m_lengthScale);

    m_thicknessScale = new QDoubleSpinBox(this);
    m_thicknessScale->setRange(0.5, 3.0);
    m_thicknessScale->setSingleStep(0.05);
    m_thicknessScale->setDecimals(2);
    m_thicknessScale->setValue(m_initialThicknessScale);
    form->addRow("Bar thickness", m_thicknessScale);

    m_showAnnotation = new QCheckBox("Show annotation", this);
    m_showAnnotation->setChecked(m_initialShowAnnotation);
    form->addRow(QString(), m_showAnnotation);

    m_panelEnabled = new QCheckBox("Background panel", this);
    m_panelEnabled->setChecked(m_initialPanelEnabled);
    form->addRow(QString(), m_panelEnabled);

    m_panelOpacity = new QSlider(Qt::Horizontal, this);
    m_panelOpacity->setRange(0, 90);
    m_panelOpacity->setValue(static_cast<int>(m_initialPanelOpacity * 100.0f));
    form->addRow("Panel opacity %", m_panelOpacity);

    // Per-bar band count control. Determine the bar type from its subtitle so we
    // can map to the correct RenderSettings setter. Hide the spinbox if the bar
    // type is unrecognized or the index is out of range.
    m_bandCount = new QSpinBox(this);
    m_bandCount->setRange(0, 32);
    m_bandCount->setSuffix(" bands");
    m_bandCount->setToolTip("Number of discrete color bands (0 = continuous)");
    {
        m_initialBandCount = initialBandCount;
        m_bandCount->setValue(m_initialBandCount);
        std::function<void(int)> setter = nullptr;
        if (m_barSubtitle == "Scalar") setter = [&](int v) { m_settings->setScalarColorBands(v); };
        else if (m_barSubtitle == "Vector") setter = [&](int v) { m_settings->setVectorColorBands(v); };
        else if (m_barSubtitle == "Streamline") setter = [&](int v) { m_settings->setStreamlineColorBands(v); };
        else if (m_barSubtitle == "Volume") setter = [&](int v) { m_settings->setVolumeColorBands(v); };
        else if (m_barSubtitle == "Slice") setter = [&](int v) { m_settings->setVolumeSliceColorBands(v); };
        if (setter) {
            connect(m_bandCount, &QSpinBox::valueChanged, this, setter);
        } else {
            m_bandCount->hide();
        }
    }
    form->addRow("Color bands", m_bandCount);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
    auto* defaultsBtn = buttons->button(QDialogButtonBox::RestoreDefaults);
    defaultsBtn->setText("Reset Defaults");

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(buttons);

    connect(m_fontFamily, &QComboBox::currentTextChanged,
            m_settings, &RenderSettings::setColorbarFontFamily);
    connect(m_fontBold, &QCheckBox::toggled,
            m_settings, &RenderSettings::setColorbarFontBold);
    connect(m_fontItalic, &QCheckBox::toggled,
            m_settings, &RenderSettings::setColorbarFontItalic);
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
        m_fontFamily->setCurrentText(QString());
        m_fontBold->setChecked(false);
        m_fontItalic->setChecked(false);
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
    m_settings->setColorbarFontFamily(m_initialFontFamily);
    m_settings->setColorbarFontBold(m_initialFontBold);
    m_settings->setColorbarFontItalic(m_initialFontItalic);
    m_settings->setColorbarFontScale(m_initialFontScale);
    m_settings->setColorbarTickFontScale(m_initialTickFontScale);
    m_settings->setColorbarLengthScale(m_initialLengthScale);
    m_settings->setColorbarThicknessScale(m_initialThicknessScale);
    m_settings->setColorbarPanelEnabled(m_initialPanelEnabled);
    m_settings->setColorbarPanelOpacity(m_initialPanelOpacity);
    m_settings->setColorbarShowAnnotation(m_initialShowAnnotation);
    if (m_bandCount->isVisible()) {
        if (m_barSubtitle == "Scalar") m_settings->setScalarColorBands(m_initialBandCount);
        else if (m_barSubtitle == "Vector") m_settings->setVectorColorBands(m_initialBandCount);
        else if (m_barSubtitle == "Streamline") m_settings->setStreamlineColorBands(m_initialBandCount);
        else if (m_barSubtitle == "Volume") m_settings->setVolumeColorBands(m_initialBandCount);
        else if (m_barSubtitle == "Slice") m_settings->setVolumeSliceColorBands(m_initialBandCount);
    }
    QDialog::reject();
}
