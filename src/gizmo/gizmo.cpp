#include "gizmo/gizmo.h"
#include <cmath>
#include <vector>
#include <iostream>

static const char* gizmoVS = R"(
    #version 330 core
    layout(location = 0) in vec3 aPos;
    layout(location = 1) in vec3 aColor;
    uniform mat4 uMVP;
    uniform bool uUseUniformColor;
    uniform vec3 uColor;
    out vec3 vColor;
    void main() {
        gl_Position = uMVP * vec4(aPos, 1.0);
        vColor = uUseUniformColor ? uColor : aColor;
    }
)";

static const char* gizmoFS = R"(
    #version 330 core
    in vec3 vColor;
    out vec4 FragColor;
    void main() {
        FragColor = vec4(vColor, 1.0);
    }
)";

static constexpr float GIZMO_CAMERA_DISTANCE = 5.0f;

Gizmo::Gizmo() : vao(0), vbo(0), shaderProgram(0), mvpLoc(-1) {}

Gizmo::~Gizmo() {
    shutdown();
}

static bool compileShaderInline(GLuint shader, const char* src) {
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info[512];
        glGetShaderInfoLog(shader, 512, nullptr, info);
        std::cerr << "Gizmo Inline Shader compilation error: " << info << std::endl;
        return false;
    }
    return true;
}

void Gizmo::init() {
    // Compile embedded GLSL shaders
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    compileShaderInline(vs, gizmoVS);
    compileShaderInline(fs, gizmoFS);

    shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vs);
    glAttachShader(shaderProgram, fs);
    glLinkProgram(shaderProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);

    mvpLoc = glGetUniformLocation(shaderProgram, "uMVP");
    useUniformColorLoc = glGetUniformLocation(shaderProgram, "uUseUniformColor");
    uniformColorLoc = glGetUniformLocation(shaderProgram, "uColor");

    // Build Cartesian Axis lines geometry
    float axesVerts[] = {
        // Position         // Color
        0,0,0,              1,0,0,
        1,0,0,              1,0,0, // X - Red

        0,0,0,              0,1,0,
        0,1,0,              0,1,0, // Y - Green

        0,0,0,              0,0,1,
        0,0,1,              0,0,1  // Z - Blue
    };

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(axesVerts), axesVerts, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Build origin hub placeholder sphere geometry
    std::vector<float> sphereVerts;
    std::vector<unsigned int> sphereIndices;
    int stacks = 8, slices = 8;
    float radius = 0.08f;

    for (int i = 0; i <= stacks; ++i) {
        float phi = 3.14159265f * (float)i / (float)stacks;
        for (int j = 0; j <= slices; ++j) {
            float theta = 2.0f * 3.14159265f * (float)j / (float)slices;
            float x = radius * std::sin(phi) * std::cos(theta);
            float y = radius * std::cos(phi);
            float z = radius * std::sin(phi) * std::sin(theta);

            sphereVerts.push_back(x);
            sphereVerts.push_back(y);
            sphereVerts.push_back(z);

            // Default color mapping values for center sphere points (White base)
            sphereVerts.push_back(0.8f);
            sphereVerts.push_back(0.8f);
            sphereVerts.push_back(0.8f);
        }
    }

    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            unsigned int p0 = i * (slices + 1) + j;
            unsigned int p1 = p0 + 1;
            unsigned int p2 = (i + 1) * (slices + 1) + j;
            unsigned int p3 = p2 + 1;

            sphereIndices.push_back(p0); sphereIndices.push_back(p2); sphereIndices.push_back(p1);
            sphereIndices.push_back(p1); sphereIndices.push_back(p2); sphereIndices.push_back(p3);
        }
    }
    sphereIndexCount = static_cast<int>(sphereIndices.size());

    glGenVertexArrays(1, &sphereVAO);
    glGenBuffers(1, &sphereVBO);
    glGenBuffers(1, &sphereEBO);

    glBindVertexArray(sphereVAO);
    glBindBuffer(GL_ARRAY_BUFFER, sphereVBO);
    glBufferData(GL_ARRAY_BUFFER, sphereVerts.size() * sizeof(float), sphereVerts.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphereEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sphereIndices.size() * sizeof(unsigned int), sphereIndices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void Gizmo::shutdown() {
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (sphereVAO) glDeleteVertexArrays(1, &sphereVAO);
    if (sphereVBO) glDeleteBuffers(1, &sphereVBO);
    if (sphereEBO) glDeleteBuffers(1, &sphereEBO);
    if (shaderProgram) glDeleteProgram(shaderProgram);
}

glm::mat4 Gizmo::getViewMatrix(float rotationX, float rotationY) {
    glm::mat4 mat(1.0f);
    mat = glm::translate(mat, glm::vec3(0.0f, 0.0f, -GIZMO_CAMERA_DISTANCE));
    mat = glm::rotate(mat, glm::radians(rotationX), glm::vec3(1.0f, 0.0f, 0.0f));
    mat = glm::rotate(mat, glm::radians(rotationY), glm::vec3(0.0f, 1.0f, 0.0f));
    return mat;
}

glm::mat4 Gizmo::getViewMatrix(const glm::quat& quat) {
    glm::mat4 mat(1.0f);
    mat = glm::translate(mat, glm::vec3(0.0f, 0.0f, -GIZMO_CAMERA_DISTANCE));
    mat = mat * glm::mat4_cast(quat);
    return mat;
}

void Gizmo::draw(int width, int height, const glm::quat& quat, float sidebarWidth) {
    if (shaderProgram == 0) return;

    int gizmoSize = viewportSize;
    int margin = 16;

    // Qt Safe Positioning Offset Calc
    int gizmoX = static_cast<int>(sidebarWidth) + margin;
    int gizmoY = margin;

    glViewport(gizmoX, gizmoY, gizmoSize, gizmoSize);

    glm::mat4 view = Gizmo::getViewMatrix(quat);
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.01f, 100.0f);
    glm::mat4 mvp = proj * view;

    glUseProgram(shaderProgram);
    glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));

    // Base Layer Pass: Coordinate Axis vectors
    glLineWidth(3.0f);
    glUniform1i(useUniformColorLoc, 0);
    glBindVertexArray(vao);
    glDrawArrays(GL_LINES, 0, 6);

    // Highlight Pass: Color adjustments for hovered cursor coordinates
    if (hoveredAxis >= 0 && hoveredAxis <= 2) {
        glUniform1i(useUniformColorLoc, 1);
        if (hoveredAxis == 0) glUniform3f(uniformColorLoc, 1.0f, 1.0f, 0.0f); // Yellow highlighting
        else if (hoveredAxis == 1) glUniform3f(uniformColorLoc, 1.0f, 1.0f, 0.0f);
        else if (hoveredAxis == 2) glUniform3f(uniformColorLoc, 1.0f, 1.0f, 0.0f);

        glLineWidth(5.0f);
        glDrawArrays(GL_LINES, hoveredAxis * 2, 2);
    }

    // Centered Origin tracking hub rendering bounds pass
    glUniform1i(useUniformColorLoc, 0);
    glBindVertexArray(sphereVAO);
    glDrawElements(GL_TRIANGLES, sphereIndexCount, GL_UNSIGNED_INT, 0);

    glBindVertexArray(0);
    glUseProgram(0);
    glLineWidth(1.0f);
}

void Gizmo::getAxisEndpoints(int width, int height, const glm::quat& quat, float sidebarWidth,
                             float& xEndX, float& xEndY,
                             float& yEndX, float& yEndY,
                             float& zEndX, float& zEndY) {
    int gizmoSize = viewportSize;
    int margin = 16;
    int gizmoX = static_cast<int>(sidebarWidth) + margin;
    int gizmoY = margin;

    glm::mat4 view = Gizmo::getViewMatrix(quat);
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.01f, 100.0f);
    glm::mat4 mvp = proj * view;

    glm::vec4 xEnd = mvp * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    glm::vec4 yEnd = mvp * glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    glm::vec4 zEnd = mvp * glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);

    auto ndcToScreen = [&](const glm::vec4& pt, float& sx, float& sy) {
        float w = pt.w;
        float nx = pt.x / w;
        float ny = pt.y / w;

        // Translate NDC boundary frames coordinates over directly to Qt Canvas dimensions
        sx = gizmoX + (nx + 1.0f) * 0.5f * gizmoSize;
        sy = height - (gizmoY + (ny + 1.0f) * 0.5f * gizmoSize);
    };

    ndcToScreen(xEnd, xEndX, xEndY);
    ndcToScreen(yEnd, yEndX, yEndY);
    ndcToScreen(zEnd, zEndX, zEndY);
}

void Gizmo::drawWithMVP(const glm::mat4& mvp) {
    if (shaderProgram == 0) return;
    glUseProgram(shaderProgram);
    glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform1i(useUniformColorLoc, 0);
    glBindVertexArray(vao);
    glDrawArrays(GL_LINES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
}