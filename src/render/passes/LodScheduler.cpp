#include "render/passes/LodScheduler.h"
#include "render/foundation/renderer.h"
#include "render/foundation/render_config.h"
#include "render/passes/MeshGLManager.h"

bool LodScheduler::tick(const RenderRenderState& state, MeshGLManager& meshManager) {
    return tick(state, meshManager, std::chrono::steady_clock::now());
}

bool LodScheduler::tick(const RenderRenderState& state, MeshGLManager& meshManager,
                        std::chrono::steady_clock::time_point now) {
    // Debounce: once the debounce period elapses with no new motion, clear
    // the moving flag so the next frame uses the full-resolution mesh.
    bool settled = false;
    if (cameraMoving.load()) {
        if (m_lastMotion.time_since_epoch().count() == 0) m_lastMotion = now;
        double dt = std::chrono::duration<double>(now - m_lastMotion).count();
        if (dt >= RenderConfig::defaults().lodDebounceSeconds) {
            cameraMoving = false;
            // Rendering is on-demand: without one more frame the last
            // (decimated) frame would stay on screen indefinitely.
            settled = true;
        }
    }

    // Throttle: only re-dispatch once per camera-motion burst.
    if (cameraMoving.load() && !m_wasCameraMoving) {
        gpuDecimationDirty = true;
    }
    m_wasCameraMoving = cameraMoving.load();

    const bool useLod = state.useLod;
    bool dispatched = false;
    if (useLod && gpuDecimationDirty.load() && meshManager.hasDecimated() && meshManager.hasFullSource()) {
        Mesh newDec;
        if (meshManager.dispatchLodCompute(*meshManager.getFullSource(), newDec)) {
            meshManager.replaceDecimatedMesh(0, std::move(newDec));
        } else {
            QString err = QString::fromStdString(meshManager.lastLodError());
            if (err.isEmpty())
                qWarning() << "[LOD] GPU compute decimation failed, using CPU fallback";
            else
                qWarning().noquote() << "[LOD] GPU compute decimation failed:\n" + err.trimmed();
        }
        gpuDecimationDirty = false;
        dispatched = true;
    }
    return dispatched || settled;
}


