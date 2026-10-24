#include "ui/cfd_bc_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace {

struct BcTypeInfo {
    cfd::BCType type;
    const char* label;
};

// Presentation order matches how a case is usually set up: walls and symmetry
// first, then the flow boundaries.
const BcTypeInfo kBcTypes[] = {
    {cfd::BCType::WallNoSlip, "No-slip wall"},
    {cfd::BCType::SlipWall, "Slip wall"},
    {cfd::BCType::Symmetry, "Symmetry"},
    {cfd::BCType::VelocityInlet, "Velocity inlet"},
    {cfd::BCType::PressureOutlet, "Pressure outlet"},
    {cfd::BCType::Outlet, "Outlet"},
    {cfd::BCType::MovingWall, "Moving wall"},
};
constexpr int kBcTypeCount = int(sizeof(kBcTypes) / sizeof(kBcTypes[0]));

int indexOfType(cfd::BCType type) {
    for (int i = 0; i < kBcTypeCount; ++i)
        if (kBcTypes[i].type == type) return i;
    return 0;
}

cfd::BCType typeAt(int index) {
    if (index < 0 || index >= kBcTypeCount) index = 0;
    return kBcTypes[index].type;
}

QString labelForType(cfd::BCType type) {
    return QString::fromUtf8(kBcTypes[indexOfType(type)].label);
}

// Conditions that take no parameters still deserve a line explaining what the
// solver will do, so the empty right-hand pane never reads as "broken".
const char* hintForType(cfd::BCType type) {
    switch (type) {
        case cfd::BCType::WallNoSlip:
            return "No-slip wall: velocity is fixed to zero at the surface.";
        case cfd::BCType::SlipWall:
            return "Slip wall: no shear stress and no flow through the surface.";
        case cfd::BCType::Symmetry:
            return "Symmetry plane: zero normal velocity, mirrored across the patch.";
        case cfd::BCType::Outlet:
            return "Outlet: fully developed outflow (zero-gradient).";
        default:
            return "";
    }
}

QLabel* sectionHeader(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    QFont f = label->font();
    f.setPixelSize(11);
    f.setWeight(QFont::Medium);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.3);
    label->setFont(f);
    return label;
}

QLabel* fieldLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    QFont f = label->font();
    f.setPixelSize(10);
    label->setFont(f);
    return label;
}

QFrame* divider(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::NoFrame);
    line->setFixedHeight(1);
    line->setStyleSheet("background-color: palette(mid);");
    return line;
}

// Fixed-pitch numerals keep the coefficient columns from shifting as values
// change width; matches the range editor's spin boxes.
QDoubleSpinBox* makeCoefficientSpin(double value, QWidget* parent) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(-1e6, 1e6);
    spin->setDecimals(4);
    spin->setValue(value);
    spin->setMinimumWidth(84);
    spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QFont f("JetBrains Mono");
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(11);
    spin->setFont(f);
    return spin;
}

} // namespace

CfdBoundaryConditionsDialog::CfdBoundaryConditionsDialog(std::vector<cfd::PatchBC> patchBCs,
                                                         std::vector<std::size_t> faceCounts,
                                                         QWidget* parent)
    : QDialog(parent), m_bcs(std::move(patchBCs)), m_faceCounts(std::move(faceCounts)) {
    setWindowTitle("Boundary Conditions");
    setModal(true);
    setMinimumSize(760, 470);

    buildTable();

    auto* detail = new QWidget(this);
    buildDetail(detail);

    auto* body = new QWidget(this);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(14);
    bodyLayout->addWidget(m_table, 0);
    auto* vline = new QFrame(body);
    vline->setFrameShape(QFrame::VLine);
    vline->setFrameShadow(QFrame::Plain);
    vline->setStyleSheet("color: palette(mid);");
    bodyLayout->addWidget(vline);
    bodyLayout->addWidget(detail, 1);

    auto* description = new QLabel(
        "Assign a condition to each mesh patch. Select a patch to edit its parameters.", this);
    description->setWordWrap(true);
    description->setStyleSheet("color: palette(mid);");

    auto* presetRow = new QHBoxLayout();
    presetRow->setSpacing(6);
    presetRow->addWidget(fieldLabel("Presets", this));
    auto* allNoSlip = new QPushButton("All: No-slip", this);
    auto* allSlip = new QPushButton("All: Slip", this);
    auto* allSymmetry = new QPushButton("All: Symmetry", this);
    for (QPushButton* b : {allNoSlip, allSlip, allSymmetry}) presetRow->addWidget(b);
    presetRow->addStretch(1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);
    root->addWidget(description);
    root->addWidget(body, 1);
    root->addLayout(presetRow);
    root->addWidget(buttons);

    // ── Wiring ───────────────────────────────────────────────────────────────
    connect(m_table, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) { if (row >= 0) selectRow(row); });

    connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                if (m_syncing) return;
                commitCurrent();
                refreshConditionCell(m_current);
                updateConditionVisibility();
            });

    connect(m_suppressBackflow, &QCheckBox::toggled, this, [this](bool) {
        if (m_syncing) return;
        commitCurrent();
        updateConditionVisibility();
    });

    for (QDoubleSpinBox* spin : {m_u, m_v, m_w, m_pressure, m_backflowU, m_backflowV, m_backflowW,
                                 m_backflowK, m_backflowOmega, m_kInlet, m_omegaInlet}) {
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this](double) { if (!m_syncing) commitCurrent(); });
    }

    connect(allNoSlip, &QPushButton::clicked, this, [this] { applyToAll(cfd::BCType::WallNoSlip); });
    connect(allSlip, &QPushButton::clicked, this, [this] { applyToAll(cfd::BCType::SlipWall); });
    connect(allSymmetry, &QPushButton::clicked, this, [this] { applyToAll(cfd::BCType::Symmetry); });

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (!m_bcs.empty()) m_table->setCurrentCell(0, 0);
}

void CfdBoundaryConditionsDialog::buildTable() {
    m_table = new QTableWidget(this);
    m_table->setColumnCount(2);
    m_table->setHorizontalHeaderLabels({"Patch", "Condition"});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setMinimumWidth(240);
    m_table->setMaximumWidth(320);
    m_table->setRowCount(int(m_bcs.size()));

    for (int row = 0; row < int(m_bcs.size()); ++row) {
        auto* nameItem = new QTableWidgetItem(QString::fromStdString(m_bcs[row].patchName));
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        const std::size_t faces = row < int(m_faceCounts.size()) ? m_faceCounts[row] : 0;
        nameItem->setToolTip(QString("%1 faces").arg(faces));
        m_table->setItem(row, 0, nameItem);

        auto* condItem = new QTableWidgetItem;
        condItem->setFlags(condItem->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, 1, condItem);
        refreshConditionCell(row);
    }
}

void CfdBoundaryConditionsDialog::buildDetail(QWidget* detail) {
    auto* lay = new QVBoxLayout(detail);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);

    m_patchTitle = new QLabel(detail);
    QFont titleFont = m_patchTitle->font();
    titleFont.setPixelSize(13);
    titleFont.setWeight(QFont::DemiBold);
    m_patchTitle->setFont(titleFont);

    m_patchMeta = new QLabel(detail);
    QFont metaFont = m_patchMeta->font();
    metaFont.setPixelSize(10);
    m_patchMeta->setFont(metaFont);
    m_patchMeta->setStyleSheet("color: palette(mid);");

    lay->addWidget(m_patchTitle);
    lay->addWidget(m_patchMeta);
    lay->addWidget(divider(detail));

    // ── Condition ──
    auto* conditionForm = new QFormLayout;
    conditionForm->setContentsMargins(0, 0, 0, 0);
    conditionForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    conditionForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    conditionForm->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_typeCombo = new QComboBox(detail);
    for (const BcTypeInfo& info : kBcTypes) m_typeCombo->addItem(QString::fromUtf8(info.label));
    conditionForm->addRow(fieldLabel("Condition", detail), m_typeCombo);
    lay->addLayout(conditionForm);

    // ── Velocity (inlet / moving wall) ──
    m_velocitySection = new QWidget(detail);
    auto* velocityLay = new QVBoxLayout(m_velocitySection);
    velocityLay->setContentsMargins(0, 0, 0, 0);
    velocityLay->setSpacing(4);
    velocityLay->addWidget(sectionHeader("Velocity", m_velocitySection));

    m_u = makeCoefficientSpin(0.0, m_velocitySection);
    m_v = makeCoefficientSpin(0.0, m_velocitySection);
    m_w = makeCoefficientSpin(0.0, m_velocitySection);
    {
        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(4);
        const char* axis[3] = {"U_x", "U_y", "U_z"};
        QDoubleSpinBox* spins[3] = {m_u, m_v, m_w};
        for (int c = 0; c < 3; ++c) {
            grid->addWidget(fieldLabel(QString::fromUtf8(axis[c]), m_velocitySection), 0, c * 2);
            grid->addWidget(spins[c], 0, c * 2 + 1);
            grid->setColumnStretch(c * 2 + 1, 1);
        }
         velocityLay->addLayout(grid);
     }
     // Turbulence inlet values (velocity inlet only)
     velocityLay->addWidget(fieldLabel("Turbulence inlet", m_velocitySection));
     m_kInlet = makeCoefficientSpin(1e-3, m_velocitySection);
     m_omegaInlet = makeCoefficientSpin(1.0, m_velocitySection);
     {
         auto* grid = new QGridLayout;
         grid->setContentsMargins(0, 0, 0, 0);
         grid->setHorizontalSpacing(8);
         grid->setVerticalSpacing(4);
         grid->addWidget(fieldLabel("k", m_velocitySection), 0, 0);
         grid->addWidget(m_kInlet, 0, 1);
         grid->addWidget(fieldLabel("omega", m_velocitySection), 0, 2);
         grid->addWidget(m_omegaInlet, 0, 3);
         grid->setColumnStretch(1, 1);
         grid->setColumnStretch(3, 1);
         velocityLay->addLayout(grid);
     }
     lay->addWidget(m_velocitySection);

    // ── Pressure & backflow (pressure outlet) ──
    m_pressureSection = new QWidget(detail);
    auto* pressureLay = new QVBoxLayout(m_pressureSection);
    pressureLay->setContentsMargins(0, 0, 0, 0);
    pressureLay->setSpacing(4);
    pressureLay->addWidget(sectionHeader("Pressure & backflow", m_pressureSection));

    m_pressure = makeCoefficientSpin(0.0, m_pressureSection);
    m_suppressBackflow = new QCheckBox("Suppress backflow", m_pressureSection);
    {
        auto* form = new QFormLayout;
        form->setContentsMargins(0, 0, 0, 0);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        form->addRow(fieldLabel("Static pressure", m_pressureSection), m_pressure);
        form->addRow(m_suppressBackflow);
        pressureLay->addLayout(form);
    }

    m_backflowBox = new QWidget(m_pressureSection);
    auto* backflowLay = new QVBoxLayout(m_backflowBox);
    backflowLay->setContentsMargins(0, 2, 0, 0);
    backflowLay->setSpacing(4);
    backflowLay->addWidget(fieldLabel("Backflow velocity", m_backflowBox));
    m_backflowU = makeCoefficientSpin(0.0, m_backflowBox);
    m_backflowV = makeCoefficientSpin(0.0, m_backflowBox);
    m_backflowW = makeCoefficientSpin(0.0, m_backflowBox);
    {
        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(4);
        const char* axis[3] = {"U_x", "U_y", "U_z"};
        QDoubleSpinBox* spins[3] = {m_backflowU, m_backflowV, m_backflowW};
        for (int c = 0; c < 3; ++c) {
            grid->addWidget(fieldLabel(QString::fromUtf8(axis[c]), m_backflowBox), 0, c * 2);
            grid->addWidget(spins[c], 0, c * 2 + 1);
            grid->setColumnStretch(c * 2 + 1, 1);
        }
        backflowLay->addLayout(grid);
    }
    backflowLay->addWidget(fieldLabel("Backflow turbulence", m_backflowBox));
    m_backflowK = makeCoefficientSpin(0.0, m_backflowBox);
    m_backflowOmega = makeCoefficientSpin(0.0, m_backflowBox);
    {
        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(4);
        grid->addWidget(fieldLabel("Turbulent k", m_backflowBox), 0, 0);
        grid->addWidget(m_backflowK, 0, 1);
        grid->addWidget(fieldLabel("Omega", m_backflowBox), 0, 2);
        grid->addWidget(m_backflowOmega, 0, 3);
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, 1);
        backflowLay->addLayout(grid);
    }
    pressureLay->addWidget(m_backflowBox);
    lay->addWidget(m_pressureSection);

    // ── Explanation for conditions with no parameters ──
    m_hint = new QLabel(detail);
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet("color: palette(mid);");
    lay->addWidget(m_hint);

    lay->addStretch(1);
}

void CfdBoundaryConditionsDialog::selectRow(int row) {
    if (row < 0 || row >= int(m_bcs.size())) return;
    m_current = row;
    loadCurrent();
}

void CfdBoundaryConditionsDialog::loadCurrent() {
    if (m_current < 0 || m_current >= int(m_bcs.size())) return;

    m_syncing = true;
    const cfd::PatchBC& bc = m_bcs[m_current];
    const std::size_t faces = m_current < int(m_faceCounts.size()) ? m_faceCounts[m_current] : 0;

    m_patchTitle->setText(QString::fromStdString(bc.patchName));
    m_patchMeta->setText(QString("%1 faces").arg(faces));
    m_typeCombo->setCurrentIndex(indexOfType(bc.type));
    m_u->setValue(bc.u);
    m_v->setValue(bc.v);
    m_w->setValue(bc.w);
    m_pressure->setValue(bc.pressure);
    m_suppressBackflow->setChecked(bc.suppressBackflow);
    m_backflowU->setValue(bc.backflowU);
    m_backflowV->setValue(bc.backflowV);
    m_backflowW->setValue(bc.backflowW);
    m_backflowK->setValue(bc.backflowK);
    m_backflowOmega->setValue(bc.backflowOmega);
    m_kInlet->setValue(bc.kInlet);
    m_omegaInlet->setValue(bc.omegaInlet);
    m_syncing = false;

    updateConditionVisibility();
}

void CfdBoundaryConditionsDialog::commitCurrent() {
    if (m_current < 0 || m_current >= int(m_bcs.size())) return;

    cfd::PatchBC& bc = m_bcs[m_current];
    bc.type = typeAt(m_typeCombo->currentIndex());
    bc.u = m_u->value();
    bc.v = m_v->value();
    bc.w = m_w->value();
    bc.pressure = m_pressure->value();
    bc.suppressBackflow = m_suppressBackflow->isChecked();
    bc.backflowU = m_backflowU->value();
    bc.backflowV = m_backflowV->value();
    bc.backflowW = m_backflowW->value();
    bc.backflowK = m_backflowK->value();
    bc.backflowOmega = m_backflowOmega->value();
    bc.kInlet = m_kInlet->value();
    bc.omegaInlet = m_omegaInlet->value();
}

void CfdBoundaryConditionsDialog::refreshConditionCell(int row) {
    if (row < 0 || row >= int(m_bcs.size())) return;
    if (QTableWidgetItem* item = m_table->item(row, 1))
        item->setText(labelForType(m_bcs[row].type));
}

void CfdBoundaryConditionsDialog::updateConditionVisibility() {
    const cfd::BCType type = typeAt(m_typeCombo->currentIndex());
    const bool hasVelocity =
        (type == cfd::BCType::VelocityInlet || type == cfd::BCType::MovingWall);
    const bool hasPressure = (type == cfd::BCType::PressureOutlet);

    m_velocitySection->setVisible(hasVelocity);
    m_pressureSection->setVisible(hasPressure);
    if (m_backflowBox) m_backflowBox->setVisible(hasPressure && m_suppressBackflow->isChecked());

    const QString hint = QString::fromUtf8(hintForType(type));
    m_hint->setText(hint);
    m_hint->setVisible(!hint.isEmpty());
}

void CfdBoundaryConditionsDialog::applyToAll(cfd::BCType type) {
    for (cfd::PatchBC& bc : m_bcs) bc.type = type;
    for (int row = 0; row < int(m_bcs.size()); ++row) refreshConditionCell(row);
    loadCurrent(); // reflect the new condition for the selected patch
}
