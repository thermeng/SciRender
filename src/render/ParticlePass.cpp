#include "render/ParticlePass.h"
#include "render/renderer.h"
#include "render/shader_utils.h"
#include "render/StreamlineSet.h"
#include "render/ColormapManager.h"

void ParticlePass::init(const ShaderSources& sources) {
    if (sources.particleVert.empty() || sources.particleFrag.empty()) return;

    particleProgram = compileProgram(sources.particleVert.c_str(), sources.particleFrag.c_str(), "Particle");
    if (particleProgram != 0) {
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
    if (!state.showParticles || streamlines.empty() || particleProgram == 0) return;

    streamlines.updateParticles(frameDt, state.particleSpeed);

    std::vector<float> particleVerts;
    streamlines.buildParticleVertices(particleVerts);

    if (particleVerts.empty()) return;

    if (particleVao == 0) {
        glCreateVertexArrays(1, &particleVao);
        glCreateBuffers(1, &particleVbo);
        glEnableVertexArrayAttrib(particleVao, 0);
        glVertexArrayAttribFormat(particleVao, 0, 3, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(particleVao, 0, 0);
        glEnableVertexArrayAttrib(particleVao, 1);
        glVertexArrayAttribFormat(particleVao, 1, 1, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
        glVertexArrayAttribBinding(particleVao, 1, 0);
        glVertexArrayVertexBuffer(particleVao, 0, particleVbo, 0, 4 * sizeof(float));
    }

    m_particleVertexCount = static_cast<int>(particleVerts.size() / 4);
    glNamedBufferData(particleVbo, particleVerts.size() * sizeof(float), particleVerts.data(), GL_DYNAMIC_DRAW);

    glUseProgram(particleProgram);
    GLboolean blendWas = glIsEnabled(GL_BLEND);
    GLboolean pointSizeWas = glIsEnabled(GL_PROGRAM_POINT_SIZE);
    GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
    GLboolean depthMaskWas;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMaskWas);
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

    if (!blendWas) glDisable(GL_BLEND);
    if (!pointSizeWas) glDisable(GL_PROGRAM_POINT_SIZE);
    if (depthWas) glEnable(GL_DEPTH_TEST);
    glDepthMask(depthMaskWas);
    glUseProgram(0);
}

void ParticlePass::shutdown() {
    if (particleProgram) { glDeleteProgram(particleProgram); particleProgram = 0; }
    if (particleVao) { glDeleteVertexArrays(1, &particleVao); particleVao = 0; }
    if (particleVbo) { glDeleteBuffers(1, &particleVbo); particleVbo = 0; }
    m_particleVertexCount = 0;
    particleColorLoc = -1;
    particleLutLoc = -1;
    particlePointSizeLoc = -1;
    particleUseColormapLoc = -1;
    particleMagRangeLoc = -1;
}
