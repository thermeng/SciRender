#include "render/LodScheduler.h"
#include "render/renderer.h"
#include "render/render_config.h"
#include "render/MeshGLManager.h"

bool LodScheduler::tick(const RenderRenderState& state, MeshGLManager& meshManager) {
    // Debounce: once the debounce period elapses with no new motion, clear
    // the moving flag so the next frame uses the full-resolution mesh.
    if (cameraMoving.load()) {
        auto now = std::chrono::steady_clock::now();
        if (m_lastMotion.time_since_epoch().count() == 0) m_lastMotion = now;
        double dt = std::chrono::duration<double>(now - m_lastMotion).count();
        if (dt >= RenderConfig::defaults().lodDebounceSeconds) {
            cameraMoving = false;
        }
    }

    // Throttle: only re-dispatch once per camera-motion burst.
    if (cameraMoving.load() && !m_wasCameraMoving) {
        gpuDecimationDirty = true;
    }
    m_wasCameraMoving = cameraMoving.load();

    const bool useLod = state.useLod;
    if (useLod && gpuDecimationDirty.load() && meshManager.hasDecimated() && meshManager.hasFullSource()) {
        Mesh newDec;
        if (meshManager.dispatchLodCompute(*meshManager.getFullSource(), newDec)) {
            meshManager.replaceDecimatedMesh(0, newDec);
        } else {
            QString err = QString::fromStdString(meshManager.lastLodError());
            if (err.isEmpty())
                qWarning() << "[LOD] GPU compute decimation failed, using CPU fallback";
            else
                qWarning().noquote() << "[LOD] GPU compute decimation failed:\n" + err.trimmed();
        }
        gpuDecimationDirty = false;
        return true;
    }
    return false;
}
