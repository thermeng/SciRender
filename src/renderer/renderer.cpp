#include "renderer/renderer.h"
#include "colormaps/Colormaps.h"
#include "camera/Camera.h"
#include "export/screenshot.h"
#include "mesh/mesh_loader.h"
#include <QOpenGLFramebufferObject>

#include <cstring>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <limits>
#include <unordered_map>
#include <QImage>
#include <QBuffer>
#include <QTimer>

// kit-wide warm tint (shared by light colors + viewport markers).
// 0 = cold blue, 0.5 = neutral white, 1 = warm red.
static glm::vec3 warmColorTint(float w) {
    if (w < 0.5f) return glm::mix(glm::vec3(0.6f, 0.7f, 1.0f), glm::vec3(1.0f, 1.0f, 1.0f), w / 0.5f);
    return glm::mix(glm::vec3(1.0f, 1.0f, 1.0f), glm::vec3(1.0f, 0.85f, 0.7f), (w - 0.5f) / 0.5f);
}

// Qt Core & UI Utilities replacing Win32 APIs
#include <QDir>
#include <QQuickOpenGLUtils>
#include <QFileInfo>
#include <QOpenGLContext>
#include <QSettings>

static bool compileShader(GLuint shader, const char* source, const char* type) {
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        printf("Shader compile error (%s): %s\n", type, log);
        return false;
    }
    return true;
}

Renderer::Renderer(QObject* parent)
    : QObject(parent), width(800), height(600), hasMeshLoaded(false) {

    // Default system initialization parameters
    meshColor[0] = 0.4f; meshColor[1] = 0.9f; meshColor[2] = 0.4f;
    surfaceColor[0] = 1.0f; surfaceColor[1] = 1.0f; surfaceColor[2] = 1.0f;
    bgColor[0] = 0.12f; bgColor[1] = 0.12f; bgColor[2] = 0.12f;

    worldCenterX = 0.0; worldCenterY = 0.0; worldCenterZ = 0.0;
    worldRadius = 1.0;

    // LOD debounce: when the camera stops moving, this fires once and forces a
    // final repaint with the full-resolution mesh.
    m_lodTimer = new QTimer(this);
    m_lodTimer->setSingleShot(true);
    m_lodTimer->setInterval(140);
    connect(m_lodTimer, &QTimer::timeout, this, &Renderer::onLodTimer);

    loadRecentFromSettings();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
Renderer::~Renderer() {
    m_destroying = true; // suppress signal emissions during teardown
    clearMeshes();
    // GL deletes must only run with a current context. The Renderer lives on the
    // GUI thread and is torn down after the scene graph, so no GL context is
    // current here — calling glDelete* would emit GL errors and leak nothing
    // recoverable. Skip them; the driver reclaims the resources with the context.
    if (QOpenGLContext::currentContext()) {
        if (shaderProgram) glDeleteProgram(shaderProgram);
        if (gridProgram) glDeleteProgram(gridProgram);
        if (gridVAO) glDeleteVertexArrays(1, &gridVAO);
        if (gridVBO) glDeleteBuffers(1, &gridVBO);
        if (colormapTex) glDeleteTextures(1, &colormapTex);
        if (vectorColormapTex) glDeleteTextures(1, &vectorColormapTex);
        gizmo.shutdown();
    }
}

#pragma GCC diagnostic pop

void Renderer::initGLAD() {
    // Harvest the current active OpenGL context created by the Qt Quick Scene Graph thread
    QOpenGLContext* currentContext = QOpenGLContext::currentContext();
    if (!currentContext) {
        qFatal("Fatal: initGLAD called but no active Qt OpenGL context was found on this thread.");
        return;
    }

    // Pass the lambda directly as a function pointer without any reinterpret_cast.
    // The compiler automatically converts a non-capturing lambda into a matching raw function pointer.
    GLADloadproc loader = [](const char* name) -> void* {
        QOpenGLContext* ctx = QOpenGLContext::currentContext();
        return ctx ? reinterpret_cast<void*>(ctx->getProcAddress(name)) : nullptr;
    };

    if (!gladLoadGLLoader(loader)) {
        qFatal("Fatal: GLAD failed to map target core OpenGL function addresses using Qt resolver hook.");
    }

    // diagnostic to confirm desktop vs ANGLE/ES context after launch.
    qDebug() << "[GL DIAGNOSTIC] VERSION:" << (const char*)glGetString(GL_VERSION)
             << "| RENDERER:" << (const char*)glGetString(GL_RENDERER)
             << "| GLSL:" << (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
}

std::string Renderer::readShaderFile(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "Failed to open shader source file: " << filePath << std::endl;
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void Renderer::initShaders() {
    // Helper lambda to safely read the shader text straight from the QRC binary resource package
    auto loadEmbeddedShader = [](const QString& rscPath) -> std::string {
        QFile file(rscPath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCritical() << "Fatal Error: Required engine shader asset missing at path:" << rscPath;
            return "";
        }
        QTextStream stream(&file);
        return stream.readAll().toStdString();
    };

    // Pull sources via the virtual module path generated by qt_add_qml_module
    std::string vertSrcStr = loadEmbeddedShader(":/RendererQTUI/src/shaders/mesh.vert");
    std::string fragSrcStr = loadEmbeddedShader(":/RendererQTUI/src/shaders/mesh.frag");

    // Defensive crash guard against empty file reads
    if (vertSrcStr.empty() || fragSrcStr.empty()) {
        qFatal("Shader compilation aborted due to unreadable file streams.");
        return;
    }

    const char* vertSrc = vertSrcStr.c_str();
    const char* fragSrc = fragSrcStr.c_str();

    GLuint vert = glCreateShader(GL_VERTEX_SHADER);
    GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);

    if (!compileShader(vert, vertSrc, "VERTEX") || !compileShader(frag, fragSrc, "FRAGMENT")) {
        glDeleteShader(vert);
        glDeleteShader(frag);
        return;
    }

    shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vert);
    glAttachShader(shaderProgram, frag);
    glLinkProgram(shaderProgram);

    GLint success;
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(shaderProgram, 512, nullptr, log);
        printf("Shader program linking error: %s\n", log);
    }

    glDeleteShader(vert);
    glDeleteShader(frag);

    // Uniform Caching Passes
    // NOTE: Names MUST match the GLSL declarations in src/shaders/mesh.{vert,frag}
    mvpLoc = glGetUniformLocation(shaderProgram, "uMVP");
    modelLoc = glGetUniformLocation(shaderProgram, "uModel");
    viewLoc = glGetUniformLocation(shaderProgram, "uView");
    lightDirLoc = glGetUniformLocation(shaderProgram, "uLightDir");
    viewPosLoc = glGetUniformLocation(shaderProgram, "uViewPos");
    wireframeLoc = glGetUniformLocation(shaderProgram, "uWireframe");
    colorLoc = glGetUniformLocation(shaderProgram, "uMeshColor");
    surfaceColorLoc = glGetUniformLocation(shaderProgram, "uSurfaceColor");

    lightFillLoc = glGetUniformLocation(shaderProgram, "uLightFill");
    lightBack1Loc = glGetUniformLocation(shaderProgram, "uLightBack1");
    lightBack2Loc = glGetUniformLocation(shaderProgram, "uLightBack2");
    lightHeadLoc = glGetUniformLocation(shaderProgram, "uLightHead");

    matAmbientLoc = glGetUniformLocation(shaderProgram, "uMatAmbient");
    matDiffuseLoc = glGetUniformLocation(shaderProgram, "uMatDiffuse");
    matSpecularLoc = glGetUniformLocation(shaderProgram, "uMatSpecular");
    matShininessLoc = glGetUniformLocation(shaderProgram, "uMatShininess");

    keyIntensityLoc = glGetUniformLocation(shaderProgram, "uKeyIntensity");
    fillIntensityLoc = glGetUniformLocation(shaderProgram, "uFillIntensity");
    headIntensityLoc = glGetUniformLocation(shaderProgram, "uHeadIntensity");
    backIntensityLoc = glGetUniformLocation(shaderProgram, "uBackIntensity");

    keyColorLoc = glGetUniformLocation(shaderProgram, "uKeyColor");
    fillColorLoc = glGetUniformLocation(shaderProgram, "uFillColor");
    backColorLoc = glGetUniformLocation(shaderProgram, "uBackColor");
    headColorLoc = glGetUniformLocation(shaderProgram, "uHeadColor");

    sliceHeightXLoc = glGetUniformLocation(shaderProgram, "uSliceHeightX");
    sliceHeightYLoc = glGetUniformLocation(shaderProgram, "uSliceHeightY");
    sliceHeightZLoc = glGetUniformLocation(shaderProgram, "uSliceHeightZ");
    invertXLoc = glGetUniformLocation(shaderProgram, "uInvertX");
    invertYLoc = glGetUniformLocation(shaderProgram, "uInvertY");
    invertZLoc = glGetUniformLocation(shaderProgram, "uInvertZ");
    filterMinLoc = glGetUniformLocation(shaderProgram, "uFilterMin");
    filterMaxLoc = glGetUniformLocation(shaderProgram, "uFilterMax");
    clipEnabledLoc = glGetUniformLocation(shaderProgram, "uClipEnabled");

    scalarMinLoc = glGetUniformLocation(shaderProgram, "uScalarMin");
    scalarMaxLoc = glGetUniformLocation(shaderProgram, "uScalarMax");
    hasScalarsLoc = glGetUniformLocation(shaderProgram, "uHasScalars");
    lutTextureLoc = glGetUniformLocation(shaderProgram, "uColormapLUT");

    // instanced vector glyph program
    std::string gvert = loadEmbeddedShader(":/RendererQTUI/src/shaders/glyph.vert");
    std::string gfrag = loadEmbeddedShader(":/RendererQTUI/src/shaders/glyph.frag");
    if (!gvert.empty() && !gfrag.empty()) {
        GLuint gv = glCreateShader(GL_VERTEX_SHADER);
        GLuint gf = glCreateShader(GL_FRAGMENT_SHADER);
        const char* gvs = gvert.c_str();
        const char* gfs = gfrag.c_str();
        glShaderSource(gv, 1, &gvs, nullptr); glCompileShader(gv);
        glShaderSource(gf, 1, &gfs, nullptr); glCompileShader(gf);
        glyphProgram = glCreateProgram();
        glAttachShader(glyphProgram, gv); glAttachShader(glyphProgram, gf);
        glLinkProgram(glyphProgram);
        glDeleteShader(gv); glDeleteShader(gf);
        glyphMvpLoc = glGetUniformLocation(glyphProgram, "uMVP");
        glyphScaleLoc = glGetUniformLocation(glyphProgram, "uScale");
        glyphLightDirLoc = glGetUniformLocation(glyphProgram, "uLightDir");
        glyphViewPosLoc = glGetUniformLocation(glyphProgram, "uViewPos");
        glyphColorLoc = glGetUniformLocation(glyphProgram, "uColor");
        glyphUseColormapLoc = glGetUniformLocation(glyphProgram, "uUseColormap");
        glyphMagMinLoc = glGetUniformLocation(glyphProgram, "uMagMin");
        glyphMagMaxLoc = glGetUniformLocation(glyphProgram, "uMagMax");
        glyphLutLoc = glGetUniformLocation(glyphProgram, "uColormapLUT");
    }
}

void Renderer::initGrid() {
    auto load = [](const QString& p) -> std::string {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { qCritical() << "grid shader missing" << p; return ""; }
        return QTextStream(&f).readAll().toStdString();
    };
    std::string vs = load(":/RendererQTUI/src/shaders/grid.vert");
    std::string fs = load(":/RendererQTUI/src/shaders/grid.frag");
    if (vs.empty() || fs.empty()) return;

    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    if (!compileShader(v, vs.c_str(), "GRID_VERT") || !compileShader(f, fs.c_str(), "GRID_FRAG")) {
        glDeleteShader(v); glDeleteShader(f); return;
    }
    gridProgram = glCreateProgram();
    glAttachShader(gridProgram, v);
    glAttachShader(gridProgram, f);
    glLinkProgram(gridProgram);
    glDeleteShader(v); glDeleteShader(f);

    gridInvViewLoc = glGetUniformLocation(gridProgram, "uInvView");
    gridInvProjLoc = glGetUniformLocation(gridProgram, "uInvProj");
    gridViewLoc    = glGetUniformLocation(gridProgram, "uView");
    gridProjLoc    = glGetUniformLocation(gridProgram, "uProj");
    gridCamPosLoc  = glGetUniformLocation(gridProgram, "uCamPos");
    gridColorLoc   = glGetUniformLocation(gridProgram, "uColor");
    gridBgLoc      = glGetUniformLocation(gridProgram, "uBg");
    gridFalloffLoc = glGetUniformLocation(gridProgram, "uFalloff");
    gridPlaneYLoc  = glGetUniformLocation(gridProgram, "uPlaneY");

    const float q[8] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f };
    glGenVertexArrays(1, &gridVAO);
    glGenBuffers(1, &gridVBO);
    glBindVertexArray(gridVAO);
    glBindBuffer(GL_ARRAY_BUFFER, gridVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void Renderer::drawGrid(const glm::mat4& view, const glm::mat4& proj) {
    if (!showGrid || gridProgram == 0) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // Force filled: the wireframe pass may have left GL_POLYGON_MODE as GL_LINE,
    // which would rasterize the fullscreen grid quad as just its edges (grid vanishes).
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glUseProgram(gridProgram);

    glm::mat4 invView = glm::inverse(view);
    glm::mat4 invProj = glm::inverse(proj);
    glUniformMatrix4fv(gridInvViewLoc, 1, GL_FALSE, glm::value_ptr(invView));
    glUniformMatrix4fv(gridInvProjLoc, 1, GL_FALSE, glm::value_ptr(invProj));
    glUniformMatrix4fv(gridViewLoc, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(gridProjLoc, 1, GL_FALSE, glm::value_ptr(proj));
    glUniform3f(gridCamPosLoc, (float)camera.position.x, (float)camera.position.y, (float)camera.position.z);
    // Pick a grid-line color that contrasts with the background so it never
    // blends into it when the user changes the background color.
    float bgLum = 0.299f * bgColor[0] + 0.587f * bgColor[1] + 0.114f * bgColor[2];
    glm::vec3 gridCol = (bgLum > 0.5f) ? glm::vec3(0.18f, 0.18f, 0.20f)
                                       : glm::vec3(0.78f, 0.78f, 0.82f);
    glUniform3f(gridColorLoc, gridCol.r, gridCol.g, gridCol.b);
    glUniform3f(gridBgLoc, bgColor[0], bgColor[1], bgColor[2]);
    glUniform1f(gridFalloffLoc, 0.02f);
    glUniform1f(gridPlaneYLoc, static_cast<float>(gridPlaneY));

    glBindVertexArray(gridVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glUseProgram(0);
}

void Renderer::toggleGrid(bool visible) {
    if (showGrid == visible) return;
    showGrid = visible;
    emit gridVisibilityChanged();
}

void Renderer::initGizmo() {
    gizmo.init();
}

void Renderer::buildMeshGL(const RenderMesh& renderMesh, std::vector<Mesh>& out) {
    Mesh mesh;
    mesh.indexCount = static_cast<int>(renderMesh.indices.size());

    glGenVertexArrays(1, &mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glGenBuffers(1, &mesh.nbo);
    glGenBuffers(1, &mesh.ebo);

    glBindVertexArray(mesh.vao);

    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, renderMesh.vertices.size() * sizeof(float), renderMesh.vertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, mesh.nbo);
    glBufferData(GL_ARRAY_BUFFER, renderMesh.normals.size() * sizeof(float), renderMesh.normals.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);

    if (!renderMesh.scalars.empty()) {
        glGenBuffers(1, &mesh.sbo);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.sbo);
        glBufferData(GL_ARRAY_BUFFER, renderMesh.scalars.size() * sizeof(float), renderMesh.scalars.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(float), (void*)0);
        glEnableVertexAttribArray(2);
        meshHasScalars = true;
    } else {
        mesh.sbo = 0;
        meshHasScalars = false;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, renderMesh.indices.size() * sizeof(unsigned int), renderMesh.indices.data(), GL_STATIC_DRAW);

    glBindVertexArray(0);
    out.push_back(mesh);
}

void Renderer::uploadMesh(const RenderMesh& renderMesh) {
    std::vector<Mesh> newFull, newDec;

    // Full-resolution mesh.
    buildMeshGL(renderMesh, newFull);

    // LOD: a coarsely decimated mesh, used only while the camera is moving.
    RenderMesh decimated = decimate(renderMesh);
    bool lodWorthwhile = !decimated.indices.empty() &&
                         decimated.indices.size() < renderMesh.indices.size() / 2;
    if (lodWorthwhile) buildMeshGL(decimated, newDec);

    // WIPE OUT OLD OPENGL HANDLES BEFORE GENERATING NEW ONES
    // Guarded by meshGLMutex so it cannot race with clearMeshes() on the UI thread.
    {
        std::lock_guard<std::mutex> lock(meshGLMutex);
        for (auto& m : meshes) destroyMesh(m);
        meshes.clear();
        for (auto& m : decimatedMeshes) destroyMesh(m);
        decimatedMeshes.clear();

        meshes = std::move(newFull);
        decimatedMeshes = std::move(newDec);
        hasDecimated = !decimatedMeshes.empty();
    }

    // (re)build instanced vector arrow glyphs from the cached source mesh
    rebuildVectorGlyphs();
}

void Renderer::markCameraMoving() {
    cameraMoving = true;
    if (m_lodTimer) m_lodTimer->start(); // (re)arm: fires 140ms after motion stops
}

void Renderer::onLodTimer() {
    cameraMoving = false;
    emit viewChanged(); // one last repaint, now with the full mesh
}

RenderMesh Renderer::decimate(const RenderMesh& in) const {
    RenderMesh out;
    const size_t nv = in.vertices.size() / 3;
    // Small meshes gain nothing from LOD and risk degeneracy — skip them.
    if (nv < 4000 || in.indices.size() < 3) return out;

    const double minX = in.bounds.minX, minY = in.bounds.minY, minZ = in.bounds.minZ;
    const double dx = in.bounds.maxX - minX, dy = in.bounds.maxY - minY, dz = in.bounds.maxZ - minZ;
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (diag < 1e-9) return out;

    // Cells per axis chosen so the cluster count is a coarse fraction of the
    // vertices (~half the "one cell per vertex" resolution => ~1/8th vertices).
    int cellsPerAxis = static_cast<int>(std::round(std::pow((double)nv, 1.0 / 3.0) * 0.5));
    cellsPerAxis = std::max(2, std::min(cellsPerAxis, 512));
    const double cell = diag / cellsPerAxis;

    auto clampCell = [&](double v, int n) {
        int i = static_cast<int>(std::floor(v / cell));
        if (i < 0) i = 0; else if (i >= n) i = n - 1;
        return i;
    };
    auto keyFor = [&](size_t i) -> uint64_t {
        const int ci = clampCell(in.vertices[3 * i + 0] - minX, cellsPerAxis);
        const int cj = clampCell(in.vertices[3 * i + 1] - minY, cellsPerAxis);
        const int ck = clampCell(in.vertices[3 * i + 2] - minZ, cellsPerAxis);
        return static_cast<uint64_t>(ci)
             | (static_cast<uint64_t>(cj) << 20)
             | (static_cast<uint64_t>(ck) << 40);
    };

    std::unordered_map<uint64_t, int> cellToNew;
    std::vector<int> remap(nv, -1);
    std::vector<double> sx, sy, sz, nx, ny, nz, sc, cnt;
    const bool hasS = !in.scalars.empty();

    for (size_t i = 0; i < nv; ++i) {
        const uint64_t k = keyFor(i);
        auto it = cellToNew.find(k);
        int newIdx;
        if (it == cellToNew.end()) {
            newIdx = static_cast<int>(cellToNew.size());
            cellToNew[k] = newIdx;
            sx.push_back(0.0); sy.push_back(0.0); sz.push_back(0.0);
            nx.push_back(0.0); ny.push_back(0.0); nz.push_back(0.0);
            sc.push_back(0.0); cnt.push_back(0.0);
        } else {
            newIdx = it->second;
        }
        remap[i] = newIdx;
        sx[newIdx] += in.vertices[3 * i + 0];
        sy[newIdx] += in.vertices[3 * i + 1];
        sz[newIdx] += in.vertices[3 * i + 2];
        nx[newIdx] += in.normals[3 * i + 0];
        ny[newIdx] += in.normals[3 * i + 1];
        nz[newIdx] += in.normals[3 * i + 2];
        if (hasS) sc[newIdx] += in.scalars[i];
        cnt[newIdx] += 1.0;
    }

    const int newCount = static_cast<int>(cellToNew.size());
    out.vertices.resize(static_cast<size_t>(newCount) * 3);
    out.normals.resize(static_cast<size_t>(newCount) * 3);
    if (hasS) out.scalars.resize(newCount);

    for (int i = 0; i < newCount; ++i) {
        const double inv = 1.0 / cnt[i];
        out.vertices[3 * i + 0] = static_cast<float>(sx[i] * inv);
        out.vertices[3 * i + 1] = static_cast<float>(sy[i] * inv);
        out.vertices[3 * i + 2] = static_cast<float>(sz[i] * inv);
        double nl = std::sqrt(nx[i] * nx[i] + ny[i] * ny[i] + nz[i] * nz[i]);
        if (nl > 1e-12) { nl = 1.0 / nl; } else { nl = 0.0; }
        out.normals[3 * i + 0] = static_cast<float>(nx[i] * nl);
        out.normals[3 * i + 1] = static_cast<float>(ny[i] * nl);
        out.normals[3 * i + 2] = static_cast<float>(nz[i] * nl);
        if (hasS) out.scalars[i] = static_cast<float>(sc[i] * inv);
    }

    // Remap triangles, dropping any that collapsed into a single cell.
    out.indices.reserve(in.indices.size());
    for (size_t t = 0; t + 2 < in.indices.size(); t += 3) {
        const int a = remap[in.indices[t]];
        const int b = remap[in.indices[t + 1]];
        const int c = remap[in.indices[t + 2]];
        if (a == b || b == c || a == c) continue;
        out.indices.push_back(a);
        out.indices.push_back(b);
        out.indices.push_back(c);
    }

    out.bounds = in.bounds; // vertices stay within the same box
    return out;
}

void Renderer::destroyMesh(Mesh& mesh) {
    glDeleteVertexArrays(1, &mesh.vao);
    glDeleteBuffers(1, &mesh.vbo);
    glDeleteBuffers(1, &mesh.nbo);
    glDeleteBuffers(1, &mesh.ebo);
    if (mesh.sbo) glDeleteBuffers(1, &mesh.sbo);
}

// build a unit arrow (local space, arrow along +Y, height 1) and a
// per-point instance buffer [ox,oy,oz, dx,dy,dz] from the active vector field.
static void buildUnitArrow(std::vector<float>& verts, std::vector<float>& norms, std::vector<unsigned int>& idx) {
    const int SEG = 8;
    const float rs = 0.04f, rh = 0.12f, yHead = 0.75f;
    for (int i = 0; i < SEG; ++i) {
        float a0 = (i / (float)SEG) * 2.0f * 3.14159265f;
        float a1 = ((i + 1) / (float)SEG) * 2.0f * 3.14159265f;
        int b = (int)verts.size() / 3;
        auto P = [&](float y, float r, float ang) { verts.push_back(cosf(ang) * r); verts.push_back(y); verts.push_back(sinf(ang) * r); };
        auto N = [&](float ang) { norms.push_back(cosf(ang)); norms.push_back(0.0f); norms.push_back(sinf(ang)); };
        P(0, rs, a0); N(a0); P(0, rs, a1); N(a1); P(yHead, rs, a1); N(a1); P(yHead, rs, a0); N(a0);
        idx.insert(idx.end(), { (unsigned)b, (unsigned)b + 1, (unsigned)b + 2, (unsigned)b, (unsigned)b + 2, (unsigned)b + 3 });
    }
    for (int i = 0; i < SEG; ++i) {
        float a0 = (i / (float)SEG) * 2.0f * 3.14159265f;
        float a1 = ((i + 1) / (float)SEG) * 2.0f * 3.14159265f;
        int b = (int)verts.size() / 3;
        auto P = [&](float y, float r, float ang) { verts.push_back(cosf(ang) * r); verts.push_back(y); verts.push_back(sinf(ang) * r); };
        glm::vec3 n0 = glm::normalize(glm::vec3(cosf(a0) * rh, 0.25f, sinf(a0) * rh));
        glm::vec3 n1 = glm::normalize(glm::vec3(cosf(a1) * rh, 0.25f, sinf(a1) * rh));
        P(yHead, rh, a0); norms.insert(norms.end(), { n0.x, n0.y, n0.z });
        P(yHead, rh, a1); norms.insert(norms.end(), { n1.x, n1.y, n1.z });
        P(1.0f, 0.0f, 0.0f); norms.insert(norms.end(), { n0.x, n0.y, n0.z });
        idx.insert(idx.end(), { (unsigned)b, (unsigned)b + 1, (unsigned)b + 2 });
    }
}

void Renderer::rebuildVectorGlyphs() {
    // teardown previous glyph GL handles
    if (vectorGlyph.vao) glDeleteVertexArrays(1, &vectorGlyph.vao);
    if (vectorGlyph.vbo) glDeleteBuffers(1, &vectorGlyph.vbo);
    if (vectorGlyph.nbo) glDeleteBuffers(1, &vectorGlyph.nbo);
    if (vectorGlyph.ebo) glDeleteBuffers(1, &vectorGlyph.ebo);
    if (vectorGlyph.instVBO) glDeleteBuffers(1, &vectorGlyph.instVBO);
    vectorGlyph = VectorGlyph{};

    if (cachedMeshSource.pointVectors.empty()) return;

    const std::string& field = cachedMeshSource.vectorName.empty()
        ? cachedMeshSource.availableVectorNames.front()
        : cachedMeshSource.vectorName;
    auto it = cachedMeshSource.pointVectors.find(field);
    if (it == cachedMeshSource.pointVectors.end()) return;
    const auto& vecArr = it->second;

    int numPts = static_cast<int>(cachedMeshSource.vertices.size() / 3);
    int stride = std::max(1, vectorStride);
    // track min/max magnitude across the (strided) sample for LUT normalization
    float magMin = std::numeric_limits<float>::max();
    float magMax = -std::numeric_limits<float>::max();
    std::vector<float> inst;
    for (int i = 0; i < numPts; i += stride) {
        float dx = vecArr[i * 3 + 0], dy = vecArr[i * 3 + 1], dz = vecArr[i * 3 + 2];
        // skip near-zero vectors so the cloud isn't cluttered with dots
        if (dx * dx + dy * dy + dz * dz < 1e-12f) continue;
        float m = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (m < magMin) magMin = m;
        if (m > magMax) magMax = m;
        inst.push_back(cachedMeshSource.vertices[i * 3 + 0]);
        inst.push_back(cachedMeshSource.vertices[i * 3 + 1]);
        inst.push_back(cachedMeshSource.vertices[i * 3 + 2]);
        inst.push_back(dx); inst.push_back(dy); inst.push_back(dz);
    }
    if (inst.empty()) return;
    vectorMagMin = magMin;
    vectorMagMax = magMax;
    emit vectorColormapChanged(); // refresh vector colorbar range
    std::vector<float> av, an; std::vector<unsigned int> ai;
    buildUnitArrow(av, an, ai);

    glGenVertexArrays(1, &vectorGlyph.vao);
    glGenBuffers(1, &vectorGlyph.vbo);
    glGenBuffers(1, &vectorGlyph.nbo);
    glGenBuffers(1, &vectorGlyph.ebo);
    glGenBuffers(1, &vectorGlyph.instVBO);

    glBindVertexArray(vectorGlyph.vao);

    glBindBuffer(GL_ARRAY_BUFFER, vectorGlyph.vbo);
    glBufferData(GL_ARRAY_BUFFER, av.size() * sizeof(float), av.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, vectorGlyph.nbo);
    glBufferData(GL_ARRAY_BUFFER, an.size() * sizeof(float), an.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vectorGlyph.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, ai.size() * sizeof(unsigned int), ai.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, vectorGlyph.instVBO);
    glBufferData(GL_ARRAY_BUFFER, inst.size() * sizeof(float), inst.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(2);
    glVertexAttribDivisor(2, 1);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribDivisor(3, 1);

    glBindVertexArray(0);
    vectorGlyph.glyphIndexCount = static_cast<int>(ai.size());
    vectorGlyph.instanceCount = static_cast<int>(inst.size() / 6);
}

void Renderer::setActiveVectorField(const QString& fieldName) {
    if (fieldName.isEmpty()) return;
    cachedMeshSource.vectorName = fieldName.toStdString();
    vectorGlyphDirty = true; // GL rebuild deferred to render thread (GUI thread has no GL ctx)
    emit meshDataUpdated();
}

// 1. Rewrite clearMeshes to be strictly for UI/Reset calls (Runs on GUI Thread)
void Renderer::clearMeshes() {
    // Free GPU resources for any uploaded meshes. Guarded by meshGLMutex so it
    // cannot race with uploadMesh() running on the render thread.
    {
        std::lock_guard<std::mutex> lock(meshGLMutex);
        for (auto& m : meshes) {
            destroyMesh(m);
        }
        meshes.clear();
        for (auto& m : decimatedMeshes) {
            destroyMesh(m);
        }
        decimatedMeshes.clear();
        hasDecimated = false;
    }
    hasMeshLoaded = false;
    meshHasScalars = false;
    triangleCount = 0;
    pointCount = 0;
    meshDataType = "";
    meshFormat = "";
    currentMeshName = "";
    if (!m_destroying) emit meshLoadStateChanged();
}

void Renderer::openRecent(const QString& filePath) {
    if (filePath.isEmpty()) return;
    // same path as the file dialog; just delegate
    loadMesh(filePath);
}

void Renderer::loadRecentFromSettings() {
    QSettings s;
    recentFiles = s.value("recentFiles").toStringList();
    recentFiles.removeAll(""); // guard against stale empty entries
}

void Renderer::saveRecentToSettings() const {
    QSettings s;
    s.setValue("recentFiles", recentFiles);
}

void Renderer::loadMesh(const QString& filePath) {
    if (filePath.isEmpty()) return;

    std::string stdPath = filePath.toStdString();
    if (stdPath.rfind("file:///", 0) == 0) {
        stdPath = stdPath.substr(8);
    } else if (stdPath.rfind("file://", 0) == 0) {
        stdPath = stdPath.substr(7);
    }

    RenderMesh loaded = loadMeshFile(stdPath);
    if (loaded.vertices.empty()) {
        std::cerr << "Engine aborted mapping out invalid/empty file path target: " << stdPath << std::endl;
        return;
    }

    // Thread-safe handoff of the CPU geometry data
    {
        std::lock_guard<std::mutex> lock(meshQueueMutex);
        dynamicMeshQueue = loaded;
    }

    // Cache the source mesh so scalar-field queries (getAvailableScalars,
    // setActiveScalarFieldStd) operate on real data rather than an empty struct.
    cachedMeshSource = loaded;

    // Update dimensions and properties safely on the UI thread
    worldMinX = loaded.bounds.minX; worldMaxX = loaded.bounds.maxX;
    worldMinY = loaded.bounds.minY; worldMaxY = loaded.bounds.maxY;
    worldMinZ = loaded.bounds.minZ; worldMaxZ = loaded.bounds.maxZ;
    // Ground the grid at the mesh's lowest point so it appears to rest on it.
    gridPlaneY = worldMinY;

    worldCenterX = loaded.bounds.centerX;
    worldCenterY = loaded.bounds.centerY;
    worldCenterZ = loaded.bounds.centerZ;
    worldRadius  = loaded.bounds.worldRadius;

    QFileInfo fileInfo(QString::fromStdString(stdPath));
    currentMeshName = fileInfo.fileName().toStdString();
    triangleCount = static_cast<int>(loaded.indices.size() / 3);
    pointCount = static_cast<int>(loaded.vertices.size() / 3); // vertices are xyz triples
    meshDataType = loaded.datasetType;
    meshFormat = loaded.fileFormat;

    // UI components read these variables immediately
    hasMeshLoaded = true;

    // reset per-mesh vector state so a newly loaded mesh doesn't
    // inherit the previous mesh's vector field / toggles / magnitude range.
    // showVectors and vectorUseColormap default off; vectorName is cleared so
    // the render thread selects the new mesh's first available field.
    setShowVectors(false);
    setVectorUseColormap(false);
    if (!loaded.pointVectors.empty()) {
        cachedMeshSource.vectorName = loaded.availableVectorNames.front();
    } else {
        cachedMeshSource.vectorName.clear();
    }
    // magnitude range is recomputed by uploadVectorGlyphs for vector meshes;
    // reset to defaults for the no-vector case so the (hidden) colorbar can't
    // display stale bounds.
    if (vectorMagMin != 0.0f || vectorMagMax != 1.0f) {
        vectorMagMin = 0.0f;
        vectorMagMax = 1.0f;
        emit vectorColormapChanged();
    }
    vectorGlyphDirty = true; // force glyph buffer rebuild for the new geometry

    // NOTE: meshChanged is an atomic flag consumed by the render thread in
    // consumeMeshChanged(); set it so the queued mesh gets uploaded.
    this->meshChanged = true;

    if (!loaded.scalars.empty()) {
        meshHasScalars = true;
        activeScalarName = loaded.scalarName;
        float minVal = loaded.scalars[0];
        float maxVal = loaded.scalars[0];
        for (float val : loaded.scalars) {
            if (val < minVal) minVal = val;
            if (val > maxVal) maxVal = val;
        }
        dataScalarMin = minVal;
        dataScalarMax = maxVal;
        scalarMin = dataScalarMin;
        scalarMax = dataScalarMax;
        // default filter spans the whole field so enabling clipping doesn't blank the mesh
        filterMin = dataScalarMin;
        filterMax = dataScalarMax;
    } else {
        meshHasScalars = false;
        dataScalarMin = 0.0f;
        dataScalarMax = 1.0f;
    }

    resetCamera();

    // push into recent files (most-recent first, dedup, cap 8)
    {
        QString absPath = QFileInfo(QString::fromStdString(stdPath)).absoluteFilePath();
        recentFiles.removeAll(absPath);
        recentFiles.prepend(absPath);
        while (recentFiles.size() > 8) recentFiles.removeLast();
        emit meshLoadStateChanged(); // refresh QML recent list
        saveRecentToSettings(); // persist across restarts
    }

    emit meshLoadStateChanged();

    // 2. Alert the CustomViewportItem to trigger a Scene Graph update
    emit meshDataUpdated();
}

void Renderer::resetCamera() {
    // 1. Point the camera's focal target directly at the center of the loaded mesh
    camera.focalPoint = glm::dvec3(worldCenterX, worldCenterY, worldCenterZ);

    // 2. Determine an ideal camera setback distance based on the mesh's bounding radius
    camera.distance = worldRadius * 2.5;
    if (camera.distance < 1.0) {
        camera.distance = 1.0; // Prevent the camera from nesting inside a flat/empty mesh
    }

    // Allow zooming out to a generous multiple of the model size (and always a
    // sensible floor) so the user can't dolly into infinite distance.
    camera.maxDistance = std::max(1000.0, worldRadius * 50.0);

    // 3. Set the near and far planes tightly but safely around the radius
    // Since we inverted Z in the projection matrix, ensure these bracket the geometry seamlessly.
    nearPlane = std::max(0.01, worldRadius * 0.01);
    farPlane  = std::max(100.0, worldRadius * 20.0);

    // 4. Reset the position vector relative to the focal point along the Z axis.
    // If the model looks tiny or huge, we need to make sure the camera sits on the 
    // positive side of the center point facing down toward it.
    camera.position = camera.focalPoint + glm::dvec3(0.0, 0.0, camera.distance);

    // 5. Force a clean baseline view orientation (Up vector along +Y axis)
    camera.viewUp = glm::dvec3(0.0, 1.0, 0.0);
    camera.orthogonalizeViewUp();

    // 6. Alert the framework that the camera matrix needs recalculating
    emit viewChanged();
    emit meshDataUpdated();
}

void Renderer::resizeViewport(int w, int h) {
    width = w;
    height = h;
}

void Renderer::computeLightDirections(glm::vec3& key, glm::vec3& fill, glm::vec3& back1, glm::vec3& back2, glm::vec3& head) {
    // Light Kit: lights live in the CAMERA frame (azimuth/elevation about
    // the camera's look-at point), so they move WITH the view. Build the camera->world
    // rotation basis and rotate each kit-local direction into world space before
    // handing it to the world-space fragment shader.
    glm::dvec3 fwd   = glm::normalize(camera.position - camera.focalPoint); // toward camera
    glm::dvec3 right = glm::normalize(glm::cross(fwd, camera.viewUp));
    glm::dvec3 up    = glm::cross(right, fwd);
    glm::mat3 M(right, up, fwd); // kit X->right, Y->up, Z->toward camera
    auto toRad = [](float deg) { return deg * 3.14159265f / 180.0f; };
    auto kitDir = [&](float az, float el) {
        float a = toRad(az), e = toRad(el);
        glm::vec3 local(std::sin(a) * std::cos(e), std::sin(e), std::cos(a) * std::cos(e));
        return glm::vec3(glm::normalize(M * local));
    };
    key  = kitDir(lightKeyAzimuth,  lightKeyElevation);
    fill = kitDir(lightFillAzimuth, lightFillElevation);
    back1 = kitDir(lightBackAzimuth,  lightBackElevation);
    back2 = kitDir(lightBackAzimuth + 180.0f, -lightBackElevation);
    head = kitDir(lightHeadAzimuth,  lightHeadElevation);
}

void Renderer::updateColormapTexture() {
    // Scalar LUT (surface) — independent guard
    bool scalarReverse = colormapReversed;
    if (colormapTex == 0 || colormapChoice != lastUploadedChoice || scalarReverse != lastUploadedReversed) {
        std::vector<unsigned char> pd; pd.reserve(256 * 3);
        for (int i = 0; i < 256; ++i) {
            float t = static_cast<float>(i) / 255.0f;
            float s = scalarReverse ? (1.0f - t) : t;
            glm::vec3 rgb = Colormaps::evaluate(s, static_cast<ColormapType>(colormapChoice));
            pd.push_back(static_cast<unsigned char>(rgb.r * 255.0f));
            pd.push_back(static_cast<unsigned char>(rgb.g * 255.0f));
            pd.push_back(static_cast<unsigned char>(rgb.b * 255.0f));
        }
        if (colormapTex == 0) glGenTextures(1, &colormapTex);
        glBindTexture(GL_TEXTURE_1D, colormapTex);
        glTexImage1D(GL_TEXTURE_1D, 0, GL_RGB, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, pd.data());
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_1D, 0);
        lastUploadedChoice = colormapChoice;
        lastUploadedReversed = scalarReverse;
    }

    // vector magnitude LUT — SEPARATE guard, independent of scalar early-return
    bool vectorReverse = vectorColormapReversed;
    if (vectorColormapTex == 0 || vectorLutDirty || vectorLastUploadedChoice != vectorColormapChoice || vectorReverse != vectorLastUploadedReversed) {
        std::vector<unsigned char> pd; pd.reserve(256 * 3);
        for (int i = 0; i < 256; ++i) {
            float t = static_cast<float>(i) / 255.0f;
            float s = vectorReverse ? (1.0f - t) : t;
            glm::vec3 rgb = Colormaps::evaluate(s, static_cast<ColormapType>(vectorColormapChoice));
            pd.push_back(static_cast<unsigned char>(rgb.r * 255.0f));
            pd.push_back(static_cast<unsigned char>(rgb.g * 255.0f));
            pd.push_back(static_cast<unsigned char>(rgb.b * 255.0f));
        }
        if (vectorColormapTex == 0) glGenTextures(1, &vectorColormapTex);
        glBindTexture(GL_TEXTURE_1D, vectorColormapTex);
        glTexImage1D(GL_TEXTURE_1D, 0, GL_RGB, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, pd.data());
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_1D, 0);
        vectorLastUploadedChoice = vectorColormapChoice;
        vectorLastUploadedReversed = vectorReverse;
        vectorLutDirty = false;
        emit vectorColormapChanged();
    }
}

void Renderer::renderFrame() {
    // GL glyph rebuild must run on the render thread (GL context
    // current here), never from GUI-thread setters that call setActiveVectorField/
    // setVectorStride. Consume the flag once so a single-change set doesn't thrash.
    if (vectorGlyphDirty.exchange(false)) {
        rebuildVectorGlyphs();
    }

    // Standard buffer setups
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    // no face culling — meshes (hexes/structured grids) show inverted/inside faces
    glDisable(GL_CULL_FACE);

    glClearColor(bgColor[0], bgColor[1], bgColor[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Establish the full-window viewport for the scene (grid + mesh). The scene
    // draws full-window; set the GL viewport here in device pixels so it matches
    // the real framebuffer on HiDPI displays (width/height are logical).
    int deviceW = static_cast<int>(width * devicePixelRatio);
    int deviceH = static_cast<int>(height * devicePixelRatio);
    glViewport(0, 0, deviceW, deviceH);

    glm::mat4 view = camera.getViewMatrix();

    // Clip planes must track the current camera distance. resetCamera() only
    // sets them once from the mesh radius, so zooming out (dolly) would otherwise
    // push the mesh past the far plane (mesh disappears) and clip the ground grid
    // at the horizon (grid cut off in a horizontal band at screen center).
    double camDist = camera.distance;
    nearPlane = std::max(0.01, camDist * 0.001);
    farPlane  = std::max(farPlane, camDist + worldRadius + 250.0);

    glm::mat4 proj = glm::perspective(
        glm::radians(45.0f),
        static_cast<float>(deviceW) / static_cast<float>(deviceH),
        static_cast<float>(nearPlane),
        static_cast<float>(farPlane)
        );

    glm::mat4 model = glm::mat4(1.0f);
    glm::mat4 mvp = proj * view * model;

    // Main Mesh Rendering Operations
    if ((showSurface || showWireframe) && !meshes.empty() && shaderProgram != 0) {
        glUseProgram(shaderProgram);
        updateColormapTexture();

        // Pass Matrix Transforms
        glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
        glUniformMatrix4fv(modelLoc, 1, GL_FALSE, glm::value_ptr(model));
        glUniformMatrix4fv(viewLoc, 1, GL_FALSE, glm::value_ptr(view));

        // Setup Light Parameters
        // Light Kit: directions are computed in the CAMERA frame inside
        // computeLightDirections() and rotated into world space, so the lights
        // track the camera (headlight stays head-on, key stays overhead-in-view).
        // The fragment shader lights in world space (vWorldNormal + uViewPos).
        glm::vec3 kDir, fDir, b1Dir, b2Dir, hDir;
        computeLightDirections(kDir, fDir, b1Dir, b2Dir, hDir);
        glUniform3fv(lightDirLoc, 1, glm::value_ptr(kDir));
        glUniform3fv(lightFillLoc, 1, glm::value_ptr(fDir));
        glUniform3fv(lightBack1Loc, 1, glm::value_ptr(b1Dir));
        glUniform3fv(lightBack2Loc, 1, glm::value_ptr(b2Dir));
        glUniform3fv(lightHeadLoc, 1, glm::value_ptr(hDir));

        glm::vec3 camPos = glm::vec3(camera.position);
        glUniform3fv(viewPosLoc, 1, glm::value_ptr(camPos));

        // Assign Shading Registers
        glUniform3fv(colorLoc, 1, meshColor);
        glUniform3fv(surfaceColorLoc, 1, surfaceColor);
        glUniform1f(matAmbientLoc, matAmbient);
        glUniform1f(matDiffuseLoc, matDiffuse);
        glUniform1f(matSpecularLoc, matSpecular);
        glUniform1f(matShininessLoc, matShininess);

        // Light Kit intensities: key = Int; fill/back/head = key / Kratio.
        float keyI = lightKitEnabled ? lightKeyIntensity : 0.0f;
        glUniform1f(keyIntensityLoc,  keyI);
        glUniform1f(fillIntensityLoc, lightKitEnabled ? keyI / lightKF : 0.0f);
        glUniform1f(headIntensityLoc, lightKitEnabled ? keyI / lightKH : 0.0f);
        glUniform1f(backIntensityLoc, lightKitEnabled ? keyI / lightKB : 0.0f);

        // Light Kit: kit-wide warm tint drives light colors.
        glm::vec3 tint = warmColorTint(lightWarm);
        const float keyCol[3]  = { tint.r,       tint.g,       tint.b };
        const float fillCol[3] = { tint.r*0.90f, tint.g*0.92f, tint.b*1.00f };
        const float backCol[3] = { tint.r*0.95f, tint.g*0.95f, tint.b*0.98f };
        const float headCol[3] = { 1.0f, 1.0f, 1.0f };
        glUniform3fv(keyColorLoc, 1, keyCol);
        glUniform3fv(fillColorLoc, 1, fillCol);
        glUniform3fv(backColorLoc, 1, backCol);
        glUniform3fv(headColorLoc, 1, headCol);

        // Assign Dynamic Clip Slices Filters
        glUniform1f(sliceHeightXLoc, sliceHeightX);
        glUniform1f(sliceHeightYLoc, sliceHeightY);
        glUniform1f(sliceHeightZLoc, sliceHeightZ);
        glUniform1i(invertXLoc, invertX ? 1 : 0);
        glUniform1i(invertYLoc, invertY ? 1 : 0);
        glUniform1i(invertZLoc, invertZ ? 1 : 0);
        glUniform1f(filterMinLoc, filterMin);
        glUniform1f(filterMaxLoc, filterMax);
        glUniform1i(clipEnabledLoc, clipEnabled ? 1 : 0);

        glUniform1f(scalarMinLoc, scalarMin);
        glUniform1f(scalarMaxLoc, scalarMax);
        glUniform1i(hasScalarsLoc, meshHasScalars ? 1 : 0);

        if (meshHasScalars && colormapTex != 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_1D, colormapTex);
            glUniform1i(lutTextureLoc, 0);
        }

        // Guard against clearMeshes()/uploadMesh() mutating `meshes` on the GUI
        // thread (both take meshGLMutex). Snapshot the handles under the lock so
        // we don't iterate the vector while it is being cleared/rebuilt.
        struct DrawEntry { GLuint vao; int indexCount; };
        std::vector<DrawEntry> drawList;
        {
            std::lock_guard<std::mutex> lock(meshGLMutex);
            // While the camera is moving, draw the coarse LOD mesh to keep framerate
            // high; otherwise draw the full-resolution mesh.
            const std::vector<Mesh>& src =
                (useLod && cameraMoving.load() && hasDecimated) ? decimatedMeshes : meshes;
            drawList.reserve(src.size());
            for (const auto& m : src) drawList.push_back({m.vao, m.indexCount});
        }

        for (const auto& e : drawList) {
            glBindVertexArray(e.vao);

            // Phase 1: Solid Base Pass (only when surface shown)
            if (showSurface) {
                glUniform1i(wireframeLoc, 0);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                glDrawElements(GL_TRIANGLES, e.indexCount, GL_UNSIGNED_INT, 0);
            }

            // Phase 2: Structural Wireframe Layer Overlay
            if (showWireframe) {
                glUniform1i(wireframeLoc, 1);
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                glEnable(GL_POLYGON_OFFSET_LINE);
                glPolygonOffset(-1.0f, -1.0f);
                glDrawElements(GL_TRIANGLES, e.indexCount, GL_UNSIGNED_INT, 0);
                glDisable(GL_POLYGON_OFFSET_LINE);
            }
        }
        glBindVertexArray(0);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); // reset at source: wireframe pass left GL_LINE
        glUseProgram(0);
    }

    // instanced vector arrow glyphs (drawn after surface, before grid/gizmo)
    if (showVectors && vectorGlyph.instanceCount > 0 && glyphProgram != 0) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); // ensure solid arrows even if wireframe pass left GL_LINE
        glUseProgram(glyphProgram);
        glUniformMatrix4fv(glyphMvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
        glUniform1f(glyphScaleLoc, vectorScale);
        glm::vec3 kDir, fDir, b1Dir, b2Dir, hDir;
        computeLightDirections(kDir, fDir, b1Dir, b2Dir, hDir);
        glUniform3fv(glyphLightDirLoc, 1, glm::value_ptr(kDir));
        glm::vec3 camPos = glm::vec3(camera.position);
        glUniform3fv(glyphViewPosLoc, 1, glm::value_ptr(camPos));
        glUniform3fv(glyphColorLoc, 1, vectorColor);
        // color by magnitude using the vector's OWN LUT (independent of scalar colormap)
        glUniform1i(glyphUseColormapLoc, vectorUseColormap ? 1 : 0);
        glUniform1f(glyphMagMinLoc, vectorMagMin);
        glUniform1f(glyphMagMaxLoc, vectorMagMax);
        if (vectorUseColormap && vectorColormapTex != 0) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_1D, vectorColormapTex);
            glUniform1i(glyphLutLoc, 1);
            glActiveTexture(GL_TEXTURE0);
        }
        glBindVertexArray(vectorGlyph.vao);
        glDrawElementsInstanced(GL_TRIANGLES, vectorGlyph.glyphIndexCount, GL_UNSIGNED_INT, 0, vectorGlyph.instanceCount);
        glBindVertexArray(0);
        glUseProgram(0);
    }

    drawGrid(view, proj);

    if (showGizmo) {
        drawGizmo();
    }

    // perf HUD — update ~4x/sec to avoid repaint spam; cheap std::chrono
    if (showFps) {
        auto now = std::chrono::steady_clock::now();
        if (m_frameCount == 0) {
            m_lastFrameTime = now; // seed clock on first frame, no measurement
            m_frameCount = 1;
        } else {
            double dt = std::chrono::duration<double>(now - m_lastFrameTime).count();
            m_frameCount++;
            m_fpsAccum += dt;
            if (m_fpsAccum >= 0.25) {
                double fps = (m_frameCount - 1) / m_fpsAccum;
                double ms = (m_fpsAccum / (m_frameCount - 1)) * 1000.0;
                fpsText = QString("FPS: %1  | %2 ms/frame")
                    .arg(fps, 0, 'f', 0)
                    .arg(ms, 0, 'f', 1);
                m_frameCount = 0;
                m_fpsAccum = 0.0;
                emit fpsChanged();
            }
            m_lastFrameTime = now;
        }
    }

    // CRITICAL CORE SYNC FIX: Restore standard Qt Quick scene graph drawing states
    QQuickOpenGLUtils::resetOpenGLState();
}

void Renderer::drawGizmo() {
    // Pure low-level overlay: isolated corner viewport, rotation-only view, raw GL.
    glDisable(GL_DEPTH_TEST);
    gizmo.draw(camera.getViewMatrix(), static_cast<float>(devicePixelRatio),
               static_cast<int>(height * devicePixelRatio));
    // Light Kit markers — kit-local dirs are constant, so they
    // stay put in the corner overlay while the axis triad rotates (proves lights
    // track the camera). Only drawn when the kit is enabled.
    if (lightKitEnabled && showLightMarkers) {
        auto kd = [](float az, float el) -> glm::vec3 {
            float a = az * 3.14159265f / 180.0f, e = el * 3.14159265f / 180.0f;
            return glm::vec3(std::sin(a) * std::cos(e), std::sin(e), std::cos(a) * std::cos(e));
        };
        glm::vec3 kitDirs[5] = {
            kd(lightKeyAzimuth,  lightKeyElevation),
            kd(lightFillAzimuth, lightFillElevation),
            kd(lightBackAzimuth,  lightBackElevation),
            kd(lightBackAzimuth + 180.0f, -lightBackElevation),
            kd(lightHeadAzimuth,  lightHeadElevation),
        };
        glm::vec3 tint = warmColorTint(lightWarm);
        glm::vec3 cols[5] = { tint, tint * 0.9f, tint * 0.95f, tint * 0.95f, glm::vec3(1.0f, 1.0f, 1.0f) };
        gizmo.drawLights(kitDirs, cols, static_cast<float>(devicePixelRatio),
                         static_cast<int>(height * devicePixelRatio));
    }
    glEnable(GL_DEPTH_TEST);
}

void Renderer::snapToOrthoView(int axis) {
    // Delegate to the Camera implementation so both stay in sync (6-axis support).
    camera.snapToOrthoView(axis);
    emit viewChanged(); // ortho snap must repaint; snapToAxisView already does this
}

void Renderer::snapToAxisView(int axis, bool flip) {
    int preset = flip ? (axis * 2 + 1) : (axis * 2);
    camera.snapToOrthoView(preset);
    emit viewChanged();
}

void Renderer::requestScreenshot(const QString& path) {
    if (path.isEmpty()) {
        return;
    }
    // the actual GL pixel read + save is unsafe here (GUI thread, no GL
    // context). Forward to the render thread (CustomViewportItem connects
    // screenshotRequested -> render() where the context is current).
    emit screenshotRequested(path);
}

bool Renderer::captureScreenshotToFile(const QString& path, QOpenGLFramebufferObject* fbo) {
    if (path.isEmpty()) return false;

    ExportConfig config;
    config.transparentBackground = screenshotTransparent;
    config.quality = screenshotQuality;
    config.format = ExportFormat::PNG;

    QString targetPath = path;
    if (targetPath.endsWith(".jpg", Qt::CaseInsensitive) || targetPath.endsWith(".jpeg", Qt::CaseInsensitive)) {
        config.format = ExportFormat::JPEG;
    } else if (targetPath.endsWith(".bmp", Qt::CaseInsensitive)) {
        config.format = ExportFormat::BMP;
    } else {
        config.format = ExportFormat::PNG;
        if (!targetPath.endsWith(".png", Qt::CaseInsensitive)) {
            targetPath += ".png";
        }
    }
    config.filePath = targetPath;

    // Called on the render thread with the GL context current. Capture from the
    // viewport FBO when provided so the saved image contains ONLY the 3D scene
    // (grid/mesh/gizmo) and NOT the QML UI chrome (rail, colorbar, status bar).
    // Reading GL_FRAMEBUFFER_BINDING here is unsafe: renderFrame() ends with
    // resetOpenGLState() which can unbind the viewport FBO, leaving the binding
    // pointing at the window/default framebuffer that holds the composited UI.
    GLuint boundFbo = 0;
    int deviceW = 0;
    int deviceH = 0;
    if (fbo) {
        boundFbo = fbo->handle();
        deviceW = fbo->width();
        deviceH = fbo->height();
    } else {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, reinterpret_cast<GLint*>(&boundFbo));
        deviceW = static_cast<int>(width * devicePixelRatio);
        deviceH = static_cast<int>(height * devicePixelRatio);
    }
    std::vector<unsigned char> pixels = ScreenshotExporter::captureFBO(boundFbo, deviceW, deviceH, config.transparentBackground);

    bool success = ScreenshotExporter::saveToFile(config.filePath, pixels, deviceW, deviceH, config);
    if (success) {
        emit screenshotCaptured(targetPath);
    }
    return success;
}

QStringList Renderer::getAvailableScalars() const {
    QStringList list;
    for (const auto& name : cachedMeshSource.availableScalarNames) {
        list.append(QString::fromStdString(name));
    }
    return list;
}

QStringList Renderer::getColormapNames() const {
    QStringList list;
    for (int i = 0; i < static_cast<int>(ColormapType::Count); ++i) {
        list.append(QString::fromUtf8(Colormaps::getName(static_cast<ColormapType>(i))));
    }
    return list;
}

QString Renderer::getColormapPreviewUri(int index) const {
    const int w = 128, h = 16;
    QImage img(w, h, QImage::Format_RGB888);
    ColormapType type = static_cast<ColormapType>(index);
    for (int x = 0; x < w; ++x) {
        float t = static_cast<float>(x) / static_cast<float>(w - 1);
        glm::vec3 c = Colormaps::evaluate(t, type);
        int r = static_cast<int>(glm::clamp(c.r, 0.0f, 1.0f) * 255.0f);
        int g = static_cast<int>(glm::clamp(c.g, 0.0f, 1.0f) * 255.0f);
        int b = static_cast<int>(glm::clamp(c.b, 0.0f, 1.0f) * 255.0f);
        for (int y = 0; y < h; ++y) img.setPixel(x, y, qRgb(r, g, b));
    }
    QByteArray ba;
    QBuffer buf(&ba);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return QString("data:image/png;base64,") + QString::fromLatin1(ba.toBase64());
}

QVariantList Renderer::getColormapStops() const {
    QVariantList out;
    const int steps = 16;
    for (int i = 0; i <= steps; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(steps);
        float s = colormapReversed ? (1.0f - t) : t;
        glm::vec3 c = Colormaps::evaluate(s, static_cast<ColormapType>(colormapChoice));
        QVariantList stop;
        stop << t << c.r << c.g << c.b;
        out.append(QVariant(stop));
    }
    return out;
}

QVariantList Renderer::getVectorColormapStops() const {
    QVariantList out;
    const int steps = 16;
    for (int i = 0; i <= steps; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(steps);
        float s = vectorColormapReversed ? (1.0f - t) : t;
        glm::vec3 c = Colormaps::evaluate(s, static_cast<ColormapType>(vectorColormapChoice));
        QVariantList stop;
        stop << t << c.r << c.g << c.b;
        out.append(QVariant(stop));
    }
    return out;
}

std::vector<std::string> Renderer::getAvailableScalarNames() const {
    std::vector<std::string> list;
    if (!cachedMeshSource.scalarName.empty()) {
        list.push_back(cachedMeshSource.scalarName);
    }
    return list;
}

void Renderer::setActiveScalarField(const QString& fieldName) {
    setActiveScalarFieldStd(fieldName.toStdString());
}

void Renderer::setActiveScalarFieldStd(const std::string& fieldName) {
    if (fieldName == activeScalarName) return;
    // fields live in pointScalars; swap the active vector and let the
    // existing render-thread upload path (meshChanged) push the new sbo.
    if (!cachedMeshSource.attributes.has_value()) return;
    auto it = cachedMeshSource.attributes->pointScalars.find(fieldName);
    if (it == cachedMeshSource.attributes->pointScalars.end()) return;

    activeScalarName = fieldName;
    cachedMeshSource.scalarName = fieldName;
    cachedMeshSource.scalars = it->second;
    dynamicMeshQueue.scalarName = fieldName;
    dynamicMeshQueue.scalars = it->second;

    if (!cachedMeshSource.scalars.empty()) {
        float mn = cachedMeshSource.scalars[0], mx = cachedMeshSource.scalars[0];
        for (float v : cachedMeshSource.scalars) { if (v < mn) mn = v; if (v > mx) mx = v; }
        dataScalarMin = mn; dataScalarMax = mx;
        scalarMin = mn; scalarMax = mx;
        filterMin = mn; filterMax = mx;
    }
    meshChanged = true; // trigger GPU re-upload of the scalar buffer
    emit meshDataUpdated();
    emit meshLoadStateChanged(); // refresh QML colorbar/labels on field switch
}

void Renderer::setWireframe(bool enabled) {
    if (showWireframe == enabled) return;
    showWireframe = enabled;
    emit wireframeChanged();
}

void Renderer::setUseLod(bool enabled) {
    if (useLod == enabled) return;
    useLod = enabled;
    cameraMoving = false; // force a clean repaint at the chosen resolution
    if (m_lodTimer) m_lodTimer->stop();
    emit viewChanged();
}

void Renderer::toggleSurface(bool visible) {
    if (showSurface == visible) return;
    showSurface = visible;
    emit surfaceVisibilityChanged();
}

void Renderer::setColormapChoice(int choice) {
    if (colormapChoice == choice) return;
    colormapChoice = choice;
    emit colormapChanged();
}

void Renderer::setColormapReversed(bool reversed) {
    if (colormapReversed == reversed) return;
    colormapReversed = reversed;
    emit colormapChanged();
}

void Renderer::setVectorColormapReversed(bool reversed) {
    if (vectorColormapReversed == reversed) return;
    vectorColormapReversed = reversed;
    emit vectorColormapChanged();
}

void Renderer::applyLightingPreset(int preset) {
    switch (preset) {
    case PRESET_STUDIO: // 3-point feel: strong key, soft fill, rim back, subtle head
        lightKeyAzimuth = 35.0f;   lightKeyElevation = 45.0f;   lightKF = 4.0f;  // Lowered key elevation slightly for better catchlights
        lightFillAzimuth = -45.0f; lightFillElevation = 20.0f;  lightKB = 1.5f;  // FIXED: Fill now shines from slightly above (20.0f)
        lightBackAzimuth = 140.0f; lightBackElevation = 30.0f;                  // Raised back light for a nicer top-down rim
        lightHeadAzimuth = 0.0f;   lightHeadElevation = 0.0f;   lightKH = 0.5f;  // FIXED: Reduced headlight so it doesn't flatten the depth
        lightKeyIntensity = 1.0f;  lightWarm = 0.5f;
        matAmbient = 0.10f; matDiffuse = 0.78f; matSpecular = 0.25f; matShininess = 0.6f;
        break;

    case PRESET_CADFLAT: // even, shadowless look for inspecting geometry (kept as-is, it's great)
        lightKeyAzimuth = 0.0f;   lightKeyElevation = 45.0f;  lightKF = 1.2f;
        lightFillAzimuth = 180.0f; lightFillElevation = 45.0f; lightKB = 1.2f;
        lightBackAzimuth = 90.0f;  lightBackElevation = 45.0f;
        lightHeadAzimuth = 0.0f;   lightHeadElevation = 0.0f;  lightKH = 1.0f;
        lightKeyIntensity = 1.0f;  lightWarm = 0.5f;
        matAmbient = 0.45f; matDiffuse = 0.8f; matSpecular = 0.0f; matShininess = 0.0f;
        break;

    case PRESET_SOFT: // gentle, low-contrast, warm
    default:
        lightKeyAzimuth = 20.0f;   lightKeyElevation = 40.0f;  lightKF = 2.5f;
        lightFillAzimuth = -30.0f; lightFillElevation = 15.0f; lightKB = 1.8f;  // FIXED: Soft fill from a slightly raised angle
        lightBackAzimuth = 120.0f; lightBackElevation = 25.0f;                  // Raised back light for softer rim dispersion
        lightHeadAzimuth = 0.0f;   lightHeadElevation = 0.0f;  lightKH = 0.8f;  // Lowered headlight to keep contrast soft but visible
        lightKeyIntensity = 1.0f;  lightWarm = 0.6f;
        matAmbient = 0.20f; matDiffuse = 0.7f; matSpecular = 0.08f; matShininess = 0.3f; // Slightly bumped ambient for "softness"
        break;
    }
    emit lightingParametersChanged();
}

void Renderer::resetLighting() {
    // Restore the default Light Kit configuration.
    lightKitEnabled = true;
    lightKeyIntensity = 0.5f;  lightWarm = 0.5f;
    lightKF = 3.0f; lightKB = 3.5f; lightKH = 3.0f;
    lightKeyAzimuth = 10.0f;  lightKeyElevation = 50.0f;
    lightFillAzimuth = -10.0f; lightFillElevation = -75.0f;
    lightBackAzimuth = 110.0f; lightBackElevation = 0.0f;
    lightHeadAzimuth = 0.0f;   lightHeadElevation = 0.0f;
    matAmbient = 0.08f; matDiffuse = 0.75f; matSpecular = 0.15f; matShininess = 0.5f;
    emit lightingParametersChanged();
}
