#include "render/VolumePass.h"
#include "render/renderer.h"
#include "render/shader_utils.h"
#include "render/ColormapManager.h"
#include <glad/gl.h>

static const float QUAD_VERTS[] = {
    -1.0f, -1.0f,
     3.0f, -1.0f,
    -1.0f,  3.0f,
};

void VolumePass::init(const ShaderSources& sources) {
    if (sources.volumeVert.empty() || sources.volumeFrag.empty()) return;

    program_.reset(compileProgram(sources.volumeVert.c_str(), sources.volumeFrag.c_str(), "Volume"));
    if (program_.has()) {
        locInvView_         = glGetUniformLocation(program_, "uInvView");
        locInvProj_         = glGetUniformLocation(program_, "uInvProj");
        locCamPos_          = glGetUniformLocation(program_, "uCamPos");
        locVolumeTex_       = glGetUniformLocation(program_, "uVolumeTex");
        locBoxMin_          = glGetUniformLocation(program_, "uBoxMin");
        locBoxMax_          = glGetUniformLocation(program_, "uBoxMax");
        locLut_             = glGetUniformLocation(program_, "uColormapLUT");
        locStepSize_        = glGetUniformLocation(program_, "uStepSize");
        locOpacity_         = glGetUniformLocation(program_, "uOpacity");
        locScalarMin_       = glGetUniformLocation(program_, "uScalarMin");
        locScalarMax_       = glGetUniformLocation(program_, "uScalarMax");
        locClipEnabled_     = glGetUniformLocation(program_, "uClipEnabled");
        locSliceHeightX_    = glGetUniformLocation(program_, "uSliceHeightX");
        locSliceHeightY_    = glGetUniformLocation(program_, "uSliceHeightY");
        locSliceHeightZ_    = glGetUniformLocation(program_, "uSliceHeightZ");
        locSliceEn_         = glGetUniformLocation(program_, "uSliceEn");
        locInvert_          = glGetUniformLocation(program_, "uInvert");
        locVolumeUseColormap_ = glGetUniformLocation(program_, "uVolumeUseColormap");
    }
}

void VolumePass::uploadVolume(const RenderRenderState& state, const std::vector<float>& scalars, int dimX, int dimY, int dimZ, const glm::vec3& boxMin, const glm::vec3& boxMax) {
    if (dimX <= 0 || dimY <= 0 || dimZ <= 0 || scalars.empty()) return;

    bool dimsChanged = (dimX_ != dimX || dimY_ != dimY || dimZ_ != dimZ);
    dimX_ = dimX;
    dimY_ = dimY;
    dimZ_ = dimZ;
    boxMin_ = boxMin;
    boxMax_ = boxMax;

    GLuint raw = volumeTex_.get();
    if (!raw || dimsChanged) {
        if (raw) {
            volumeTex_.reset();
        }
        glCreateTextures(GL_TEXTURE_3D, 1, &raw);
        glTextureStorage3D(raw, 1, GL_R32F, dimX, dimY, dimZ);
        glTextureParameteri(raw, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(raw, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(raw, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTextureParameteri(raw, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(raw, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        volumeTex_.reset(raw);
    }
    glTextureSubImage3D(raw, 0, 0, 0, 0, dimX, dimY, dimZ, GL_RED, GL_FLOAT, scalars.data());

    if (!vaoInitialized_) {
        glCreateVertexArrays(1, quadVao_.ptr());
        glCreateBuffers(1, quadVbo_.ptr());
        glNamedBufferData(quadVbo_, sizeof(QUAD_VERTS), QUAD_VERTS, GL_STATIC_DRAW);
        glEnableVertexArrayAttrib(quadVao_, 0);
        glVertexArrayAttribFormat(quadVao_, 0, 2, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(quadVao_, 0, 0);
        glVertexArrayVertexBuffer(quadVao_, 0, quadVbo_, 0, 2 * sizeof(float));
        vaoInitialized_ = true;
    }
}

void VolumePass::draw(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj, const ColormapManager& colormap) {
    if (!state.showVolume || !volumeTex_.has() || !program_.has()) return;

    GLboolean blendWas = glIsEnabled(GL_BLEND);
    GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
    GLboolean depthMaskWas;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMaskWas);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);

    glUseProgram(program_);

    glm::mat4 invView = glm::inverse(view);
    glm::mat4 invProj = glm::inverse(proj);
    glUniformMatrix4fv(locInvView_, 1, GL_FALSE, glm::value_ptr(invView));
    glUniformMatrix4fv(locInvProj_, 1, GL_FALSE, glm::value_ptr(invProj));
    glm::vec3 camPos = glm::vec3(state.camera.position);
    glUniform3fv(locCamPos_, 1, glm::value_ptr(camPos));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, volumeTex_.get());
    glUniform1i(locVolumeTex_, 0);

    float extent = glm::length(boxMax_ - boxMin_);
    float stepSize = state.volumeStepSize * static_cast<float>(extent);
    glUniform1f(locStepSize_, stepSize);
    glUniform1f(locOpacity_, state.volumeOpacity);

    glUniform1f(locScalarMin_, state.dataScalarMin);
    glUniform1f(locScalarMax_, state.dataScalarMax);
    glUniform3fv(locBoxMin_, 1, glm::value_ptr(boxMin_));
    glUniform3fv(locBoxMax_, 1, glm::value_ptr(boxMax_));

    glUniform1i(locVolumeUseColormap_, state.volumeUseColormap ? 1 : 0);

    if (state.volumeUseColormap && colormap.volumeTexture() != 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_1D, colormap.volumeTexture());
        glUniform1i(locLut_, 1);
    } else {
        glUniform1i(locLut_, 0);
    }

    glUniform1i(locClipEnabled_, state.clipEnabled ? 1 : 0);
    glUniform1f(locSliceHeightX_, state.sliceHeightX);
    glUniform1f(locSliceHeightY_, state.sliceHeightY);
    glUniform1f(locSliceHeightZ_, state.sliceHeightZ);
    glUniform3f(locSliceEn_, state.sliceEnabledX ? 1.0f : 0.0f, state.sliceEnabledY ? 1.0f : 0.0f, state.sliceEnabledZ ? 1.0f : 0.0f);
    glUniform3f(locInvert_, state.invertX ? 1.0f : 0.0f, state.invertY ? 1.0f : 0.0f, state.invertZ ? 1.0f : 0.0f);

    glBindVertexArray(quadVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    if (!blendWas) glDisable(GL_BLEND);
    if (depthWas) glEnable(GL_DEPTH_TEST);
    glDepthMask(depthMaskWas);
    glUseProgram(0);
}

void VolumePass::shutdown() {
    program_.reset();
    volumeTex_.reset();
    quadVao_.reset();
    quadVbo_.reset();
    vaoInitialized_ = false;
    dimX_ = dimY_ = dimZ_ = 0;
    locInvView_ = locInvProj_ = locCamPos_ = locVolumeTex_ = -1;
    locBoxMin_ = locBoxMax_ = locLut_ = locStepSize_ = locOpacity_ = -1;
    locScalarMin_ = locScalarMax_ = locClipEnabled_ = locSliceHeightX_ = -1;
    locSliceHeightY_ = locSliceHeightZ_ = locSliceEn_ = locInvert_ = locVolumeUseColormap_ = -1;
}
