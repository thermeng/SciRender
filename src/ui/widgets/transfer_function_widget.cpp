#include "transfer_function_widget.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QLinearGradient>
#include <cmath>

TransferFunctionWidget::TransferFunctionWidget(QWidget* parent): QWidget(parent) {
    setMinimumHeight(120);
    setMinimumWidth(200);
    setMouseTracking(true);
    // Three distinct gestures drive this widget and none of them were
    // discoverable: add is Ctrl+click OR double-click, remove is right-click,
    // and the two endpoints are pinned to the ends of the range in x. Without
    // this the only way to find out was to guess.
    setToolTip(tr("Drag a point to move it.\n"
                  "Double-click, or Ctrl+click, to add a point.\n"
                  "Right-click a point to remove it.\n"
                  "The first and last points stay pinned to the ends of the range."));
}

// Top margin is sized for the readout row (see paintEvent) and the bottom
    // margin for the two range labels, so neither can ever overlap the other or
    // the plot. Both are constants, so selecting a point does not resize the
    // graph and make the curve jump.
QRect TransferFunctionWidget::graphRect() const {
    return rect().adjusted(10, kReadoutRowHeight, -10, -20);
}

QPointF TransferFunctionWidget::toWidget(const tf::Point& pt) const {
    QRect r = graphRect();
    float x = r.left() + pt.pos * r.width();
    float y = r.bottom() - pt.alpha * r.height();
    return {x,y};
}
tf::Point TransferFunctionWidget::fromWidget(const QPoint& p) const {
    QRect r = graphRect();
    float nx = (r.width()>0) ? float(p.x()-r.left())/r.width() : 0;
    float ny = (r.height()>0) ? float(r.bottom()-p.y())/r.height() : 0;
    nx = std::clamp(nx,0.0f,1.0f); ny = std::clamp(ny,0.0f,1.0f);
    return {nx,ny};
}
int TransferFunctionWidget::hitTest(const QPoint& p) const {
    for (int i=0;i<(int)m_curve.size();++i) {
        QPointF w = toWidget(m_curve[i]);
        float dx = w.x()-p.x(), dy=w.y()-p.y();
        if (dx*dx+dy*dy < 64) return i;
    }
    return -1;
}
void TransferFunctionWidget::setCurve(const tf::Curve& c) {
    m_curve=c; tf::normalize(m_curve); update();
}
void TransferFunctionWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRect r = graphRect();
    // Palette-derived throughout: this used to hardcode dark greys, which left a
    // near-black rectangle inside the light-theme panel. Only the curve/line blue
    // and the amber drag highlight stay fixed — they are the curve's identity
    // colour and read correctly against both Base variants.
    const QColor base    = palette().color(QPalette::Base);
    const QColor mid     = palette().color(QPalette::Mid);
    const QColor textCol = palette().color(QPalette::Text);
    // background
    p.fillRect(rect(), base);
    p.fillRect(r, base.darker(112)); // slightly recessed in both themes
    p.setPen(mid);
    p.drawRect(r);
    // grid
    p.setPen(mid);
    for(int i=1;i<4;++i){ int x=r.left()+r.width()*i/4; p.drawLine(x,r.top(),x,r.bottom()); }
    for(int i=1;i<4;++i){ int y=r.top()+r.height()*i/4; p.drawLine(r.left(),y,r.right(),y); }
    // curve polygon fill
    if (m_curve.size()>=2) {
        QPainterPath path;
        QPointF first = toWidget(m_curve.front());
        path.moveTo(first.x(), r.bottom());
        path.lineTo(first);
        for(size_t i=1;i<m_curve.size();++i) path.lineTo(toWidget(m_curve[i]));
        QPointF last = toWidget(m_curve.back());
        path.lineTo(last.x(), r.bottom());
        path.lineTo(first.x(), r.bottom());
        p.fillPath(path, QColor(100,180,255,60));
    }
    // line
    p.setPen(QPen(QColor(100,180,255),2));
    for(size_t i=1;i<m_curve.size();++i){
        p.drawLine(toWidget(m_curve[i-1]), toWidget(m_curve[i]));
    }
    // points
    for(size_t i=0;i<m_curve.size();++i){
        QPointF w = toWidget(m_curve[i]);
        bool sel = (int)i==m_dragIdx;
        p.setBrush(sel? QColor(255,220,100) : textCol);
        p.setPen(palette().color(QPalette::Dark));
        p.drawEllipse(w, 5,5);
    }
    // Readout for the point being moved, in its own reserved row above the plot
    // rather than sharing the axis-label row below it — centred text of any
    // width collided with both range labels. Built by concatenation rather than
    // QString::arg: the arg(float, char, int) overload binds 3 to fieldWidth,
    // not precision, and the resulting string did not render usefully. Plain
    // "opacity" instead of an alpha glyph, which is absent from the small
    // fallback font on some systems and would drop out of the string.
    if (m_dragIdx>=0 && m_dragIdx<(int)m_curve.size()) {
        const tf::Point& pt = m_curve[m_dragIdx];
        // x is mapped through the field range, so it shows the scalar value the
        // curve is keyed on rather than the normalized position.
        const float dataVal = m_lo + pt.pos*(m_hi-m_lo);
        const QString readout = "x " + QString::number(dataVal, 'f', 3)
                              + "   opacity " + QString::number(qRound(pt.alpha*100)) + "%";
        p.setPen(textCol);
        p.drawText(QRect(r.left(), rect().top(), r.width(), kReadoutRowHeight),
                   Qt::AlignHCenter | Qt::AlignVCenter, readout);
    }
    // Axis range labels, one per end of the plot. Aligned to the graph's actual
    // edges rather than offset by a fixed number of pixels: a hardcoded offset
    // only lined up for one label width, so a single-digit max floated short of
    // the right edge and a wide value overflowed past it.
    p.setPen(textCol);
    QFont f = p.font(); f.setPixelSize(9); p.setFont(f);
    constexpr int kLabelRowHeight = 18;
    const int kLabelBox = 70; // generous: holds a negative multi-digit range
    p.drawText(QRect(r.left(), r.bottom()+1, kLabelBox, kLabelRowHeight),
               Qt::AlignLeft | Qt::AlignVCenter, QString::number(m_lo,'g',3));
    p.drawText(QRect(r.right()-kLabelBox, r.bottom()+1, kLabelBox, kLabelRowHeight),
               Qt::AlignRight | Qt::AlignVCenter, QString::number(m_hi,'g',3));
}

void TransferFunctionWidget::mousePressEvent(QMouseEvent* e){
    if(e->button()==Qt::LeftButton){
        int idx = hitTest(e->pos());
        if(idx>=0){ m_dragIdx=idx; m_dragging=true; }
        else if(e->modifiers() & Qt::ControlModifier){
            // add point
            auto pt = fromWidget(e->pos());
            m_curve.push_back(pt);
            tf::normalize(m_curve);
            // find new idx
            for(int i=0;i<(int)m_curve.size();++i) if(std::abs(m_curve[i].pos-pt.pos)<1e-4f){ m_dragIdx=i; break; }
            m_dragging=true;
            emit curveChanged(m_curve);
            update();
        } else {
            // Clicked empty space: drop the selection so the readout does not
            // keep showing a point the user is no longer touching.
            m_dragIdx=-1;
            update();
        }
    } else if(e->button()==Qt::RightButton){
        int idx = hitTest(e->pos());
        if(idx>=0 && m_curve.size()>2){
            m_curve.erase(m_curve.begin()+idx);
            tf::normalize(m_curve);
            m_dragIdx=-1;
            emit curveChanged(m_curve);
            update();
        }
    }
}
void TransferFunctionWidget::mouseMoveEvent(QMouseEvent* e){
    if(m_dragging && m_dragIdx>=0){
        auto pt = fromWidget(e->pos());
        // endpoints locked to x=0 / x=1
        if(m_dragIdx==0) pt.pos=0.0f;
        if(m_dragIdx==(int)m_curve.size()-1) pt.pos=1.0f;
        m_curve[m_dragIdx]=pt;
        tf::normalize(m_curve);
        // re-find drag idx after sort
        for(int i=0;i<(int)m_curve.size();++i) if(std::abs(m_curve[i].pos-pt.pos)<1e-5f && std::abs(m_curve[i].alpha-pt.alpha)<1e-5f){ m_dragIdx=i; break; }
        emit curveChanged(m_curve);
        update();
        return;
    }
    // Hover affordance: show a move cursor only over an actual point. Guarded so
    // the cursor is not re-set on every mouse move across the whole widget.
    const Qt::CursorShape want = hitTest(e->pos())>=0 ? Qt::SizeAllCursor : Qt::ArrowCursor;
    if(want!=cursor().shape()) setCursor(want);
}
void TransferFunctionWidget::mouseReleaseEvent(QMouseEvent* e){
    Q_UNUSED(e)
    m_dragging=false;
    // Keep m_dragIdx so the readout stays up while the point stays selected;
    // the next press over empty space clears it.
}
void TransferFunctionWidget::mouseDoubleClickEvent(QMouseEvent* e){
    if(e->button()==Qt::LeftButton){
        int idx = hitTest(e->pos());
        if(idx<0){
            auto pt = fromWidget(e->pos());
            m_curve.push_back(pt);
            tf::normalize(m_curve);
            emit curveChanged(m_curve);
            update();
        } else {
            // Double-clicking an existing point selects it so its values can be
            // read, instead of doing nothing.
            m_dragIdx=idx;
            update();
        }
    }
}
