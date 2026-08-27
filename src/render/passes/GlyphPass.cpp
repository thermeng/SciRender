#include "render/passes/GlyphPass.h"
#include "render/foundation/renderer.h"
#include "render/foundation/shader_utils.h"
#include "render/streamlines/VectorGlyphSet.h"
#include "render/passes/ColormapManager.h"

void GlyphPass::init(const ShaderSources& sources) {
    if (sources.glyphVert.empty() || sources.glyphFrag.empty()) return;

    glyphProgram.reset(compileProgram(sources.glyphVert.c_str(), sources.glyphFrag.c_str(), "Glyph"));
    if (glyphProgram.has()) {
        glyphLutLoc = glGetUniformLocation(glyphProgram, "uColormapLUT");
        glyphViewPosLoc = glGetUniformLocation(glyphProgram, "uViewPos");
        glyphNumBandsLoc = glGetUniformLocation(glyphProgram, "uNumBands");
        glyphUboIndex = glGetUniformBlockIndex(glyphProgram, "GlyphUBO");
        if (glyphUboIndex != GL_INVALID_INDEX) {
            glUniformBlockBinding(glyphProgram, glyphUboIndex, 1);
            glCreateBuffers(1, glyphUbo.ptr());
            glNamedBufferData(glyphUbo, sizeof(GlyphUBOData), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_UNIFORM_BUFFER, 1, glyphUbo);
        }
    }
}

void GlyphPass::draw(const RenderRenderState& state,
                     const glm::mat4& view,
                     const glm::mat4& proj,
                     const VectorGlyphSet& glyphs,
                     const ColormapManager& colormap) {
    if (!state.showVectors || glyphs.instanceCount <= 0 || glyphProgram == 0) return;

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glUseProgram(glyphProgram);

    glm::vec3 kDir, fDir, b1Dir, b2Dir, hDir;
    state.lighting.computeDirections(state.camera.position, state.camera.focalPoint, state.camera.viewUp,
        kDir, fDir, b1Dir, b2Dir, hDir);
    glm::vec3 camPos = glm::vec3(state.camera.position);
    GlyphUBOData ubo{};
    ubo.mvp = proj * view * glm::mat4(1.0f);
    // Fixed magnitude window, pre-transformed into the shader's normalized
    // space (glyph.vert transforms raw magnitude before normalizing).
    const float magMin = state.glyphMagRangeOverrideEnabled
        ? applyVectorMagTransform(state.glyphMagRangeLo, state.vectorMagTransform) : glyphs.magMin;
    const float magMax = state.glyphMagRangeOverrideEnabled
        ? applyVectorMagTransform(state.glyphMagRangeHi, state.vectorMagTransform) : glyphs.magMax;
    ubo.scale_magMin_magMax_scaleByMag = glm::vec4(state.vectorScale, magMin, magMax, state.vectorScaleByMagnitude ? 1.0f : 0.0f);
    ubo.meshExtent_magTransform_viewPosY_colorR = glm::vec4(glyphs.meshExtent, float(state.vectorMagTransform), camPos.y, state.vectorColor[0]);
    ubo.lightDir_colorGB = glm::vec4(kDir, state.vectorColor[1]);
    ubo.colorB_colormode = glm::vec4(state.vectorColor[2], float(state.vectorColorMode), 0.0f, 0.0f);
    glm::vec3 cMin(glyphs.compMin[0], glyphs.compMin[1], glyphs.compMin[2]);
    glm::vec3 cMax(glyphs.compMax[0], glyphs.compMax[1], glyphs.compMax[2]);
    for (int i = 0; i < 3; ++i) {
        if (state.glyphCompRangeOverrideEnabled[i]) {
            cMin[i] = state.glyphCompRangeLo[i];
            cMax[i] = state.glyphCompRangeHi[i];
        }
    }
    ubo.compMin = glm::vec4(cMin, 0.0f);
    ubo.compMax = glm::vec4(cMax, 0.0f);
    ubo.pbr = glm::vec4(state.lighting.matRoughness, state.lighting.matMetallic, 0.0f, 0.0f);
    glNamedBufferSubData(glyphUbo, 0, sizeof(GlyphUBOData), &ubo);
    if (glyphViewPosLoc != -1) glUniform3fv(glyphViewPosLoc, 1, glm::value_ptr(camPos));
    if (glyphNumBandsLoc != -1) glUniform1f(glyphNumBandsLoc, static_cast<float>(state.vectorColorBands));
    if (state.vectorColorMode > 0 && colormap.vectorTexture() != 0) {
        glBindTextureUnit(1, colormap.vectorTexture());
        glUniform1i(glyphLutLoc, 1);
    }
    glBindVertexArray(glyphs.vao);
    glDrawElementsInstanced(GL_TRIANGLES, glyphs.glyphIndexCount, GL_UNSIGNED_INT, 0, glyphs.instanceCount);
    glBindVertexArray(0);
    glUseProgram(0);
}

void GlyphPass::shutdown() {
    glyphProgram.reset();
    glyphUbo.reset();
    glyphUboIndex = GL_INVALID_INDEX;
    glyphLutLoc = -1;
    glyphViewPosLoc = -1;
    glyphNumBandsLoc = -1;
}
