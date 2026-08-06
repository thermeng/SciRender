#include "render/MeshPass.h"
#include "render/renderer.h"
#include "render/shader_utils.h"
#include "render/MeshGLManager.h"
#include "render/ColormapManager.h"

void MeshPass::init(const ShaderSources& sources) {
    if (sources.meshVert.empty() || sources.meshFrag.empty()) return;

    shaderProgram.reset(compileProgram(sources.meshVert.c_str(), sources.meshFrag.c_str(), "Mesh"));
    if (shaderProgram.has()) {
        meshUboIndex = glGetUniformBlockIndex(shaderProgram, "MeshUBO");
        glUniformBlockBinding(shaderProgram, meshUboIndex, 0);
        lutTextureLoc = glGetUniformLocation(shaderProgram, "uColormapLUT");
    }
}

MeshPassResult MeshPass::draw(const RenderRenderState& state,
                               const glm::mat4& view,
                               const glm::mat4& proj,
                               const glm::mat4& model,
                               const std::vector<std::pair<GLuint, int>>& drawList,
                               const std::vector<int>& drawVerts,
                               const MeshGLManager& meshManager,
                               const ColormapManager& colormap) {
    MeshPassResult result;

    if (!meshManager.hasMeshes() || !shaderProgram.has()) return result;

    glUseProgram(shaderProgram);

    if (!meshUbo.has()) {
        glCreateBuffers(1, meshUbo.ptr());
        glNamedBufferData(meshUbo, sizeof(MeshUBOData), nullptr, GL_DYNAMIC_DRAW);
    }
    if (meshUboIndex != GL_INVALID_INDEX)
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, meshUbo);

    MeshUBOData ubo{};
    glm::mat4 mvp = proj * view * model;
    ubo.mvp = mvp;
    ubo.model = model;
    ubo.viewPos_ps = glm::vec4(glm::vec3(state.camera.position), state.pointSize);
    ubo.meshColor_wire = glm::vec4(state.meshColor[0], state.meshColor[1], state.meshColor[2], 0.0f);
    ubo.surfaceColor_sop = glm::vec4(state.surfaceColor[0], state.surfaceColor[1], state.surfaceColor[2], state.surfaceOpacity);
    ubo.point_clip = glm::vec4(0.0f, state.pointUseScalar ? 1.0f : 0.0f, state.pointOpacity, state.clipEnabled ? 1.0f : 0.0f);

    glm::vec3 kDir, fDir, b1Dir, b2Dir, hDir;
    state.lighting.computeDirections(state.camera.position, state.camera.focalPoint, state.camera.viewUp,
        kDir, fDir, b1Dir, b2Dir, hDir);
    ubo.lightDir = glm::vec4(kDir, 0.0f);
    ubo.lightFill = glm::vec4(fDir, 0.0f);
    ubo.lightBack1 = glm::vec4(b1Dir, 0.0f);
    ubo.lightBack2 = glm::vec4(b2Dir, 0.0f);
    ubo.lightHead = glm::vec4(hDir, 0.0f);

    auto warmTint = [](float w) -> glm::vec3 {
        if (w < 0.5f) return glm::mix(glm::vec3(0.6f, 0.7f, 1.0f), glm::vec3(1.0f), w / 0.5f);
        return glm::mix(glm::vec3(1.0f), glm::vec3(1.0f, 0.85f, 0.7f), (w - 0.5f) / 0.5f);
    };
    glm::vec3 tint = warmTint(state.lighting.lightWarm);
    ubo.keyColor = glm::vec4(tint, 0.0f);
    ubo.fillColor = glm::vec4(tint * glm::vec3(0.90f, 0.92f, 1.00f), 0.0f);
    ubo.backColor = glm::vec4(tint * glm::vec3(0.95f, 0.95f, 0.98f), 0.0f);
    ubo.headColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    ubo.scalars = glm::vec4(state.scalarMin, state.scalarMax, (state.meshHasScalars && state.meshUseScalarColor) ? 1.0f : 0.0f, 0.0f);
    ubo.sliceY = glm::vec4(state.sliceHeightX, state.sliceHeightY, state.sliceHeightZ, 0.0f);
    ubo.sliceEn = glm::vec4(state.sliceEnabledX ? 1.0f : 0.0f, state.sliceEnabledY ? 1.0f : 0.0f, state.sliceEnabledZ ? 1.0f : 0.0f, 0.0f);
    ubo.invert = glm::vec4(state.invertX ? 1.0f : 0.0f, state.invertY ? 1.0f : 0.0f, state.invertZ ? 1.0f : 0.0f, 0.0f);
    ubo.filter = glm::vec4(state.filterMin, state.filterMax, 0.0f, 0.0f);
    float keyI = state.lighting.lightKitEnabled ? state.lighting.lightKeyIntensity : 0.0f;
    float kf = std::max(state.lighting.lightKF, 0.001f);
    float kh = std::max(state.lighting.lightKH, 0.001f);
    float kb = std::max(state.lighting.lightKB, 0.001f);
    ubo.intensities = glm::vec4(keyI, state.lighting.lightKitEnabled ? keyI / kf : 0.0f, state.lighting.lightKitEnabled ? keyI / kb : 0.0f, state.lighting.lightKitEnabled ? keyI / kh : 0.0f);
    ubo.material = glm::vec4(state.lighting.matAmbient, state.lighting.matDiffuse, state.lighting.matSpecular, 0.0f);
    ubo.pbr = glm::vec4(state.lighting.matRoughness, state.lighting.matMetallic, 0.0f, 0.0f);
    glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);

    if (state.meshHasScalars && state.meshUseScalarColor && colormap.scalarTexture() != 0) {
        glBindTextureUnit(0, colormap.scalarTexture());
        glUniform1i(lutTextureLoc, 0);
    }

    if (state.showSurface) {
        const bool opaque = state.surfaceOpacity >= 1.0f;
        if (opaque) {
            drawOpaque(state, drawList);
        } else {
            result.transparentMeshes = drawList;
        }
    }

    drawOverlays(state, ubo, drawList, drawVerts, meshManager);

    glUseProgram(0);

    return result;
}

void MeshPass::drawOpaque(const RenderRenderState& state,
                           const std::vector<std::pair<GLuint, int>>& drawList) {
    for (size_t di = 0; di < drawList.size(); ++di) {
        glBindVertexArray(drawList[di].first);
        const bool cull = state.cullMode != 0;
        if (cull) { glEnable(GL_CULL_FACE); glCullFace(state.cullMode == 2 ? GL_FRONT : GL_BACK); }
        else glDisable(GL_CULL_FACE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0f, 1.0f);
        glDrawElements(GL_TRIANGLES, drawList[di].second, GL_UNSIGNED_INT, 0);
        glDisable(GL_POLYGON_OFFSET_FILL);
        if (cull) glDisable(GL_CULL_FACE);
    }
    glBindVertexArray(0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

void MeshPass::drawOverlays(const RenderRenderState& state,
                            MeshUBOData& ubo,
                            const std::vector<std::pair<GLuint, int>>& drawList,
                            const std::vector<int>& drawVerts,
                            const MeshGLManager& meshManager) {
    for (size_t di = 0; di < drawList.size(); ++di) {
        glBindVertexArray(drawList[di].first);

        if (state.showWireframe) {
            glLineWidth(state.lineWidth);
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glEnable(GL_POLYGON_OFFSET_LINE);
            glPolygonOffset(-1.0f, -1.0f);
            ubo.meshColor_wire = glm::vec4(state.meshColor[0], state.meshColor[1], state.meshColor[2], 1.0f);
            glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);
            glDrawElements(GL_TRIANGLES, drawList[di].second, GL_UNSIGNED_INT, 0);
            glDisable(GL_POLYGON_OFFSET_LINE);
            glLineWidth(1.0f);
            ubo.meshColor_wire.w = 0.0f;
            glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);
        }

        if (state.showPoints && drawVerts[di] > 0) {
            GLboolean pointSizeWas = glIsEnabled(GL_PROGRAM_POINT_SIZE);
            glEnable(GL_PROGRAM_POINT_SIZE);

            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            ubo.point_clip.x = 1.0f;
            glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);
            glDrawArrays(GL_POINTS, 0, drawVerts[di]);
            ubo.point_clip.x = 0.0f;
            glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);
            glDisable(GL_BLEND);

            if (pointSizeWas) glEnable(GL_PROGRAM_POINT_SIZE); else glDisable(GL_PROGRAM_POINT_SIZE);
        }
    }
    glBindVertexArray(0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

void MeshPass::drawCellEdges(const RenderRenderState& state,
                              const glm::mat4& view,
                              const glm::mat4& proj,
                              const glm::mat4& model,
                              const MeshGLManager& meshManager) {
    if (!state.showCellEdges || !shaderProgram.has()) return;

    auto ce = meshManager.getCellEdgeLine();
    if (ce.first == 0 || ce.second <= 0) return;

    MeshUBOData ubo{};
    glm::mat4 mvp = proj * view * model;
    ubo.mvp = mvp;
    ubo.model = model;
    ubo.viewPos_ps = glm::vec4(glm::vec3(state.camera.position), state.pointSize);
    ubo.meshColor_wire = glm::vec4(state.meshColor[0], state.meshColor[1], state.meshColor[2], 1.0f);
    ubo.surfaceColor_sop = glm::vec4(state.surfaceColor[0], state.surfaceColor[1], state.surfaceColor[2], 1.0f);
    glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);

    glUseProgram(shaderProgram);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, meshUbo);

    glEnable(GL_DEPTH_TEST);
    glLineWidth(state.cellEdgeLineWidth);
    glBindVertexArray(ce.first);
    glDrawArrays(GL_LINES, 0, ce.second);
    glBindVertexArray(0);
    glLineWidth(1.0f);
}

void MeshPass::shutdown() {
    shaderProgram.reset();
    meshUbo.reset();
    meshUboIndex = GL_INVALID_INDEX;
    lutTextureLoc = -1;
}
