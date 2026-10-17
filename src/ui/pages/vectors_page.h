#pragma once

#include "sidebar_page.h"

class QCheckBox;
class QColorDialog;
class QComboBox;
class QColor;

namespace sidebar {

// Vectors section page (glyphs / surface LIC). Handles its own gating on
// meshLoadStateChanged, field combo population on meshDataUpdated, and
// vis-mode group sync on viewChanged.
class VectorsPage : public SidebarPage {
    Q_OBJECT
public:
    explicit VectorsPage(RenderSettings* settings, QWidget* parent = nullptr);

private:
    void refreshPageFields();

    QComboBox* m_vectorCombo = nullptr;
    QComboBox* m_vectorPlacementCombo = nullptr;
    QComboBox* m_vectorVisModeCombo = nullptr;
    QWidget* m_vectorFieldGroup = nullptr;
    QWidget* m_vectorGlyphGroup = nullptr;
    QWidget* m_vectorLicGroup = nullptr;
    QCheckBox* m_vecShowCb = nullptr; // legacy, hidden; mirrored from settings

    QColorDialog* m_vectorColorDialog = nullptr;
};

} // namespace sidebar
