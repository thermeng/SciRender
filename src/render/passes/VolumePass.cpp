#include "render/passes/VolumePass.h"
#include "render/foundation/renderer.h"
#include "render/foundation/shader_utils.h"
#include "render/passes/ColormapManager.h"
#include <glad/gl.h>
#include <cmath>

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
        locOrtho_           = glGetUniformLocation(program_, "uOrtho");
        locSafeExtent_      = glGetUniformLocation(program_, "uSafeExtent");
        locBaseStepSize_    = glGetUniformLocation(program_, "uBaseStepSize");
        locPixelFootprintScale_ = glGetUniformLocation(program_, "uPixelFootprintScale");
        locVolumeTex_       = glGetUniformLocation(program_, "uVolumeTex");
        locBoxMin_          = glGetUniformLocation(program_, "uBoxMin");
        locBoxMax_          = glGetUniformLocation(program_, "uBoxMax");
        locLut_             = glGetUniformLocation(program_, "uColormapLUT");
        locOpacity_         = glGetUniformLocation(program_, "uOpacity");
        locScalarMin_       = glGetUniformLocation(program_, "uScalarMin");
        locScalarMax_       = glGetUniformLocation(program_, "uScalarMax");
        locClipEnabled_     = glGetUniformLocation(program_, "uClipEnabled");
        locSliceHeightX_    = glGetUniformLocation(program_, "uClipHeightX");
        locSliceHeightY_    = glGetUniformLocation(program_, "uClipHeightY");
        locSliceHeightZ_    = glGetUniformLocation(program_, "uClipHeightZ");
        locSliceEn_         = glGetUniformLocation(program_, "uClipEn");
        locInvert_          = glGetUniformLocation(program_, "uInvert");
        locVolumeUseColormap_ = glGetUniformLocation(program_, "uVolumeUseColormap");
        locNumBands_ = glGetUniformLocation(program_, "uNumBands");
    }
    if (!vaoInitialized_) {
        setupVertexBuffer(quadVao_, quadVbo_, QUAD_VERTS, sizeof(QUAD_VERTS), 2 * sizeof(float),
                          { { 0, 2, 0 } }, GL_STATIC_DRAW);
        vaoInitialized_ = true;
    }
}

void VolumePass::draw(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj,
                      const ColormapManager& colormap, float pixelFootprintScale,
                      GLuint volumeTex, const glm::vec3& boxMin, const glm::vec3& boxMax) {
    if (!state.showVolume || !volumeTex || !program_.has()) return;

    boxMin_ = boxMin;
    boxMax_ = boxMax;

    // Save engine state we mutate; restored automatically on scope exit.
    GLStateGuard guard;

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
    glUniform1i(locOrtho_, state.orthographic ? 1 : 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, volumeTex);
    glUniform1i(locVolumeTex_, 0);

    glm::vec3 boxExtent = boxMax_ - boxMin_;
    glm::vec3 safeExtent = glm::vec3(
        std::abs(boxExtent.x) < 1e-8f ? 1.0f : boxExtent.x,
        std::abs(boxExtent.y) < 1e-8f ? 1.0f : boxExtent.y,
        std::abs(boxExtent.z) < 1e-8f ? 1.0f : boxExtent.z);
    glUniform3fv(locSafeExtent_, 1, glm::value_ptr(safeExtent));

    float extent = glm::length(boxMax_ - boxMin_);
    float baseStepSize = state.volumeStepSize * extent;
    glUniform1f(locBaseStepSize_, baseStepSize);
    glUniform1f(locPixelFootprintScale_, pixelFootprintScale);
    glUniform1f(locOpacity_, state.volumeOpacity);

    // Fixed colormap window (coupled: also drives Beer-Lambert density) or the
    // auto-tracked data range.
    const float mapMin = state.volumeColorRangeOverrideEnabled ? state.volumeColorRangeLo : state.dataScalarMin;
    const float mapMax = state.volumeColorRangeOverrideEnabled ? state.volumeColorRangeHi : state.dataScalarMax;
    glUniform1f(locScalarMin_, mapMin);
    glUniform1f(locScalarMax_, mapMax);
    glUniform3fv(locBoxMin_, 1, glm::value_ptr(boxMin_));
    glUniform3fv(locBoxMax_, 1, glm::value_ptr(boxMax_));

    glUniform1i(locVolumeUseColormap_, state.volumeUseColormap ? 1 : 0);

    if (locNumBands_ != -1) glUniform1f(locNumBands_, static_cast<float>(state.volumeColorBands));

    if (state.volumeUseColormap && colormap.volumeTexture() != 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_1D, colormap.volumeTexture());
        glUniform1i(locLut_, 1);
    } else {
        glUniform1i(locLut_, 0);
    }

    glUniform1i(locClipEnabled_, state.clipEnabled ? 1 : 0);
    glUniform1f(locSliceHeightX_, state.clipHeightX);
    glUniform1f(locSliceHeightY_, state.clipHeightY);
    glUniform1f(locSliceHeightZ_, state.clipHeightZ);
    glUniform3f(locSliceEn_, state.clipEnabledX ? 1.0f : 0.0f, state.clipEnabledY ? 1.0f : 0.0f, state.clipEnabledZ ? 1.0f : 0.0f);
    glUniform3f(locInvert_, state.invertX ? 1.0f : 0.0f, state.invertY ? 1.0f : 0.0f, state.invertZ ? 1.0f : 0.0f);

    glBindVertexArray(quadVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glUseProgram(0);
}

void VolumePass::clearVolume() {
    boxMin_ = boxMax_ = glm::vec3(0.0f);
}

void VolumePass::shutdown() {
    program_.reset();
    quadVao_.reset();
    quadVbo_.reset();
    vaoInitialized_ = false;
    locInvView_ = locInvProj_ = locCamPos_ = locOrtho_ = locVolumeTex_ = -1;
    locBoxMin_ = locBoxMax_ = locLut_ = locOpacity_ = -1;
    locScalarMin_ = locScalarMax_ = locClipEnabled_ = locSliceHeightX_ = -1;
    locSliceHeightY_ = locSliceHeightZ_ = locSliceEn_ = locInvert_ = locVolumeUseColormap_ = -1;
    locSafeExtent_ = locBaseStepSize_ = locPixelFootprintScale_ = -1;
    locNumBands_ = -1;
}
