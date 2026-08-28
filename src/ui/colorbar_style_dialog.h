#pragma once
// Live-apply colorbar style dialog: edits flow straight into RenderSettings so
// the viewport re-renders on every change; Cancel restores the values the
// dialog was opened with.

#include <QDialog>

class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QSlider;
class QSpinBox;
class QLabel;
class RenderSettings;
class RangeEditor;

class ColorbarStyleDialog : public QDialog {
    Q_OBJECT
public:
    explicit ColorbarStyleDialog(RenderSettings* settings, const QString& barSubtitle,
                                 int initialBandCount, QWidget* parent = nullptr);

protected:
    void reject() override;

private:
    void syncRangeFromSettings();
    void applyRangeEnable(bool enabled);
    void applyRangeWindow(double lo, double hi);
    RenderSettings* m_settings = nullptr;
    QString m_barSubtitle;
    QComboBox* m_fontFamily = nullptr;
    QCheckBox* m_fontBold = nullptr;
    QCheckBox* m_fontItalic = nullptr;
    QDoubleSpinBox* m_fontScale = nullptr;
    QDoubleSpinBox* m_tickFontScale = nullptr;
    QDoubleSpinBox* m_lengthScale = nullptr;
    QDoubleSpinBox* m_thicknessScale = nullptr;
    QCheckBox* m_panelEnabled = nullptr;
    QSlider* m_panelOpacity = nullptr;
    QCheckBox* m_showAnnotation = nullptr;
    QSpinBox* m_bandCount = nullptr;
    QComboBox* m_paletteCombo = nullptr;
    QCheckBox* m_paletteReverse = nullptr;
    QCheckBox* m_rangeEnable = nullptr;
    RangeEditor* m_rangeEditor = nullptr;
    QLabel* m_rangeModeLabel = nullptr;
    int m_rangeBoundComp = -1; // -1 mag, 0..2 comp for Vector/Streamline
    int m_initialBandCount = 0;
    int m_initialPaletteChoice = 0;
    bool m_initialPaletteReversed = false;
    // Fixed range snapshots per pass (shared slot for Scalar/Volume/Slice)
    bool m_initialSimpleRangeEnabled = false;
    float m_initialSimpleRangeLo = 0.0f;
    float m_initialSimpleRangeHi = 1.0f;
    // Glyph adaptive
    bool m_initialGlyphMagEnabled = false;
    float m_initialGlyphMagLo = 0.0f;
    float m_initialGlyphMagHi = -1.0f;
    bool m_initialGlyphCompEnabled[3] = {false, false, false};
    float m_initialGlyphCompLo[3] = {0.0f, 0.0f, 0.0f};
    float m_initialGlyphCompHi[3] = {-1.0f, -1.0f, -1.0f};
    // Streamline adaptive
    bool m_initialStreamlineMagEnabled = false;
    float m_initialStreamlineMagLo = 0.0f;
    float m_initialStreamlineMagHi = -1.0f;
    bool m_initialStreamlineCompEnabled[3] = {false, false, false};
    float m_initialStreamlineCompLo[3] = {0.0f, 0.0f, 0.0f};
    float m_initialStreamlineCompHi[3] = {-1.0f, -1.0f, -1.0f};
    QString m_initialFontFamily;
    bool m_initialFontBold = false;
    bool m_initialFontItalic = false;
    float m_initialFontScale = 1.0f;
    float m_initialTickFontScale = 1.0f;
    float m_initialLengthScale = 1.0f;
    float m_initialThicknessScale = 1.0f;
    bool m_initialPanelEnabled = false;
    float m_initialPanelOpacity = 0.55f;
    bool m_initialShowAnnotation = true;
};
