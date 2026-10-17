#include "screenshot_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "ui_screenshot_page.h"
#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"

namespace sidebar {

ScreenshotPage::ScreenshotPage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    auto* content = new QWidget;
    Ui::ScreenshotPage ssUi;
    ssUi.setupUi(content);
    sb::fixLayoutOverflow(content);

    auto* s = S();

    connect(ssUi.saveBtn, &QPushButton::clicked, this, [this]() { emit saveRequested(); });

    ssUi.transCb->setChecked(s->getScreenshotTransparent());
    connect(ssUi.transCb, &QCheckBox::toggled, s, &RenderSettings::setScreenshotTransparent);

    auto* resCombo = ssUi.resCombo;
    resCombo->setMinimumWidth(sb::kSidebarWidth - sb::kNavWidthApprox - 20);
    resCombo->setCurrentIndex(s->getScreenshotResolution());
    connect(resCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            s, &RenderSettings::setScreenshotResolution);

    auto* aaCombo = ssUi.aaCombo;
    aaCombo->setMinimumWidth(sb::kSidebarWidth - sb::kNavWidthApprox - 20);
    aaCombo->setCurrentIndex(s->getScreenshotAASamples() <= 0 ? 0 : s->getScreenshotAASamples() <= 2 ? 1 : 2);
    connect(aaCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            s, [s](int idx) {
                const int samples = idx == 0 ? 0 : (idx == 1 ? 2 : 4);
                s->setScreenshotAASamples(samples);
            });

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

    setContent(content);
}

} // namespace sidebar
