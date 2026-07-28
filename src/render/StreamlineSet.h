#pragma once

#include <glad/gl.h>

#include <string>
#include <vector>

struct RenderMesh;

class StreamlineSet {
public:
    StreamlineSet() = default;
    ~StreamlineSet() = default;

    GLuint vao = 0;
    GLuint vbo = 0;
    int lineCount = 0; // vertex count

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

private:
    void teardownGL();
};