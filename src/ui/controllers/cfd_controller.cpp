#include "ui/controllers/cfd_controller.h"

#include "cfd/amg_settings_bridge.h"
#include "cfd/cfd_mesh_adapter.h"
#include "cfd/cfd_result_builder.h"
#include "cfd/cfd_types.h"

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/SIMPLE.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/SSTModel.h"
#include "io/VTKXMLWriter.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>

#include <memory>
#include <mutex>

using namespace cfd;

CfdController::CfdController(QObject* parent) : QObject(parent) {}

CfdController::~CfdController() = default;

bool CfdController::loadMesh(const QString& path) {
    m_mesh = cfd::loadMesh(path.toStdString());
    if (!m_mesh.error.empty()) {
        emit failed(QString::fromStdString(m_mesh.error));
        return false;
    }
    m_meshPtr = m_mesh.mesh2;
    emit meshLoaded();
    return true;
}

void CfdController::run() {
    if (!m_meshPtr) { emit failed("No mesh loaded"); return; }
    m_cancel.store(false);
    uint64_t gen = ++m_gen;

    auto meshPtr = m_meshPtr;
    auto settings = m_settings;

    m_velocity = std::make_shared<fvm::VectorField<2>>(*meshPtr, 0.0);
    m_pressure = std::make_shared<fvm::Field<2>>(*meshPtr, 0.0);

    fvm::SIMPLE<2>::Config sc;
    sc.maxIterations = settings.maxIterations;
    sc.tolerance = settings.tolerance;
    sc.alphaU = settings.alphaU;
    sc.alphaP = settings.alphaP;
    sc.simplec = settings.simplec;
    sc.rhieChow = settings.rhieChow;
    sc.convectionScheme = settings.scheme == 1 ? fvm::ConvectionScheme::Central
                          : (settings.scheme == 2 ? fvm::ConvectionScheme::SecondOrderUpwind : fvm::ConvectionScheme::Upwind);
    sc.printFrequency = settings.printFrequency;
    sc.residualHistoryWindow = settings.residualHistoryWindow;
    sc.pressureCorrectors = settings.pressureCorrectors;
    sc.limitPressureCorrection = settings.limitPressureCorrection;
    sc.pressureCorrectionLimit = settings.pressureCorrectionLimit;
    sc.pressureRefCell = settings.pressureRefCell == 0 ? SIZE_MAX : settings.pressureRefCell;
    sc.pressureUseAMG = settings.pressureUseAMG;
    sc.pressureUseAMGPreconditioner = settings.pressureUseAMGPreconditioner;
    sc.pressureAmgThreshold = settings.pressureAmgThreshold;
    sc.pressureAmgConfig = toFvmAmg(settings.pressureAmg);
    sc.useAMGMomentum = settings.useAMGMomentum;
    sc.momentumAmgThreshold = settings.momentumAmgThreshold;
    sc.momentumAmgCycles = settings.momentumAmgCycles;
    sc.momentumAmgMaxResidual = settings.momentumAmgMaxResidual;
    sc.momentumSolverStats = settings.momentumSolverStats;
    sc.momentumAmgConfig = toFvmAmg(settings.momentumAmg);
    sc.iterationCallback = [this, gen](int iter, double cont, double mom) {
        if (gen != m_gen) return;
        if (m_cancel.load()) return;
        emit progress(iter, cont, mom);
    };

    auto solver = std::make_shared<fvm::SIMPLE<2>>(*meshPtr, settings.rho, settings.nu, sc);
    if (settings.turbulence) solver->enableTurbulence(settings.kInf, settings.omegaInf, settings.alphaK, settings.alphaOmega);
    solver->bindVelocity(*m_velocity);
    solver->bindPressure(*m_pressure);

    // Apply per-patch BCs
    for (const auto& pbc : m_patchBCs) {
        auto it = meshPtr->boundaryGroups().find(pbc.patchName);
        if (it == meshPtr->boundaryGroups().end()) continue;
        const std::vector<std::size_t>& faces = it->second;
        std::string typeStr;
        switch (pbc.type) {
            case cfd::BCType::VelocityInlet: typeStr = "inlet"; break;
            case cfd::BCType::PressureOutlet: typeStr = "pressure_outlet"; break;
            case cfd::BCType::Outlet: typeStr = "outlet"; break;
            case cfd::BCType::WallNoSlip: typeStr = "wall"; break;
            case cfd::BCType::SlipWall: typeStr = "slip_wall"; break;
            case cfd::BCType::Symmetry: typeStr = "symmetry"; break;
            case cfd::BCType::MovingWall: typeStr = "moving_wall"; break;
        }
        if (pbc.type == cfd::BCType::PressureOutlet) {
            solver->setPressureOutletOnFacesWithBackflow(faces, pbc.pressure, pbc.suppressBackflow,
                pbc.backflowU, pbc.backflowV, pbc.backflowW, pbc.backflowK, pbc.backflowOmega);
        } else {
            bool slip = (pbc.type == cfd::BCType::SlipWall || pbc.type == cfd::BCType::Symmetry);
            solver->setVelocityBCOnFacesWithSlip(faces, typeStr, pbc.u, pbc.v, pbc.w, slip, pbc.pressure);
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        m_solver = solver;
    }

    QFutureWatcher<void>* w = new QFutureWatcher<void>(this);
    connect(w, &QFutureWatcher<void>::finished, this, [this, w, gen]() {
        w->deleteLater();
        if (gen != m_gen) return;
        if (m_cancel.load()) { emit finished("Cancelled"); return; }
        emit finished("Solve complete");
    });

    auto fut = QtConcurrent::run([this, gen, solver]() {
        if (gen != m_gen) return;
        solver->solve(*m_velocity, *m_pressure);
    });
    w->setFuture(fut);
}

std::shared_ptr<const RenderMesh> CfdController::initialize() {
    if (!m_meshPtr || !m_mesh.renderMesh) return nullptr;
    // Invalidate any in-flight solve generation so its result cannot be applied
    // after this reset, then start from a uniform zero field.
    ++m_gen;
    m_velocity = std::make_shared<fvm::VectorField<2>>(*m_meshPtr, 0.0);
    m_pressure = std::make_shared<fvm::Field<2>>(*m_meshPtr, 0.0);
    return resultMesh();
}

void CfdController::cancel() {
    m_cancel.store(true);
    std::shared_ptr<fvm::SIMPLE<2>> solver;
    {
        std::lock_guard<std::mutex> lk(m_solverMutex);
        solver = m_solver;
    }
    if (solver) solver->requestCancel();
}

void CfdController::exportVtu(const QString& path) {
    if (!m_meshPtr || !m_velocity || !m_pressure) return;
    std::array<const fvm::Field<2>*, 2> vel{&m_velocity->operator[](0), &m_velocity->operator[](1)};
    const fvm::SSTModel<2>* turb = m_solver ? m_solver->turbulence() : nullptr;
    const fvm::Field<2>* k = turb ? &turb->k() : nullptr;
    const fvm::Field<2>* om = turb ? &turb->omega() : nullptr;
    const fvm::Field<2>* nt = turb ? &turb->nut() : nullptr;
    fvm::VTKXMLWriter<2>::writeWithVelocity(path.toStdString(), *m_meshPtr, *m_pressure, vel, "pressure", k, om, nt);
}

std::shared_ptr<const RenderMesh> CfdController::resultMesh() const {
    if (!m_meshPtr || !m_velocity || !m_pressure || !m_mesh.renderMesh) return nullptr;
    const fvm::SSTModel<2>* turb = m_solver ? m_solver->turbulence() : nullptr;
    return buildResultMesh(m_mesh.renderMesh, *m_meshPtr, *m_velocity, *m_pressure, turb, m_settings);
}
