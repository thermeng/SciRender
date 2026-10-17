#include "view_display_page.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include "ui_view_display_page.h"
#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"

namespace sidebar {

ViewDisplayPage::ViewDisplayPage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    auto* content = new QWidget;
    Ui::ViewDisplayPage viewUi;
    viewUi.setupUi(content);
    sb::fixLayoutOverflow(content);

    auto* s = S();

    auto* orthoCb = viewUi.parallelCb;
    orthoCb->setChecked(s->getOrthographic());
    connect(orthoCb, &QCheckBox::toggled, s, &RenderSettings::setOrthographic);

    auto* rotateCb = viewUi.autoRotateCb;
    rotateCb->setChecked(s->getAutoRotate());
    connect(rotateCb, &QCheckBox::toggled, s, &RenderSettings::setAutoRotate);

    auto* resetCamBtn = viewUi.resetCamBtn;
    connect(resetCamBtn, &QPushButton::clicked, s, &RenderSettings::resetCamera);

    {
        auto* cb = viewUi.wireframeCb;
        auto* slider = viewUi.wireframeSlider;
        cb->setChecked(s->isWireframe());
        cb->setEnabled(true);
        connect(cb, &QCheckBox::toggled, s, &RenderSettings::setWireframe);
        slider->setRange(10, 100);
        slider->setValue(static_cast<int>(s->getLineWidth() * 10));
        slider->setEnabled(s->isWireframe());
        slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(slider, &QSlider::valueChanged, s, [s](int v) { s->setLineWidth(v / 10.0); });
        connect(cb, &QCheckBox::toggled, slider, &QWidget::setEnabled);
        m_wireframeCb = cb;
    }
    auto* surfaceCb = viewUi.surfaceCb;
    auto* pointsCb = viewUi.pointsCb;
    surfaceCb->setChecked(s->isSurfaceVisible());
    pointsCb->setChecked(s->getShowPoints());
    connect(surfaceCb, &QCheckBox::toggled, s, &RenderSettings::toggleSurface);
    connect(pointsCb, &QCheckBox::toggled, s, &RenderSettings::setShowPoints);
    m_surfaceCb = surfaceCb;
    m_pointsCb = pointsCb;
    viewUi.bboxCb->setChecked(s->getShowBounds());
    connect(viewUi.bboxCb, &QCheckBox::toggled, s, &RenderSettings::setShowBounds);
    m_bboxCb = viewUi.bboxCb;
    viewUi.defectsCb->setChecked(s->getShowQualityOverlay());
    connect(viewUi.defectsCb, &QCheckBox::toggled, s, &RenderSettings::setShowQualityOverlay);
    m_defectsCb = viewUi.defectsCb;

    {
        auto* slider = viewUi.pointSizeSlider;
        auto* valueLabel = viewUi.pointSizeValue;
        slider->setRange(static_cast<int>(1 * 1000), static_cast<int>(20 * 1000));
        slider->setValue(static_cast<int>(s->getPointSize() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            s->setPointSize(v);
        });
    }

    auto* scalarCb = viewUi.scalarCb;
    scalarCb->setChecked(s->getPointUseScalar());
    connect(scalarCb, &QCheckBox::toggled, s, &RenderSettings::setPointUseScalar);
    m_scalarCb = scalarCb;

    connect(pointsCb, &QCheckBox::toggled, viewUi.pointGroup, &QWidget::setVisible);
    viewUi.pointGroup->setVisible(s->getShowPoints());

    {
        auto* slider = viewUi.surfaceOpacitySlider;
        auto* valueLabel = viewUi.surfaceOpacityValue;
        slider->setRange(static_cast<int>(0.0 * 1000), static_cast<int>(1 * 1000));
        slider->setValue(static_cast<int>(s->getSurfaceOpacity() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            s->setSurfaceOpacity(v);
        });
    }
    {
        auto* slider = viewUi.pointOpacitySlider;
        auto* valueLabel = viewUi.pointOpacityValue;
        slider->setRange(static_cast<int>(0.0 * 1000), static_cast<int>(1 * 1000));
        slider->setValue(static_cast<int>(s->getPointOpacity() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            s->setPointOpacity(v);
        });
    }

    {
        auto* spin = viewUi.peelLayersSpin;
        spin->setValue(s->getMaxPeelLayers());
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), s, &RenderSettings::setMaxPeelLayers);
    }

    auto* gizmoCb = viewUi.gizmoCb;
    gizmoCb->setChecked(s->isGizmoVisible());
    connect(gizmoCb, &QCheckBox::toggled, s, &RenderSettings::setGizmoVisible);
    m_gizmoCb = gizmoCb;

    auto* gizmoSizeCombo = viewUi.gizmoSizeCombo;
    gizmoSizeCombo->setCurrentIndex(s->getGizmoSizeChoice());
    connect(gizmoSizeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            s, &RenderSettings::setGizmoSizeChoice);
    m_gizmoSizeCombo = gizmoSizeCombo;

    auto* gizmoCornerCombo = viewUi.gizmoCornerCombo;
    gizmoCornerCombo->setCurrentIndex(s->getGizmoCorner());
    connect(gizmoCornerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            s, &RenderSettings::setGizmoCorner);
    m_gizmoCornerCombo = gizmoCornerCombo;

    auto* fpsCb = viewUi.fpsCb;
    fpsCb->setChecked(s->getShowFps());
    connect(fpsCb, &QCheckBox::toggled, s, &RenderSettings::setShowFps);
    m_fpsCb = fpsCb;

    auto* parallelCb = viewUi.parallelCb;
    parallelCb->setChecked(s->getOrthographic());
    connect(parallelCb, &QCheckBox::toggled, s, &RenderSettings::setOrthographic);
    m_parallelCb = parallelCb;

    QColorDialog** dlgPtrs[4] = { &m_meshColorDialog, &m_surfaceColorDialog,
                                  &m_bgColorDialog, &m_textColorDialog };
    const char* names[4] = { "Wireframe", "Surface", "Background", "Text" };
    auto getters = { &RenderSettings::getMeshColorQml, &RenderSettings::getSurfaceColorQml,
                     &RenderSettings::getBgColorQml, &RenderSettings::getTextColorQml };
    void (RenderSettings::*setters[4])(const QColor&) = {
        &RenderSettings::setMeshColorQml, &RenderSettings::setSurfaceColorQml,
        &RenderSettings::setBgColorQml, &RenderSettings::setTextColorQml };
    QPushButton* placeholder[4] = { viewUi.wireframeColorBtn, viewUi.surfaceColorBtn,
                                    viewUi.bgColorBtn, viewUi.textColorBtn };
    int i = 0;
    for (auto getter : getters) {
        content->layout()->replaceWidget(placeholder[i],
            sb::detail::createColorButton(this, s, names[i], (s->*getter)(), dlgPtrs[i], setters[i]));
        delete placeholder[i];
        ++i;
    }

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

    // Keep quick-bar / shortcut toggles mirrored in the checkboxes.
    connect(s, &RenderSettings::viewChanged, this, [this](ChangeFlags) { syncPage(); });

    setContent(content);
}

void ViewDisplayPage::syncPage() {
    auto* s = S();
    if (m_wireframeCb) m_wireframeCb->setChecked(s->isWireframe());
    if (m_surfaceCb)   m_surfaceCb->setChecked(s->isSurfaceVisible());
    if (m_pointsCb)    m_pointsCb->setChecked(s->getShowPoints());
    if (m_bboxCb)      m_bboxCb->setChecked(s->getShowBounds());
    if (m_defectsCb)   m_defectsCb->setChecked(s->getShowQualityOverlay());
    if (m_scalarCb)    m_scalarCb->setChecked(s->getPointUseScalar());
    if (m_gizmoCb)     m_gizmoCb->setChecked(s->isGizmoVisible());
    if (m_gizmoSizeCombo)   m_gizmoSizeCombo->setCurrentIndex(s->getGizmoSizeChoice());
    if (m_gizmoCornerCombo) m_gizmoCornerCombo->setCurrentIndex(s->getGizmoCorner());
    if (m_fpsCb)       m_fpsCb->setChecked(s->getShowFps());
    if (m_parallelCb)  m_parallelCb->setChecked(s->getOrthographic());
}

} // namespace sidebar
