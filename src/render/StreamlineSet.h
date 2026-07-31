#pragma once

#include <glad/gl.h>

#include <glm/glm.hpp>

#include <random>
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

    // Stored streamline paths for particle animation.
    struct StreamlinePath {
        std::vector<glm::vec3> points;
        std::vector<float> speedAtPoint;
        float totalLength = 0.0f;
    };
    std::vector<StreamlinePath> paths;

    // Particle animation state
    struct Particle {
        int pathIndex = 0;
        float t = 0.0f;
    };
    std::vector<Particle> particles;
    std::mt19937 particleRng{ std::random_device{}() };

    bool empty() const { return lineCount == 0; }
    bool seedsEmpty() const { return seedCount == 0; }

    // CPU-side result of streamline computation (no GL state).
    // Produced by compute() on any thread, consumed by uploadGL() on the GL thread.
    struct StreamlineResult {
        std::vector<float> verts;           // interleaved ribbon vertices
        std::vector<float> seedVerts;       // [x,y,z] per seed
        std::vector<float> arrowVerts;      // interleaved arrowhead vertices
        std::vector<StreamlinePath> paths;
        int seedCount = 0;
        int lineCount = 0;
        int arrowCount = 0;
        float magMin = 0.0f;
        float magMax = 0.0f;
    };

    // CPU-heavy computation: seed generation, filtering, RK4 tracing, vertex
    // building.  Runs on any thread (no GL calls).  Returns a result ready
    // for uploadGL().
    StreamlineResult compute(const RenderMesh& mesh, int seedCountParam, float stepSize, int maxSteps,
                             const std::string& fieldName, const std::string& mode,
                             double planePos, double jitter, int planeCountU, int planeCountV,
                             bool showArrows, int arrowSpacing, float arrowSize,
                             float ribbonWidth, float taperFactor);

    // GL upload: teardown old buffers, create new VAOs/VBOs, upload data.
    // Must be called on the GL thread.
    void uploadGL(StreamlineResult&& result, bool showArrows, float arrowSize);

    void rebuild(const RenderMesh& mesh, int seedCountParam, float stepSize, int maxSteps,
                 const std::string& fieldName, const std::string& mode,
                 double planePos, double jitter, int planeCountU, int planeCountV,
                 bool showArrows, int arrowSpacing, float arrowSize,
                 float ribbonWidth, float taperFactor);

    void shutdown();

    static float magSq(const glm::vec3& v);
    static glm::mat3 buildFrame(const glm::vec3& dir);
    static std::vector<float> generateArrowhead(const glm::vec3& pos, const glm::vec3& dir, float height, float radius, int segments, float mag);
    static std::vector<glm::vec3> generateSeeds(const RenderMesh& mesh, int seedCount, const std::string& mode, double planePos, double jitter, int planeCountU = 10, int planeCountV = 10);

    struct StructuredGridInfo {
        std::vector<float> xs, ys, zs;
        int dimX = 0, dimY = 0, dimZ = 0;
        const glm::vec3* data = nullptr;
        int count = 0;
        std::vector<bool> cellActive; // (dimX-1)*(dimY-1)*(dimZ-1) bitmask
        const float* verts = nullptr; // vertex positions (x,y,z interleaved) for non-Cartesian fallback
        int vertCount = 0;
        float avgSpacing = 1.0f; // estimated grid spacing for distance cutoffs
    };

    static StructuredGridInfo buildStructuredGridInfo(const RenderMesh& mesh, const glm::vec3* data, int count);
    static glm::vec3 evalFieldTrilinear(const StructuredGridInfo& info, const glm::vec3& pos);
    static bool isInsideDomain(const StructuredGridInfo& info, const glm::vec3& pos);
    static glm::vec3 evalFieldNearest(const RenderMesh& mesh, const glm::vec3& pos, const glm::vec3* data, int count, int searchCount);

    void initParticles(int count);
    void updateParticles(float dt, float speed);
    void buildParticleVertices(std::vector<float>& outVerts);
    void teardownParticles();

private:
    void teardownGL();
};