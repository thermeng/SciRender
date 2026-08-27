#pragma once

#include <QWidget>

class QDoubleSpinBox;
class QPushButton;

class RangeEditor : public QWidget {
    Q_OBJECT
public:
    explicit RangeEditor(QWidget* parent = nullptr);

    void setBounds(double lo, double hi);
    void setWindow(double lo, double hi);
    std::pair<double, double> window() const;
    void setEnabled(bool enabled);

signals:
    void windowEdited(double lo, double hi);

private slots:
    void syncFromSpinboxes();

private:
    QDoubleSpinBox* m_minSpin = nullptr;
    QDoubleSpinBox* m_maxSpin = nullptr;
    QPushButton* m_resetBtn = nullptr;
    double m_boundLo = 0.0;
    double m_boundHi = 1.0;
};
