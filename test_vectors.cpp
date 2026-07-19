// Standalone correctness check for the VTK vector parsing + flatten pipeline.
// Compiles the REAL parser/loader/utility sources (no Qt, no GL).
#include "core/mesh_loader.h"
#include <cstdio>
#include <cmath>
#include <vector>

using namespace mesh_utils;

static int failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ printf("FAIL: %s\n", msg); ++failures; } } while(0)

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1]
        : "C:/Users/Neel/Downloads/unstructured_swirl_ascii.vtk";

    RenderMesh mesh = parseVTK(path);

    // 1. Points parsed. The parser reads 800 source points; flat-shading
    // (computeNormals) may later split them into more render vertices, so the
    // unambiguous "800 points were parsed" signal is the per-point vector
    // run length (one vec3 per source point).
    const int npts = (int)(mesh.vertices.size() / 3);
    printf("points=%d (render verts after flat-split) indices=%zu hasVectors=%d vectorName='%s'\n",
           npts, mesh.indices.size(), (int)mesh.meshHasVectors(), mesh.vectorName.c_str());
    CHECK(mesh.pointVectorsData.size() == 800, "parser must read 800 source points (per-point vector run)");
    CHECK(mesh.meshHasVectors(), "mesh should report vectors present");
    CHECK(!mesh.vectorName.empty(), "vectorName should be set");

    // 2. Available vector field
    CHECK(mesh.availableVectorNames.size() == 1, "expected exactly one vector field");
    CHECK(mesh.availableVectorNames[0] == "velocity", "field name should be 'velocity'");

    // 3. Flattened contiguous buffer holds 800 vec3
    size_t cnt = 0;
    const glm::vec3* data = mesh.vectorFieldData("velocity", cnt);
    CHECK(data != nullptr, "vectorFieldData('velocity') must resolve");
    CHECK(cnt == 800, "vector field run length must be 800");
    CHECK(mesh.pointVectorsData.size() == 800, "contiguous buffer must hold 800 vec3");

    // Vector loops are bounded by the VECTOR run count (cnt), not the (possibly
    // flat-shading-expanded) geometry vertex count — the glyph renderer does the
    // same via limit = min(numPts, count).
    const int nvec = static_cast<int>(cnt);

    // 4. Cross-check against the raw file: point i vertex vs vector i must agree
    //    with a tangential swirl about +Z plus constant +Z axial flow.
    //    Expected tangent (unit) at (x,y): (-y, x, 0)/r  (counterclockwise about +Z).
    int badTangential = 0, badAxial = 0;
    for (int i = 0; i < nvec; ++i) {
        float px = mesh.vertices[i*3+0], py = mesh.vertices[i*3+1], pz = mesh.vertices[i*3+2];
        glm::vec3 v = data[i];
        float r = std::sqrt(px*px + py*py);
        // axial component should be ~0.2
        if (std::fabs(v.z - 0.2f) > 1e-3f) ++badAxial;
        if (r > 1e-4f) {
            glm::vec3 tang(-py/r, px/r, 0.0f);
            float vmag = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
            if (vmag > 1e-6f) {
                float dot = (v.x*tang.x + v.y*tang.y + v.z*tang.z) / vmag;
                // tangential weight = sqrt(1 - (axial/vmag)^2); allow tolerance
                float tangWeight = std::sqrt(std::max(0.0f, 1.0f - (0.2f/vmag)*(0.2f/vmag)));
                if (dot < tangWeight - 0.05f) ++badTangential;
            }
        }
    }
    printf("badAxial=%d badTangential=%d\n", badAxial, badTangential);
    CHECK(badAxial == 0, "every vector must carry +0.2 axial (Z) component");
    CHECK(badTangential == 0, "in-plane vector component must be tangential to swirl");

    // 5. Magnitude range sanity (for colorbar/glyph scaling)
    float mMin = 1e30f, mMax = -1e30f;
    for (int i = 0; i < nvec; ++i) {
        glm::vec3 v = data[i];
        float m = std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);
        mMin = std::min(mMin, m); mMax = std::max(mMax, m);
    }
    printf("magMin=%.4f magMax=%.4f\n", mMin, mMax);
    CHECK(mMin > 0.1f && mMin < 0.3f, "min magnitude near 0.2 (origin pts)");
    CHECK(mMax > 0.4f && mMax < 0.7f, "max magnitude near 0.54 (outer ring)");

    if (failures == 0) printf("\nALL VECTOR PIPELINE CHECKS PASSED\n");
    else printf("\n%d CHECK(S) FAILED\n", failures);
    return failures;
}
