#include "ui/uikit.h"

#include "ui/widgets/wheel_guard.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>

namespace uikit {

QFont mono(int pixelSize) {
    QFont f("JetBrains Mono");
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(pixelSize);
    return f;
}

QLabel* header(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    QFont f = label->font();
    f.setPixelSize(11);
    f.setWeight(QFont::Medium);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.3);
    label->setFont(f);
    return label;
}

QLabel* note(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet("color: palette(mid);");
    QFont f = label->font();
    f.setPixelSize(10);
    label->setFont(f);
    return label;
}

QFrame* hline(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::NoFrame);
    line->setFixedHeight(1);
    line->setStyleSheet("background-color: palette(mid); border: none;");
    return line;
}

QFrame* vline(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::NoFrame);
    line->setFixedWidth(1);
    line->setStyleSheet("background-color: palette(mid); border: none;");
    return line;
}

QFormLayout* form(QWidget* host) {
    auto* f = new QFormLayout(host);
    f->setContentsMargins(kGroupMargin, 6, kGroupMargin, kGroupMargin);
    f->setSpacing(kGroupSpacing);
    f->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    f->setRowWrapPolicy(QFormLayout::DontWrapRows);
    f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    f->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    return f;
}

QGroupBox* group(const QString& title, QFormLayout** out) {
    auto* box = new QGroupBox(title);
    if (out) *out = form(box);
    else form(box);
    return box;
}

QDialogButtonBox* buttons(QDialog* dlg, bool resetDefaults,
                          std::function<bool()> beforeAccept) {
    QDialogButtonBox::StandardButtons kinds =
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel;
    if (resetDefaults) kinds |= QDialogButtonBox::RestoreDefaults;
    auto* box = new QDialogButtonBox(kinds, dlg);
    if (resetDefaults)
        box->button(QDialogButtonBox::RestoreDefaults)->setText("Reset Defaults");
    QObject::connect(box, &QDialogButtonBox::accepted, dlg, [dlg, beforeAccept]() {
        if (beforeAccept && !beforeAccept()) return;
        dlg->accept();
    });
    QObject::connect(box, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    return box;
}

void prepareDialog(QDialog* dlg, int minW, int minH) {
    dlg->setModal(true);
    if (auto* lay = dlg->layout()) {
        lay->setContentsMargins(kDialogMargin, kDialogMargin, kDialogMargin,
                                kDialogMargin);
        lay->setSpacing(kDialogSpacing);
    }
    if (minW > 0) dlg->setMinimumWidth(minW);
    if (minH > 0) dlg->setMinimumHeight(minH);
    installWheelGuard(dlg);
}

} // namespace uikit