#pragma once

#include <glad/gl.h>

#include <glm/glm.hpp>

#include <string>
#include <vector>

struct RenderMesh;

class StreamlineSet {
public:
    StreamlineSet() = default;
    ~StreamlineSet() = default;

    GLuint vao = 0;
    GLuint vbo = 0;
    int lineCount = 0;

    GLuint seedVao = 0;
    GLuint seedVbo = 0;
    int seedCount = 0;

    GLuint arrowVao = 0;
    GLuint arrowVbo = 0;
    int arrowCount = 0;

    float magMin = 0.0f;
    float magMax = 1.0f;

    bool empty() const { return lineCount == 0; }
    bool seedsEmpty() const { return seedCount == 0; }

    void rebuild(const RenderMesh& mesh, int seedCount, float stepSize, int maxSteps,
                 const std::string& fieldName, const std::string& mode,
                 double planePos, double jitter, bool showArrows, int arrowSpacing, float arrowSize,
                 float ribbonWidth, float taperFactor);

    void shutdown();

    static float magSq(const glm::vec3& v);
    static glm::mat3 buildFrame(const glm::vec3& dir);
    static std::vector<float> generateArrowhead(const glm::vec3& pos, const glm::vec3& dir, float height, float radius, int segments, float mag);
    static std::vector<glm::vec3> generateSeeds(const RenderMesh& mesh, int seedCount, const std::string& mode, double planePos, double jitter);

    struct StructuredGridInfo {
        std::vector<float> xs, ys, zs;
        int dimX = 0, dimY = 0, dimZ = 0;
        const glm::vec3* data = nullptr;
        int count = 0;
    };

    static StructuredGridInfo buildStructuredGridInfo(const RenderMesh& mesh, const glm::vec3* data, int count);
    static glm::vec3 evalFieldTrilinear(const StructuredGridInfo& info, const glm::vec3& pos);
    static glm::vec3 evalFieldNearest(const RenderMesh& mesh, const glm::vec3& pos, const glm::vec3* data, int count, int searchCount);

private:
    void teardownGL();
};