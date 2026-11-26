#include "ui/widgets/colormap_combo.h"
#include "core/Colormaps.h"

#include <QApplication>
#include <QPainter>
#include <QStyle>

static constexpr int kControlHeight = 24;

PalettePreviewDelegate::PalettePreviewDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {}

void PalettePreviewDelegate::paint(QPainter* painter,
                                   const QStyleOptionViewItem& option,
                                   const QModelIndex& index) const {
    painter->save();
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);

    const QVariant roleData = index.data(Qt::UserRole);
    const int colormapIndex = roleData.isValid() ? roleData.toInt() : index.row();
    const ColormapType type = static_cast<ColormapType>(colormapIndex);
    int previewWidth = 56;
    int previewHeight = 14;
    int margin = 4;
    QRect previewRect(opt.rect.left() + margin,
                      opt.rect.top() + (opt.rect.height() - previewHeight) / 2,
                      previewWidth, previewHeight);
    QImage img(previewWidth, previewHeight, QImage::Format_RGB888);
    for (int x = 0; x < previewWidth; ++x) {
        float t = static_cast<float>(x) / static_cast<float>(previewWidth - 1);
        glm::vec3 c = Colormaps::evaluate(t, type);
        int r = static_cast<int>(glm::clamp(c.r, 0.0f, 1.0f) * 255.0f);
        int g = static_cast<int>(glm::clamp(c.g, 0.0f, 1.0f) * 255.0f);
        int b = static_cast<int>(glm::clamp(c.b, 0.0f, 1.0f) * 255.0f);
        for (int y = 0; y < previewHeight; ++y) img.setPixel(x, y, qRgb(r, g, b));
    }
    painter->drawImage(previewRect, img);
    QString name = QString::fromUtf8(Colormaps::getName(type));
    if (!Colormaps::isCurated(type)) name += QStringLiteral(" (legacy)");
    painter->setPen(opt.palette.color(QPalette::Text));
    painter->drawText(opt.rect.adjusted(previewRect.right() + 6, 0, -margin, 0),
                      Qt::AlignVCenter | Qt::AlignLeft, name);
    painter->restore();
}

QSize PalettePreviewDelegate::sizeHint(const QStyleOptionViewItem&,
                                       const QModelIndex&) const {
    return QSize(180, kControlHeight);
}

QComboBox* createColormapCombo(int currentChoice,
                               std::function<void(int)> onChoose,
                               QWidget* parent) {
    auto* combo = new QComboBox(parent);
    combo->setItemDelegate(new PalettePreviewDelegate(combo));
    combo->setFixedHeight(kControlHeight);
    combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    const int choice = currentChoice;
    for (ColormapType type : Colormaps::curatedTypes()) {
        const int value = static_cast<int>(type);
        combo->addItem(QString::fromUtf8(Colormaps::getName(type)), value);
    }

    int currentIdx = combo->findData(choice);
    if (currentIdx >= 0) combo->setCurrentIndex(currentIdx);

    QObject::connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     [combo, onChoose](int idx) {
                         if (idx < 0) return;
                         const QVariant data = combo->itemData(idx);
                         if (data.isValid()) onChoose(data.toInt());
                     });
    return combo;
}
