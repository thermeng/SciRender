#include "line_plot_widget.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QToolTip>
#include <QFontMetrics>
#include <algorithm>
#include <cmath>

LinePlotWidget::LinePlotWidget(QWidget* p):QWidget(p){ setMouseTracking(true); setMinimumHeight(200); }

void LinePlotWidget::setData(const std::vector<float>& d, const std::vector<float>& v, float mn,float mx, const QString& f,int pl){
    m_dists=d; m_vals=v; m_vMin=mn; m_vMax=mx; m_field=f; m_placement=pl; m_hover=-1; update();
}
void LinePlotWidget::clear(){ m_dists.clear(); m_vals.clear(); m_hover=-1; update(); }

void LinePlotWidget::paintEvent(QPaintEvent*){
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing,true);
    p.fillRect(rect(), palette().color(QPalette::Window));
    if(m_dists.empty()||m_vals.empty()){ p.setPen(palette().color(QPalette::WindowText)); QFont f=p.font(); f.setItalic(true); p.setFont(f); p.drawText(rect(),Qt::AlignCenter,tr("No line data — press Sample")); return; }
    m_rect=QRect(m_left,m_top,width()-m_left-m_right,height()-m_top-m_bottom);
    if(m_rect.width()<=0||m_rect.height()<=0) return;
    // grid
    QColor grid=palette().color(QPalette::WindowText); grid.setAlpha(72);
    p.setPen(QPen(grid,1,Qt::SolidLine));
    for(int i=0;i<=4;++i){ int y=m_rect.top()+i*m_rect.height()/4; p.drawLine(m_rect.left(),y,m_rect.right(),y); }
    for(int i=1;i<4;++i){ int x=m_rect.left()+i*m_rect.width()/4; p.drawLine(x,m_rect.top(),x,m_rect.bottom()); }
    p.setPen(QPen(palette().color(QPalette::WindowText),1)); p.drawRect(m_rect);
    // line
    float dMin=m_dists.front(), dMax=m_dists.back(); if(std::abs(dMax-dMin)<1e-12f) dMax=dMin+1;
    float vRange=m_vMax-m_vMin; if(std::abs(vRange)<1e-12f) vRange=1;
    QPainterPath path; bool first=true;
    for(size_t i=0;i<m_vals.size();++i){
        float t=(m_dists[i]-dMin)/(dMax-dMin);
        float v=(m_vals[i]-m_vMin)/vRange;
        if(!std::isfinite(v)||!std::isfinite(t)) continue;
        int x=m_rect.left()+int(t*m_rect.width());
        int y=m_rect.bottom()-int(v*m_rect.height());
        if(first){ path.moveTo(x,y); first=false; } else path.lineTo(x,y);
    }
    p.setPen(QPen(QColor(0x4a,0x90,0xe2),2)); p.drawPath(path);
    // hover dot
    if(m_hover>=0 && m_hover < (int)m_vals.size()){
        float t=(m_dists[m_hover]-dMin)/(dMax-dMin);
        float v=(m_vals[m_hover]-m_vMin)/vRange;
        int x=m_rect.left()+int(t*m_rect.width());
        int y=m_rect.bottom()-int(v*m_rect.height());
        p.setBrush(QColor(0xff,0x8c,0x00)); p.setPen(Qt::NoPen); p.drawEllipse(QPoint(x,y),4,4);
    }
    // axes
    p.setPen(palette().color(QPalette::WindowText));
    QFont small=p.font(); small.setPointSize(small.pointSize()-1); p.setFont(small); QFontMetrics fm(small);
    for(int i=0;i<=4;++i){
        float v=m_vMin + (m_vMax-m_vMin)*(1.0f - i/4.0f);
        int y=m_rect.top()+i*m_rect.height()/4;
        QString txt=QString::number(v,'g',4); txt=fm.elidedText(txt,Qt::ElideRight, m_left-6);
        p.drawText(QRect(0,y-8,m_left-4,16), Qt::AlignRight|Qt::AlignVCenter, txt);
    }
    for(int i=0;i<=4;++i){
        float d=dMin + (dMax-dMin)*(i/4.0f);
        int x=m_rect.left()+i*m_rect.width()/4;
        QString txt=QString::number(d,'g',4); txt=fm.elidedText(txt,Qt::ElideRight,58);
        QRect r; int fl=Qt::AlignTop;
        if(i==0){ r=QRect(x, m_rect.bottom()+2, 60, m_bottom-4); fl|=Qt::AlignLeft; if(r.right()>width()-2) r.moveRight(width()-2); }
        else if(i==4){ r=QRect(x-60,m_rect.bottom()+2,60,m_bottom-4); fl|=Qt::AlignRight; if(r.left()<m_left) r.moveLeft(m_left); if(r.right()>width()-2) r.moveRight(width()-2); }
        else { r=QRect(x-30,m_rect.bottom()+2,60,m_bottom-4); fl|=Qt::AlignHCenter; }
        p.drawText(r, fl, txt);
    }
    // title
    { QFont tf=p.font(); tf.setBold(true); p.setFont(tf);
      QString pl=m_placement==1?"Cell":"Vertex";
      QString title=QString("%1 [%2]  Line %3→%4").arg(m_field).arg(pl).arg(dMin,0,'g',4).arg(dMax,0,'g',4);
      p.drawText(QRect(m_rect.left(),0,m_rect.width(),m_top), Qt::AlignCenter, title);
    }
}
void LinePlotWidget::mouseMoveEvent(QMouseEvent* e){
    if(m_dists.empty()||m_rect.width()<=0){ m_hover=-1; update(); return; }
    QPoint pos=e->pos();
    if(!m_rect.contains(pos)){ if(m_hover!=-1){m_hover=-1; update();} return;}
    float dMin=m_dists.front(), dMax=m_dists.back(); if(std::abs(dMax-dMin)<1e-12f) return;
    float t=float(pos.x()-m_rect.left())/m_rect.width();
    float d=dMin + t*(dMax-dMin);
    // nearest index
    int best=0; float bestD=std::abs(m_dists[0]-d);
    for(size_t i=1;i<m_dists.size();++i){ float dd=std::abs(m_dists[i]-d); if(dd<bestD){bestD=dd; best=int(i);} }
    if(best!=m_hover){ m_hover=best; update();}
    QToolTip::showText(e->globalPosition().toPoint(), tr("d=%1  v=%2").arg(m_dists[best],0,'g',4).arg(m_vals[best],0,'g',4), this);
}
void LinePlotWidget::leaveEvent(QEvent*){ if(m_hover!=-1){m_hover=-1; update();}}
