#include "render/passes/MeshPass.h"
#include "render/foundation/renderer.h"
#include "render/foundation/LightingModel.h"
#include "render/foundation/shader_utils.h"
#include "render/passes/MeshGLManager.h"
#include "render/passes/ColormapManager.h"

void MeshPass::init(const ShaderSources& sources) {
    if (sources.meshVert.empty() || sources.meshFrag.empty()) return;

    shaderProgram.reset(compileProgram(sources.meshVert.c_str(), sources.meshFrag.c_str(), "Mesh"));
    if (shaderProgram.has()) {
        meshUboIndex = glGetUniformBlockIndex(shaderProgram, "MeshUBO");
        glUniformBlockBinding(shaderProgram, meshUboIndex, 0);
        lutTextureLoc = glGetUniformLocation(shaderProgram, "uColormapLUT");
    }

    if (!sources.meshClipGeo.empty()) {
        clipShaderProgram.reset(compileProgramWithGS(
            sources.meshVert.c_str(), sources.meshClipGeo.c_str(),
            sources.meshFrag.c_str(), "MeshCrinkleClip"));
        if (clipShaderProgram.has()) {
            GLuint clipIdx = glGetUniformBlockIndex(clipShaderProgram, "MeshUBO");
            if (clipIdx != GL_INVALID_INDEX)
                glUniformBlockBinding(clipShaderProgram, clipIdx, 0);
            clipLutTextureLoc = glGetUniformLocation(clipShaderProgram, "uColormapLUT");
        }
    }

    if (!sources.meshWireVert.empty() && !sources.meshWireGeo.empty()
        && !sources.meshWireFrag.empty()) {
        wireProgram.reset(compileProgramWithGS(
            sources.meshWireVert.c_str(), sources.meshWireGeo.c_str(),
            sources.meshWireFrag.c_str(), "MeshWire"));
        if (wireProgram.has()) {
            wireMvpLoc       = glGetUniformLocation(wireProgram, "uMVP");
            wireViewportLoc  = glGetUniformLocation(wireProgram, "uViewport");
            wireHalfWidthLoc = glGetUniformLocation(wireProgram, "uHalfWidth");
            wireColorLoc     = glGetUniformLocation(wireProgram, "uWireColor");
        }
    }
}

MeshPassResult MeshPass::draw(const RenderRenderState& state,
                                const glm::mat4& view,
                                const glm::mat4& proj,
                                const glm::mat4& model,
                                const std::vector<std::pair<GLuint, int>>& drawList,
                                const std::vector<int>& drawVerts,
                                const std::vector<std::pair<GLuint, int>>& edgeDrawList,
                                const MeshGLManager& meshManager,
                                const ColormapManager& colormap) {
    MeshPassResult result;

     if (!meshManager.hasMeshes() || !shaderProgram.has()) return result;

    activateProgram(state, colormap);
    MeshUBOData ubo = makeUbo(state, view, proj, model);

    if (state.showSurface) {
        const bool opaque = state.surfaceOpacity >= 1.0f;
        if (opaque) {
            drawOpaque(state, drawList);
            drawOverlays(state, ubo, drawList, drawVerts, edgeDrawList, meshManager);
        } else {
            // Surface goes through depth peeling; overlays are drawn later by
            // drawOverlaysAfterTransparent() so the wireframe/points survive.
            result.transparentMeshes = drawList;
        }
    } else {
        drawOverlays(state, ubo, drawList, drawVerts, edgeDrawList, meshManager);
    }

    glUseProgram(0);

    return result;
}

void MeshPass::activateProgram(const RenderRenderState& state, const ColormapManager& colormap) {
    const bool useCrinkleClip = state.crinkleClipMode && clipShaderProgram.has();
    GlProgram& prog = useCrinkleClip ? clipShaderProgram : shaderProgram;
    glUseProgram(prog);

    if (meshUboIndex != GL_INVALID_INDEX)
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, meshUbo);

    GLint activeLutLoc = useCrinkleClip ? clipLutTextureLoc : lutTextureLoc;
    if (state.meshHasScalars && state.meshUseScalarColor && colormap.scalarTexture() != 0) {
        glBindTextureUnit(0, colormap.scalarTexture());
        glUniform1i(activeLutLoc, 0);
    }
}

MeshUBOData MeshPass::makeUbo(const RenderRenderState& state,
                              const glm::mat4& view,
                              const glm::mat4& proj,
                              const glm::mat4& model) {
    if (!meshUbo.has()) {
        glCreateBuffers(1, meshUbo.ptr());
        glNamedBufferData(meshUbo, sizeof(MeshUBOData), nullptr, GL_DYNAMIC_DRAW);
    }

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

    glm::vec3 tint = LightingModel::warmTint(state.lighting.lightWarm);
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
    const bool useCrinkleClip = state.crinkleClipMode && clipShaderProgram.has();
    ubo.shadingMode = glm::vec4(state.flatShading ? 1.0f : 0.0f, useCrinkleClip ? 1.0f : 0.0f, 0.0f, 0.0f);
    glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);
    return ubo;
}

void MeshPass::drawOverlaysAfterTransparent(const RenderRenderState& state,
                                            const glm::mat4& view,
                                            const glm::mat4& proj,
                                            const glm::mat4& model,
                                            const std::vector<std::pair<GLuint, int>>& drawList,
                                            const std::vector<int>& drawVerts,
                                            const std::vector<std::pair<GLuint, int>>& edgeDrawList,
                                            const MeshGLManager& meshManager,
                                            const ColormapManager& colormap) {
    if (!shaderProgram.has() || drawList.empty()) return;
    activateProgram(state, colormap);
    MeshUBOData ubo = makeUbo(state, view, proj, model);
    drawOverlays(state, ubo, drawList, drawVerts, edgeDrawList, meshManager);
    glUseProgram(0);
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
                            const std::vector<std::pair<GLuint, int>>& edgeDrawList,
                            const MeshGLManager& meshManager) {
    const bool useCrinkleClip = state.crinkleClipMode && clipShaderProgram.has();

    // ── Cell-boundary wireframe (GL_LINES via edge VAOs) ──────────────────────
    // When cellWireframe is enabled and edge VAOs are available, render cell
    // edges directly instead of the triangle-edge (glPolygonMode GL_LINE)
    // fallback. Decimated LOD meshes have no edge VAOs, so they fall through
    // to the per-mesh GL_LINE approach below.
    if (state.showWireframe && state.cellWireframe && !edgeDrawList.empty()) {
        GLStateGuard guard;

        ubo.meshColor_wire = glm::vec4(state.meshColor[0], state.meshColor[1], state.meshColor[2], 1.0f);
        glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);

        // Preferred path: GS-expanded screen-space quads. glLineWidth() is
        // unreliable in the core profile (drivers may clamp to 1.0), so the
        // requested thickness is applied by wire.geo in device pixels instead.
        // Under crinkle clip the legacy path is kept: the clip program's
        // per-fragment discard cannot apply to the expanded quads.
        const bool useThickWire = !useCrinkleClip && wireProgram.has()
                                  && wireMvpLoc >= 0 && wireColorLoc >= 0
                                  && wireViewportLoc >= 0 && wireHalfWidthLoc >= 0;
        if (useThickWire) {
            GLint vp[4];
            glGetIntegerv(GL_VIEWPORT, vp);
            glUseProgram(wireProgram);
            glUniformMatrix4fv(wireMvpLoc, 1, GL_FALSE, glm::value_ptr(ubo.mvp));
            glUniform4fv(wireColorLoc, 1, glm::value_ptr(ubo.meshColor_wire));
            glUniform2f(wireViewportLoc, static_cast<float>(vp[2]), static_cast<float>(vp[3]));
            glUniform1f(wireHalfWidthLoc, 0.5f * state.lineWidth);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-1.0f, -1.0f);
            glDisable(GL_CULL_FACE);
            for (const auto& e : edgeDrawList) {
                if (e.second <= 0) continue;
                glBindVertexArray(e.first);
                glDrawElements(GL_LINES, e.second, GL_UNSIGNED_INT, 0);
            }
            glBindVertexArray(0);
            glDisable(GL_POLYGON_OFFSET_FILL);
        } else {
            glLineWidth(state.lineWidth);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glEnable(GL_POLYGON_OFFSET_LINE);
            glPolygonOffset(-1.0f, -1.0f);

            for (const auto& e : edgeDrawList) {
                if (e.second <= 0) continue;
                glBindVertexArray(e.first);
                glDrawElements(GL_LINES, e.second, GL_UNSIGNED_INT, 0);
            }
            glBindVertexArray(0);

            glDisable(GL_POLYGON_OFFSET_LINE);
            glLineWidth(1.0f);
        }

        ubo.meshColor_wire.w = 0.0f;
        glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);
    }

    // ── Per-mesh overlay pass (triangle-edge wireframe + points) ─────────────
    const bool useCellEdges = state.showWireframe && state.cellWireframe && !edgeDrawList.empty();
    for (size_t di = 0; di < drawList.size(); ++di) {
        glBindVertexArray(drawList[di].first);

        if (state.showWireframe && !useCellEdges) {
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
            if (useCrinkleClip) {
                glUseProgram(shaderProgram);
            }
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
            if (useCrinkleClip) {
                glUseProgram(clipShaderProgram);
            }
        }
    }
    glBindVertexArray(0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

void MeshPass::shutdown() {
    shaderProgram.reset();
    clipShaderProgram.reset();
    meshUbo.reset();
    meshUboIndex = GL_INVALID_INDEX;
    lutTextureLoc = -1;
    clipLutTextureLoc = -1;
}


