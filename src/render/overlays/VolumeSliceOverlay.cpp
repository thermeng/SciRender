#include "render/overlays/VolumeSliceOverlay.h"
#include "render/foundation/shader_utils.h"
#include "render/foundation/renderer.h"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>

void VolumeSliceOverlay::init(const ShaderSources& sources) {
    if (sources.volumeSliceVert.empty() || sources.volumeSliceFrag.empty()) return;

    m_program.reset(compileProgram(sources.volumeSliceVert.c_str(), sources.volumeSliceFrag.c_str(), "VolumeSlice"));
    if (m_program.has()) {
        m_mvpLoc         = glGetUniformLocation(m_program, "uMVP");
        m_volumeTexLoc   = glGetUniformLocation(m_program, "uVolumeTex");
        m_boxMinLoc      = glGetUniformLocation(m_program, "uBoxMin");
        m_boxMaxLoc      = glGetUniformLocation(m_program, "uBoxMax");
        m_lutLoc         = glGetUniformLocation(m_program, "uColormapLUT");
        m_useColormapLoc = glGetUniformLocation(m_program, "uUseColormap");
        m_scalarMinLoc   = glGetUniformLocation(m_program, "uScalarMin");
        m_scalarMaxLoc   = glGetUniformLocation(m_program, "uScalarMax");
        m_alphaLoc       = glGetUniformLocation(m_program, "uAlpha");
        m_numBandsLoc    = glGetUniformLocation(m_program, "uNumBands");
        m_axisLoc        = glGetUniformLocation(m_program, "uAxis");
    }
}

void VolumeSliceOverlay::buildQuad(float worldPos, int axis, const glm::vec3& boxMin, const glm::vec3& boxMax) {
    glm::vec3 c[4];
    if (axis == 0) {
        c[0] = glm::vec3(worldPos, boxMin.y, boxMin.z);
        c[1] = glm::vec3(worldPos, boxMax.y, boxMin.z);
        c[2] = glm::vec3(worldPos, boxMin.y, boxMax.z);
        c[3] = glm::vec3(worldPos, boxMax.y, boxMax.z);
    } else if (axis == 1) {
        c[0] = glm::vec3(boxMin.x, worldPos, boxMin.z);
        c[1] = glm::vec3(boxMax.x, worldPos, boxMin.z);
        c[2] = glm::vec3(boxMin.x, worldPos, boxMax.z);
        c[3] = glm::vec3(boxMax.x, worldPos, boxMax.z);
    } else {
        c[0] = glm::vec3(boxMin.x, boxMin.y, worldPos);
        c[1] = glm::vec3(boxMax.x, boxMin.y, worldPos);
        c[2] = glm::vec3(boxMin.x, boxMax.y, worldPos);
        c[3] = glm::vec3(boxMax.x, boxMax.y, worldPos);
    }

    float verts[4 * 3 + 4 * 3];
    float* p = verts;
    for (int i = 0; i < 4; ++i) {
        p[0] = c[i].x; p[1] = c[i].y; p[2] = c[i].z;
        p += 3;
    }
    const int lineIdx[4] = {0, 1, 3, 2};
    for (int i = 0; i < 4; ++i) {
        int idx = lineIdx[i];
        p[0] = c[idx].x; p[1] = c[idx].y; p[2] = c[idx].z;
        p += 3;
    }

    if (!m_vao.has()) {
        setupVertexBuffer(m_vao, m_vbo, nullptr, 0, 3 * sizeof(float), { { 0, 3, 0 } }, GL_DYNAMIC_DRAW);
    }
    glNamedBufferData(m_vbo, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glVertexArrayVertexBuffer(m_vao, 0, m_vbo, 0, 3 * sizeof(float));
}

void VolumeSliceOverlay::draw(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj,
                              const ColormapManager& colormap, GLuint sliceTex[3],
                              const glm::vec3& boxMin, const glm::vec3& boxMax,
                              const RenderMesh* mesh) {
    if (!state.anySlicePlaneEnabled() || !m_program.has()) return;

    glm::mat4 mvp = proj * view;

    // Save engine state we mutate; restored automatically on scope exit.
    GLStateGuard guard;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);

    glUseProgram(m_program);
    glUniformMatrix4fv(m_mvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform3fv(m_boxMinLoc, 1, glm::value_ptr(boxMin));
    glUniform3fv(m_boxMaxLoc, 1, glm::value_ptr(boxMax));
    glUniform1i(m_useColormapLoc, state.volumeSliceUseColormap ? 1 : 0);
    if (m_numBandsLoc != -1) glUniform1f(m_numBandsLoc, static_cast<float>(state.volumeSliceColorBands));

    if (state.volumeSliceUseColormap && colormap.volumeSliceTexture() != 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_1D, colormap.volumeSliceTexture());
        glUniform1i(m_lutLoc, 1);
    } else {
        glUniform1i(m_lutLoc, 0);
    }

    for (int axis = 0; axis < 3; ++axis) {
        if (!state.slicePlaneEnabled[axis] || !sliceTex[axis]) continue;
        float worldPos = boxMin[axis] + (boxMax[axis] - boxMin[axis]) * state.slicePlanePos[axis];
        buildQuad(worldPos, axis, boxMin, boxMax);
        const float mapMin = state.sliceColorRangeOverrideEnabled ? state.sliceColorRangeLo : state.sliceScalarMin[axis];
        const float mapMax = state.sliceColorRangeOverrideEnabled ? state.sliceColorRangeHi : state.sliceScalarMax[axis];
        glUniform1f(m_scalarMinLoc, mapMin);
        glUniform1f(m_scalarMaxLoc, mapMax);
        glUniform1f(m_alphaLoc, state.slicePlaneOpacity[axis]);
        if (m_axisLoc != -1) glUniform1i(m_axisLoc, axis);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_3D, sliceTex[axis]);
        glUniform1i(m_volumeTexLoc, 0);
        glBindVertexArray(m_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDrawArrays(GL_LINE_LOOP, 4, 4);
    }

    glBindVertexArray(0);

    glUseProgram(0);
}

void VolumeSliceOverlay::shutdown() {
    m_program.reset();
    m_vao.reset();
    m_vbo.reset();
}


