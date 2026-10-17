#include "postproc_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui_postproc_page.h"
#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"

namespace sidebar {

PostProcPage::PostProcPage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    auto* content = new QWidget;
    Ui::PostprocPage ppUi;
    ppUi.setupUi(content);
    sb::fixLayoutOverflow(content);

    auto* s = S();
    m_velCombo = ppUi.velocityCombo;
    m_presCombo = ppUi.pressureCombo;
    m_vortCb = ppUi.vorticityCb;
    m_helCb = ppUi.helicityCb;
    m_qCb = ppUi.qCb;
    m_gradPCb = ppUi.gradPCb;
    m_autoCb = ppUi.autoCb;
    m_extractBtn = ppUi.extractBtn;
    m_statusLbl = ppUi.statusLabel;
    if (m_statusLbl) {
        m_statusLbl->setWordWrap(true);
        m_statusLbl->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        m_statusLbl->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_statusLbl->setMinimumHeight(88);
        m_statusLbl->setStyleSheet("QLabel { padding: 8px; background: #f6f8fa; border: 1px solid #e1e4e8; border-radius: 6px; font-family: monospace; font-size: 9pt; color: #24292e; }");
    }
    m_velCombo->addItems(s->getAvailableVectors());
    m_velCombo->setCurrentText(s->getPostVelocitySource());
    m_presCombo->addItems(s->getAvailableScalars());
    m_presCombo->setCurrentText(s->getPostPressureSource());
    m_vortCb->setChecked(s->getPostWantVorticity());
    m_helCb->setChecked(s->getPostWantHelicity());
    m_qCb->setChecked(s->getPostWantQ());
    m_gradPCb->setChecked(s->getPostWantGradP());
    m_autoCb->setChecked(s->getPostAutoReextract());
    connect(m_velCombo, &QComboBox::currentTextChanged, s, [this, s](const QString &t){ if (!t.isEmpty()) s->setPostVelocitySource(t); });
    connect(m_presCombo, &QComboBox::currentTextChanged, s, [this, s](const QString &t){ if (!t.isEmpty()) s->setPostPressureSource(t); });
    // Also handle re-select of same index (activated fires even when currentTextChanged doesn't)
    connect(m_velCombo, QOverload<int>::of(&QComboBox::activated), s, [this, s](int idx){ if(idx>=0) s->setPostVelocitySource(m_velCombo->itemText(idx)); });
    connect(m_presCombo, QOverload<int>::of(&QComboBox::activated), s, [this, s](int idx){ if(idx>=0) s->setPostPressureSource(m_presCombo->itemText(idx)); });
    connect(m_vortCb, &QCheckBox::toggled, s, &RenderSettings::setPostWantVorticity);
    connect(m_helCb, &QCheckBox::toggled, s, &RenderSettings::setPostWantHelicity);
    connect(m_qCb, &QCheckBox::toggled, s, &RenderSettings::setPostWantQ);
    connect(m_gradPCb, &QCheckBox::toggled, s, &RenderSettings::setPostWantGradP);
    connect(m_autoCb, &QCheckBox::toggled, s, &RenderSettings::setPostAutoReextract);
    connect(m_extractBtn, &QPushButton::clicked, s, &RenderSettings::extractPostFields);

    connect(s, &RenderSettings::meshDataUpdated, this, &PostProcPage::refreshPage);

    refreshPage();
    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();
    setContent(content);
}

void PostProcPage::refreshPage() {
    auto* s = S();
    const bool hasMesh = s->getHasMeshLoaded();
    const bool cart = s->isPostCartesian();
    const bool canExtract = s->canExtractPostFields();
    // Sync stale velocity/pressure sources (e.g., previous mesh had "velocity" but new mesh has "U")
    // This fixes bug where extraction required manual reselect.
    {
        auto vecs = s->getAvailableVectors();
        QString curVel = s->getPostVelocitySource();
        m_velCombo->blockSignals(true);
        if (hasMesh && !vecs.isEmpty() && !vecs.contains(curVel)) {
            curVel = vecs.first();
            s->setPostVelocitySource(curVel);
        }
        auto scals = s->getAvailableScalars();
        QString curPres = s->getPostPressureSource();
        m_presCombo->blockSignals(true);
        if (hasMesh && !scals.isEmpty() && !scals.contains(curPres)) {
            // Prefer first scalar that is not a derived vector magnitude if possible, but any will do
            curPres = scals.first();
            s->setPostPressureSource(curPres);
        }
        m_velCombo->clear(); m_velCombo->addItems(vecs); m_velCombo->setCurrentText(curVel); m_velCombo->setEnabled(hasMesh);
        m_velCombo->blockSignals(false);
        m_presCombo->clear(); m_presCombo->addItems(scals); m_presCombo->setCurrentText(curPres); m_presCombo->setEnabled(hasMesh);
        m_presCombo->blockSignals(false);
    }
    if (m_statusLbl) {
        if (!hasMesh) m_statusLbl->setText("No mesh loaded");
        else if (!canExtract) m_statusLbl->setText(cart?"Cartesian structured grid required":"Point vectors/scalars required");
        else if (s->isPostBusy()) m_statusLbl->setText("Extracting...");
        else {
            QString txt = s->getPostStatus();
            if (txt.isEmpty()) txt = cart?"Cartesian grid ready":"Unstructured grid ready (LSQ)";
            m_statusLbl->setText(txt);
        }
    }
    if (m_extractBtn) m_extractBtn->setEnabled(hasMesh && canExtract && !s->isPostBusy());
}

} // namespace sidebar
