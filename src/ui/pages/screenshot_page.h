#pragma once

#include "sidebar_page.h"

namespace sidebar {

// Screenshot settings page (transparent background, resolution, MSAA).
// Emits saveRequested so MainWindow (which owns the save dialog, shortcuts and
// screenshot plumbing) performs the capture.
class ScreenshotPage : public SidebarPage {
    Q_OBJECT
public:
    explicit ScreenshotPage(RenderSettings* settings, QWidget* parent = nullptr);

signals:
    void saveRequested();
};

} // namespace sidebar
