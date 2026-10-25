#include "render/settings/derived_volume_controller.h"

#include <QtConcurrent/QtConcurrentRun>

DerivedVolumeController::DerivedVolumeController(Renderer& renderer, QObject* parent)
    : QObject(parent)
    , m_renderer(renderer) {
    connect(&m_watcher, &QFutureWatcher<BuildResult>::finished,
            this, &DerivedVolumeController::onBuilt);
}

std::uint64_t DerivedVolumeController::topologySignature(const RenderMesh& mesh) {
    // Everything the grid depends on: geometry, cell connectivity/types and the
    // AABB. Scalar contents are deliberately excluded.
    std::uint64_t h = 1469598103934665603ull;
    h = mesh_utils::hashBytes(mesh.vertices.data(), mesh.vertices.size() * sizeof(float), h);
    h = mesh_utils::hashBytes(mesh.cellConnectivity.data(),
                              mesh.cellConnectivity.size() * sizeof(std::uint32_t), h);
    h = mesh_utils::hashBytes(mesh.cellTypes.data(),
                              mesh.cellTypes.size() * sizeof(std::uint8_t), h);
    if (!mesh.cellOffsets.empty()) {
        h = mesh_utils::hashBytes(mesh.cellOffsets.data(),
                                  mesh.cellOffsets.size() * sizeof(std::uint32_t), h);
    }
    h = mesh_utils::hashBytes(&mesh.bounds, sizeof(mesh.bounds), h);
    return h ? h : 1;
}

void DerivedVolumeController::setCurrentMesh(std::shared_ptr<const RenderMesh> mesh) {
    if (!mesh || !volume::eligible(*mesh)) {
        // A 2D/surface mesh has nothing to voxelize; drop any previous grid.
        m_mesh = std::move(mesh);
        m_signature = 0;
        ++m_token;
        m_renderer.setPendingDerivedVolume(nullptr);
        return;
    }

    const std::uint64_t sig = topologySignature(*mesh);
    m_mesh = std::move(mesh);
    // Same topology (a re-solved frame, or a scalar-only refresh): the built or
    // in-flight grid is still valid.
    if (sig == m_signature) return;

    m_signature = sig;
    launch();
}

void DerivedVolumeController::launch() {
    const uint64_t token = ++m_token;
    const auto meshPtr = m_mesh;
    m_watcher.setFuture(QtConcurrent::run([meshPtr, token]() -> BuildResult {
        BuildResult result;
        result.token = token;
        if (!meshPtr) return result;
        auto grid = std::make_shared<volume::DerivedGrid>();
        volume::BuildOptions options;
        if (volume::build(*meshPtr, options, *grid) && !grid->empty()) {
            result.grid = std::move(grid);
        }
        return result;
    }));
}

void DerivedVolumeController::onBuilt() {
    BuildResult result = m_watcher.result();
    if (result.token != m_token) return; // superseded by a newer mesh / clear
    m_renderer.setPendingDerivedVolume(std::move(result.grid));
    emit built();
}

void DerivedVolumeController::clear() {
    ++m_token;
    m_watcher.waitForFinished();
    m_watcher.setFuture(QFuture<BuildResult>());
    m_mesh.reset();
    m_signature = 0;
    m_renderer.setPendingDerivedVolume(nullptr);
}
