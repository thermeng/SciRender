#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

class Gizmo {
public:
    Gizmo();
    ~Gizmo();

    void init();

    // width/height/sidebarWidth are in DEVICE pixels (already scaled by dpr on the caller side).
    void draw(int width, int height, const glm::quat& quat, float sidebarWidth);
    void shutdown();

    // Computes on-screen (device-px, OpenGL-origin bottom-left) tip positions for each axis.
    // Used by the renderer for click hit-testing.
    void getAxisEndpoints(int width, int height, const glm::quat& quat, float sidebarWidth,
                          float& xEndX, float& xEndY,
                          float& yEndX, float& yEndY,
                          float& zEndX, float& zEndY);

    void setSize(int pixels) { viewportSize = pixels; }
    int getSize() const { return viewportSize; }
    void setHoveredAxis(int axis) { hoveredAxis = axis; }
    int getHoveredAxis() const { return hoveredAxis; }

    // Hit-test a device-px point (OpenGL-origin bottom-left) against the three axis tips.
    // Returns 0=X, 1=Y, 2=Z, or -1 if not near any tip.
    int pickAxis(int width, int height, const glm::quat& quat, float sidebarWidth,
                 float px, float py, float radiusPx = 18.0f) const;

    void drawWithMVP(const glm::mat4& mvp);

    static glm::mat4 getViewMatrix(float rotationX, float rotationY);
    static glm::mat4 getViewMatrix(const glm::quat& quat);

private:
    GLuint vao = 0, vbo = 0;
    GLuint shaderProgram = 0;
    GLint mvpLoc = -1;

    // Arrow cone geometry (unit axis-aligned, scaled/oriented per axis at draw time)
    GLuint coneVAO = 0, coneVBO = 0, coneEBO = 0;
    int coneIndexCount = 0;

    // Center sphere origin marker geometry handles
    GLuint sphereVAO = 0, sphereVBO = 0, sphereEBO = 0;
    int sphereIndexCount = 0;

    // Shader uniform lookup cache parameters
    GLint useUniformColorLoc = -1;
    GLint uniformColorLoc = -1;

    int viewportSize = 200;
    int hoveredAxis = -1;

    // Shared MVP computation so draw() and getAxisEndpoints() stay in lockstep.
    static glm::mat4 computeMVP(const glm::quat& quat);
    void axisTipScreen(int width, int height, float sidebarWidth,
                               const glm::mat4& mvp, const glm::vec4& worldTip,
                               float& sx, float& sy) const;
};
