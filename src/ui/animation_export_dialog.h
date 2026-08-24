#pragma once
// Export Animation dialog: format (AVI/PNG), fps, resolution, quality and
// frame range for a loaded .pvd sequence.

#include <QDialog>
#include "render/settings/AnimationExporter.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;

class AnimationController;

class AnimationExportDialog : public QDialog {
    Q_OBJECT
public:
    AnimationExportDialog(AnimationController* controller, QSize viewportSize,
                          QWidget* parent = nullptr);

    AnimationExportConfig config() const;

private:
    void browseAvi();
    void browsePngDir();

    AnimationController* m_controller = nullptr;
    QSize m_viewportSize;

    QCheckBox* m_aviCheck = nullptr;
    QLineEdit* m_aviPath = nullptr;
    QCheckBox* m_pngCheck = nullptr;
    QLineEdit* m_pngDir = nullptr;
    QDoubleSpinBox* m_fps = nullptr;
    QComboBox* m_resolution = nullptr;
    QSpinBox* m_quality = nullptr;
    QSpinBox* m_from = nullptr;
    QSpinBox* m_to = nullptr;
};
