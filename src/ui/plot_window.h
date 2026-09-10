#pragma once
#include <QWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QTabWidget>
#include "plot_widget.h"
#include "line_plot_widget.h"

class RenderSettings;

class PlotWindow : public QWidget {
    Q_OBJECT
public:
    explicit PlotWindow(RenderSettings* settings, QWidget* parent = nullptr);
    ~PlotWindow() override = default;

    void refreshFieldList();
Q_SIGNALS:
    void requestCloseHistogram();

private slots:
    void onRecompute();
    void onExportCsv();
    void onExportPng();
    void onBinsChanged(int);
    void onSettingsMeshDataUpdated();
    void onSampleLine();
    void onLineExportCsv();
    void onLineExportPng();

private:
    void recomputeHistogram();
    void recomputeLine();
    void updateStatsLabel();
    void updatePlacementOptions();
    void syncLineProbeUIFromSettings();

    RenderSettings* m_settings = nullptr;
    PlotWidget* m_plot = nullptr;
    LinePlotWidget* m_linePlot = nullptr;
    QComboBox* m_fieldCombo = nullptr;
    QComboBox* m_placementCombo = nullptr;
    QSpinBox* m_binsSpin = nullptr;
    QPushButton* m_recomputeBtn = nullptr;
    QPushButton* m_csvBtn = nullptr;
    QPushButton* m_pngBtn = nullptr;
    QLabel* m_statsLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    // line tab
    QComboBox* m_lineFieldCombo = nullptr;
    QComboBox* m_linePlacementCombo = nullptr;
    QSpinBox* m_samplesSpin = nullptr;
    QDoubleSpinBox *m_p0x=nullptr,*m_p0y=nullptr,*m_p0z=nullptr,*m_p1x=nullptr,*m_p1y=nullptr,*m_p1z=nullptr;
    QCheckBox* m_showProbeCb=nullptr;
    QPushButton* m_lineSampleBtn=nullptr;
    QPushButton* m_lineCsvBtn=nullptr;
    QPushButton* m_linePngBtn=nullptr;
    QLabel* m_lineStats=nullptr;
    QLabel* m_lineStatus=nullptr;
    QTabWidget* m_tabs=nullptr;
    // cached line data for export
    std::vector<float> m_lineDists, m_lineVals;
    float m_lineMin=0,m_lineMax=1;
    QString m_lineField;
    int m_linePlacement=0;
};
