#pragma once
// Live-apply colorbar style dialog: edits flow straight into RenderSettings so
// the viewport re-renders on every change; Cancel restores the values the
// dialog was opened with.

#include <QDialog>

class QDoubleSpinBox;
class QCheckBox;
class QSlider;
class RenderSettings;

class ColorbarStyleDialog : public QDialog {
    Q_OBJECT
public:
    explicit ColorbarStyleDialog(RenderSettings* settings, QWidget* parent = nullptr);

protected:
    void reject() override;

private:
    RenderSettings* m_settings = nullptr;
    QDoubleSpinBox* m_fontScale = nullptr;
    QDoubleSpinBox* m_tickFontScale = nullptr;
    QDoubleSpinBox* m_lengthScale = nullptr;
    QDoubleSpinBox* m_thicknessScale = nullptr;
    QCheckBox* m_panelEnabled = nullptr;
    QSlider* m_panelOpacity = nullptr;
    QCheckBox* m_showAnnotation = nullptr;
    float m_initialFontScale = 1.0f;
    float m_initialTickFontScale = 1.0f;
    float m_initialLengthScale = 1.0f;
    float m_initialThicknessScale = 1.0f;
    bool m_initialPanelEnabled = false;
    float m_initialPanelOpacity = 0.55f;
    bool m_initialShowAnnotation = true;
};
