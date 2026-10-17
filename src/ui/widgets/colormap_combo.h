#pragma once
#include <QComboBox>
#include <QStyledItemDelegate>
#include <functional>

class PalettePreviewDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit PalettePreviewDelegate(QObject* parent = nullptr);
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
};

// Reusable colormap combo with swatch preview.
// currentChoice = ColormapType index. onChoose called on index change.
QComboBox* createColormapCombo(int currentChoice,
                               std::function<void(int)> onChoose,
                               QWidget* parent = nullptr);
