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

    // Qt Quick Migration: Explicit width and height replaces legacy GLFW handles
    void draw(int width, int height, const glm::quat& quat, float sidebarWidth);
    void shutdown();

    // Qt Quick Migration: Framebuffer dimension passing replaces legacy GLFW polling
    void getAxisEndpoints(int width, int height, const glm::quat& quat, float sidebarWidth,
                          float& xEndX, float& xEndY,
                          float& yEndX, float& yEndY,
                          float& zEndX, float& zEndY);

    void setSize(int pixels) { viewportSize = pixels; }
    int getSize() const { return viewportSize; }
    void setHoveredAxis(int axis) { hoveredAxis = axis; }
    void drawWithMVP(const glm::mat4& mvp);

    static glm::mat4 getViewMatrix(float rotationX, float rotationY);
    static glm::mat4 getViewMatrix(const glm::quat& quat);

private:
    GLuint vao = 0, vbo = 0;
    GLuint shaderProgram = 0;
    GLint mvpLoc = -1;

    // Center sphere origin marker geometry handles
    GLuint sphereVAO = 0, sphereVBO = 0, sphereEBO = 0;
    int sphereIndexCount = 0;

    // Shader uniform lookup cache parameters
    GLint useUniformColorLoc = -1;
    GLint uniformColorLoc = -1;

    int viewportSize = 100;
    int hoveredAxis = -1;
};