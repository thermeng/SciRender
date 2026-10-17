#pragma once

#include <QWidget>

class RenderSettings;
class QScrollArea;

namespace sidebar {

// Common scaffolding for the sidebar section pages: each page is a QWidget
// holding a single no-frame, widget-resizable QScrollArea. Subclasses build
// their content widget and hand it to setContent(), which applies the shared
// panel styling as well. Pages self-wire to RenderSettings signals in their
// constructors; MainWindow only constructs them and fans out mesh updates.
class SidebarPage : public QWidget {
    Q_OBJECT
public:
    explicit SidebarPage(RenderSettings* settings, QWidget* parent = nullptr);
    ~SidebarPage() override;

protected:
    RenderSettings* S() const { return s; }
    // Put `content` into the scroll area; when styled, run applyPanelStyling
    // on it (headers/dividers/parameter labels).
    void setContent(QWidget* content, bool styled = true);

private:
    RenderSettings* s = nullptr;
    QScrollArea* scroll = nullptr;
};

} // namespace sidebar
