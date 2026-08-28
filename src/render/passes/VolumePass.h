#pragma once

#include "render/foundation/gl_raii.h"

#include <glm/glm.hpp>

struct RenderRenderState;
struct ShaderSources;
class ColormapManager;

class VolumePass {
public:
    void init(const ShaderSources& sources);
    void draw(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj,
              const ColormapManager& colormap, float pixelFootprintScale,
              GLuint volumeTex, const glm::vec3& boxMin, const glm::vec3& boxMax);
    void shutdown();
    void clearVolume();
    const glm::vec3& boxMin() const { return boxMin_; }
    const glm::vec3& boxMax() const { return boxMax_; }

private:
    GlProgram program_;
    GlVao quadVao_;
    GlBuffer quadVbo_;
    GLint locInvView_ = -1;
    GLint locInvProj_ = -1;
    GLint locCamPos_ = -1;
    GLint locOrtho_ = -1;
    GLint locVolumeTex_ = -1;
    GLint locBoxMin_ = -1;
    GLint locBoxMax_ = -1;
    GLint locLut_ = -1;
    GLint locOpacity_ = -1;
    GLint locScalarMin_ = -1;
    GLint locScalarMax_ = -1;
    GLint locClipEnabled_ = -1;
    GLint locSliceHeightX_ = -1;
    GLint locSliceHeightY_ = -1;
    GLint locSliceHeightZ_ = -1;
    GLint locSliceEn_ = -1;
    GLint locInvert_ = -1;
    GLint locVolumeUseColormap_ = -1;
    GLint locSafeExtent_ = -1;
    GLint locBaseStepSize_ = -1;
    GLint locPixelFootprintScale_ = -1;
    GLint locNumBands_ = -1;
    glm::vec3 boxMin_;
    glm::vec3 boxMax_;
    bool vaoInitialized_ = false;
};


