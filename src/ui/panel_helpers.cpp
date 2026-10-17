#include "panel_helpers.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <algorithm>

#include "render/settings/render_settings.h"

namespace sb {

SliderRow createLightSlider(const QString& label, double value, double from, double to,
                            double step, int decimals, std::function<void(double)> cb,
                            const QString& labelObjName) {
    SliderRow row;
    auto* widget = new QWidget;
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* lbl = new QLabel(label);
    if (!labelObjName.isEmpty()) lbl->setObjectName(labelObjName);
    lbl->setFixedWidth(kLabelWidth);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    lbl->setWordWrap(false);
    layout->addWidget(lbl);

    row.slider = new QSlider(Qt::Horizontal);
    row.slider->setMinimum(static_cast<int>(from * 1000));
    row.slider->setMaximum(static_cast<int>(to * 1000));
    row.slider->setSingleStep(static_cast<int>(step * 1000));
    row.slider->setValue(static_cast<int>(value * 1000));
    layout->addWidget(row.slider, 1, Qt::AlignVCenter);

    row.valueLabel = new QLabel(QString::number(value, 'f', decimals));
    row.valueLabel->setFixedWidth(36);
    row.valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(row.valueLabel, 0, Qt::AlignVCenter);

    row.callback = cb;
    auto updateLabel = [row, decimals](int raw) {
        double v = raw / 1000.0;
        row.valueLabel->setText(QString::number(v, 'f', decimals));
    };
    QObject::connect(row.slider, &QSlider::valueChanged, updateLabel);
    if (cb) {
        QObject::connect(row.slider, &QSlider::valueChanged, [cb](int raw) { cb(raw / 1000.0); });
    }
    return row;
}

void fixLayoutOverflow(QWidget* root) {
    if (!root) return;
    for (QLineEdit* le : root->findChildren<QLineEdit*>()) {
        le->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        le->setMaximumWidth(QWIDGETSIZE_MAX);
        le->setMinimumWidth(32);
    }
    for (QSpinBox* sbx : root->findChildren<QSpinBox*>()) {
        sbx->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        sbx->setMaximumWidth(QWIDGETSIZE_MAX);
        sbx->setMinimumWidth(40);
    }
    for (QDoubleSpinBox* dsb : root->findChildren<QDoubleSpinBox*>()) {
        dsb->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        dsb->setMaximumWidth(QWIDGETSIZE_MAX);
        dsb->setMinimumWidth(40);
    }

    // Custom-painted panels (e.g. TransferFunctionWidget) hard-code a wide
    // minimum so their drawing area stays usable. Inside a ~220px panel that
    // floor overflows, and because every page disables the horizontal
    // scrollbar the widget clips instead of shrinking. Relax those minimums to
    // something a narrow panel can still honour.
    //
    // Deliberately conservative: only leaf widgets whose intrinsic width is not
    // load-bearing are touched. Views, tabs, sliders, scroll areas and any
    // layout-owning container are skipped — squeezing a table or tab strip
    // would be a worse bug than the one being fixed.
    constexpr int kMinCustomWidth = 120;
    const auto children = root->findChildren<QWidget*>();
    for (QWidget* w : children) {
        if (qobject_cast<QAbstractItemView*>(w) || qobject_cast<QTabWidget*>(w) ||
            qobject_cast<QAbstractSlider*>(w) || qobject_cast<QGroupBox*>(w) ||
            qobject_cast<QFrame*>(w) || qobject_cast<QLineEdit*>(w) ||
            qobject_cast<QSpinBox*>(w) || qobject_cast<QDoubleSpinBox*>(w) ||
            qobject_cast<QComboBox*>(w) || qobject_cast<QAbstractButton*>(w) ||
            qobject_cast<QLabel*>(w) || qobject_cast<QScrollArea*>(w) ||
            w->layout())
            continue;
        // Skip anything that parents other widgets: that is a container whose
        // width is driven by its contents, not a custom-painted leaf.
        if (w->children().size() > 0) continue;
        w->setMaximumWidth(QWIDGETSIZE_MAX);
        if (w->minimumWidth() > kMinCustomWidth)
            w->setMinimumWidth(kMinCustomWidth);
    }
}

void applyPanelStyling(QWidget* root) {
    if (!root) return;

    // Establish a clear size distinction
    const int kHeaderFontSize = 11; // 11px Uppercase + Bold works well for headers
    const int kParamFontSize = 10;  // Slightly smaller muted text for parameters

    // Collect all child labels once
    const auto allLabels = root->findChildren<QLabel*>();

    // ── Phase 1: Style Parameter Labels ──────────────────────────────────────
    for (QLabel* lbl : allLabels) {
        const QString name = lbl->objectName();
        if (name.endsWith("Label") && !name.endsWith("Header")) {
            QFont f = lbl->font();
            f.setPixelSize(kParamFontSize);
            f.setBold(false);
            lbl->setFont(f);
            lbl->setStyleSheet("background: transparent;");
        }
    }

    // ── Phase 2: Identify Section Headers ────────────────────────────────────
    struct HeaderEntry {
        QVBoxLayout* layout = nullptr; // Strictly QVBoxLayout to prevent row divider bugs
        int index = -1;
        QLabel* label = nullptr;
    };
    QList<HeaderEntry> headers;

    for (QLabel* lbl : allLabels) {
        if (!lbl->objectName().endsWith("Header")) continue;

        // Find the actual layout containing this widget item (handles nested layouts)
        QLayoutItem* item = nullptr;
        QLayout* containingLayout = nullptr;

        // Search parent hierarchy if needed, or query parent widget's layout
        if (QWidget* parent = lbl->parentWidget()) {
            if (QLayout* lay = parent->layout()) {
                // Check if directly in parent layout or in one of its sub-layouts
                for (QObject* child : parent->children()) {
                    if (auto* box = qobject_cast<QVBoxLayout*>(child)) {
                        int idx = box->indexOf(lbl);
                        if (idx != -1) {
                            containingLayout = box;
                            break;
                        }
                    }
                }
                // Fallback to direct parent layout check
                if (!containingLayout) {
                    containingLayout = lay;
                }
            }
        }

        // Strictly check for Vertical Layouts so horizontal headers don't get dividers inserted
        auto* vBox = qobject_cast<QVBoxLayout*>(containingLayout);
        if (!vBox) continue;

        int idx = vBox->indexOf(lbl);
        if (idx != -1) {
            headers.append({vBox, idx, lbl});
        }
    }

    // ── Phase 3: Insert Dividers (Bottom-to-Top) ─────────────────────────────
    QMap<QVBoxLayout*, QList<int>> indicesByLayout;
    for (const auto& e : headers) {
        // Prevent duplicate dividers if a divider line already precedes this header
        bool hasDivider = false;
        if (e.index > 0) {
            if (QLayoutItem* prevItem = e.layout->itemAt(e.index - 1)) {
                if (QWidget* w = prevItem->widget()) {
                    if (w->property("isHeaderDivider").toBool()) {
                        hasDivider = true;
                    }
                }
            }
        }

        if (!hasDivider) {
            indicesByLayout[e.layout].append(e.index);
        }
    }

    // Insert separators descending by index
    for (auto it = indicesByLayout.begin(); it != indicesByLayout.end(); ++it) {
        QList<int> indices = it.value();
        std::sort(indices.begin(), indices.end(), std::greater<int>());

        for (int idx : indices) {
            auto* line = new QFrame;
            line->setProperty("isHeaderDivider", true); // Flag to prevent duplicate insertion on re-runs
            line->setFrameShape(QFrame::NoFrame);
            line->setFixedHeight(1);
            line->setStyleSheet("background-color: palette(mid); margin-top: 8px; margin-bottom: 4px;");
            it.key()->insertWidget(idx, line);
        }
    }

    // ── Phase 4: Apply Subtle Header Typography ───────────────────────────────
    for (const auto& e : headers) {
        QFont f = e.label->font();
        f.setWeight(QFont::Medium);  // or f.setBold(true) with 10px
        f.setPixelSize(11);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 0.3);
        e.label->setFont(f);
        e.label->setStyleSheet("background: transparent; padding-top: 6px;");
    }
}

QPushButton* createSwatchButton(const QString& text, const QColor& color,
                                std::function<void()> onClicked) {
    auto* btn = new QPushButton(text);
    btn->setFixedHeight(kControlHeight);
    btn->setMinimumWidth(kSidebarWidth - kNavWidthApprox - 36);
    QPixmap pix(14, 14);
    pix.fill(color);
    btn->setIcon(pix);
    btn->setIconSize(QSize(14, 14));
    if (onClicked)
        QObject::connect(btn, &QPushButton::clicked, onClicked);
    return btn;
}

void addCtlRow(QVBoxLayout* lay, const QString& labelObjName,
               const QString& text, QWidget* ctl, int ctlStretch) {
    auto* row = new QWidget;
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);
    auto* lbl = new QLabel(text);
    lbl->setObjectName(labelObjName);
    lbl->setFixedWidth(kLabelWidth);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    rowLayout->addWidget(lbl);
    rowLayout->addWidget(ctl, ctlStretch);
    lay->addWidget(row);
}

void addSliderRow(RenderSettings* settings, QVBoxLayout* lay, const QString& labelObjName,
                  const QString& text, double value, double from,
                  double to, int decimals,
                  void (RenderSettings::*setter)(double)) {
    auto row = createLightSlider(text, value, from, to, 0.001, decimals,
                                 [settings, setter](double v) { (settings->*setter)(v); },
                                 labelObjName);
    lay->addWidget(row.slider->parentWidget());
}

} // namespace sb
