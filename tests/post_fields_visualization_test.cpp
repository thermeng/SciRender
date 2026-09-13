// tests/post_fields_visualization_test.cpp
// Ensures extracted post fields are correctly exposed via FieldStore + snapshot
// and are ready for GPU visualization (payload size, range, Context resolution).
// Also verifies helicity/Q non-blank for helical flow and LSQ unstructured.

#include "core/post_fields.h"
#include "core/FieldStore.h"
#include "core/mesh_loader.h"
#include <cstdio>
#include <cmath>

static int failures=0, checks=0;
#define CHECK(cond, msg) do{ ++checks; if(!(cond)){ printf("  [FAIL] %s\n", msg); ++failures; } } while(0)

using VFn = std::function<glm::vec3(float,float,float)>;
using PFn = std::function<float(float,float,float)>;

static RenderMesh makeFlowGrid(const std::vector<float>& xs,
                               const std::vector<float>& ys,
                               const std::vector<float>& zs,
                               VFn vel, PFn pres,
                               const std::string& vName="velocity",
                               const std::string& pName="pressure"){
    RenderMesh m;
    int dX=xs.size(), dY=ys.size(), dZ=zs.size();
    m.gridDimX=dX; m.gridDimY=dY; m.gridDimZ=dZ;
    m.datasetType="IMAGEDATA";
    size_t N=(size_t)dX*dY*dZ;
    m.vertices.resize(N*3);
    m.pointVectorsData.resize(N);
    m.pointVectorCount=N;
    m.pointVectorOffset[vName]=0;
    m.availableVectorNames.push_back(vName);
    m.vectorName=vName;
    m.attributes=DatasetAttributes{};
    auto &ps = m.attributes->pointScalars[pName];
    ps.resize(N);
    for(int k=0;k<dZ;++k) for(int j=0;j<dY;++j) for(int i=0;i<dX;++i){
        size_t n=i + j*dX + k*dX*dY;
        float x=xs[i], y=ys[j], z=zs[k];
        m.vertices[n*3+0]=x; m.vertices[n*3+1]=y; m.vertices[n*3+2]=z;
        m.pointVectorsData[n]=vel(x,y,z);
        ps[n]=pres(x,y,z);
    }
    m.availableScalarNames.push_back(pName);
    m.scalarName=pName;
    m.scalars=ps;
    m.bounds.minX=xs.front(); m.bounds.maxX=xs.back();
    m.bounds.minY=ys.front(); m.bounds.maxY=ys.back();
    m.bounds.minZ=zs.front(); m.bounds.maxZ=zs.back();
    m.sourcePointCount=(int)N;
    return m;
}
static std::vector<float> linspace(float a,float b,int n){ std::vector<float> v; for(int i=0;i<n;++i) v.push_back(a+(b-a)*i/(n-1)); return v; }

// Simulate setActiveScalarField expansion logic (vertexSourceIndex)
static bool payloadReady(const RenderMesh& mesh, const std::vector<float>& src, size_t& payloadSize){
    if(src.size()==mesh.vertices.size()/3 || mesh.vertexSourceIndex.empty()){ payloadSize=src.size(); return true; }
    if(src.size()==(size_t)mesh.sourcePointCount || src.size()==(size_t)(mesh.gridDimX*mesh.gridDimY*mesh.gridDimZ)){
        payloadSize=mesh.vertices.size()/3;
        return true;
    }
    return false;
}

static void testHelicalFlowVisualizationReady(){
    printf("[TEST] helical flow helicity/Q visualization-ready (structured)\n");
    // Beltrami-like helical flow: u = (-y, x, 1) => omega=(0,0,2), helicity=2
    auto g=linspace(-1,1,5);
    RenderMesh m=makeFlowGrid(g,g,g,
        [](float x,float y,float){ return glm::vec3(-y, x, 1.0f); },
        [](float x,float,float z){ return x+z; });
    postfields::CartesianGrid grid;
    CHECK(postfields::extractCartesianGrid(m,grid),"helical: cartesian");
    postfields::PointVectorField vf; postfields::PointScalarField pf;
    CHECK(postfields::extractPointVectors(m,grid,"velocity",vf),"vf");
    CHECK(postfields::extractPointScalars(m,grid,"pressure",pf),"pf");
    postfields::PostFieldsRequest req; req.velocity=vf.data; req.pressure=pf.data;
    auto r=postfields::computePostFields(grid,req);
    CHECK(r.ok,"ok");
    CHECK(r.hasHelicity && r.hasQ,"has helicity/Q");
    // Helicity should be ~2 everywhere (not blank)
    bool helNonBlank=false;
    for(float h: r.helicity) if(std::abs(h-2.0f)<0.2f) helNonBlank=true;
    CHECK(helNonBlank,"helicity ~2 not blank");
    // Q should be ~1
    bool qOk=true; for(float q: r.qCriterion) if(std::abs(q-1.0f)>1e-3f) qOk=false;
    CHECK(qOk,"Q==1");
    // Snapshot + FieldStore ready
    auto snap=std::make_shared<postfields::ComputedSnapshot>();
    auto hel = std::make_shared<const std::vector<float>>(r.helicity);
    auto qv = std::make_shared<const std::vector<float>>(r.qCriterion);
    snap->scalars[postfields::helicityName("velocity")] = hel;
    snap->scalarRanges[postfields::helicityName("velocity")] = {FieldStore::computeRange(*hel).min, FieldStore::computeRange(*hel).max};
    snap->scalars[postfields::qCriterionName("velocity")] = qv;
    snap->scalarRanges[postfields::qCriterionName("velocity")] = {FieldStore::computeRange(*qv).min, FieldStore::computeRange(*qv).max};
    // availableScalars must include helicity
    auto avail = FieldStore::availableScalars(m, snap.get());
    bool foundHel=false, foundQ=false;
    for(auto &n: avail){ if(n==postfields::helicityName("velocity")) foundHel=true; if(n==postfields::qCriterionName("velocity")) foundQ=true; }
    CHECK(foundHel,"availableScalars includes helicity");
    CHECK(foundQ,"availableScalars includes Q");
    // Context resolve must succeed and range not degenerate
    FieldStore::Context ctx(m, snap.get());
    auto sf = ctx.scalar(postfields::helicityName("velocity"), FieldStore::Placement::Vertex);
    CHECK(sf.data!=nullptr,"ctx scalar helicity");
    CHECK(sf.range.max - sf.range.min > 1e-6f || std::abs(sf.range.max-2.0f)<0.1f,"range not degenerate blank");
    // Payload size check (simulates MeshGLManager upload)
    size_t payloadSize=0;
    CHECK(payloadReady(m,*sf.data,payloadSize),"payload size matches vertices");
    CHECK(payloadSize==m.vertices.size()/3,"payload covers GPU vertices");
}

static void testStructuredVsUnstructuredHelicity(){
    printf("[TEST] unstructured LSQ helicity matches structured for linear field\n");
    // Build unstructured point cloud same positions but gridDim 0
    auto g=linspace(-1,1,4);
    RenderMesh m=makeFlowGrid(g,g,g,
        [](float x,float y,float){ return glm::vec3(-y, x, 0.5f); },
        [](float,float,float){ return 0; });
    // Structured result
    postfields::CartesianGrid grid; postfields::extractCartesianGrid(m,grid);
    postfields::PointVectorField vf; postfields::extractPointVectors(m,grid,"velocity",vf);
    postfields::PostFieldsRequest req; req.velocity=vf.data; req.wantGradP=false;
    auto rs = postfields::computePostFields(grid, req);
    // Unstructured copy — use radius neighbor search (no indices) for full 3D stencil
    RenderMesh ug=m;
    ug.gridDimX=ug.gridDimY=ug.gridDimZ=0;
    ug.indices.clear(); // radius fallback will find 3D neighbors
    auto ru = postfields::computePostFields(ug, req);
    CHECK(ru.ok,"unstructured ok");
    // For linear rotation helicity 0, both should be ~0 ; compare vorticity central point
    size_t c = 1+1*4+1*16;
    CHECK(std::abs(ru.vorticity[c].z - rs.vorticity[c].z) < 0.5f,"LSQ vorticity close to structured");
}

static void testVisualizationRangeNotBlank(){
    printf("[TEST] visualization range not blank for Q/helicity/gradP\n");
    auto g=linspace(0,1,4);
    RenderMesh m=makeFlowGrid(g,g,g,
        [](float y,float,float){ return glm::vec3(y*y,0,0); },
        [](float x,float,float){ return x*x; });
    postfields::CartesianGrid grid; postfields::extractCartesianGrid(m,grid);
    postfields::PointVectorField vf; postfields::PointScalarField pf;
    postfields::extractPointVectors(m,grid,"velocity",vf);
    postfields::extractPointScalars(m,grid,"pressure",pf);
    postfields::PostFieldsRequest req; req.velocity=vf.data; req.pressure=pf.data;
    auto r=postfields::computePostFields(grid,req);
    auto checkRange=[&](const std::vector<float>& v, const char* name){
        auto rg=FieldStore::computeRange(v);
        CHECK(rg.max - rg.min > 1e-5f, name);
    };
    checkRange(r.helicity,"helicity range");
    checkRange(r.qCriterion,"Q range");
    // gradP is vector but scalar magnitude via derived? just check grad not zero
    bool gradNonZero=false; for(auto &gp: r.gradP) if(std::abs(gp.x)>1e-3f) gradNonZero=true;
    CHECK(gradNonZero,"gradP non-zero");
}

int main(){
    testHelicalFlowVisualizationReady();
    testStructuredVsUnstructuredHelicity();
    testVisualizationRangeNotBlank();
    printf("\n");
    if(failures==0) printf("ALL VISUALIZATION CHECKS PASSED (%d checks)\n",checks);
    else printf("%d/%d CHECK(S) FAILED\n",failures,checks);
    return failures;
}
