#include "line_plot_widget.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QToolTip>
#include <QFontMetrics>
#include <algorithm>
#include <cmath>

namespace {
// Curve colors: active solid blue, compare dashed rust, pinned palette cycles.
constexpr const QRgb kPinnedPalette[] = {
    0x3AA054, 0xD09030, 0x8A54B0, 0x30A0A0, 0xC23A3A, 0x7070A0
};
constexpr int kMaxPinnedLines = int(sizeof(kPinnedPalette) / sizeof(kPinnedPalette[0]));
}

LinePlotWidget::LinePlotWidget(QWidget* p):QWidget(p){ setMouseTracking(true); setMinimumHeight(200); }

void LinePlotWidget::setData(const std::vector<float>& d, const std::vector<float>& v, float mn,float mx, const QString& f,int pl){
    m_dists=d; m_vals=v; m_vMin=mn; m_vMax=mx; m_field=f; m_placement=pl; m_hover=-1; m_hoverLine=-1; update();
}
void LinePlotWidget::setCompareSeries(const std::vector<float>& vals, const QString& label){
    m_cVals = vals; m_cLabel = label; m_hover=-1; m_hoverLine=-1; update();
}
void LinePlotWidget::setPinnedLines(std::vector<LineSeries> lines){
    m_pinned = std::move(lines); m_hover=-1; m_hoverLine=-1; update();
}
void LinePlotWidget::clear(){
    m_dists.clear(); m_vals.clear(); m_cVals.clear(); m_pinned.clear(); m_hover=-1; m_hoverLine=-1; update();
}

void LinePlotWidget::paintEvent(QPaintEvent*){
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing,true);
    p.fillRect(rect(), palette().color(QPalette::Window));
    if(m_dists.empty()||m_vals.empty()){ p.setPen(palette().color(QPalette::WindowText)); QFont f=p.font(); f.setItalic(true); p.setFont(f); p.drawText(rect(),Qt::AlignCenter,tr("No line data — pick a field & endpoints in the sidebar")); return; }
    // Left gutter fits the widest Y tick label (formatted below), so labels
    // hug the axis frame instead of floating in a fixed margin.
    QFont small=p.font(); small.setPointSize(small.pointSize()-1); p.setFont(small);
    QFontMetrics fm(small);
    float vRange=m_vMax-m_vMin; if(std::abs(vRange)<1e-12f) vRange=1;
    int yMaxW=0;
    for(int i=0;i<=4;++i){
        float v=m_vMin + vRange*(1.0f - i/4.0f);
        yMaxW=std::max(yMaxW, fm.horizontalAdvance(formatProbeValue(v, m_tickFormat)));
    }
    const int left = std::max(48, yMaxW + 34);   // 34px reserved for the rotated axis title column
    // Legend band: chips wrap onto their own rows BELOW the title row, so they
    // never collide with it. Chips list ONLY non-active curves (pinned lines +
    // compare placement): the active line needs no chip — the title already
    // names field, placement and range. Chip rects double as hit targets
    // (stored in m_chips): left-click toggles visibility, right-click removes.
    bool hasCompare = !m_cVals.empty() && m_cVals.size() == m_dists.size() && m_cLabel.size();
    struct ChipDesc { QString label; QColor color; bool dashed = false; bool hidden = false; int pinned = -1; };
    std::vector<ChipDesc> chips;
    for (size_t s = 0; s < m_pinned.size(); ++s)
        chips.push_back({ m_pinned[s].label, QColor(kPinnedPalette[s % kMaxPinnedLines]), false, m_pinned[s].hidden, int(s) });
    if (hasCompare)
        chips.push_back({ m_cLabel, QColor(0xB0,0x54,0x3A), true, false, -1 });
    // Greedy wrap: fill rows left→right, each row right-aligned to the frame.
    const int chipLeft = 8;
    const int chipRight = width() - m_right;
    struct ChipRow { int first, count; };
    std::vector<ChipRow> rows;
    {
        int runStart = 0, x = chipLeft;
        for (size_t i = 0; i < chips.size(); ++i) {
            int w = fm.horizontalAdvance(chips[i].label) + 26;
            if (i > runStart && x + w > chipRight) { rows.push_back({ runStart, int(i) - runStart }); runStart = int(i); x = chipLeft; }
            x += w;
        }
        if (!chips.empty()) rows.push_back({ runStart, int(chips.size()) - runStart });
    }
    const int topTotal = m_top + int(rows.size()) * 18;
    m_rect=QRect(left,topTotal,width()-left-m_right,height()-topTotal-m_bottom);
    if(m_rect.width()<=0||m_rect.height()<=0) return;
    m_chips.clear();
    for (size_t r = 0; r < rows.size(); ++r) {
        const auto& row = rows[r];
        // right-align this row's chips
        int rowW = 0;
        for (int i = row.first; i < row.first + row.count; ++i) rowW += fm.horizontalAdvance(chips[i].label) + 26;
        int x = chipRight - rowW;
        int y = m_top + int(r) * 18 + 2;
        for (int i = row.first; i < row.first + row.count; ++i) {
            const auto& c = chips[i];
            int w = fm.horizontalAdvance(c.label) + 26;
            p.setPen(QPen(c.hidden ? QColor(0x80,0x80,0x80) : c.color, 2,
                          (c.dashed || c.hidden) ? Qt::DashLine : Qt::SolidLine));
            p.drawLine(x+2, y+8, x+18, y+8);
            p.setPen(c.hidden ? QColor(0x90,0x90,0x90) : palette().color(QPalette::WindowText));
            p.drawText(QRect(x+22, y, w-24, 14), Qt::AlignLeft|Qt::AlignVCenter, c.label);
            m_chips.push_back({ QRect(x, y, w, 16), c.pinned });
            x += w;
        }
    }
    // grid
    QColor grid=palette().color(QPalette::WindowText); grid.setAlpha(72);
    p.setPen(QPen(grid,1,Qt::SolidLine));
    for(int i=0;i<=4;++i){ int y=m_rect.top()+i*m_rect.height()/4; p.drawLine(m_rect.left(),y,m_rect.right(),y); }
    for(int i=1;i<4;++i){ int x=m_rect.left()+i*m_rect.width()/4; p.drawLine(x,m_rect.top(),x,m_rect.bottom()); }
    p.setPen(QPen(palette().color(QPalette::WindowText),1)); p.drawRect(m_rect);
    // curve builder: each series maps its own dists to the full plot width;
    // non-finite values break the path instead of bridging across the gap.
    auto makePath = [&](const std::vector<float>& dists, const std::vector<float>& vals) {
        QPainterPath path; bool pen=false;
        if (dists.size() < 2) return path;
        float lo = dists.front(), hi = dists.back();
        if (std::abs(hi - lo) < 1e-12f) hi = lo + 1;
        for (size_t i = 0; i < vals.size() && i < dists.size(); ++i) {
            float t = (dists[i] - lo) / (hi - lo);
            float v = (vals[i] - m_vMin) / vRange;
            if (!std::isfinite(v) || !std::isfinite(t)) { pen=false; continue; }
            int x = m_rect.left() + int(t * m_rect.width());
            int y = m_rect.bottom() - int(v * m_rect.height());
            if (!pen) { path.moveTo(x, y); pen=true; } else path.lineTo(x, y);
        }
        return path;
    };
    // X positions for the hover dot need the same normalization per series.
    auto sampleX = [&](const std::vector<float>& dists, int idx) {
        float lo = dists.front(), hi = dists.back();
        if (std::abs(hi - lo) < 1e-12f) hi = lo + 1;
        float t = (dists[idx] - lo) / (hi - lo);
        return m_rect.left() + int(t * m_rect.width());
    };
    // pinned curves: oldest first, drawn UNDER the active line; hidden = skipped
    for (size_t s = 0; s < m_pinned.size(); ++s) {
        const auto& ps = m_pinned[s];
        if (ps.hidden || ps.dists.size() < 2) continue;
        p.setPen(QPen(QColor(kPinnedPalette[s % kMaxPinnedLines]), 2));
        p.drawPath(makePath(ps.dists, ps.vals));
    }
    // compare curve (other placement, same dists): dashed, drawn UNDER the primary
    if (hasCompare) {
        p.setPen(QPen(QColor(0xB0,0x54,0x3A), 2, Qt::DashLine));
        p.drawPath(makePath(m_dists, m_cVals));
    }
    p.setPen(QPen(QColor(0x4a,0x90,0xe2),2));
    p.drawPath(makePath(m_dists, m_vals));
    // hovered pinned line: redraw ON TOP, thicker — near-overlapping curves
    // (pins drawn under the active line) otherwise can't be told apart
    if (m_hoverLine >= 0 && m_hoverLine < (int)m_pinned.size() && !m_pinned[m_hoverLine].hidden &&
        m_pinned[m_hoverLine].dists.size() >= 2) {
        p.setPen(QPen(QColor(kPinnedPalette[m_hoverLine % kMaxPinnedLines]), 3));
        p.drawPath(makePath(m_pinned[m_hoverLine].dists, m_pinned[m_hoverLine].vals));
    }
    // hover dot: orange on the active line, series color on a pinned line
    {
        const std::vector<float>* hd = nullptr; const std::vector<float>* hv = nullptr; QColor hc(0xff,0x8c,0x00);
        if (m_hoverLine < 0) { hd=&m_dists; hv=&m_vals; }
        else if (m_hoverLine < (int)m_pinned.size()) { hd=&m_pinned[m_hoverLine].dists; hv=&m_pinned[m_hoverLine].vals; hc=QColor(kPinnedPalette[m_hoverLine % kMaxPinnedLines]); }
        if (hd && hv && m_hover >= 0 && m_hover < (int)hv->size() && m_hover < (int)hd->size() &&
            std::isfinite((*hv)[m_hover])) {
            int x = sampleX(*hd, m_hover);
            float v = ((*hv)[m_hover] - m_vMin) / vRange;
            int y = m_rect.bottom() - int(v * m_rect.height());
            p.setBrush(hc); p.setPen(Qt::NoPen); p.drawEllipse(QPoint(x,y),4,4);
        }
    }
    // axes
    p.setPen(palette().color(QPalette::WindowText));
    p.setFont(small);
    for(int i=0;i<=4;++i){
        float v=m_vMin + vRange*(1.0f - i/4.0f);
        int y=m_rect.top()+i*m_rect.height()/4;
        QString txt=formatProbeValue(v, m_tickFormat);
        int tickW = fm.horizontalAdvance(txt);
        int maxW = yMaxW;
        if (tickW > maxW) { txt = fm.elidedText(txt, Qt::ElideRight, maxW); tickW = maxW; }
        // right-aligned, 6px off the frame so nothing floats in dead margin
        p.drawText(QRect(m_rect.left()-6-tickW, y-8, tickW, 16), Qt::AlignRight|Qt::AlignVCenter, txt);
    }
    float dMin=m_dists.front(), dMax=m_dists.back(); if(std::abs(dMax-dMin)<1e-12f) dMax=dMin+1;
    for(int i=0;i<=4;++i){
        float d=dMin + (dMax-dMin)*(i/4.0f);
        int x=m_rect.left()+i*m_rect.width()/4;
        QString txt=formatProbeValue(d, m_tickFormat); txt=fm.elidedText(txt,Qt::ElideRight,58);
        QRect r; int fl=Qt::AlignTop;
        if(i==0){ r=QRect(x, m_rect.bottom()+2, 60, m_bottom-4); fl|=Qt::AlignLeft; if(r.right()>width()-2) r.moveRight(width()-2); }
        else if(i==4){ r=QRect(x-60,m_rect.bottom()+2,60,m_bottom-4); fl|=Qt::AlignRight; if(r.left()<m_rect.left()) r.moveLeft(m_rect.left()); if(r.right()>width()-2) r.moveRight(width()-2); }
        else { r=QRect(x-30,m_rect.bottom()+2,60,m_bottom-4); fl|=Qt::AlignHCenter; }
        p.drawText(r, fl, txt);
    }
    // title
    { QFont tf=p.font(); tf.setBold(true); p.setFont(tf);
      QString pl=m_placement==1?"Cell":"Vertex";
      QString title=QString("%1 [%2]  Line %3→%4").arg(m_field).arg(pl).arg(dMin,0,'g',4).arg(dMax,0,'g',4);
      if (hasCompare) title=QString("%1 [%2 vs %3]  Line %4→%5").arg(m_field).arg(pl).arg(m_cLabel).arg(dMin,0,'g',4).arg(dMax,0,'g',4);
      if (!m_pinned.empty()) title+=tr("  (+%1)").arg(m_pinned.size());
      p.drawText(QRect(m_rect.left(),0,m_rect.width(),m_top), Qt::AlignCenter, title);
    }
    // axis titles: Y = field[placement], X = distance along the probe line
    { QFont tf=p.font(); tf.setItalic(false); p.setFont(tf);
      QString pl=m_placement==1?"Cell":"Vertex";
      p.save();
      p.translate(10, m_rect.top() + m_rect.height()/2.0);
      p.rotate(-90);
      p.drawText(QRect(-m_rect.height()/2, -7, m_rect.height(), 14), Qt::AlignCenter,
                 QString("%1 [%2]").arg(m_field).arg(pl));
      p.restore();
      p.drawText(QRect(m_rect.left(), m_rect.bottom()+2, m_rect.width(), m_bottom-2),
                 Qt::AlignHCenter|Qt::AlignBottom, tr("Distance along line"));
    }
}
void LinePlotWidget::mouseMoveEvent(QMouseEvent* e){
    // Legend chips first: interactive chips give a hint tooltip & pointing hand
    for (const auto& ch : m_chips) {
        if (ch.r.contains(e->pos())) {
            if (ch.pinned >= 0) {
                if (cursor().shape() != Qt::PointingHandCursor) setCursor(Qt::PointingHandCursor);
                const auto& ps = m_pinned[ch.pinned];
                QToolTip::showText(e->globalPosition().toPoint(),
                    tr("%1\nClick: hide/show — Right-click: remove").arg(ps.tipLabel.isEmpty()?ps.label:ps.tipLabel), this);
            } else if (cursor().shape() != Qt::ArrowCursor) {
                setCursor(Qt::ArrowCursor);
            }
            if (m_hover != -1 || m_hoverLine != -1) { m_hover=-1; m_hoverLine=-1; update(); }
            return;
        }
    }
    if (cursor().shape() != Qt::ArrowCursor) setCursor(Qt::ArrowCursor);
    bool anyData = !m_dists.empty() || !m_pinned.empty();
    if(!anyData || m_rect.width()<=0){ m_hover=-1; m_hoverLine=-1; update(); return; }
    QPoint pos=e->pos();
    if(!m_rect.contains(pos)){ if(m_hover!=-1||m_hoverLine!=-1){ m_hover=-1; m_hoverLine=-1; update(); } return; }
    // Every series spans the full plot width, so the mouse x maps to a param
    // fraction t that ALL lines share — x alone can't tell lines apart. The
    // hovered line is therefore the curve whose VALUE at t is vertically
    // nearest the cursor; the tooltip then lists every visible line's value.
    float t=float(pos.x()-m_rect.left())/float(m_rect.width());
    float vRange=m_vMax-m_vMin; if(std::abs(vRange)<1e-12f) vRange=1;
    auto nearest = [](const std::vector<float>& dists, float t) {
        if (dists.size() < 2) return -1;
        float lo = dists.front(), hi = dists.back();
        if (std::abs(hi - lo) < 1e-12f) hi = lo + 1;
        int best=-1; float bestD=1e30f;
        for (size_t i = 0; i < dists.size(); ++i) {
            float dd = std::abs((dists[i]-lo)/(hi-lo) - t);
            if (dd < bestD) { bestD=dd; best=int(i); }
        }
        return best;
    };
    struct Row { int line; int idx; QString label; QString tip; };
    std::vector<Row> rows;
    float bestY=1e30f; int hoverRow=-1;
    auto consider = [&](int line, const std::vector<float>& d, const std::vector<float>& v,
                        const QString& label, const QString& tip) {
        // Note: dists/vals lengths always match by construction (same N per line,
        // guaranteed by PlotWindow::sampleLine).
        int idx = nearest(d, t);
        if (idx < 0 || idx >= (int)d.size() || idx >= (int)v.size()) return;
        float val = v[idx];
        if (!std::isfinite(val)) return; // no finite value here → not hoverable
        float yPix = float(m_rect.bottom()) - (val - m_vMin)/vRange * float(m_rect.height());
        float dy = std::abs(float(pos.y()) - yPix);
        if (dy < bestY) { bestY = dy; hoverRow = int(rows.size()); }
        rows.push_back({ line, idx, label, tip });
    };
    QString activeLabel = tr("%1 [%2]").arg(m_field).arg(m_placement==1?tr("Cell"):tr("Vertex"));
    if (!m_dists.empty())
        consider(-1, m_dists, m_vals, activeLabel, activeLabel);
    for (size_t s = 0; s < m_pinned.size(); ++s) {
        if (m_pinned[s].hidden) continue;
        consider(int(s), m_pinned[s].dists, m_pinned[s].vals, m_pinned[s].label,
                 m_pinned[s].tipLabel.isEmpty() ? m_pinned[s].label : m_pinned[s].tipLabel);
    }
    // Tooltip only when the cursor is actually ON (or within a few px of) a
    // curve — free plot space shows no tooltip so stray hovers don't trigger.
    constexpr float kHoverPx = 12.0f;
    if (hoverRow < 0 || bestY > kHoverPx) {
        if (m_hover!=-1 || m_hoverLine!=-1) { m_hover=-1; m_hoverLine=-1; update(); }
        QToolTip::hideText();
        return;
    }
    if (rows[hoverRow].line != m_hoverLine || rows[hoverRow].idx != m_hover) {
        m_hoverLine = rows[hoverRow].line; m_hover = rows[hoverRow].idx;
        QToolTip::hideText(); // stale text would linger across the row switch otherwise
        update();
    }
    const Row& hr = rows[hoverRow];
    const auto& hd2 = hr.line < 0 ? m_dists : m_pinned[hr.line].dists;
    const auto& hv2 = hr.line < 0 ? m_vals : m_pinned[hr.line].vals;
    QString tip = tr("%1  d=%2  v=%3").arg(hr.tip.isEmpty()?hr.label:hr.tip)
                                      .arg(hd2[hr.idx],0,'g',4).arg(hv2[hr.idx],0,'g',4);
    QToolTip::showText(e->globalPosition().toPoint(), tip, this);
}
void LinePlotWidget::mousePressEvent(QMouseEvent* e){
    for (const auto& ch : m_chips) {
        if (ch.pinned >= 0 && ch.r.contains(e->pos())) {
            if (e->button() == Qt::LeftButton) emit pinnedVisibilityToggled(ch.pinned);
            else if (e->button() == Qt::RightButton) emit pinnedRemoveRequested(ch.pinned);
            return;
        }
    }
}
void LinePlotWidget::leaveEvent(QEvent*){ if(m_hover!=-1 || m_hoverLine!=-1){ m_hover=-1; m_hoverLine=-1; update();}}
