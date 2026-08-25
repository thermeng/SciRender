#include "render/passes/ParticlePass.h"
#include "render/foundation/renderer.h"
#include "render/foundation/shader_utils.h"
#include "render/streamlines/StreamlineSet.h"
#include "render/passes/ColormapManager.h"
#include <glm/gtc/type_ptr.hpp>

void ParticlePass::init(const ShaderSources& sources) {
    if (sources.particleVert.empty() || sources.particleFrag.empty()) return;

    particleProgram.reset(compileProgram(sources.particleVert.c_str(), sources.particleFrag.c_str(), "Particle"));
    if (particleProgram.has()) {
        particleColorLoc = glGetUniformLocation(particleProgram, "uColor");
        particleMvpLoc = glGetUniformLocation(particleProgram, "uMVP");
        particleSizeRefWLoc = glGetUniformLocation(particleProgram, "uSizeRefW");
        particleLutLoc = glGetUniformLocation(particleProgram, "uColormapLUT");
        particlePointSizeLoc = glGetUniformLocation(particleProgram, "uPointSize");
        particleUseColormapLoc = glGetUniformLocation(particleProgram, "uUseColormap");
        particleMagRangeLoc = glGetUniformLocation(particleProgram, "uParticleMagRange");
    }
}

void ParticlePass::draw(const RenderRenderState& state,
                        float frameDt,
                        const glm::mat4& mvp,
                        StreamlineSet& streamlines,
                        const ColormapManager& colormap) {
    if (!state.showParticles || !state.showStreamlines || streamlines.empty() || !particleProgram.has()) return;
    // [P3] Clamp dt so a window stall/resize cannot make particles leap.
    const float dt = (frameDt < 0.1f) ? frameDt : 0.1f;
    streamlines.updateParticles(dt, state.particleSpeed);

    std::vector<float> particleVerts;
    streamlines.buildParticleVertices(particleVerts);

    if (particleVerts.empty()) return;

    if (!particleVao.has()) {
        setupVertexBuffer(particleVao, particleVbo, nullptr, 0, 5 * sizeof(float),
                          { { 0, 3, 0 }, { 1, 1, 3 * sizeof(float) }, { 2, 1, 4 * sizeof(float) } }, GL_DYNAMIC_DRAW);
    }

    m_particleVertexCount = static_cast<int>(particleVerts.size() / 5);
    glNamedBufferData(particleVbo, particleVerts.size() * sizeof(float), particleVerts.data(), GL_DYNAMIC_DRAW);

    glUseProgram(particleProgram);
    // Save engine state we mutate; restored automatically on scope exit.
    GLStateGuard guard;
    glEnable(GL_BLEND);
    // [Glow toggle] Luminous additive compositing vs standard alpha blending.
    if (state.particleAdditive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    else                        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    // [P1] Own MVP uniform — no dependency on the streamline UBO binding.
    glUniformMatrix4fv(particleMvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
    // [V2] Reference depth for perspective point-size attenuation.
    const float sizeRefW = state.orthographic
        ? 1.0f
        : glm::distance(glm::vec3(state.camera.position), glm::vec3(state.camera.focalPoint));
    glUniform1f(particleSizeRefWLoc, sizeRefW);
    glUniform1f(particlePointSizeLoc, state.particleSize);

    if (state.streamlineUseColormap && colormap.streamlineTexture() != 0) {
        glBindTextureUnit(1, colormap.streamlineTexture());
        glUniform1i(particleLutLoc, 1);
        glUniform1i(particleUseColormapLoc, 1);
    } else {
        glm::vec4 pc(state.streamlineColor[0], state.streamlineColor[1], state.streamlineColor[2], 1.0f);
        glUniform4fv(particleColorLoc, 1, glm::value_ptr(pc));
        glUniform1i(particleUseColormapLoc, 0);
    }
    glUniform2f(particleMagRangeLoc, streamlines.magMin, streamlines.magMax);

    glBindVertexArray(particleVao);
    glDrawArrays(GL_POINTS, 0, m_particleVertexCount);
    glBindVertexArray(0);

    glUseProgram(0);
}

void ParticlePass::shutdown() {
    particleProgram.reset();
    particleVao.reset();
    particleVbo.reset();
    m_particleVertexCount = 0;
    particleColorLoc = -1;
    particleLutLoc = -1;
    particlePointSizeLoc = -1;
    particleUseColormapLoc = -1;
    particleMagRangeLoc = -1;
}


