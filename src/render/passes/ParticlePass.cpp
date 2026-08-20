#include "render/passes/ParticlePass.h"
#include "render/foundation/renderer.h"
#include "render/foundation/shader_utils.h"
#include "render/streamlines/StreamlineSet.h"
#include "render/passes/ColormapManager.h"

void ParticlePass::init(const ShaderSources& sources) {
    if (sources.particleVert.empty() || sources.particleFrag.empty()) return;

    particleProgram.reset(compileProgram(sources.particleVert.c_str(), sources.particleFrag.c_str(), "Particle"));
    if (particleProgram.has()) {
        particleColorLoc = glGetUniformLocation(particleProgram, "uColor");
        particleLutLoc = glGetUniformLocation(particleProgram, "uColormapLUT");
        particlePointSizeLoc = glGetUniformLocation(particleProgram, "uPointSize");
        particleUseColormapLoc = glGetUniformLocation(particleProgram, "uUseColormap");
        particleMagRangeLoc = glGetUniformLocation(particleProgram, "uParticleMagRange");
    }
}

void ParticlePass::draw(const RenderRenderState& state,
                        float frameDt,
                        StreamlineSet& streamlines,
                        const ColormapManager& colormap) {
    if (!state.showParticles || !state.showStreamlines || streamlines.empty() || !particleProgram.has()) return;
    streamlines.updateParticles(frameDt, state.particleSpeed);

    std::vector<float> particleVerts;
    streamlines.buildParticleVertices(particleVerts);

    if (particleVerts.empty()) return;

    if (!particleVao.has()) {
        setupVertexBuffer(particleVao, particleVbo, nullptr, 0, 4 * sizeof(float),
                          { { 0, 3, 0 }, { 1, 1, 3 * sizeof(float) } }, GL_DYNAMIC_DRAW);
    }

    m_particleVertexCount = static_cast<int>(particleVerts.size() / 4);
    glNamedBufferData(particleVbo, particleVerts.size() * sizeof(float), particleVerts.data(), GL_DYNAMIC_DRAW);

    glUseProgram(particleProgram);
    // Save engine state we mutate; restored automatically on scope exit.
    GLStateGuard guard;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

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


