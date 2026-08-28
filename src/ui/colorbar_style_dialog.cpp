#include "ui/colorbar_style_dialog.h"
#include "ui/colormap_combo.h"
#include "ui/range_editor.h"

#include "render/settings/render_settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <functional>

ColorbarStyleDialog::ColorbarStyleDialog(RenderSettings* settings, const QString& barSubtitle,
                                           int initialBandCount, QWidget* parent)
    : QDialog(parent), m_settings(settings), m_barSubtitle(barSubtitle) {
    setWindowTitle(m_barSubtitle.isEmpty() ? QString("Colorbar Style")
                                           : QString("Colorbar Style — %1").arg(m_barSubtitle));
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

    // Snapshot palette per bar subtitle for Cancel restore.
    if (m_barSubtitle == "Scalar") {
        m_initialPaletteChoice = m_settings->getColormapChoice();
        m_initialPaletteReversed = m_settings->getColormapReversed();
    } else if (m_barSubtitle == "Vector") {
        m_initialPaletteChoice = m_settings->getVectorColormapChoice();
        m_initialPaletteReversed = m_settings->getVectorColormapReversed();
    } else if (m_barSubtitle == "Streamline") {
        m_initialPaletteChoice = m_settings->getStreamlineColormapChoice();
        m_initialPaletteReversed = m_settings->getStreamlineColormapReversed();
    } else if (m_barSubtitle == "Volume") {
        m_initialPaletteChoice = m_settings->getVolumeColormapChoice();
        m_initialPaletteReversed = m_settings->getVolumeColormapReversed();
    } else if (m_barSubtitle == "Slice") {
        m_initialPaletteChoice = m_settings->getVolumeSliceColormapChoice();
        m_initialPaletteReversed = m_settings->getVolumeSliceColormapReversed();
    }

    // Snapshot Fixed Range per bar for Cancel restore.
    if (m_barSubtitle == "Scalar") {
        m_initialSimpleRangeEnabled = m_settings->getColorRangeOverrideEnabled();
        m_initialSimpleRangeLo = m_settings->getColorRangeLo();
        m_initialSimpleRangeHi = m_settings->getColorRangeHi();
    } else if (m_barSubtitle == "Volume") {
        m_initialSimpleRangeEnabled = m_settings->getVolumeColorRangeOverrideEnabled();
        m_initialSimpleRangeLo = m_settings->getVolumeColorRangeLo();
        m_initialSimpleRangeHi = m_settings->getVolumeColorRangeHi();
    } else if (m_barSubtitle == "Slice") {
        m_initialSimpleRangeEnabled = m_settings->getSliceColorRangeOverrideEnabled();
        m_initialSimpleRangeLo = m_settings->getSliceColorRangeLo();
        m_initialSimpleRangeHi = m_settings->getSliceColorRangeHi();
    } else if (m_barSubtitle == "Vector") {
        m_initialGlyphMagEnabled = m_settings->getGlyphMagRangeOverrideEnabled();
        m_initialGlyphMagLo = m_settings->getGlyphMagRangeLo();
        m_initialGlyphMagHi = m_settings->getGlyphMagRangeHi();
        for (int c = 0; c < 3; ++c) {
            m_initialGlyphCompEnabled[c] = m_settings->getGlyphCompRangeOverrideEnabled(c);
            m_initialGlyphCompLo[c] = m_settings->getGlyphCompRangeLo(c);
            m_initialGlyphCompHi[c] = m_settings->getGlyphCompRangeHi(c);
        }
        int mode = m_settings->getVectorColorMode();
        m_rangeBoundComp = (mode >= 2) ? mode - 2 : -1;
    } else if (m_barSubtitle == "Streamline") {
        m_initialStreamlineMagEnabled = m_settings->getStreamlineMagRangeOverrideEnabled();
        m_initialStreamlineMagLo = m_settings->getStreamlineMagRangeLo();
        m_initialStreamlineMagHi = m_settings->getStreamlineMagRangeHi();
        for (int c = 0; c < 3; ++c) {
            m_initialStreamlineCompEnabled[c] = m_settings->getStreamlineCompRangeOverrideEnabled(c);
            m_initialStreamlineCompLo[c] = m_settings->getStreamlineCompRangeLo(c);
            m_initialStreamlineCompHi[c] = m_settings->getStreamlineCompRangeHi(c);
        }
        int mode = m_settings->getStreamlineColorMode();
        m_rangeBoundComp = (mode >= 2) ? mode - 2 : -1;
    }

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

    // Palette per bar. Live-apply like other style rows. Use shared combo with
    // swatch preview. Use Colormap toggle stays in sidebar per spec.
    {
        if (!m_barSubtitle.isEmpty()) {
            auto* palHeader = new QLabel(QString("Palette — %1").arg(m_barSubtitle), this);
            QFont f = palHeader->font(); f.setBold(true); palHeader->setFont(f);
            form->addRow(palHeader);
        }
        auto paletteSetter = [this](int v) {
            if (m_barSubtitle == "Scalar") m_settings->setColormapChoice(v);
            else if (m_barSubtitle == "Vector") m_settings->setVectorColormapChoice(v);
            else if (m_barSubtitle == "Streamline") m_settings->setStreamlineColormapChoice(v);
            else if (m_barSubtitle == "Volume") m_settings->setVolumeColormapChoice(v);
            else if (m_barSubtitle == "Slice") m_settings->setVolumeSliceColormapChoice(v);
        };
        m_paletteCombo = createColormapCombo(m_initialPaletteChoice, paletteSetter, this);
        form->addRow("Palette", m_paletteCombo);
        if (m_barSubtitle.isEmpty() || (m_barSubtitle != "Scalar" && m_barSubtitle != "Vector" &&
            m_barSubtitle != "Streamline" && m_barSubtitle != "Volume" && m_barSubtitle != "Slice")) {
            m_paletteCombo->setEnabled(false);
        }
        m_paletteReverse = new QCheckBox("Reverse Palette", this);
        m_paletteReverse->setChecked(m_initialPaletteReversed);
        form->addRow(QString(), m_paletteReverse);
        auto reverseSetter = [this](bool v) {
            if (m_barSubtitle == "Scalar") m_settings->setColormapReversed(v);
            else if (m_barSubtitle == "Vector") m_settings->setVectorColormapReversed(v);
            else if (m_barSubtitle == "Streamline") m_settings->setStreamlineColormapReversed(v);
            else if (m_barSubtitle == "Volume") m_settings->setVolumeColormapReversed(v);
            else if (m_barSubtitle == "Slice") m_settings->setVolumeSliceColormapReversed(v);
        };
        connect(m_paletteReverse, &QCheckBox::toggled, this, reverseSetter);
        // Keep dialog in sync if palette changes externally (sidebar alias) while open.
        connect(m_settings, &RenderSettings::viewChanged, this, [this](ChangeFlags flags) {
            if (!(flags & ChangeFlag::Colormap)) return;
            int curChoice = 0; bool curRev = false;
            if (m_barSubtitle == "Scalar") { curChoice = m_settings->getColormapChoice(); curRev = m_settings->getColormapReversed(); }
            else if (m_barSubtitle == "Vector") { curChoice = m_settings->getVectorColormapChoice(); curRev = m_settings->getVectorColormapReversed(); }
            else if (m_barSubtitle == "Streamline") { curChoice = m_settings->getStreamlineColormapChoice(); curRev = m_settings->getStreamlineColormapReversed(); }
            else if (m_barSubtitle == "Volume") { curChoice = m_settings->getVolumeColormapChoice(); curRev = m_settings->getVolumeColormapReversed(); }
            else if (m_barSubtitle == "Slice") { curChoice = m_settings->getVolumeSliceColormapChoice(); curRev = m_settings->getVolumeSliceColormapReversed(); }
            else return;
            if (m_paletteCombo && m_paletteCombo->currentIndex() != curChoice) {
                m_paletteCombo->blockSignals(true); m_paletteCombo->setCurrentIndex(curChoice); m_paletteCombo->blockSignals(false);
            }
            if (m_paletteReverse && m_paletteReverse->isChecked() != curRev) {
                m_paletteReverse->blockSignals(true); m_paletteReverse->setChecked(curRev); m_paletteReverse->blockSignals(false);
            }
        });
    }

    // Fixed Range per bar (RangeEditor + Enable checkbox). Adaptive for Vector/Streamline.
    {
        auto* rangeHeader = new QLabel(QString("Fixed Range — %1").arg(m_barSubtitle.isEmpty() ? QString("scalar") : m_barSubtitle), this);
        QFont hf = rangeHeader->font(); hf.setBold(true); rangeHeader->setFont(hf);
        form->addRow(rangeHeader);

        m_rangeModeLabel = new QLabel(this);
        m_rangeModeLabel->setStyleSheet("color: gray; font-size: 10px;");
        m_rangeModeLabel->setWordWrap(true);
        form->addRow(m_rangeModeLabel);

        m_rangeEnable = new QCheckBox("Fixed Range", this);
        if (m_barSubtitle == "Scalar") m_rangeEnable->setToolTip("Map a fixed value range to the palette; values below/above clamp to the end colors");
        else if (m_barSubtitle == "Volume") m_rangeEnable->setToolTip("Map a fixed value range to the volume palette; values outside clamp to the end colors");
        else if (m_barSubtitle == "Slice") m_rangeEnable->setToolTip("Map a fixed value range to the slice palette; values outside clamp to the end colors");
        else if (m_barSubtitle == "Vector") m_rangeEnable->setToolTip("Map a fixed range to glyph palette for the active 'Color By' mode; values outside clamp to end colors");
        else if (m_barSubtitle == "Streamline") m_rangeEnable->setToolTip("Map a fixed range to streamline palette for the active 'Color By' mode; values outside clamp to end colors");
        form->addRow(m_rangeEnable);

        m_rangeEditor = new RangeEditor(this);
        form->addRow(m_rangeEditor);

        // Live-apply wiring
        connect(m_rangeEnable, &QCheckBox::toggled, this, [this](bool v) { applyRangeEnable(v); });
        connect(m_rangeEditor, &RangeEditor::windowEdited, this, [this](double lo, double hi) { applyRangeWindow(lo, hi); });

        // Initial sync
        syncRangeFromSettings();

        // External changes while open (sidebar alias, mesh load, mode switch)
        connect(m_settings, &RenderSettings::viewChanged, this, [this](ChangeFlags flags) {
            if (flags & (ChangeFlag::Colormap | ChangeFlag::Display | ChangeFlag::Vectors)) syncRangeFromSettings();
        });
        connect(m_settings, &RenderSettings::meshLoadStateChanged, this, [this]() { syncRangeFromSettings(); });
        connect(m_settings, &RenderSettings::meshDataUpdated, this, [this]() { syncRangeFromSettings(); });
    }

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
        // Reset palette to per-pass defaults (covers requirement: Reset resets palette + all other settings).
        int defChoice = 0;
        if (m_barSubtitle == "Scalar") defChoice = 0;
        else if (m_barSubtitle == "Vector") defChoice = 0;
        else if (m_barSubtitle == "Streamline") defChoice = 3;
        else if (m_barSubtitle == "Volume") defChoice = 3;
        else if (m_barSubtitle == "Slice") defChoice = 3;
        if (m_paletteCombo) m_paletteCombo->setCurrentIndex(defChoice);
        if (m_paletteReverse) m_paletteReverse->setChecked(false);
        if (m_bandCount && m_bandCount->isVisible()) m_bandCount->setValue(0);
        // Reset Fixed Range to full data bounds (disabled)
        if (m_rangeEnable) m_rangeEnable->setChecked(false);
        // RangeEditor Reset button will snap window to bounds, but also force sync
        // to clear override-driven window. Disabling already handled via toggled signal.
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
    // Restore palette (atomic with style)
    if (m_barSubtitle == "Scalar") { m_settings->setColormapChoice(m_initialPaletteChoice); m_settings->setColormapReversed(m_initialPaletteReversed); }
    else if (m_barSubtitle == "Vector") { m_settings->setVectorColormapChoice(m_initialPaletteChoice); m_settings->setVectorColormapReversed(m_initialPaletteReversed); }
    else if (m_barSubtitle == "Streamline") { m_settings->setStreamlineColormapChoice(m_initialPaletteChoice); m_settings->setStreamlineColormapReversed(m_initialPaletteReversed); }
    else if (m_barSubtitle == "Volume") { m_settings->setVolumeColormapChoice(m_initialPaletteChoice); m_settings->setVolumeColormapReversed(m_initialPaletteReversed); }
    else if (m_barSubtitle == "Slice") { m_settings->setVolumeSliceColormapChoice(m_initialPaletteChoice); m_settings->setVolumeSliceColormapReversed(m_initialPaletteReversed); }
    // Restore Fixed Range per bar
    if (m_barSubtitle == "Scalar") {
        m_settings->setColorRangeOverrideEnabled(m_initialSimpleRangeEnabled);
        m_settings->setColorRangeLo(m_initialSimpleRangeLo);
        m_settings->setColorRangeHi(m_initialSimpleRangeHi);
    } else if (m_barSubtitle == "Volume") {
        m_settings->setVolumeColorRangeOverrideEnabled(m_initialSimpleRangeEnabled);
        m_settings->setVolumeColorRangeLo(m_initialSimpleRangeLo);
        m_settings->setVolumeColorRangeHi(m_initialSimpleRangeHi);
    } else if (m_barSubtitle == "Slice") {
        m_settings->setSliceColorRangeOverrideEnabled(m_initialSimpleRangeEnabled);
        m_settings->setSliceColorRangeLo(m_initialSimpleRangeLo);
        m_settings->setSliceColorRangeHi(m_initialSimpleRangeHi);
    } else if (m_barSubtitle == "Vector") {
        m_settings->setGlyphMagRangeOverrideEnabled(m_initialGlyphMagEnabled);
        m_settings->setGlyphMagRangeLo(m_initialGlyphMagLo);
        m_settings->setGlyphMagRangeHi(m_initialGlyphMagHi);
        for (int c = 0; c < 3; ++c) {
            m_settings->setGlyphCompRangeOverrideEnabled(c, m_initialGlyphCompEnabled[c]);
            m_settings->setGlyphCompRangeLo(c, m_initialGlyphCompLo[c]);
            m_settings->setGlyphCompRangeHi(c, m_initialGlyphCompHi[c]);
        }
    } else if (m_barSubtitle == "Streamline") {
        m_settings->setStreamlineMagRangeOverrideEnabled(m_initialStreamlineMagEnabled);
        m_settings->setStreamlineMagRangeLo(m_initialStreamlineMagLo);
        m_settings->setStreamlineMagRangeHi(m_initialStreamlineMagHi);
        for (int c = 0; c < 3; ++c) {
            m_settings->setStreamlineCompRangeOverrideEnabled(c, m_initialStreamlineCompEnabled[c]);
            m_settings->setStreamlineCompRangeLo(c, m_initialStreamlineCompLo[c]);
            m_settings->setStreamlineCompRangeHi(c, m_initialStreamlineCompHi[c]);
        }
    }
    if (m_bandCount->isVisible()) {
        if (m_barSubtitle == "Scalar") m_settings->setScalarColorBands(m_initialBandCount);
        else if (m_barSubtitle == "Vector") m_settings->setVectorColorBands(m_initialBandCount);
        else if (m_barSubtitle == "Streamline") m_settings->setStreamlineColorBands(m_initialBandCount);
        else if (m_barSubtitle == "Volume") m_settings->setVolumeColorBands(m_initialBandCount);
        else if (m_barSubtitle == "Slice") m_settings->setVolumeSliceColorBands(m_initialBandCount);
    }
    QDialog::reject();
}

void ColorbarStyleDialog::syncRangeFromSettings() {
    if (!m_rangeEnable || !m_rangeEditor) return;

    // Helper to get scalar-style bounds
    auto scalarBounds = [this]() -> std::pair<double,double> {
        double lo = m_settings->getDataScalarMinQml();
        double hi = m_settings->getDataScalarMaxQml();
        if (!(hi > lo)) hi = lo + 1.0;
        return {lo, hi};
    };

    if (m_barSubtitle == "Scalar") {
        auto [bLo, bHi] = scalarBounds();
        bool en = m_settings->getColorRangeOverrideEnabled();
        double lo = m_settings->getColorRangeLo();
        double hi = m_settings->getColorRangeHi();
        bool hasScalars = m_settings->hasMeshScalars();
        m_rangeModeLabel->setText("Scalar field range");
        m_rangeModeLabel->setVisible(true);
        m_rangeEnable->blockSignals(true); m_rangeEnable->setChecked(en); m_rangeEnable->setEnabled(hasScalars); m_rangeEnable->blockSignals(false);
        m_rangeEditor->blockSignals(true);
        m_rangeEditor->setBounds(bLo, bHi);
        if (hi > lo) m_rangeEditor->setWindow(lo, hi); else m_rangeEditor->setWindow(bLo, bHi);
        m_rangeEditor->blockSignals(false);
        m_rangeEnable->setToolTip("Map a fixed value range to the palette; values below/above clamp to the end colors");
        m_rangeEditor->setEnabled(en && m_rangeEnable->isEnabled());
        m_rangeEditor->setToolTip(m_rangeEnable->toolTip());
    } else if (m_barSubtitle == "Volume") {
        auto [bLo, bHi] = scalarBounds();
        bool en = m_settings->getVolumeColorRangeOverrideEnabled();
        double lo = m_settings->getVolumeColorRangeLo();
        double hi = m_settings->getVolumeColorRangeHi();
        bool hasVol = m_settings->hasVolumeData();
        m_rangeModeLabel->setText("Volume field range");
        m_rangeModeLabel->setVisible(true);
        m_rangeEnable->blockSignals(true); m_rangeEnable->setChecked(en); m_rangeEnable->setEnabled(hasVol); m_rangeEnable->blockSignals(false);
        m_rangeEditor->blockSignals(true);
        m_rangeEditor->setBounds(bLo, bHi);
        if (hi > lo) m_rangeEditor->setWindow(lo, hi); else m_rangeEditor->setWindow(bLo, bHi);
        m_rangeEditor->blockSignals(false);
        m_rangeEditor->setEnabled(en && m_rangeEnable->isEnabled());
    } else if (m_barSubtitle == "Slice") {
        auto [bLo, bHi] = scalarBounds();
        bool en = m_settings->getSliceColorRangeOverrideEnabled();
        double lo = m_settings->getSliceColorRangeLo();
        double hi = m_settings->getSliceColorRangeHi();
        bool hasVol = m_settings->hasVolumeData();
        m_rangeModeLabel->setText("Slice field range (shared across X/Y/Z planes)");
        m_rangeModeLabel->setVisible(true);
        m_rangeEnable->blockSignals(true); m_rangeEnable->setChecked(en); m_rangeEnable->setEnabled(hasVol); m_rangeEnable->blockSignals(false);
        m_rangeEditor->blockSignals(true);
        m_rangeEditor->setBounds(bLo, bHi);
        if (hi > lo) m_rangeEditor->setWindow(lo, hi); else m_rangeEditor->setWindow(bLo, bHi);
        m_rangeEditor->blockSignals(false);
        m_rangeEditor->setEnabled(en && m_rangeEnable->isEnabled());
    } else if (m_barSubtitle == "Vector") {
        int mode = m_settings->getVectorColorMode();
        int comp = (mode >= 2) ? mode - 2 : -1;
        m_rangeBoundComp = comp;
        bool solidOrNone = (mode == 0) || !(m_settings->hasMeshVectors() || m_settings->hasMeshCellVectors());
        if (solidOrNone) {
            m_rangeModeLabel->setText("Fixed Range disabled — Color By is Solid Color or no vectors");
            m_rangeModeLabel->setVisible(true);
            m_rangeEnable->blockSignals(true); m_rangeEnable->setChecked(false); m_rangeEnable->setEnabled(false); m_rangeEnable->blockSignals(false);
            m_rangeEditor->setEnabled(false);
            return;
        }
        double bLo = 0, bHi = 1, lo, hi; bool en = false;
        if (comp < 0) {
            en = m_settings->getGlyphMagRangeOverrideEnabled();
            lo = m_settings->getGlyphMagRangeLo();
            hi = m_settings->getGlyphMagRangeHi();
            if (m_settings->backend()) { bLo = m_settings->backend()->vectorMagMin(); bHi = m_settings->backend()->vectorMagMax(); if (!(bHi > bLo)) bHi = bLo + 1.0; }
            m_rangeModeLabel->setText("Mapping: Magnitude");
        } else {
            en = m_settings->getGlyphCompRangeOverrideEnabled(comp);
            lo = m_settings->getGlyphCompRangeLo(comp);
            hi = m_settings->getGlyphCompRangeHi(comp);
            if (m_settings->backend()) { bLo = m_settings->backend()->vectorCompMin(comp); bHi = m_settings->backend()->vectorCompMax(comp); if (!(bHi > bLo)) bHi = bLo + 1.0; }
            m_rangeModeLabel->setText(QString("Mapping: %1 Component").arg(comp == 0 ? "X" : comp == 1 ? "Y" : "Z"));
        }
        m_rangeModeLabel->setVisible(true);
        m_rangeEnable->blockSignals(true); m_rangeEnable->setChecked(en); m_rangeEnable->setEnabled(true); m_rangeEnable->blockSignals(false);
        m_rangeEditor->blockSignals(true);
        m_rangeEditor->setBounds(bLo, bHi);
        if (hi > lo) m_rangeEditor->setWindow(lo, hi); else m_rangeEditor->setWindow(bLo, bHi);
        m_rangeEditor->blockSignals(false);
        m_rangeEditor->setEnabled(en && m_rangeEnable->isEnabled());
    } else if (m_barSubtitle == "Streamline") {
        int mode = m_settings->getStreamlineColorMode();
        int comp = (mode >= 2) ? mode - 2 : -1;
        m_rangeBoundComp = comp;
        bool solidOrNone = (mode == 0) || !m_settings->hasMeshVectors();
        if (solidOrNone) {
            m_rangeModeLabel->setText("Fixed Range disabled — Color By is Solid Color or no vectors");
            m_rangeModeLabel->setVisible(true);
            m_rangeEnable->blockSignals(true); m_rangeEnable->setChecked(false); m_rangeEnable->setEnabled(false); m_rangeEnable->blockSignals(false);
            m_rangeEditor->setEnabled(false);
            return;
        }
        double bLo = 0, bHi = 1, lo, hi; bool en = false;
        if (comp < 0) {
            en = m_settings->getStreamlineMagRangeOverrideEnabled();
            lo = m_settings->getStreamlineMagRangeLo();
            hi = m_settings->getStreamlineMagRangeHi();
            if (m_settings->backend()) { bLo = m_settings->backend()->streamlineMagMin(); bHi = m_settings->backend()->streamlineMagMax(); if (!(bHi > bLo)) bHi = bLo + 1.0; }
            m_rangeModeLabel->setText("Mapping: Magnitude");
        } else {
            en = m_settings->getStreamlineCompRangeOverrideEnabled(comp);
            lo = m_settings->getStreamlineCompRangeLo(comp);
            hi = m_settings->getStreamlineCompRangeHi(comp);
            if (m_settings->backend()) { bLo = m_settings->backend()->streamlineCompMin(comp); bHi = m_settings->backend()->streamlineCompMax(comp); if (!(bHi > bLo)) bHi = bLo + 1.0; }
            m_rangeModeLabel->setText(QString("Mapping: %1 Component").arg(comp == 0 ? "X" : comp == 1 ? "Y" : "Z"));
        }
        m_rangeModeLabel->setVisible(true);
        m_rangeEnable->blockSignals(true); m_rangeEnable->setChecked(en); m_rangeEnable->setEnabled(true); m_rangeEnable->blockSignals(false);
        m_rangeEditor->blockSignals(true);
        m_rangeEditor->setBounds(bLo, bHi);
        if (hi > lo) m_rangeEditor->setWindow(lo, hi); else m_rangeEditor->setWindow(bLo, bHi);
        m_rangeEditor->blockSignals(false);
        m_rangeEditor->setEnabled(en && m_rangeEnable->isEnabled());
    } else {
        m_rangeModeLabel->setVisible(false);
        m_rangeEnable->setEnabled(false);
        m_rangeEditor->setEnabled(false);
    }
}

void ColorbarStyleDialog::applyRangeEnable(bool v) {
    if (m_barSubtitle == "Scalar") m_settings->setColorRangeOverrideEnabled(v);
    else if (m_barSubtitle == "Volume") m_settings->setVolumeColorRangeOverrideEnabled(v);
    else if (m_barSubtitle == "Slice") m_settings->setSliceColorRangeOverrideEnabled(v);
    else if (m_barSubtitle == "Vector") {
        if (m_rangeBoundComp < 0) m_settings->setGlyphMagRangeOverrideEnabled(v);
        else m_settings->setGlyphCompRangeOverrideEnabled(m_rangeBoundComp, v);
    } else if (m_barSubtitle == "Streamline") {
        if (m_rangeBoundComp < 0) m_settings->setStreamlineMagRangeOverrideEnabled(v);
        else m_settings->setStreamlineCompRangeOverrideEnabled(m_rangeBoundComp, v);
    }
    if (m_rangeEditor) m_rangeEditor->setEnabled(v && m_rangeEnable && m_rangeEnable->isEnabled());
}

void ColorbarStyleDialog::applyRangeWindow(double lo, double hi) {
    if (m_barSubtitle == "Scalar") { m_settings->setColorRangeLo(static_cast<float>(lo)); m_settings->setColorRangeHi(static_cast<float>(hi)); }
    else if (m_barSubtitle == "Volume") { m_settings->setVolumeColorRangeLo(static_cast<float>(lo)); m_settings->setVolumeColorRangeHi(static_cast<float>(hi)); }
    else if (m_barSubtitle == "Slice") { m_settings->setSliceColorRangeLo(static_cast<float>(lo)); m_settings->setSliceColorRangeHi(static_cast<float>(hi)); }
    else if (m_barSubtitle == "Vector") {
        if (m_rangeBoundComp < 0) { m_settings->setGlyphMagRangeLo(static_cast<float>(lo)); m_settings->setGlyphMagRangeHi(static_cast<float>(hi)); }
        else { m_settings->setGlyphCompRangeLo(m_rangeBoundComp, static_cast<float>(lo)); m_settings->setGlyphCompRangeHi(m_rangeBoundComp, static_cast<float>(hi)); }
    } else if (m_barSubtitle == "Streamline") {
        if (m_rangeBoundComp < 0) { m_settings->setStreamlineMagRangeLo(static_cast<float>(lo)); m_settings->setStreamlineMagRangeHi(static_cast<float>(hi)); }
        else { m_settings->setStreamlineCompRangeLo(m_rangeBoundComp, static_cast<float>(lo)); m_settings->setStreamlineCompRangeHi(m_rangeBoundComp, static_cast<float>(hi)); }
    }
}
