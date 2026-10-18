#pragma once

#include <QEvent>
#include <QObject>
#include <QWidget>

#include <QAbstractSpinBox>
#include <QComboBox>

// Spin boxes and combo boxes swallow wheel events, so on a scrollable page a
// user who is only trying to scroll silently changes whichever numeric field
// happens to be under the cursor. That is how the "minimum cells" threshold
// ended up showing "always" instead of its default.
//
// This filter makes such a control ignore the wheel unless it already has
// keyboard focus. Ignoring (rather than accepting) the event lets Qt's wheel
// propagation pass it to the enclosing QScrollArea, so the page still scrolls.
class WheelGuard : public QObject {
public:
    explicit WheelGuard(QObject* parent = nullptr) : QObject(parent) {}

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (ev->type() == QEvent::Wheel) {
            auto* widget = qobject_cast<QWidget*>(obj);
            if (widget && !widget->hasFocus()) {
                ev->ignore();
                return true; // stop the control from acting on it
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};

// Install one guard over every wheel-sensitive control below `root`. Call once
// per dialog, after all children exist; `root` takes ownership.
inline void installWheelGuard(QWidget* root) {
    auto* guard = new WheelGuard(root);
    const auto spins = root->findChildren<QAbstractSpinBox*>();
    for (QAbstractSpinBox* spin : spins) spin->installEventFilter(guard);
    const auto combos = root->findChildren<QComboBox*>();
    for (QComboBox* combo : combos) combo->installEventFilter(guard);
}
