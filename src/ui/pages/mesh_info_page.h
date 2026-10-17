#pragma once

#include "sidebar_page.h"

#include <QHash>

class QLabel;
class QString;

namespace sidebar {

// Mesh statistics page (type/format/quality/bounding-box readouts). Self-wired:
// refreshes on meshLoadStateChanged and meshDataUpdated.
class MeshInfoPage : public SidebarPage {
    Q_OBJECT
public:
    explicit MeshInfoPage(RenderSettings* settings, QWidget* parent = nullptr);

private:
    void refreshPage();

    QHash<QString, QLabel*> m_labels;
};

} // namespace sidebar
