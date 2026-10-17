#include "sidebar_page.h"

#include <QScrollArea>
#include <QVBoxLayout>

#include "ui/panel_helpers.h"

namespace sidebar {

SidebarPage::SidebarPage(RenderSettings* settings, QWidget* parent)
    : QWidget(parent), s(settings) {
    scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(scroll);
}

SidebarPage::~SidebarPage() = default;

void SidebarPage::setContent(QWidget* content, bool styled) {
    if (styled) sb::applyPanelStyling(content);
    scroll->setWidget(content);
}

} // namespace sidebar
