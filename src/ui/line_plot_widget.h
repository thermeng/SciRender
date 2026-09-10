#pragma once
#include <QWidget>
#include <vector>

class LinePlotWidget : public QWidget {
    Q_OBJECT
public:
    explicit LinePlotWidget(QWidget* parent=nullptr);
    void setData(const std::vector<float>& dists, const std::vector<float>& values,
                 float vMin, float vMax, const QString& field, int placement);
    void clear();
protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
private:
    std::vector<float> m_dists, m_vals;
    float m_vMin=0,m_vMax=1;
    QString m_field;
    int m_placement=0;
    int m_hover=-1;
    QRect m_rect;
    int m_left=56,m_right=16,m_top=20,m_bottom=30;
};
