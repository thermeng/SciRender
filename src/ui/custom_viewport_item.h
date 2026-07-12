#pragma once

#include <QtQuick/QQuickItem>
#include <QPoint>
#include "renderer/renderer.h"

class CustomViewportItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(Renderer* renderer READ renderer WRITE setRenderer NOTIFY rendererChanged)

public:
    explicit CustomViewportItem(QQuickItem* parent = nullptr);
    ~CustomViewportItem() override = default;

    Renderer* renderer() const { return m_renderer; }
    void setRenderer(Renderer* r);

signals:
    void rendererChanged();

protected:
    // Capture user input directly inside the QML frame area
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

    // Catch viewport size alterations mid-session
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    Renderer* m_renderer = nullptr;
    QPoint m_lastMousePos;
    bool m_isRightClick = false;
};