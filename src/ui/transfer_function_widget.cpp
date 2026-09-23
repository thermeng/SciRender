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
}

QRect TransferFunctionWidget::graphRect() const {
    return rect().adjusted(10,10, -10, -20);
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
    // background
    p.fillRect(rect(), palette().color(QPalette::Base));
    p.fillRect(r, QColor(30,30,30));
    p.setPen(QColor(80,80,80));
    p.drawRect(r);
    // grid
    p.setPen(QColor(60,60,60));
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
        p.setBrush(sel? QColor(255,220,100): QColor(200,200,200));
        p.setPen(QColor(20,20,20));
        p.drawEllipse(w, 5,5);
    }
    // labels
    p.setPen(palette().color(QPalette::Text));
    QFont f = p.font(); f.setPixelSize(9); p.setFont(f);
    p.drawText(r.left(), r.bottom()+12, QString::number(m_lo,'g',3));
    p.drawText(r.right()-40, r.bottom()+12, QString::number(m_hi,'g',3));
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
    }
}
void TransferFunctionWidget::mouseReleaseEvent(QMouseEvent* e){
    Q_UNUSED(e)
    m_dragging=false;
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
        }
    }
}
