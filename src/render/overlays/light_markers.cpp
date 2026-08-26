#include "render/overlays/light_markers.h"

#include <glad/gl.h>
#include "render/foundation/shader_utils.h"
#include "render/overlays/gizmo.h"
#include <QOpenGLContext>
#include <cstring>

// Simple solid-color shader for light-marker discs.
static const char* markVS = R"(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
uniform mat4 uMVP;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";
static const char* markFS = R"(
#version 460 core
in vec3 vColor;
out vec4 frag;
void main() { frag = vec4(vColor, 1.0); }
)";

LightMarkerOverlay::~LightMarkerOverlay() { shutdown(); }

bool LightMarkerOverlay::buildProgram() {
    markProgram.reset(compileProgram(markVS, markFS, "LightMarkers/mark"));
    if (!markProgram.has()) return false;
    markMvpLoc = glGetUniformLocation(markProgram, "uMVP");
    GLint posLoc = glGetAttribLocation(markProgram, "aPos");
    GLint colLoc = glGetAttribLocation(markProgram, "aColor");
    return markMvpLoc >= 0 && posLoc >= 0 && colLoc >= 0;
}

bool LightMarkerOverlay::init() {
    if (isInitialized()) return true;

    if (!buildProgram()) return false;

    GLint mPosLoc = glGetAttribLocation(markProgram, "aPos");
    GLint mColLoc = glGetAttribLocation(markProgram, "aColor");

    // Marker disc VBO (dynamic): 5 lights × 6 verts × vec6(px.xy + rgb)
    setupVertexBuffer(markVAO, markVBO, nullptr, 5 * 6 * 6 * sizeof(float), 6 * sizeof(float),
                      { { mPosLoc, 3, 0 }, { mColLoc, 3, 3 * sizeof(float) } }, GL_DYNAMIC_DRAW);

    return true;
}

void LightMarkerOverlay::shutdown() {
    if (!QOpenGLContext::currentContext()) return;
    markVAO.reset(); markVBO.reset(); markProgram.reset();
}

void LightMarkerOverlay::draw(const glm::vec3 dirs[5], const glm::vec3 cols[5], float dpr,
                              int deviceW, int deviceH, int corner, int foot) {
    if (!isInitialized()) return;

    // Save engine state we mutate; restored automatically on scope exit.
    GLStateGuard guard;

    const int s = static_cast<int>(foot * dpr);
    const glm::ivec2 ro = Gizmo::rectOrigin(deviceW, deviceH, dpr, foot, corner);
    glViewport(ro.x, ro.y, s, s);

    GLint curClipOrigin = GL_UPPER_LEFT;
    glGetIntegerv(GL_CLIP_ORIGIN, &curClipOrigin);
    glClipControl(static_cast<GLenum>(curClipOrigin), GL_NEGATIVE_ONE_TO_ONE);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); // force filled discs (see Gizmo::draw)

    // px space [0..foot]x[0..foot] -> clip; markers are constant kit-local dirs.
    const glm::mat4 pxMVP = glm::ortho(0.0f, (float)foot, 0.0f, (float)foot, -1.0f, 1.0f);
    const float r = 6.0f; // disc radius in px

    glUseProgram(markProgram);
    glUniformMatrix4fv(markMvpLoc, 1, GL_FALSE, glm::value_ptr(pxMVP));
    glBindVertexArray(markVAO);
    // Build all 5 marker discs with per-vertex color, then single upload + single draw.
    float verts[5][6][6];
    for (int i = 0; i < 5; ++i) {
        float cx = (dirs[i].x * 0.5f + 0.5f) * foot;
        float cy = (dirs[i].y * 0.5f + 0.5f) * foot;
        const float q[6][6] = {
            { cx - r, cy - r, 0.0f, cols[i].r, cols[i].g, cols[i].b },
            { cx + r, cy - r, 0.0f, cols[i].r, cols[i].g, cols[i].b },
            { cx - r, cy + r, 0.0f, cols[i].r, cols[i].g, cols[i].b },
            { cx + r, cy - r, 0.0f, cols[i].r, cols[i].g, cols[i].b },
            { cx + r, cy + r, 0.0f, cols[i].r, cols[i].g, cols[i].b },
            { cx - r, cy + r, 0.0f, cols[i].r, cols[i].g, cols[i].b },
        };
        std::memcpy(verts[i], q, sizeof(q));
    }
    glNamedBufferSubData(markVBO, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 5 * 6);  // 5 marker discs
    glBindVertexArray(0);
    glUseProgram(0);
}
