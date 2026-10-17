#include "scalar_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QSlider>
#include <QVBoxLayout>

#include "ui_scalar_page.h"
#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"

namespace sidebar {

ScalarPage::ScalarPage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    auto* content = new QWidget;
    Ui::ScalarPage scalarUi;
    scalarUi.setupUi(content);
    sb::fixLayoutOverflow(content);

    auto* s = S();

    // Wire up static controls from .ui file
    auto* showCb = scalarUi.showCb;
    m_showCb = showCb;
    m_optionsGroup = scalarUi.optionsGroup;
    showCb->setChecked(s->getMeshUseScalarColor());
    connect(showCb, &QCheckBox::toggled, s, &RenderSettings::setMeshUseScalarColor);
    scalarUi.optionsGroup->setEnabled(s->hasMeshScalars());

    m_scalarCombo = scalarUi.scalarCombo;
    m_scalarCombo->addItems(s->getAvailableScalars());
    m_scalarCombo->setCurrentText(s->getActiveScalarNameQml());
    m_scalarCombo->setEnabled(s->hasMeshScalars());
    m_scalarCombo->setMinimumWidth(sb::kSidebarWidth - sb::kNavWidthApprox - 20);
    connect(m_scalarCombo, &QComboBox::activated, s, [this, s](int idx) {
        s->setActiveScalarField(m_scalarCombo->itemText(idx));
    });

    auto* scalarPlacementCombo = scalarUi.placementCombo;
    m_placementCombo = scalarPlacementCombo;
    scalarPlacementCombo->addItems(s->getScalarPlacementOptions());
    scalarPlacementCombo->setCurrentIndex(s->getScalarPlacement());
    scalarPlacementCombo->setEnabled(s->hasScalarCellData());
    connect(scalarPlacementCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            s, &RenderSettings::setScalarPlacement);

    // All colorbar properties (palette, bands, ticks, fixed range, visibility)
    // live in the Colorbar Style dialog — nothing colorbar-related stays here.

    {
        auto* filterEnabledCb = scalarUi.filterEnabledCb;
        m_filterEnabledCb = filterEnabledCb;
        filterEnabledCb->setChecked(s->getFilterEnabled());
        connect(filterEnabledCb, &QCheckBox::toggled, s, &RenderSettings::setFilterEnabled);
        connect(s, &RenderSettings::viewChanged, this, [this](ChangeFlags) {
            if (m_filterEnabledCb) {
                bool en = S()->getFilterEnabled();
                m_filterEnabledCb->blockSignals(true);
                m_filterEnabledCb->setChecked(en);
                m_filterEnabledCb->blockSignals(false);
                m_filterMinSlider->setEnabled(en);
                m_filterMaxSlider->setEnabled(en);
                m_filterMinField->setEnabled(en);
                m_filterMaxField->setEnabled(en);
            }
        });
        // Initial enabled state
        bool enInit = s->getFilterEnabled();
        scalarUi.minFilterSlider->setEnabled(enInit);
        scalarUi.maxFilterSlider->setEnabled(enInit);
        scalarUi.minFilterField->setEnabled(enInit);
        scalarUi.maxFilterField->setEnabled(enInit);
    }

    {
        auto* slider = scalarUi.minFilterSlider;
        auto* field = scalarUi.minFilterField;
        m_filterMinSlider = slider;
        m_filterMinField = field;
        connect(slider, &QSlider::valueChanged, this, [field, s](int raw) {
            double v = raw / 1000.0;
            field->setText(QString::number(v, 'f', 3));
            s->setFilterMin(v);
        });
        connect(field, &QLineEdit::editingFinished, this, [field, slider, s]() {
            bool ok = false;
            double v = field->text().toDouble(&ok);
            if (!ok) v = s->getFilterMin();
            v = qBound(s->getDataScalarMinQml(), v, s->getDataScalarMaxQml());
            v = std::min(v, static_cast<double>(s->getFilterMax()));
            slider->setValue(static_cast<int>(std::lround(v * 1000.0)));
            s->setFilterMin(v);
        });
    }
    {
        auto* slider = scalarUi.maxFilterSlider;
        auto* field = scalarUi.maxFilterField;
        m_filterMaxSlider = slider;
        m_filterMaxField = field;
        connect(slider, &QSlider::valueChanged, this, [field, s](int raw) {
            double v = raw / 1000.0;
            field->setText(QString::number(v, 'f', 3));
            s->setFilterMax(v);
        });
        connect(field, &QLineEdit::editingFinished, this, [field, slider, s]() {
            bool ok = false;
            double v = field->text().toDouble(&ok);
            if (!ok) v = s->getFilterMax();
            v = qBound(s->getDataScalarMinQml(), v, s->getDataScalarMaxQml());
            v = std::max(v, static_cast<double>(s->getFilterMin()));
            slider->setValue(static_cast<int>(std::lround(v * 1000.0)));
            s->setFilterMax(v);
        });
    }
    connect(s, &RenderSettings::meshLoadStateChanged, this, &ScalarPage::refreshFilterRange);

    auto* resetFilterBtn = scalarUi.resetFilterBtn;
    connect(resetFilterBtn, &QPushButton::clicked, s, [s, this]() {
        s->setFilterMin(s->getDataScalarMinQml());
        s->setFilterMax(s->getDataScalarMaxQml());
        refreshFilterRange();
    });

    // Field combo population + show-callback gating on mesh (re)load — was a
    // branch of the big meshLoadStateChanged / meshDataUpdated lambdas in
    // MainWindow::connectSettings.
    connect(s, &RenderSettings::meshDataUpdated, this, &ScalarPage::refreshPageFields);
    connect(s, &RenderSettings::meshLoadStateChanged, this, &ScalarPage::refreshPageFields);
    connect(s, &RenderSettings::viewChanged, this, [this](ChangeFlags) { refreshPageFields(); });

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

    setContent(content);
}

void ScalarPage::refreshPageFields() {
    auto* s = S();
    if (m_scalarCombo) {
        m_scalarCombo->blockSignals(true);
        m_scalarCombo->clear();
        m_scalarCombo->addItems(s->getAvailableScalars());
        m_scalarCombo->setCurrentText(s->getActiveScalarNameQml());
        m_scalarCombo->setEnabled(s->hasMeshScalars());
        m_scalarCombo->blockSignals(false);
    }
    if (m_placementCombo) {
        m_placementCombo->blockSignals(true);
        m_placementCombo->setCurrentIndex(s->getScalarPlacement());
        m_placementCombo->setEnabled(s->hasScalarCellData());
        m_placementCombo->blockSignals(false);
    }
    if (m_showCb) {
        bool ok = s->hasMeshScalars();
        m_showCb->setEnabled(ok);
        if (ok) {
            m_showCb->setChecked(s->getMeshUseScalarColor());
        } else {
            m_showCb->setChecked(false);
        }
        if (m_optionsGroup) m_optionsGroup->setEnabled(ok);
    }
}

void ScalarPage::refreshFilterRange() {
    auto* s = S();
    const double minVal = s->getDataScalarMinQml();
    const double maxVal = s->getDataScalarMaxQml();
    auto applyOne = [minVal, maxVal](QSlider* slider, QLineEdit* field, double filterVal) {
        if (!slider || !field) return;
        slider->setRange(static_cast<int>(std::lround(minVal * 1000.0)),
                         static_cast<int>(std::lround(maxVal * 1000.0)));
        int vi = static_cast<int>(std::lround(filterVal * 1000.0));
        slider->blockSignals(true);
        slider->setValue(vi);
        slider->blockSignals(false);
        field->setText(QString::number(filterVal, 'f', 3));
    };
    applyOne(m_filterMinSlider, m_filterMinField, s->getFilterMin());
    applyOne(m_filterMaxSlider, m_filterMaxField, s->getFilterMax());
}

} // namespace sidebar
