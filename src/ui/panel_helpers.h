#pragma once

// Shared helpers for sidebar panel pages. These used to live as static
// functions at the top of main_window.cpp; every session page uses them.

#include <functional>
#include <QColor>
#include <QColorDialog>
#include <QPushButton>

class QLabel;
class QVBoxLayout;
class QWidget;
class QSlider;
class QPushButton;
class QColorDialog;
class RenderSettings;

namespace sb {

// Layout constants shared by every sidebar page.
inline constexpr int kSidebarWidth = 240;
inline constexpr int kNavWidthApprox = 140;
inline constexpr int kLabelWidth = 72;
inline constexpr int kControlHeight = 24;

// Labeled slider row (LightSlider equivalent): fixed-width label on the left,
// H slider in the middle, right-aligned numeric value label on the right.
struct SliderRow {
    QSlider* slider = nullptr;
    QLabel* valueLabel = nullptr;
    std::function<void(double)> callback;
};
SliderRow createLightSlider(const QString& label, double value, double from, double to,
                            double step, int decimals, std::function<void(double)> cb,
                            const QString& labelObjName = {});

// Fix layout overflow by making labels and value widgets flexible. .ui files
// have fixed 72px labels + 36-52px value widgets that overflow when the
// sidebar width < 220px.
void fixLayoutOverflow(QWidget* root);

// Apply panel styling (section headers, parameter labels, dividers).
void applyPanelStyling(QWidget* root);

// Swatch button (color swatch + label).
QPushButton* createSwatchButton(const QString& text, const QColor& color,
                                std::function<void()> onClicked);

// Row helpers for code-built pages.
void addCtlRow(QVBoxLayout* lay, const QString& labelObjName, const QString& text,
               QWidget* ctl, int ctlStretch = 0);
void addSliderRow(RenderSettings* settings, QVBoxLayout* lay, const QString& labelObjName,
                  const QString& text, double value, double from, double to, int decimals,
                  void (RenderSettings::*setter)(double));

namespace detail {

// Swatch button backed by a lazily-created QColorDialog. Shared by all pages;
// templated on the RenderSettings setter pointer so it can live in a header.
template <typename Setter>
QPushButton* createColorButton(QWidget* owner, RenderSettings* settings,
                               const QString& name, QColor initialColor,
                               QColorDialog** dialogPtr, Setter setterMember) {
    auto* btn = createSwatchButton(name, initialColor, nullptr);
    QPushButton::connect(btn, &QPushButton::clicked, btn,
        [owner, btn, name, initialColor, dialogPtr, settings, setterMember]() {
            if (!*dialogPtr) {
                *dialogPtr = new QColorDialog(initialColor, owner);
                (*dialogPtr)->setOption(QColorDialog::ShowAlphaChannel, false);
                QObject::connect(*dialogPtr, &QColorDialog::colorSelected, settings, setterMember);
                QObject::connect(*dialogPtr, &QColorDialog::colorSelected, btn, [btn](const QColor& c) {
                    QPixmap pix(14, 14); pix.fill(c); btn->setIcon(pix);
                });
            }
            (*dialogPtr)->open();
        });
    return btn;
}

} // namespace detail

} // namespace sb
