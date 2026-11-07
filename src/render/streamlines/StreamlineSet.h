#pragma once

#include "render/foundation/gl_raii.h"
#include "core/post_fields.h"

#include <glm/glm.hpp>

#include <random>
#include <string>
#include <vector>

struct RenderMesh;

class StreamlineSet {
public:
    StreamlineSet() = default;
    ~StreamlineSet() = default;

    GlVao vao;
    GlBuffer vbo;
    int lineCount = 0;
    bool renderAsLines = false;

    GlVao seedVao;
    GlBuffer seedVbo;
    int seedCount = 0;

    GlVao arrowVao;
    GlBuffer arrowVbo;
    int arrowCount = 0;

    float magMin = 0.0f;
    float magMax = 1.0f;
    float compMin[3] = { 0.0f, 0.0f, 0.0f };
    float compMax[3] = { 0.0f, 0.0f, 0.0f };

    struct StreamlinePath {
        std::vector<glm::vec3> points;
        std::vector<float> speedAtPoint;
        std::vector<float> cumulativeLength;
        float totalLength = 0.0f;
    };
    std::vector<StreamlinePath> paths;

    struct Particle {
        int pathIndex = 0;
        float t = 0.0f;
    };
    std::vector<Particle> particles;
    std::mt19937 particleRng{ std::random_device{}() };

    bool empty() const { return lineCount == 0; }
    bool seedsEmpty() const { return seedCount == 0; }

    struct StreamlineResult {
        std::vector<float> verts;
        std::vector<float> seedVerts;
        std::vector<float> arrowVerts;
        std::vector<StreamlinePath> paths;
         int seedCount = 0;
         int lineCount = 0;
         int arrowCount = 0;
         float magMin = 0.0f;
         float magMax = 1.0f;
         float compMin[3] = { 0.0f, 0.0f, 0.0f };
         float compMax[3] = { 0.0f, 0.0f, 0.0f };
         float tubeBaseWidth = 0.005f;
         int style = 1;
     };

    // Static entry point: no `this` state, safe to call from a worker
    // thread while the render thread owns the live StreamlineSet.
    static StreamlineResult computeStatic(const RenderMesh& mesh, int seedCountParam, float stepSize, int maxSteps,
                             const std::string& fieldName, const std::string& mode, const std::string& direction,
                             double planePos, double jitter, int planeCountU, int planeCountV,
                             bool showArrows, float arrowSpacingFrac, float arrowSize,
                             float ribbonWidth, float taperFactor, int style = 1, float maxPathLength = 0.0f,
                             const postfields::ComputedSnapshot* snap = nullptr);

    StreamlineResult compute(const RenderMesh& mesh, int seedCountParam, float stepSize, int maxSteps,
                             const std::string& fieldName, const std::string& mode, const std::string& direction,
                             double planePos, double jitter, int planeCountU, int planeCountV,
                                  bool showArrows, float arrowSpacingFrac, float arrowSize,
                                  float ribbonWidth, float taperFactor, int style = 1, float maxPathLength = 0.0f,
                                  const postfields::ComputedSnapshot* snap = nullptr);

    void uploadGL(StreamlineResult&& result, bool showArrows, float arrowSize);

    void rebuild(const RenderMesh& mesh, int seedCountParam, float stepSize, int maxSteps,
                 const std::string& fieldName, const std::string& mode, const std::string& direction,
                 double planePos, double jitter, int planeCountU, int planeCountV,
                 bool showArrows, float arrowSpacingFrac, float arrowSize,
                  float ribbonWidth, float taperFactor, int style = 1, float maxPathLength = 0.0f,
                  const postfields::ComputedSnapshot* snap = nullptr);

    void initParticles(int count);
    void clearParticles();
    void updateParticles(float dt, float speed);
    void buildParticleVertices(std::vector<float>& outVerts);

    void shutdown();

    static float magSq(const glm::vec3& v);
    float tubeBaseWidth() const { return m_tubeBaseWidth; }
    float m_tubeBaseWidth = 0.005f;
    static void samplePath(const StreamlinePath& path, float t, glm::vec3& outPos, float& outMag);
    static glm::mat3 buildFrame(const glm::vec3& dir);
    static std::vector<float> generateArrowhead(const glm::vec3& pos, const glm::vec3& dir, float height, float radius, int segments, float mag, const glm::vec3& comp);
    // Logical arrowhead placement: evenly distributed by arc length with tip,
    // taper, and anti-overlap guards. Returns arc distances from the path start.
    static std::vector<float> computeArrowPlacement(float pathLength, float extent,
                                                    float spacingFraction, float arrowSize,
                                                    float taperFactor);
    static std::vector<glm::vec3> generateSeeds(const RenderMesh& mesh, int seedCount, const std::string& mode, double planePos, double jitter,
                                                int planeCountU = 10, int planeCountV = 10,
                                                const std::vector<glm::vec3>* positionsOverride = nullptr);

    struct StructuredGridInfo {
        std::vector<float> xs, ys, zs;
        int dimX = 0, dimY = 0, dimZ = 0;
        const glm::vec3* data = nullptr;
        int count = 0;
        std::vector<char> cellActive;
        const float* verts = nullptr;
        int vertCount = 0;
        float avgSpacing = 1.0f;
        std::vector<float> posStorage; // owns verts when they are not mesh.vertices
    };

    static StructuredGridInfo buildStructuredGridInfo(const RenderMesh& mesh, const glm::vec3* data, int count);
    static StructuredGridInfo buildStructuredGridInfo(const std::vector<glm::vec3>& positions, int dimX, int dimY, int dimZ,
                                                     const glm::vec3* data, int count);
    static glm::vec3 evalFieldTrilinear(const StructuredGridInfo& info, const glm::vec3& pos);
    static bool isInsideDomain(const StructuredGridInfo& info, const glm::vec3& pos);
    static glm::vec3 evalFieldNearest(const RenderMesh& mesh, const glm::vec3& pos, const glm::vec3* data, int count, int searchCount,
                                      const std::vector<glm::vec3>* positionsOverride = nullptr);

    void teardownParticles();

private:
    void teardownGL();
};

