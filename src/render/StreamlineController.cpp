#include "render/StreamlineController.h"
#include "render/shader_utils.h"
#include "render/renderer.h"
#include "render/ColormapManager.h"
#include "render/StreamlineSet.h"
#include "render/render_config.h"
#include <glm/gtc/type_ptr.hpp>

void StreamlineController::init(const ShaderSources& sources) {
    if (!sources.streamlineVert.empty() && !sources.streamlineFrag.empty()) {
        m_streamlineProgram.reset(compileProgram(sources.streamlineVert.c_str(), sources.streamlineFrag.c_str(), "Streamline"));
        if (m_streamlineProgram.has()) {
            m_streamlineLutLoc = glGetUniformLocation(m_streamlineProgram, "uColormapLUT");
            if (!m_streamlineUbo.has()) {
                glCreateBuffers(1, m_streamlineUbo.ptr());
                glNamedBufferData(m_streamlineUbo, sizeof(StreamlineUBOData), nullptr, GL_DYNAMIC_DRAW);
            }
        }
    }

    if (!sources.seedVert.empty() && !sources.seedFrag.empty()) {
        m_seedProgram.reset(compileProgram(sources.seedVert.c_str(), sources.seedFrag.c_str(), "Seed"));
        if (m_seedProgram.has()) {
            m_seedMvpLoc = glGetUniformLocation(m_seedProgram, "uMVP");
            m_seedModelLoc = glGetUniformLocation(m_seedProgram, "uModel");
            m_seedColorLoc = glGetUniformLocation(m_seedProgram, "uColor");
            m_seedPointSizeLoc = glGetUniformLocation(m_seedProgram, "uPointSize");
            m_seedLightDirLoc = glGetUniformLocation(m_seedProgram, "uLightDir");
        }
    }
}

void StreamlineController::dispatchCompute(const RenderRenderState& state,
                                            std::shared_ptr<const RenderMesh> mesh,
                                            StreamlineSet& streamlineSet) {
    if (!state.showStreamlines) return;
    auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_streamlineRequestTime).count();
    if (dt >= kStreamlineDebounceSec && m_streamlineRequestTime.time_since_epoch().count() > 0
        && mesh
        && !m_computeRunning.load()) {
        m_streamlineRequestTime = {}; // reset

        m_cancelFlag = false;
        if (m_worker.joinable()) m_worker.join();

        auto meshCopy = mesh;
        int   seedCount     = state.streamlineSeedCount;
        float stepSize      = state.streamlineStepSize;
        int   maxSteps      = state.streamlineMaxSteps;
        std::string field   = state.streamlineVectorField;
        std::string mode    = state.seedMode;
        std::string direction = state.streamlineDirection;
        double planePos     = state.seedPlanePos;
        double jitter       = state.seedJitter;
        int   planeCountU   = state.seedPlaneCountU;
        int   planeCountV   = state.seedPlaneCountV;
        bool  showArrows    = state.showStreamlineArrows;
        int   arrowSpacing  = state.streamlineArrowSpacing;
        float arrowSize     = state.streamlineArrowSize;
        float ribbonWidth   = state.streamlineRibbonWidth;
        float taperFactor   = state.streamlineTaperFactor;

        m_computeRunning = true;
        m_worker = std::thread(
            [this, &streamlineSet, meshCopy, seedCount, stepSize, maxSteps, field, mode, direction,
             planePos, jitter, planeCountU, planeCountV, showArrows, arrowSpacing,
             arrowSize, ribbonWidth, taperFactor]() {
                auto result = streamlineSet.compute(
                    *meshCopy, seedCount, stepSize, maxSteps, field, mode, direction,
                    planePos, jitter, planeCountU, planeCountV,
                    showArrows, arrowSpacing, arrowSize, ribbonWidth, taperFactor);

                if (m_cancelFlag.load()) {
                    m_computeRunning = false;
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(m_resultMutex);
                    m_pendingResult = std::make_unique<StreamlineSet::StreamlineResult>(std::move(result));
                }
                m_computeRunning = false;
            });
    }
}

void StreamlineController::consumeResult(const RenderRenderState& state, StreamlineSet& streamlineSet) {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    if (m_pendingResult) {
        streamlineSet.uploadGL(std::move(*m_pendingResult), state.showStreamlineArrows, state.streamlineArrowSize);
        streamlineSet.initParticles(state.particleCount);
        m_pendingResult.reset();
    }
}

void StreamlineController::draw(const RenderRenderState& state, StreamlineSet& streamlineSet,
                                 const ColormapManager& colormap, const glm::mat4& mvp,
                                 double animationTime, const glm::vec3& lightDir) {
    // Streamlines
    if ((state.showStreamlines && !streamlineSet.empty()) ||
        (state.showStreamlineArrows && streamlineSet.arrowVao.has() && streamlineSet.arrowCount > 0 && state.showStreamlines)) {
        if (m_streamlineProgram.has()) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glUseProgram(m_streamlineProgram);
            GLboolean blendWas = glIsEnabled(GL_BLEND);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            if (!m_streamlineUbo.has()) {
                glCreateBuffers(1, m_streamlineUbo.ptr());
                glNamedBufferData(m_streamlineUbo, sizeof(StreamlineUBOData), nullptr, GL_DYNAMIC_DRAW);
            }
            glm::vec3 camPos = glm::vec3(state.camera.position);
            StreamlineUBOData ubo{};
            ubo.mvp = mvp;
            ubo.model = glm::mat4(1.0f);
            ubo.viewPos = glm::vec4(camPos, 0.0f);
            ubo.lightDir = glm::vec4(lightDir, 0.0f);
            ubo.time_opacity = glm::vec4(static_cast<float>(animationTime), state.streamlineOpacity, 0.0f, 0.0f);
            ubo.color_useColormap = glm::vec4(state.streamlineColor[0], state.streamlineColor[1], state.streamlineColor[2], state.streamlineUseColormap ? 1.0f : 0.0f);
            ubo.magRange = glm::vec4(streamlineSet.magMin, streamlineSet.magMax, 0.0f, 0.0f);
            ubo.material = glm::vec4(state.streamlineAmbient, state.streamlineDiffuse, state.streamlineSpecular, static_cast<float>(state.streamlineSpecularPower));
            ubo.ribbon = glm::vec4(state.streamlineRibbonWidth, state.streamlineTaperFactor, 0.0f, 0.0f);
            ubo.arrowParams = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
            ubo.pbr = glm::vec4(state.lighting.matRoughness, state.lighting.matMetallic, 0.0f, 0.0f);
            glBindBufferBase(GL_UNIFORM_BUFFER, 3, m_streamlineUbo);
            glNamedBufferSubData(m_streamlineUbo, 0, sizeof(StreamlineUBOData), &ubo);
            if (state.streamlineUseColormap && colormap.streamlineTexture() != 0) {
                glBindTextureUnit(1, colormap.streamlineTexture());
                glUniform1i(m_streamlineLutLoc, 1);
            }
            if (state.showStreamlines && !streamlineSet.empty()) {
                glBindVertexArray(streamlineSet.vao);
                glDrawArrays(GL_TRIANGLES, 0, streamlineSet.lineCount);
                glBindVertexArray(0);
            }
            if (state.showStreamlineArrows && streamlineSet.arrowVao.has() && streamlineSet.arrowCount > 0) {
                glBindVertexArray(streamlineSet.arrowVao);
                glDrawArrays(GL_TRIANGLES, 0, streamlineSet.arrowCount);
                glBindVertexArray(0);
            }
            if (blendWas) glEnable(GL_BLEND); else glDisable(GL_BLEND);
            glUseProgram(0);
        }
    }

    // Seed points
    if (state.showStreamlines && state.showSeeds && !streamlineSet.seedsEmpty() && m_seedProgram.has()) {
        glUseProgram(m_seedProgram);
        GLboolean pointSizeWas = glIsEnabled(GL_PROGRAM_POINT_SIZE);
        GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glEnable(GL_DEPTH_TEST);
        glm::vec4 seedColor(state.seedPointColor[0], state.seedPointColor[1], state.seedPointColor[2], 1.0f);
        glm::mat4 seedModel(1.0f);
        glUniformMatrix4fv(m_seedMvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
        glUniformMatrix4fv(m_seedModelLoc, 1, GL_FALSE, glm::value_ptr(seedModel));
        glUniform4fv(m_seedColorLoc, 1, glm::value_ptr(seedColor));
        glUniform1f(m_seedPointSizeLoc, state.seedPointSize);
        glUniform4fv(m_seedLightDirLoc, 1, glm::value_ptr(glm::vec4(lightDir, 0.0f)));
        glBindVertexArray(streamlineSet.seedVao);
        glDrawArrays(GL_POINTS, 0, streamlineSet.seedCount);
        glBindVertexArray(0);
        glUseProgram(0);
        if (pointSizeWas) glEnable(GL_PROGRAM_POINT_SIZE); else glDisable(GL_PROGRAM_POINT_SIZE);
        if (depthWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    }
}

void StreamlineController::cancelAndJoin() {
    m_cancelFlag = true;
    if (m_worker.joinable()) m_worker.join();
}

void StreamlineController::shutdown() {
    cancelAndJoin();
    m_streamlineUbo.reset();
    m_streamlineProgram.reset();
    m_seedProgram.reset();
}
