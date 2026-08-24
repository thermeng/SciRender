#include "ui/animation_export_dialog.h"

#include "render/settings/animation_controller.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

AnimationExportDialog::AnimationExportDialog(AnimationController* controller,
                                             QSize viewportSize, QWidget* parent)
    : QDialog(parent), m_controller(controller), m_viewportSize(viewportSize) {
    setWindowTitle("Export Animation");
    setModal(true);
    setMinimumWidth(420);

    const int frameCount = controller ? controller->frameCount() : 0;

    auto* form = new QFormLayout;

    // --- AVI ---
    m_aviCheck = new QCheckBox("AVI video (MJPEG)", this);
    m_aviCheck->setChecked(true);
    form->addRow(QString(), m_aviCheck);
    auto* aviRow = new QWidget(this);
    auto* aviLayout = new QHBoxLayout(aviRow);
    aviLayout->setContentsMargins(0, 0, 0, 0);
    m_aviPath = new QLineEdit(aviRow);
    m_aviPath->setPlaceholderText("output.avi");
    auto* aviBrowse = new QToolButton(aviRow);
    aviBrowse->setText("...");
    aviLayout->addWidget(m_aviPath, 1);
    aviLayout->addWidget(aviBrowse);
    form->addRow("AVI file", aviRow);

    // --- PNG sequence ---
    m_pngCheck = new QCheckBox("PNG frame sequence (lossless)", this);
    form->addRow(QString(), m_pngCheck);
    auto* pngRow = new QWidget(this);
    auto* pngLayout = new QHBoxLayout(pngRow);
    pngLayout->setContentsMargins(0, 0, 0, 0);
    m_pngDir = new QLineEdit(pngRow);
    m_pngDir->setPlaceholderText("output folder");
    m_pngDir->setEnabled(false);
    auto* pngBrowse = new QToolButton(pngRow);
    pngBrowse->setText("...");
    pngLayout->addWidget(m_pngDir, 1);
    pngLayout->addWidget(pngBrowse);
    form->addRow("PNG folder", pngRow);

    // --- Timing ---
    m_fps = new QDoubleSpinBox(this);
    m_fps->setRange(1.0, 60.0);
    m_fps->setDecimals(1);
    m_fps->setSingleStep(1.0);
    m_fps->setValue(controller ? controller->fps() : 8.0);
    form->addRow("FPS", m_fps);

    // --- Resolution ---
    m_resolution = new QComboBox(this);
    m_resolution->addItem(QString("Match window (%1 x %2)")
                              .arg(m_viewportSize.width()).arg(m_viewportSize.height()));
    m_resolution->addItem("1280 x 720");
    m_resolution->addItem("1920 x 1080");
    m_resolution->addItem("2560 x 1440");
    m_resolution->addItem("3840 x 2160");
    m_resolution->setCurrentIndex(2);
    form->addRow("Resolution", m_resolution);

    // --- Quality ---
    m_quality = new QSpinBox(this);
    m_quality->setRange(40, 100);
    m_quality->setValue(90);
    m_quality->setSuffix(" %");
    form->addRow("JPEG quality", m_quality);

    // --- Range (1-based for the user) ---
    m_from = new QSpinBox(this);
    m_from->setRange(1, qMax(1, frameCount));
    m_from->setValue(1);
    m_to = new QSpinBox(this);
    m_to->setRange(1, qMax(1, frameCount));
    m_to->setValue(frameCount);
    auto* rangeRow = new QWidget(this);
    auto* rangeLayout = new QHBoxLayout(rangeRow);
    rangeLayout->setContentsMargins(0, 0, 0, 0);
    rangeLayout->addWidget(m_from);
    rangeLayout->addWidget(new QLabel("to", rangeRow));
    rangeLayout->addWidget(m_to);
    rangeLayout->addStretch(1);
    form->addRow("Frames", rangeRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(buttons);

    connect(aviBrowse, &QToolButton::clicked, this, &AnimationExportDialog::browseAvi);
    connect(pngBrowse, &QToolButton::clicked, this, &AnimationExportDialog::browsePngDir);
    connect(m_aviCheck, &QCheckBox::toggled, m_aviPath, &QLineEdit::setEnabled);
    connect(m_pngCheck, &QCheckBox::toggled, m_pngDir, &QLineEdit::setEnabled);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!m_aviCheck->isChecked() && !m_pngCheck->isChecked()) {
            m_aviCheck->setFocus();
            return;
        }
        if (m_from->value() > m_to->value()) {
            m_from->setFocus();
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void AnimationExportDialog::browseAvi() {
    QString path = QFileDialog::getSaveFileName(this, "Save AVI", m_aviPath->text(),
                                                "AVI video (*.avi)");
    if (!path.isEmpty()) {
        if (!path.endsWith(".avi", Qt::CaseInsensitive)) path += ".avi";
        m_aviPath->setText(path);
    }
}

void AnimationExportDialog::browsePngDir() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select PNG output folder",
                                                    m_pngDir->text());
    if (!dir.isEmpty()) m_pngDir->setText(dir);
}

AnimationExportConfig AnimationExportDialog::config() const {
    AnimationExportConfig cfg;
    cfg.writeAvi = m_aviCheck->isChecked();
    cfg.writePng = m_pngCheck->isChecked();
    cfg.aviPath = m_aviPath->text();
    if (cfg.writeAvi && !cfg.aviPath.endsWith(".avi", Qt::CaseInsensitive))
        cfg.aviPath += ".avi";
    cfg.pngDir = m_pngDir->text();
    cfg.fps = m_fps->value();
    cfg.jpegQuality = m_quality->value();
    cfg.firstFrame = m_from->value() - 1;
    cfg.lastFrame = m_to->value() - 1;
    switch (m_resolution->currentIndex()) {
        case 0:  cfg.width = m_viewportSize.width(); cfg.height = m_viewportSize.height(); break;
        case 1:  cfg.width = 1280; cfg.height = 720; break;
        case 2:  cfg.width = 1920; cfg.height = 1080; break;
        case 3:  cfg.width = 2560; cfg.height = 1440; break;
        default: cfg.width = 3840; cfg.height = 2160; break;
    }
    return cfg;
}
