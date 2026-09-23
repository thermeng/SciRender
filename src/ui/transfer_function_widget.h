#pragma once
#include <QWidget>
#include <vector>
#include "core/transfer_function.h"

class TransferFunctionWidget : public QWidget {
    Q_OBJECT
public:
    explicit TransferFunctionWidget(QWidget* parent=nullptr);
    void setCurve(const tf::Curve& c);
    tf::Curve curve() const { return m_curve; }
    void setFieldRange(float lo, float hi) { m_lo=lo; m_hi=hi; update(); }
signals:
    void curveChanged(const tf::Curve& c);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
private:
    tf::Curve m_curve = tf::defaultCurve();
    int m_dragIdx = -1;
    bool m_dragging=false;
    float m_lo=0,m_hi=1;
    int hitTest(const QPoint& p) const;
    QPointF toWidget(const tf::Point& pt) const;
    tf::Point fromWidget(const QPoint& p) const;
    QRect graphRect() const;
};
