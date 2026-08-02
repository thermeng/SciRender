#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <glad/gl.h>
#include <glm/glm.hpp>
#include "render/StreamlineSet.h"

struct RenderRenderState;
struct ShaderSources;
class ColormapManager;

class StreamlineController {
public:
    void init(const ShaderSources& sources);

    void dispatchCompute(const RenderRenderState& state,
                         std::shared_ptr<const RenderMesh> mesh,
                         StreamlineSet& streamlineSet);

    void consumeResult(const RenderRenderState& state, StreamlineSet& streamlineSet);

    void draw(const RenderRenderState& state, StreamlineSet& streamlineSet,
              const ColormapManager& colormap, const glm::mat4& mvp,
              double animationTime, const glm::vec3& lightDir);

    void cancelAndJoin();
    void shutdown();

    void requestRecompute() { m_streamlineRequestTime = std::chrono::steady_clock::now(); }

    std::atomic<bool> streamlineDirty{false};
    std::atomic<bool> particleCountDirty{false};

private:
    GLuint m_streamlineProgram = 0;
    GLuint m_streamlineUbo = 0;
    GLint m_streamlineLutLoc = -1;

    GLuint m_seedProgram = 0;
    GLint m_seedMvpLoc = -1;
    GLint m_seedModelLoc = -1;
    GLint m_seedColorLoc = -1;
    GLint m_seedPointSizeLoc = -1;
    GLint m_seedLightDirLoc = -1;

    std::chrono::steady_clock::time_point m_streamlineRequestTime;
    static constexpr double kStreamlineDebounceSec = 0.15;

    std::mutex m_resultMutex;
    std::unique_ptr<StreamlineSet::StreamlineResult> m_pendingResult;
    std::thread m_worker;
    std::atomic<bool> m_computeRunning{false};
    std::atomic<bool> m_cancelFlag{false};
};
