#include "mesh_info_page.h"

#include <QLabel>
#include <QLayout>

#include "ui_mesh_info_page.h"
#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"

namespace sidebar {

MeshInfoPage::MeshInfoPage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    auto* content = new QWidget;
    Ui::MeshInfoPage meshUi;
    meshUi.setupUi(content);
    sb::fixLayoutOverflow(content);
    m_labels.clear();

    auto* s = S();

    auto addInfoRow = [this](const QString& label, QLabel* valueWidget) {
        m_labels[label] = valueWidget;
    };

    addInfoRow("Type", meshUi.typeValue);
    addInfoRow("Format", meshUi.formatValue);

    addInfoRow("Triangles", meshUi.trianglesValue);
    addInfoRow("Points", meshUi.pointsValue);

    addInfoRow("Degenerate", meshUi.degenerateValue);
    addInfoRow("Open Edges", meshUi.openEdgesValue);
    addInfoRow("Non-manifold Edge", meshUi.nonManifoldEValue);
    addInfoRow("Non-manifold Vertex", meshUi.nonManifoldVValue);
    addInfoRow("Watertight", meshUi.watertightValue);

    auto addBB = [&](const QString& row, int axis, QLabel* widget) {
        if (widget) m_labels[QString("BB:%1:%2").arg(row).arg(axis)] = widget;
    };

    addBB("Min", 0, meshUi.bbMinX);
    addBB("Min", 1, meshUi.bbMinY);
    addBB("Min", 2, meshUi.bbMinZ);
    addBB("Max", 0, meshUi.bbMaxX);
    addBB("Max", 1, meshUi.bbMaxY);
    addBB("Max", 2, meshUi.bbMaxZ);
    addBB("Delta", 0, meshUi.bbDeltaX);
    addBB("Delta", 1, meshUi.bbDeltaY);
    addBB("Delta", 2, meshUi.bbDeltaZ);

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

    refreshPage();
    connect(s, &RenderSettings::meshLoadStateChanged, this, &MeshInfoPage::refreshPage);
    connect(s, &RenderSettings::meshDataUpdated, this, &MeshInfoPage::refreshPage);

    setContent(content);
}

void MeshInfoPage::refreshPage() {
    auto* s = S();

    auto setInfo = [&](const QString& label, const QString& value) {
        auto it = m_labels.find(label);
        if (it == m_labels.end()) return;
        it.value()->setText(value);
    };
    setInfo("Type",       s->getMeshDataType());
    setInfo("Format",     s->getMeshFormat());
    setInfo("Triangles",  QString::number(s->getTriangleCount()));
    setInfo("Points",     QString::number(s->getPointCount()));
    setInfo("Degenerate", QString::number(s->getDegenerateFaces()));
    setInfo("Open Edges", QString::number(s->getOpenEdges()));
    setInfo("Non-manifold Edge", QString::number(s->getNonManifoldEdges()));
    setInfo("Non-manifold Vertex", QString::number(s->getNonManifoldVerts()));
    setInfo("Watertight", s->getWatertight() ? "Yes" : "No");

    // Quality overlay colors — match QualityOverlayRenderer defect colors
    auto colorQuality = [&](const QString& label, int count, const char* defectColor) {
        auto it = m_labels.find(label);
        if (it == m_labels.end()) return;
        it.value()->setStyleSheet(count > 0
            ? QString("color: %1; font-weight: bold;").arg(defectColor)
            : QString());
    };
    colorQuality("Degenerate",         s->getDegenerateFaces(),   "#FF6666");
    colorQuality("Open Edges",         s->getOpenEdges(),         "#FFAA44");
    colorQuality("Non-manifold Edge",  s->getNonManifoldEdges(),  "#FF44FF");
    colorQuality("Non-manifold Vertex", s->getNonManifoldVerts(), "#FF44FF");
    auto wtIt = m_labels.find("Watertight");
    if (wtIt != m_labels.end())
        wtIt.value()->setStyleSheet(s->getWatertight() ? "color: #4CAF50;" : "color: #FF6666;");

    auto setBB = [&](const QString& row, int axis, double v) {
        auto it = m_labels.find(QString("BB:%1:%2").arg(row).arg(axis));
        if (it != m_labels.end()) it.value()->setText(QString::number(v, 'f', 3));
    };
    const double min[3] = { s->getWorldMinX(), s->getWorldMinY(), s->getWorldMinZ() };
    const double max[3] = { s->getWorldMaxX(), s->getWorldMaxY(), s->getWorldMaxZ() };
    for (int i = 0; i < 3; ++i) {
        setBB("Min",   i, min[i]);
        setBB("Max",   i, max[i]);
        setBB("Delta", i, max[i] - min[i]);
    }
}

} // namespace sidebar
