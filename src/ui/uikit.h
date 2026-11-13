#pragma once

#include <QFont>
#include <QString>
#include <QWidget>

#include <functional>

class QDialog;
class QDialogButtonBox;
class QFormLayout;
class QFrame;
class QGroupBox;
class QLabel;

// One place for the visual rules shared by every dialog and code-built sidebar
// page. Fusion supplies the chrome; these helpers only normalize the parts the
// project has historically re-created per dialog: spacing, label typography,
// the mono face for numeric values, and the Ok/Cancel (plus Reset Defaults)
// button row.
namespace uikit {

inline constexpr int kLabelWidth = 72;
inline constexpr int kSpinWidth = 110;
inline constexpr int kComboWidth = 150;

inline constexpr int kDialogMargin = 12;
inline constexpr int kDialogSpacing = 10;
inline constexpr int kGroupMargin = 8;
inline constexpr int kGroupSpacing = 4;

// The only non-default face in the app: fixed-pitch digits so residual and
// coefficient columns do not wander as values change width.
QFont mono(int pixelSize = 11);

// Section caption inside a group or dialog: 11 px, medium, slight tracking.
QLabel* header(const QString& text, QWidget* parent = nullptr);
// Muted line: 10 px, palette(mid), wrapped. Use for captions and hints, not for
// values the user must read as data.
QLabel* note(const QString& text, QWidget* parent = nullptr);
// 1 px rules. Palette-driven so they follow light/dark without a stylesheet.
QFrame* hline(QWidget* parent = nullptr);
QFrame* vline(QWidget* parent = nullptr);

// Fixed-width numeric field with the mono face. Works for any widget type.
template <typename W>
W* numeric(W* w, int width = kSpinWidth) {
    w->setFixedWidth(width);
    w->setFont(mono());
    return w;
}

// The one form preset: right-aligned labels, fixed fields, no row wrapping.
QFormLayout* form(QWidget* host);
// Group box plus its form. `out` receives the form (may be null).
QGroupBox* group(const QString& title, QFormLayout** out = nullptr);

// Ok/Cancel, with an optional third "Reset Defaults" button. Accepted/rejected
// are wired here so no dialog re-implements it. `beforeAccept` returns false to
// keep the dialog open (validation).
QDialogButtonBox* buttons(QDialog* dlg, bool resetDefaults = false,
                          std::function<bool()> beforeAccept = {});

// Margins, spacing, modality and the wheel guard. Call after the dialog's
// layout exists and all children are built. minW/minH <= 0 leave that
// dimension alone.
void prepareDialog(QDialog* dlg, int minW = 560, int minH = 0);

} // namespace uikit