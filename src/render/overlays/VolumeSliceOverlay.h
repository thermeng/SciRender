#pragma once

#include "render/foundation/gl_raii.h"
#include <glm/glm.hpp>
#include <string>

struct RenderRenderState;
struct ShaderSources;
class ColormapManager;

class RenderMesh;

class VolumeSliceOverlay {
public:
    void init(const ShaderSources& sources);
    void draw(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj,
              const ColormapManager& colormap, GLuint volumeTex,
              const glm::vec3& boxMin, const glm::vec3& boxMax,
              const RenderMesh* mesh);
    void shutdown();

private:
    void buildQuad(float worldPos, int axis, const glm::vec3& boxMin, const glm::vec3& boxMax);

    GlProgram m_program;
    GlVao m_vao;
    GlBuffer m_vbo;

    GLint m_mvpLoc = -1;
    GLint m_volumeTexLoc = -1;
    GLint m_boxMinLoc = -1;
    GLint m_boxMaxLoc = -1;
    GLint m_lutLoc = -1;
    GLint m_useColormapLoc = -1;
    GLint m_scalarMinLoc = -1;
    GLint m_scalarMaxLoc = -1;
    GLint m_alphaLoc = -1;

    float m_worldPos = 0.5f;
    int m_axis = 1;
    glm::vec3 m_boxMin{0.0f};
    glm::vec3 m_boxMax{1.0f};
};


