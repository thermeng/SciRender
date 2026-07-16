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
        colormap.shutdown();
        vectorGlyph.shutdown();
        gizmo.shutdown();
        colorbarOverlay.shutdown();
    }
}
#pragma GCC diagnostic pop

void Renderer::initGLAD() {
    QOpenGLContext* currentContext = QOpenGLContext::currentContext();
    if (!currentContext) {
        qFatal("Fatal: initGLAD called but no active Qt OpenGL context was found on this thread.");
        return;
    }

    GLADloadproc loader = [](const char* name) -> void* {
        QOpenGLContext* ctx = QOpenGLContext::currentContext();
        return ctx ? reinterpret_cast<void*>(ctx->getProcAddress(name)) : nullptr;
    };

    if (!gladLoadGLLoader(loader)) {
        qFatal("Fatal: GLAD failed to map target core OpenGL function addresses using Qt resolver hook.");
    }

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
    auto loadEmbeddedShader = [](const QString& rscPath) -> std::string {
        QFile file(rscPath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCritical() << "Fatal Error: Required engine shader asset missing at path:" << rscPath;
            return "";
        }
        QTextStream stream(&file);
        return stream.readAll().toStdString();
    };

    std::string vertSrcStr = loadEmbeddedShader(":/RendererQTUI/src/shaders/mesh.vert");
    std::string fragSrcStr = loadEmbeddedShader(":/RendererQTUI/src/shaders/mesh.frag");

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
    colorbarOverlay.init();
}

void Renderer::uploadMesh(const RenderMesh& renderMesh) {
    meshManager.upload(renderMesh);
    // (re)build instanced vector arrow glyphs from the cached source mesh
    vectorGlyph.rebuild(cachedMeshSource, vectorStride);
    emit vectorColormapChanged(); // refresh vector colorbar range
}

void Renderer::markCameraMoving() {
    cameraMoving = true;
    if (m_lodTimer) m_lodTimer->start(); // (re)arm: fires 140ms after motion stops
}

void Renderer::onLodTimer() {
    cameraMoving = false;
    emit viewChanged(); // one last repaint, now with the full mesh
}

void Renderer::computeLightDirections(glm::vec3& key, glm::vec3& fill, glm::vec3& back1, glm::vec3& back2, glm::vec3& head) {
    lighting.computeDirections(camera.position, camera.focalPoint, camera.viewUp,
                              key, fill, back1, back2, head);
}

void Renderer::clearMeshes() {
    meshManager.clear();
    if (QOpenGLContext::currentContext()) vectorGlyph.shutdown();
    else vectorGlyph = VectorGlyphSet{}; // no GL ctx on GUI teardown: just drop handles

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

    // NOTE: meshChanged is an atomic flag consumed by the render thread in
    // consumeMeshChanged(); set it so the queued mesh gets uploaded.
    meshManager.meshChanged = true;

    worldMinX = loaded.bounds.minX; worldMaxX = loaded.bounds.maxX;
    worldMinY = loaded.bounds.minY; worldMaxY = loaded.bounds.maxY;
    worldMinZ = loaded.bounds.minZ; worldMaxZ = loaded.bounds.maxZ;
    gridPlaneY = worldMinY; // ground the grid at the mesh's lowest point

    worldCenterX = loaded.bounds.centerX;
    worldCenterY = loaded.bounds.centerY;
    worldCenterZ = loaded.bounds.centerZ;
    worldRadius  = loaded.bounds.worldRadius;

    QFileInfo fileInfo(QString::fromStdString(stdPath));
    currentMeshName = fileInfo.fileName().toStdString();
    triangleCount = static_cast<int>(loaded.indices.size() / 3);
    pointCount = static_cast<int>(loaded.vertices.size() / 3);
    meshDataType = loaded.datasetType;
    meshFormat = loaded.fileFormat;

    hasMeshLoaded = true;

    // Reset per-mesh vector state so a newly loaded mesh doesn't inherit the
    // previous mesh's vector field / toggles / magnitude range.
    setShowVectors(false);
    setVectorUseColormap(false);
    setClipEnabled(false); // disable clipping on a fresh mesh
    if (!loaded.pointVectors.empty()) {
        cachedMeshSource.vectorName = loaded.availableVectorNames.front();
    } else {
        cachedMeshSource.vectorName.clear();
    }
    if (vectorGlyph.magMin != 0.0f || vectorGlyph.magMax != 1.0f) {
        vectorGlyph.magMin = 0.0f;
        vectorGlyph.magMax = 1.0f;
        emit vectorColormapChanged();
    }
    vectorGlyphDirty = true; // force glyph buffer rebuild for the new geometry

    if (!loaded.scalars.empty()) {
        meshHasScalars = true;
        setShowScalarColorbar(true);
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
        filterMin = dataScalarMin;
        filterMax = dataScalarMax;
    } else {
        meshHasScalars = false;
        setShowScalarColorbar(false);
        dataScalarMin = 0.0f;
        dataScalarMax = 1.0f;
    }

    resetCamera();

    {
        QString absPath = QFileInfo(QString::fromStdString(stdPath)).absoluteFilePath();
        recentFiles.removeAll(absPath);
        recentFiles.prepend(absPath);
        while (recentFiles.size() > 8) recentFiles.removeLast();
        emit meshLoadStateChanged();
        saveRecentToSettings();
    }

    emit meshLoadStateChanged();
    emit meshDataUpdated();
}

void Renderer::resetCamera() {
    camera.focalPoint = glm::dvec3(worldCenterX, worldCenterY, worldCenterZ);

    // Fit the whole bounding box in view. worldRadius is only half the largest
    // single axis, which under-frames elongated/cubic meshes in some viewports
    // (the mesh spills past the edges). Use the full box diagonal so any mesh
    // shape/orientation is fully contained, and pad for the 45° FOV + aspect.
    const double dx = worldMaxX - worldMinX;
    const double dy = worldMaxY - worldMinY;
    const double dz = worldMaxZ - worldMinZ;
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double fitRadius = diag * 0.5;

    // Account for the narrower of (horizontal, vertical) due to aspect ratio so
    // the mesh never clips at the short edge of a non-square viewport.
    const double aspect = (height > 0) ? (static_cast<double>(width) / static_cast<double>(height)) : 1.0;
    const double fov = glm::radians(45.0);
    const double vFov = fov;
    const double hFov = 2.0 * std::atan(std::tan(fov * 0.5) * aspect);
    const double effFov = std::min(vFov, hFov);
    double dist = fitRadius / std::tan(effFov * 0.5);
    dist *= 1.3; // margin so it isn't edge-to-edge

    camera.distance = dist;
    if (camera.distance < 1.0) camera.distance = 1.0;
    camera.maxDistance = std::max(1000.0, camera.distance * 50.0);
    nearPlane = std::max(0.01, camera.distance * 0.01);
    farPlane  = std::max(100.0, camera.distance * 20.0);
    camera.position = camera.focalPoint + glm::dvec3(0.0, 0.0, camera.distance);
    camera.viewUp = glm::dvec3(0.0, 1.0, 0.0);
    camera.orthogonalizeViewUp();
    emit viewChanged();
    emit meshDataUpdated();
}

void Renderer::resizeViewport(int w, int h) {
    width = w;
    height = h;
}

void Renderer::setColormapChoice(int choice) {
    if (colormap.scalarChoice() == choice) return;
    colormap.setScalarChoice(choice);
    emit colormapChanged();
}

void Renderer::setColormapReversed(bool reversed) {
    if (colormap.scalarReversed() == reversed) return;
    colormap.setScalarReversed(reversed);
    emit colormapChanged();
}

void Renderer::setVectorColormapReversed(bool reversed) {
    if (colormap.vectorReversed() == reversed) return;
    colormap.setVectorReversed(reversed);
    emit vectorColormapChanged();
}

void Renderer::applyLightingPreset(int preset) {
    lighting.applyPreset(preset);
    emit lightingParametersChanged();
}

void Renderer::resetLighting() {
    lighting.reset();
    emit lightingParametersChanged();
}

void Renderer::setWireframe(bool enabled) {
    if (showWireframe == enabled) return;
    showWireframe = enabled;
    emit wireframeChanged();
}

void Renderer::setUseLod(bool enabled) {
    if (useLod == enabled) return;
    useLod = enabled;
    cameraMoving = false;
    if (m_lodTimer) m_lodTimer->stop();
    emit viewChanged();
}

void Renderer::toggleSurface(bool visible) {
    if (showSurface == visible) return;
    showSurface = visible;
    emit surfaceVisibilityChanged();
}

void Renderer::snapToOrthoView(int axis) {
    camera.snapToOrthoView(axis);
    emit viewChanged();
}

void Renderer::snapToAxisView(int axis, bool flip) {
    int preset = flip ? (axis * 2 + 1) : (axis * 2);
    camera.snapToOrthoView(preset);
    emit viewChanged();
}

void Renderer::requestScreenshot(const QString& path) {
    if (path.isEmpty()) return;
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
    // Alpha is only meaningful for PNG exports; for JPEG/BMP we capture 3
    // channels so the byte layout matches saveToFile (which also drops alpha
    // for non-PNG). This keeps capture and save in lockstep.
    const bool captureTransparent = config.transparentBackground && (config.format == ExportFormat::PNG);
    std::vector<unsigned char> pixels = ScreenshotExporter::captureFBO(boundFbo, deviceW, deviceH, captureTransparent);

    bool success = ScreenshotExporter::saveToFile(config.filePath, pixels, deviceW, deviceH, config);
    if (success) emit screenshotCaptured(targetPath);
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
    int choice = colormap.scalarChoice();
    bool reversed = colormap.scalarReversed();
    for (int i = 0; i <= steps; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(steps);
        float s = reversed ? (1.0f - t) : t;
        glm::vec3 c = Colormaps::evaluate(s, static_cast<ColormapType>(choice));
        QVariantList stop;
        stop << t << c.r << c.g << c.b;
        out.append(QVariant(stop));
    }
    return out;
}

QVariantList Renderer::getVectorColormapStops() const {
    QVariantList out;
    const int steps = 16;
    int choice = colormap.vectorChoice();
    bool reversed = colormap.vectorReversed();
    for (int i = 0; i <= steps; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(steps);
        float s = reversed ? (1.0f - t) : t;
        glm::vec3 c = Colormaps::evaluate(s, static_cast<ColormapType>(choice));
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
    meshManager.meshChanged = true; // trigger GPU re-upload of the scalar buffer
    emit meshDataUpdated();
    emit meshLoadStateChanged();
}

void Renderer::setActiveVectorField(const QString& fieldName) {
    if (fieldName.isEmpty()) return;
    cachedMeshSource.vectorName = fieldName.toStdString();
    vectorGlyphDirty = true; // GL rebuild deferred to render thread
    emit meshDataUpdated();
}

void Renderer::drawGizmo() {
    glDisable(GL_DEPTH_TEST);
    gizmo.draw(camera.getViewMatrix(), static_cast<float>(devicePixelRatio),
               static_cast<int>(height * devicePixelRatio));
    if (lighting.lightKitEnabled && lighting.showLightMarkers) {
        glm::vec3 kitDirs[5] = {
            LightingModel::kitDirection(lighting.lightKeyAzimuth,  lighting.lightKeyElevation),
            LightingModel::kitDirection(lighting.lightFillAzimuth, lighting.lightFillElevation),
            LightingModel::kitDirection(lighting.lightBackAzimuth,  lighting.lightBackElevation),
            LightingModel::kitDirection(lighting.lightBackAzimuth + 180.0f, -lighting.lightBackElevation),
            LightingModel::kitDirection(lighting.lightHeadAzimuth,  lighting.lightHeadElevation),
        };
        auto warmTint = [](float w) -> glm::vec3 {
            if (w < 0.5f) return glm::mix(glm::vec3(0.6f,0.7f,1.0f), glm::vec3(1.0f), w/0.5f);
            return glm::mix(glm::vec3(1.0f), glm::vec3(1.0f,0.85f,0.7f), (w-0.5f)/0.5f);
        };
        glm::vec3 tint = warmTint(lighting.lightWarm);
        glm::vec3 cols[5] = { tint, tint * 0.9f, tint * 0.95f, tint * 0.95f, glm::vec3(1.0f, 1.0f, 1.0f) };
        gizmo.drawLights(kitDirs, cols, static_cast<float>(devicePixelRatio),
                         static_cast<int>(height * devicePixelRatio));
    }
    glEnable(GL_DEPTH_TEST);
}

void Renderer::drawColorbarLegends(int deviceW, int deviceH) {
    if (deviceW <= 0 || deviceH <= 0) return;
    const float dpr = static_cast<float>(devicePixelRatio);

    // Scalar colorbar: bottom-right (corner 0).
    if (hasMeshLoaded && meshHasScalars && showScalarColorbar) {
        ColorbarData data;
        data.visible = true;
        data.title = QString::fromStdString(activeScalarName);
        data.stops = getColormapStops();
        // The gradient is drawn top->t=1 (max) -> bottom->t=0 (min), so the top
        // tick label must be the MAX value. i=0 is the top of the bar.
        const int tickCount = colorbarTicks;
        const float range = dataScalarMax - dataScalarMin;
        for (int i = 0; i < tickCount; ++i) {
            const float frac = tickCount > 1 ? static_cast<float>(i) / static_cast<float>(tickCount - 1) : 0.0f;
            const float v = dataScalarMax - range * frac; // top = max, bottom = min
            data.tickLabels.append(QString::number(v, 'f', 3));
        }
        colorbarOverlay.draw(dpr, deviceW, deviceH, data, 0);
    }

    // Vector magnitude colorbar: top-right (corner 1).
    if (showVectors && vectorUseColormap && hasMeshLoaded && !getAvailableVectors().isEmpty()) {
        ColorbarData data;
        data.visible = true;
        data.title = getVectorField() + "\u{27A1}";
        data.stops = getVectorColormapStops();
        // Same orientation as the scalar bar: top of the gradient is the MAX
        // magnitude, so the top label is magMax.
        const int tickCount = colorbarTicks;
        const float range = vectorGlyph.magMax - vectorGlyph.magMin;
        for (int i = 0; i < tickCount; ++i) {
            const float frac = tickCount > 1 ? static_cast<float>(i) / static_cast<float>(tickCount - 1) : 0.0f;
            const float v = vectorGlyph.magMax - range * frac; // top = max, bottom = min
            data.tickLabels.append(QString::number(v, 'f', 3));
        }
        colorbarOverlay.draw(dpr, deviceW, deviceH, data, 1);
    }
}

void Renderer::renderFrame() {
    // GL glyph rebuild must run on the render thread. Consume the flag once.
    if (vectorGlyphDirty.exchange(false)) {
        vectorGlyph.rebuild(cachedMeshSource, vectorStride);
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);

    // Opaque background normally; when exporting a transparent PNG, clear with
    // zero alpha so the background stays transparent. The viewport FBO is
    // allocated with an RGBA8 color attachment, so this alpha is actually retained.
    const float clearAlpha = screenshotTransparent ? 0.0f : 1.0f;
    glClearColor(bgColor[0], bgColor[1], bgColor[2], clearAlpha);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    int deviceW = static_cast<int>(width * devicePixelRatio);
    int deviceH = static_cast<int>(height * devicePixelRatio);
    glViewport(0, 0, deviceW, deviceH);

    glm::mat4 view = camera.getViewMatrix();

    // Clip planes must track the current camera distance.
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

    // Keep LUTs (scalar + vector) in sync every frame so palette/choice/reverse
    // changes take effect even when the surface itself is hidden.
    colormap.update();

    if ((showSurface || showWireframe) && meshManager.hasMeshes() && shaderProgram != 0) {
        glUseProgram(shaderProgram);

        glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
        glUniformMatrix4fv(modelLoc, 1, GL_FALSE, glm::value_ptr(model));
        glUniformMatrix4fv(viewLoc, 1, GL_FALSE, glm::value_ptr(view));

        glm::vec3 kDir, fDir, b1Dir, b2Dir, hDir;
        computeLightDirections(kDir, fDir, b1Dir, b2Dir, hDir);
        glUniform3fv(lightDirLoc, 1, glm::value_ptr(kDir));
        glUniform3fv(lightFillLoc, 1, glm::value_ptr(fDir));
        glUniform3fv(lightBack1Loc, 1, glm::value_ptr(b1Dir));
        glUniform3fv(lightBack2Loc, 1, glm::value_ptr(b2Dir));
        glUniform3fv(lightHeadLoc, 1, glm::value_ptr(hDir));

        glm::vec3 camPos = glm::vec3(camera.position);
        glUniform3fv(viewPosLoc, 1, glm::value_ptr(camPos));

        glUniform3fv(colorLoc, 1, meshColor);
        glUniform3fv(surfaceColorLoc, 1, surfaceColor);
        glUniform1f(matAmbientLoc, lighting.matAmbient);
        glUniform1f(matDiffuseLoc, lighting.matDiffuse);
        glUniform1f(matSpecularLoc, lighting.matSpecular);
        glUniform1f(matShininessLoc, lighting.matShininess);

        float keyI = lighting.lightKitEnabled ? lighting.lightKeyIntensity : 0.0f;
        glUniform1f(keyIntensityLoc,  keyI);
        glUniform1f(fillIntensityLoc, lighting.lightKitEnabled ? keyI / lighting.lightKF : 0.0f);
        glUniform1f(headIntensityLoc, lighting.lightKitEnabled ? keyI / lighting.lightKH : 0.0f);
        glUniform1f(backIntensityLoc, lighting.lightKitEnabled ? keyI / lighting.lightKB : 0.0f);

        auto warmTint = [](float w) -> glm::vec3 {
            if (w < 0.5f) return glm::mix(glm::vec3(0.6f,0.7f,1.0f), glm::vec3(1.0f), w/0.5f);
            return glm::mix(glm::vec3(1.0f), glm::vec3(1.0f,0.85f,0.7f), (w-0.5f)/0.5f);
        };
        glm::vec3 tint = warmTint(lighting.lightWarm);
        const float keyCol[3]  = { tint.r,       tint.g,       tint.b };
        const float fillCol[3] = { tint.r*0.90f, tint.g*0.92f, tint.b*1.00f };
        const float backCol[3] = { tint.r*0.95f, tint.g*0.95f, tint.b*0.98f };
        const float headCol[3] = { 1.0f, 1.0f, 1.0f };
        glUniform3fv(keyColorLoc, 1, keyCol);
        glUniform3fv(fillColorLoc, 1, fillCol);
        glUniform3fv(backColorLoc, 1, backCol);
        glUniform3fv(headColorLoc, 1, headCol);

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

        if (meshHasScalars && colormap.scalarTexture() != 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_1D, colormap.scalarTexture());
            glUniform1i(lutTextureLoc, 0);
        }

        std::vector<std::pair<GLuint, int>> drawList;
        meshManager.snapshotDrawList(drawList, useLod, cameraMoving.load());

        for (const auto& e : drawList) {
            glBindVertexArray(e.first);

            if (showSurface) {
                glUniform1i(wireframeLoc, 0);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                glDrawElements(GL_TRIANGLES, e.second, GL_UNSIGNED_INT, 0);
            }

            if (showWireframe) {
                glUniform1i(wireframeLoc, 1);
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                glEnable(GL_POLYGON_OFFSET_LINE);
                glPolygonOffset(-1.0f, -1.0f);
                glDrawElements(GL_TRIANGLES, e.second, GL_UNSIGNED_INT, 0);
                glDisable(GL_POLYGON_OFFSET_LINE);
            }
        }
        glBindVertexArray(0);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glUseProgram(0);
    }

    if (showVectors && vectorGlyph.instanceCount > 0 && glyphProgram != 0) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glUseProgram(glyphProgram);
        glUniformMatrix4fv(glyphMvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
        glUniform1f(glyphScaleLoc, vectorScale);
        glm::vec3 kDir, fDir, b1Dir, b2Dir, hDir;
        computeLightDirections(kDir, fDir, b1Dir, b2Dir, hDir);
        glUniform3fv(glyphLightDirLoc, 1, glm::value_ptr(kDir));
        glm::vec3 camPos = glm::vec3(camera.position);
        glUniform3fv(glyphViewPosLoc, 1, glm::value_ptr(camPos));
        glUniform3fv(glyphColorLoc, 1, vectorColor);
        glUniform1i(glyphUseColormapLoc, vectorUseColormap ? 1 : 0);
        glUniform1f(glyphMagMinLoc, vectorGlyph.magMin);
        glUniform1f(glyphMagMaxLoc, vectorGlyph.magMax);
        if (vectorUseColormap && colormap.vectorTexture() != 0) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_1D, colormap.vectorTexture());
            glUniform1i(glyphLutLoc, 1);
            glActiveTexture(GL_TEXTURE0);
        }
        glBindVertexArray(vectorGlyph.vao);
        glDrawElementsInstanced(GL_TRIANGLES, vectorGlyph.glyphIndexCount, GL_UNSIGNED_INT, 0, vectorGlyph.instanceCount);
        glBindVertexArray(0);
        glUseProgram(0);
    }

    // In transparent export mode, skip the procedural grid entirely so only the
    // mesh (and gizmo) show over a transparent background.
    if (!screenshotTransparent) drawGrid(view, proj);

    if (showGizmo) drawGizmo();

    // Draw the colorbar legends into the FBO so they appear in screenshots
    // (including transparent PNG exports). Mirrors the QML overlay visibility.
    drawColorbarLegends(deviceW, deviceH);

    if (showFps) {
        auto now = std::chrono::steady_clock::now();
        if (m_frameCount == 0) {
            m_lastFrameTime = now;
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

    QQuickOpenGLUtils::resetOpenGLState();
}
