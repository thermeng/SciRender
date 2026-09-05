#include "render/passes/MeshPass.h"
#include "render/foundation/renderer.h"
#include "render/foundation/LightingModel.h"
#include "render/foundation/shader_utils.h"
#include "render/passes/MeshGLManager.h"
#include "render/passes/ColormapManager.h"

void MeshPass::init(const ShaderSources& sources) {
    if (sources.meshVert.empty() || sources.meshFrag.empty()) return;

    const std::string fragFull = injectPbrCommon(sources.meshFrag.c_str(), sources.pbrFragCommon);
    shaderProgram.reset(compileProgram(sources.meshVert.c_str(), fragFull.c_str(), "Mesh"));
    if (shaderProgram.has()) {
        meshUboIndex = glGetUniformBlockIndex(shaderProgram, "MeshUBO");
        glUniformBlockBinding(shaderProgram, meshUboIndex, 0);
        lutTextureLoc = glGetUniformLocation(shaderProgram, "uColormapLUT");
        numBandsLoc = glGetUniformLocation(shaderProgram, "uNumBands");
    }

    if (!sources.meshVert.empty() && !sources.surfaceLicFrag.empty()) {
        const std::string licFragFull = injectPbrCommon(sources.surfaceLicFrag.c_str(), sources.pbrFragCommon);
        licShaderProgram.reset(compileProgram(sources.meshVert.c_str(), licFragFull.c_str(), "SurfaceLIC"));
        if (licShaderProgram.has()) {
            GLuint licIdx = glGetUniformBlockIndex(licShaderProgram, "MeshUBO");
            if (licIdx != GL_INVALID_INDEX)
                glUniformBlockBinding(licShaderProgram, licIdx, 0);
            licLutTextureLoc = glGetUniformLocation(licShaderProgram, "uColormapLUT");
            licNumBandsLoc = glGetUniformLocation(licShaderProgram, "uNumBands");
            licVectorTexLoc = glGetUniformLocation(licShaderProgram, "uVectorTex");
            licNoiseTexLoc = glGetUniformLocation(licShaderProgram, "uNoiseTex");
            licBoxMinLoc = glGetUniformLocation(licShaderProgram, "uBoxMin");
            licBoxMaxLoc = glGetUniformLocation(licShaderProgram, "uBoxMax");
            licStepsLoc = glGetUniformLocation(licShaderProgram, "uLicSteps");
            licStepSizeLoc = glGetUniformLocation(licShaderProgram, "uLicStepSize");
            licNoiseFreqLoc = glGetUniformLocation(licShaderProgram, "uLicNoiseFreq");
            licBoundaryModeLoc = glGetUniformLocation(licShaderProgram, "uLicBoundaryMode");
            licUvwScaleLoc = glGetUniformLocation(licShaderProgram, "uUvwScale");
            licUvwOffsetLoc = glGetUniformLocation(licShaderProgram, "uUvwOffset");
        }
        if (!sources.meshClipGeo.empty() && licShaderProgram.has()) {
            licClipShaderProgram.reset(compileProgramWithGS(
                sources.meshVert.c_str(), sources.meshClipGeo.c_str(),
                licFragFull.c_str(), "SurfaceLICCrinkleClip"));
            if (licClipShaderProgram.has()) {
                GLuint licClipIdx = glGetUniformBlockIndex(licClipShaderProgram, "MeshUBO");
                if (licClipIdx != GL_INVALID_INDEX)
                    glUniformBlockBinding(licClipShaderProgram, licClipIdx, 0);
                licClipLutTextureLoc = glGetUniformLocation(licClipShaderProgram, "uColormapLUT");
                licClipNumBandsLoc = glGetUniformLocation(licClipShaderProgram, "uNumBands");
                licClipVectorTexLoc = glGetUniformLocation(licClipShaderProgram, "uVectorTex");
                licClipNoiseTexLoc = glGetUniformLocation(licClipShaderProgram, "uNoiseTex");
                licClipBoxMinLoc = glGetUniformLocation(licClipShaderProgram, "uBoxMin");
                licClipBoxMaxLoc = glGetUniformLocation(licClipShaderProgram, "uBoxMax");
                licClipStepsLoc = glGetUniformLocation(licClipShaderProgram, "uLicSteps");
                licClipStepSizeLoc = glGetUniformLocation(licClipShaderProgram, "uLicStepSize");
                licClipNoiseFreqLoc = glGetUniformLocation(licClipShaderProgram, "uLicNoiseFreq");
                licClipBoundaryModeLoc = glGetUniformLocation(licClipShaderProgram, "uLicBoundaryMode");
                licClipUvwScaleLoc = glGetUniformLocation(licClipShaderProgram, "uUvwScale");
                licClipUvwOffsetLoc = glGetUniformLocation(licClipShaderProgram, "uUvwOffset");
            }
        }
    }

    if (!sources.meshClipGeo.empty()) {
        clipShaderProgram.reset(compileProgramWithGS(
            sources.meshVert.c_str(), sources.meshClipGeo.c_str(),
            fragFull.c_str(), "MeshCrinkleClip"));
        if (clipShaderProgram.has()) {
            GLuint clipIdx = glGetUniformBlockIndex(clipShaderProgram, "MeshUBO");
            if (clipIdx != GL_INVALID_INDEX)
                glUniformBlockBinding(clipShaderProgram, clipIdx, 0);
            clipLutTextureLoc = glGetUniformLocation(clipShaderProgram, "uColormapLUT");
            clipNumBandsLoc = glGetUniformLocation(clipShaderProgram, "uNumBands");
        }
    }

    wireframePass.init(sources);
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
    if (!meshManager.hasMeshes() || !shaderProgram.has())
        return {};
    return drawCommon(state, view, proj, model, drawList, drawVerts, edgeDrawList,
                      meshManager, colormap,
                      [&](const RenderRenderState& s, const ColormapManager& c) {
                          activateProgram(s, c);
                      },
                      false);
}

void MeshPass::activateProgram(const RenderRenderState& state, const ColormapManager& colormap) {
    const bool useCrinkleClip = state.crinkleClipMode && clipShaderProgram.has();
    GlProgram& prog = useCrinkleClip ? clipShaderProgram : shaderProgram;
    glUseProgram(prog);

    if (meshUboIndex != GL_INVALID_INDEX)
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, meshUbo);

    GLint activeLutLoc = useCrinkleClip ? clipLutTextureLoc : lutTextureLoc;
    GLint activeNumBandsLoc = useCrinkleClip ? clipNumBandsLoc : numBandsLoc;
    if (activeNumBandsLoc != -1) glUniform1f(activeNumBandsLoc, static_cast<float>(state.scalarColorBands));
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



    float rawA = std::clamp(state.surfaceOpacity, 0.0f, 1.0f);
    float remappedA = (rawA <= 0.0f) ? 0.0f : std::pow(rawA, 1.8f);
    ubo.surfaceColor_sop = glm::vec4(state.surfaceColor[0], state.surfaceColor[1], state.surfaceColor[2], remappedA);
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
    ubo.scalars = glm::vec4(state.colorMapMin(), state.colorMapMax(), (state.meshHasScalars && state.meshUseScalarColor) ? 1.0f : 0.0f, 0.0f);
    ubo.clipY = glm::vec4(state.clipHeightX, state.clipHeightY, state.clipHeightZ, 0.0f);
    ubo.clipEn = glm::vec4(state.clipEnabledX ? 1.0f : 0.0f, state.clipEnabledY ? 1.0f : 0.0f, state.clipEnabledZ ? 1.0f : 0.0f, 0.0f);
    ubo.invert = glm::vec4(state.invertX ? 1.0f : 0.0f, state.invertY ? 1.0f : 0.0f, state.invertZ ? 1.0f : 0.0f, 0.0f);
    ubo.filter = glm::vec4(state.filterMin, state.filterMax, state.filterEnabled ? 1.0f : 0.0f, 0.0f);
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

void MeshPass::activateLicProgram(const RenderRenderState& state, const ColormapManager& colormap,
                                    GLuint vectorTex, GLuint noiseTex,
                                    const glm::vec3& boxMin, const glm::vec3& boxMax,
                                    int texDimX, int texDimY, int texDimZ) {
    const bool useCrinkleClip = state.crinkleClipMode && licClipShaderProgram.has();
    GlProgram& prog = useCrinkleClip ? licClipShaderProgram : licShaderProgram;
    glUseProgram(prog);
    if (meshUboIndex != GL_INVALID_INDEX)
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, meshUbo);
    GLint numBandsLocActive = useCrinkleClip ? licClipNumBandsLoc : licNumBandsLoc;
    GLint vecTexLocActive = useCrinkleClip ? licClipVectorTexLoc : licVectorTexLoc;
    GLint noiseTexLocActive = useCrinkleClip ? licClipNoiseTexLoc : licNoiseTexLoc;
    GLint boxMinLocActive = useCrinkleClip ? licClipBoxMinLoc : licBoxMinLoc;
    GLint boxMaxLocActive = useCrinkleClip ? licClipBoxMaxLoc : licBoxMaxLoc;
    GLint stepsLocActive = useCrinkleClip ? licClipStepsLoc : licStepsLoc;
    GLint stepSizeLocActive = useCrinkleClip ? licClipStepSizeLoc : licStepSizeLoc;
    GLint noiseFreqLocActive = useCrinkleClip ? licClipNoiseFreqLoc : licNoiseFreqLoc;
    GLint boundaryLocActive = useCrinkleClip ? licClipBoundaryModeLoc : licBoundaryModeLoc;
    GLint lutLocActive = useCrinkleClip ? licClipLutTextureLoc : licLutTextureLoc;
    GLint uvwScaleLocActive = useCrinkleClip ? licClipUvwScaleLoc : licUvwScaleLoc;
    GLint uvwOffsetLocActive = useCrinkleClip ? licClipUvwOffsetLoc : licUvwOffsetLoc;
    if (numBandsLocActive != -1) glUniform1f(numBandsLocActive, static_cast<float>(state.scalarColorBands));
    if (vecTexLocActive != -1 && vectorTex != 0) {
        glBindTextureUnit(2, vectorTex);
        glUniform1i(vecTexLocActive, 2);
    }
    if (noiseTexLocActive != -1 && noiseTex != 0) {
        glBindTextureUnit(3, noiseTex);
        glUniform1i(noiseTexLocActive, 3);
    }
    if (boxMinLocActive != -1) glUniform3fv(boxMinLocActive, 1, &boxMin.x);
    if (boxMaxLocActive != -1) glUniform3fv(boxMaxLocActive, 1, &boxMax.x);
    if (stepsLocActive != -1) glUniform1i(stepsLocActive, state.licSteps);
    if (stepSizeLocActive != -1) {
        glm::vec3 extent = boxMax - boxMin;
        float diag = glm::length(extent);
        if (!(diag > 1e-6f)) diag = 1.0f;
        float worldStep = state.licStepSize * diag;
        worldStep = std::clamp(worldStep, 1e-6f, diag * 2.0f);
        glUniform1f(stepSizeLocActive, worldStep);
    }
    if (noiseFreqLocActive != -1) glUniform1f(noiseFreqLocActive, state.licNoiseFreq);
    if (boundaryLocActive != -1) glUniform1i(boundaryLocActive, 0);
    glm::vec3 uvwScale(1.0f);
    glm::vec3 uvwOffset(0.0f);
    (void)texDimX; (void)texDimY; (void)texDimZ;
    if (uvwScaleLocActive != -1) glUniform3fv(uvwScaleLocActive, 1, &uvwScale.x);
    if (uvwOffsetLocActive != -1) glUniform3fv(uvwOffsetLocActive, 1, &uvwOffset.x);
    GLuint lutTex = colormap.scalarTexture() ? colormap.scalarTexture() : colormap.vectorTexture();
    if (lutLocActive != -1 && lutTex != 0) {
        glBindTextureUnit(1, lutTex);
        glUniform1i(lutLocActive, 1);
    }
}

MeshPassResult MeshPass::drawLic(const RenderRenderState& state,
                        const glm::mat4& view,
                        const glm::mat4& proj,
                        const glm::mat4& model,
                        const std::vector<std::pair<GLuint, int>>& drawList,
                        const std::vector<int>& drawVerts,
                        const std::vector<std::pair<GLuint, int>>& edgeDrawList,
                        const MeshGLManager& meshManager,
                        const ColormapManager& colormap,
                        const LicResources& licRes) {
    if (!meshManager.hasMeshes() || !licShaderProgram.has() || !licRes.valid())
        return {};
    return drawCommon(state, view, proj, model, drawList, drawVerts, edgeDrawList,
                      meshManager, colormap,
                      [&](const RenderRenderState& s, const ColormapManager& c) {
                           activateLicProgram(s, c, licRes.vectorTex, licRes.noiseTex, licRes.boxMin, licRes.boxMax, licRes.texDimX, licRes.texDimY, licRes.texDimZ);
                      },
                      true);
}

template <typename Activator>
MeshPassResult MeshPass::drawCommon(const RenderRenderState& state,
                                    const glm::mat4& view,
                                    const glm::mat4& proj,
                                    const glm::mat4& model,
                                    const std::vector<std::pair<GLuint, int>>& drawList,
                                    const std::vector<int>& drawVerts,
                                    const std::vector<std::pair<GLuint, int>>& edgeDrawList,
                                    const MeshGLManager& meshManager,
                                    const ColormapManager& colormap,
                                    Activator activate,
                                    bool licTransparent) {
    MeshPassResult result;
    activate(state, colormap);
    MeshUBOData ubo = makeUbo(state, view, proj, model);

    if (state.showSurface) {
        const bool opaque = state.surfaceOpacity >= 1.0f;
        if (opaque) {
            drawOpaque(state, drawList);
            drawOverlays(state, ubo, drawList, drawVerts, edgeDrawList, meshManager);
        } else if (licTransparent) {



            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            for (size_t di = 0; di < drawList.size(); ++di) {
                glBindVertexArray(drawList[di].first);
                const bool cull = state.cullMode != 0;
                if (cull) { glEnable(GL_CULL_FACE); glCullFace(state.cullMode == 2 ? GL_FRONT : GL_BACK); }
                else glDisable(GL_CULL_FACE);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                glDrawElements(GL_TRIANGLES, drawList[di].second, GL_UNSIGNED_INT, 0);
                if (cull) glDisable(GL_CULL_FACE);
            }
            glBindVertexArray(0);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glDisable(GL_BLEND);
            drawOverlays(state, ubo, drawList, drawVerts, edgeDrawList, meshManager);
        } else {


            result.transparentMeshes = drawList;
        }
    } else {
        drawOverlays(state, ubo, drawList, drawVerts, edgeDrawList, meshManager);
    }
    glUseProgram(0);
    return result;
}

void MeshPass::drawOverlays(const RenderRenderState& state,
                            MeshUBOData& ubo,
                            const std::vector<std::pair<GLuint, int>>& drawList,
                            const std::vector<int>& drawVerts,
                            const std::vector<std::pair<GLuint, int>>& edgeDrawList,
                            const MeshGLManager& meshManager) {
    const bool useCrinkleClip = state.crinkleClipMode && clipShaderProgram.has();




    wireframePass.draw(state, ubo, meshUbo, drawList, edgeDrawList);


    for (size_t di = 0; di < drawList.size(); ++di) {
        glBindVertexArray(drawList[di].first);

        if (state.showPoints && drawVerts[di] > 0) {

            glUseProgram(useCrinkleClip ? clipShaderProgram : shaderProgram);
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, meshUbo);
            GLboolean pointSizeWas = glIsEnabled(GL_PROGRAM_POINT_SIZE);
            glEnable(GL_PROGRAM_POINT_SIZE);

            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);


            constexpr GLintptr kPointClipOff = GLintptr(offsetof(MeshUBOData, point_clip));
            ubo.point_clip.x = 1.0f;
            glNamedBufferSubData(meshUbo, kPointClipOff, sizeof(glm::vec4), &ubo.point_clip);
            glDrawArrays(GL_POINTS, 0, drawVerts[di]);
            ubo.point_clip.x = 0.0f;
            glNamedBufferSubData(meshUbo, kPointClipOff, sizeof(glm::vec4), &ubo.point_clip);
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
    licShaderProgram.reset();
    licClipShaderProgram.reset();
    meshUbo.reset();
    meshUboIndex = GL_INVALID_INDEX;
    lutTextureLoc = -1;
    clipLutTextureLoc = -1;
    numBandsLoc = -1;
    clipNumBandsLoc = -1;
    licLutTextureLoc = -1;
    licClipLutTextureLoc = -1;
    licNumBandsLoc = -1;
    licClipNumBandsLoc = -1;
    licVectorTexLoc = -1;
    licClipVectorTexLoc = -1;
    licNoiseTexLoc = -1;
    licClipNoiseTexLoc = -1;
    licBoxMinLoc = -1;
    licClipBoxMinLoc = -1;
    licBoxMaxLoc = -1;
    licClipBoxMaxLoc = -1;
    licStepsLoc = -1;
    licClipStepsLoc = -1;
    licStepSizeLoc = -1;
    licClipStepSizeLoc = -1;
    licNoiseFreqLoc = -1;
    licClipNoiseFreqLoc = -1;
    licBoundaryModeLoc = -1;
    licClipBoundaryModeLoc = -1;
    licUvwScaleLoc = -1;
    licClipUvwScaleLoc = -1;
    licUvwOffsetLoc = -1;
    licClipUvwOffsetLoc = -1;
    wireframePass.shutdown();
}
