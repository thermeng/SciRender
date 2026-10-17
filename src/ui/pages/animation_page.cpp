#include "animation_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontMetrics>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <QTimer>

#include "ui_animation_page.h"
#include "render/settings/animation_controller.h"
#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"

namespace sidebar {

namespace {
// Chrome overhead (arrow + frame): drawn by the style, not part of the text,
// so any combo width computed from font metrics must add this back.
inline constexpr int kComboChromeWidth = 28;
} // namespace

AnimationPage::AnimationPage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    auto* content = new QWidget;
    Ui::AnimationPage animUi;
    animUi.setupUi(content);
    sb::fixLayoutOverflow(content);

    auto* s = S();
    auto* ctrl = s->anim();

    m_playBtn       = animUi.playBtn;
    m_stepBackBtn   = animUi.stepBackBtn;
    m_stepFwdBtn    = animUi.stepFwdBtn;
    m_frameCombo    = animUi.frameCombo;
    m_jumpTimeSpin  = animUi.jumpTimeSpin;
    m_jumpTimeBtn   = animUi.jumpTimeBtn;
    m_fpsSpin       = animUi.fpsSpin;
    m_loopFromSpin  = animUi.loopFromSpin;
    m_loopToSpin    = animUi.loopToSpin;
    m_loopRangeCb   = animUi.loopRangeCb;
    m_statusLabel   = animUi.statusLabel;
    m_sequenceLabel = animUi.sequenceLabel;
    m_loopCb        = animUi.loopCb;
    m_scaleCombo    = animUi.scaleCombo;
    m_exportBtn     = animUi.exportBtn;
    m_bufferBar     = animUi.bufferBar;

    // Combo sizing. fixLayoutOverflow deliberately skips QComboBox, so the fixed
    // sizes in animation_page.ui reach the running UI untouched.
    //
    // frameCombo is pinned to minimumSize/maximumSize 180x22 / 200x22: the 22px
    // height capped it below every other control on the page (the house height is
    // 24), and the 200px maximum hard-clamped the width so long frame labels
    // clipped. Drop both clamps and size from the contents instead.
    m_frameCombo->setMinimumWidth(0);
    m_frameCombo->setMaximumWidth(QWIDGETSIZE_MAX);
    m_frameCombo->setMinimumHeight(0);
    m_frameCombo->setMaximumHeight(QWIDGETSIZE_MAX);
    // Frame items read like "12.5 s (#30)"; 12 chars keeps the combo wide enough
    // for the usual label without letting one long item dictate the layout.
    m_frameCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_frameCombo->setMinimumContentsLength(12);

    // scaleCombo shares its row with the "Colormap scale" label, so the layout
    // hands it only the leftover width and "Whole sequence" ran under the arrow.
    // Reserve the widest item plus the arrow explicitly instead of letting the
    // row squeeze it.
    {
        QFontMetrics fm(m_scaleCombo->font());
        int widest = 0;
        for (int i = 0; i < m_scaleCombo->count(); ++i)
            widest = qMax(widest, fm.horizontalAdvance(m_scaleCombo->itemText(i)));
        m_scaleCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        m_scaleCombo->setMinimumWidth(widest + kComboChromeWidth);
    }

    // Transport and time-seek row sizing. These two rows overflow the ~220px
    // content width of the 240px sidebar: stepBack(80)+play(80)+stepFwd(80) and
    // jumpLabel(52)+jumpSpin(96)+jumpBtn(80) both exceed it once spacing is
    // added. Use media-player style arrows for the transport buttons (tooltips
    // carry the full wording) and cap the row minimums so Qt stretches each
    // widget to share the space instead of pushing the page too wide.
    m_stepBackBtn->setText(QChar(0x25C0));
    m_stepFwdBtn->setText(QChar(0x25B6));
    for (auto* b : { m_stepBackBtn, m_playBtn, m_stepFwdBtn, m_jumpTimeBtn })
        b->setMinimumWidth(64);
    m_jumpTimeSpin->setMinimumWidth(60);

    // Transport
    connect(m_playBtn, &QPushButton::clicked, ctrl, &AnimationController::togglePlay);
    connect(m_stepBackBtn, &QPushButton::clicked, ctrl, &AnimationController::stepBackward);
    connect(m_stepFwdBtn, &QPushButton::clicked, ctrl, &AnimationController::stepForward);
    connect(m_loopCb, &QCheckBox::toggled, ctrl, &AnimationController::setLoop);
    connect(m_scaleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), s,
            [s](int idx) { s->setAnimScaleGlobal(idx == 0); });
    connect(m_frameCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), s,
            [s](int idx) { if (idx >= 0) s->anim()->seek(idx); });
    connect(m_fpsSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), s,
            [s](double v) {
                s->anim()->setSpeedMultiplier(1.0);
                s->anim()->setFps(v);
            });
    connect(m_jumpTimeBtn, &QPushButton::clicked, this, [this]() {
        if (!m_jumpTimeSpin) return;
        S()->anim()->seekToTime(m_jumpTimeSpin->value());
    });
    connect(m_jumpTimeSpin, &QDoubleSpinBox::returnPressed, this,
            [this]() { if (m_jumpTimeBtn) m_jumpTimeBtn->click(); });
    connect(m_loopFromSpin, QOverload<int>::of(&QSpinBox::valueChanged), s,
            [s, this](int from) { s->anim()->setLoopRange(from, m_loopToSpin->value()); });
    connect(m_loopToSpin, QOverload<int>::of(&QSpinBox::valueChanged), s,
            [s, this](int to) { s->anim()->setLoopRange(m_loopFromSpin->value(), to); });
    connect(m_loopRangeCb, &QCheckBox::toggled, s,
            [s](bool v) { s->anim()->setLoopRangeActive(v); });

    // Open a .pvd straight from the page.
    connect(animUi.openPvdBtn, &QPushButton::clicked, this, [s]() {
        QString path = QFileDialog::getOpenFileName(nullptr, "Load PVD Animation", QString(),
            "PVD collections (*.pvd);;All files (*)");
        if (!path.isEmpty()) s->loadMesh(path);
    });

    connect(m_exportBtn, &QPushButton::clicked, this, [this]() { emit exportRequested(); });

    // Controller → UI sync (stateChanged covers play/pause/seek/load). The nav
    // badge for this section rides the same signal; MainWindow keeps that duty.
    connect(ctrl, &AnimationController::stateChanged, this, &AnimationPage::refreshPage);

    refreshPage();
    // The .ui page releases the sender early, and stateChanged may not have
    // fired yet when the page is constructed; refresh once queued.
    QTimer::singleShot(0, this, &AnimationPage::refreshPage);

    setContent(content);
}

void AnimationPage::refreshPage() {
    auto* s = S();
    auto* ctrl = s->anim();
    const bool has = ctrl->hasSequence();

    m_playBtn->setEnabled(has);
    m_stepBackBtn->setEnabled(has);
    m_stepFwdBtn->setEnabled(has);
    m_loopCb->setEnabled(has);
    m_scaleCombo->setEnabled(has);
    m_frameCombo->setEnabled(has);
    m_jumpTimeSpin->setEnabled(has);
    m_jumpTimeBtn->setEnabled(has);
    m_fpsSpin->setEnabled(has);
    m_loopFromSpin->setEnabled(has);
    m_loopToSpin->setEnabled(has);
    m_loopRangeCb->setEnabled(has);
    m_exportBtn->setEnabled(has);

    if (!has) {
        m_sequenceLabel->setText("No sequence loaded");
        m_statusLabel->setText(QString());
        m_playBtn->setText("Play");
        m_frameCount = -1;
        if (m_frameCombo) m_frameCombo->clear();
        return;
    }

    m_playBtn->setText(ctrl->isPlaying() ? "Pause" : "Play");
    m_loopCb->blockSignals(true);
    m_loopCb->setChecked(ctrl->loop());
    m_loopCb->blockSignals(false);
    m_scaleCombo->blockSignals(true);
    m_scaleCombo->setCurrentIndex(s->getAnimScaleGlobal() ? 0 : 1);
    m_scaleCombo->blockSignals(false);

    const int n = ctrl->frameCount();
    if (n != m_frameCount) {
        m_frameCount = n;
        m_frameCombo->blockSignals(true);
        m_frameCombo->clear();
        for (int i = 0; i < n; ++i) {
            const double t = ctrl->frameTime(i);
            m_frameCombo->addItem(
                QString("Frame %1 / %2  [t = %3 s]").arg(i + 1).arg(n).arg(t, 0, 'f', 4));
        }
        m_frameCombo->blockSignals(false);
    }
    m_frameCombo->blockSignals(true);
    m_frameCombo->setCurrentIndex(std::clamp(ctrl->currentFrame(), 0, std::max(0, n - 1)));
    m_frameCombo->blockSignals(false);

    m_jumpTimeSpin->blockSignals(true);
    m_jumpTimeSpin->setMaximum(n > 0 ? ctrl->frameTime(n - 1) * 1.01 : 1000.0);
    m_jumpTimeSpin->setValue(ctrl->currentTime());
    m_jumpTimeSpin->blockSignals(false);

    m_fpsSpin->blockSignals(true);
    m_fpsSpin->setValue(ctrl->fps());
    m_fpsSpin->blockSignals(false);

    m_loopFromSpin->blockSignals(true);
    m_loopToSpin->blockSignals(true);
    m_loopFromSpin->setRange(0, std::max(0, n - 1));
    m_loopToSpin->setRange(0, std::max(0, n - 1));
    m_loopFromSpin->setValue(ctrl->loopRangeStart());
    m_loopToSpin->setValue(ctrl->loopRangeEnd());
    m_loopFromSpin->blockSignals(false);
    m_loopToSpin->blockSignals(false);

    m_loopRangeCb->blockSignals(true);
    m_loopRangeCb->setChecked(ctrl->loopRangeActive());
    m_loopRangeCb->blockSignals(false);

    m_sequenceLabel->setText(ctrl->sequenceName());

    if (ctrl->isBuffering()) {
        m_statusLabel->setText("Buffering…");
        m_statusLabel->setStyleSheet("color: #FFAA44;");
        if (m_bufferBar) {
            m_bufferBar->setEnabled(true);
            m_bufferBar->setValue(static_cast<int>(ctrl->bufferingProgress() * 1000));
        }
    } else {
        m_statusLabel->setText(QString());
        m_statusLabel->setStyleSheet(QString());
        if (m_bufferBar) {
            m_bufferBar->setEnabled(false);
            m_bufferBar->setValue(0);
        }
    }
}

} // namespace sidebar
