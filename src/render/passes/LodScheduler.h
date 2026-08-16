#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <atomic>
#include <chrono>

struct RenderRenderState;
class MeshGLManager;

class LodScheduler {
public:
    LodScheduler() = default;

    void setCameraMoving() {
        cameraMoving = true;
        m_lastMotion = std::chrono::steady_clock::now();
    }

    bool tick(const RenderRenderState& state, MeshGLManager& meshManager);
    bool isCameraMoving() const { return cameraMoving.load(); }
    void markDirty() { gpuDecimationDirty = true; }
    void reset() {
        cameraMoving = false;
        m_wasCameraMoving = false;
        gpuDecimationDirty = false;
        m_lastMotion = {};
    }

private:
    std::atomic<bool> cameraMoving{false};
    std::chrono::steady_clock::time_point m_lastMotion;
    bool m_wasCameraMoving = false;
    std::atomic<bool> gpuDecimationDirty{false};
};
