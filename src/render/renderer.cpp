#include "render/renderer.h"
#include "core/Colormaps.h"
#include "core/Camera.h"
#include "core/mesh_loader.h"
#include <QOpenGLFramebufferObject>

#include <cstring>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
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
#include <QPainter>
#include <QFont>

#include <QDir>
#include <QDateTime>
#include <QRegularExpression>
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

Renderer::Renderer()
    : m_state() {
    // Default system initialization parameters (mirror RenderSettings defaults;
    // the first synchronize() overwrites these with the GUI snapshot).
    m_state.meshColor[0] = 0.4f; m_state.meshColor[1] = 0.9f; m_state.meshColor[2] = 0.4f;
    m_state.surfaceColor[0] = 1.0f; m_state.surfaceColor[1] = 1.0f; m_state.surfaceColor[2] = 1.0f;
    m_state.bgColor[0] = 0.12f; m_state.bgColor[1] = 0.12f; m_state.bgColor[2] = 0.12f;
    m_state.worldCenterX = 0.0; m_state.worldCenterY = 0.0; m_state.worldCenterZ = 0.0;
    m_state.worldRadius = 1.0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
Renderer::~Renderer() {
    m_destroying = true;
    // GL deletes must only run with a current context. The Renderer lives on the
    // render thread and is torn down after the scene graph, so no GL context is
    // current here -> skip them; the driver reclaims the resources with the context.
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

    std::string vertSrcStr = loadEmbeddedShader(":/SciRenderUI/src/shaders/mesh.vert");
    std::string fragSrcStr = loadEmbeddedShader(":/SciRenderUI/src/shaders/mesh.frag");

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
    meshColorLoc = glGetUniformLocation(shaderProgram, "uMeshColor");
    pointSizeLoc = glGetUniformLocation(shaderProgram, "uPointSize");
    isPointLoc = glGetUniformLocation(shaderProgram, "uIsPoint");
    pointUseScalarLoc = glGetUniformLocation(shaderProgram, "uPointUseScalar");
    pointOpacityLoc = glGetUniformLocation(shaderProgram, "uPointOpacity");
    surfaceOpacityLoc = glGetUniformLocation(shaderProgram, "uSurfaceOpacity");

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
    std::string gvert = loadEmbeddedShader(":/SciRenderUI/src/shaders/glyph.vert");
    std::string gfrag = loadEmbeddedShader(":/SciRenderUI/src/shaders/glyph.frag");
    if (!gvert.empty() && !gfrag.empty()) {
        GLuint gv = glCreateShader(GL_VERTEX_SHADER);
        GLuint gf = glCreateShader(GL_FRAGMENT_SHADER);
        if (!compileShader(gv, gvert.c_str(), "GLYPH_VERT") || !compileShader(gf, gfrag.c_str(), "GLYPH_FRAG")) {
            glDeleteShader(gv);
            glDeleteShader(gf);
            glyphProgram = 0;
        } else {
            glyphProgram = glCreateProgram();
            glAttachShader(glyphProgram, gv); glAttachShader(glyphProgram, gf);
            glLinkProgram(glyphProgram);

            GLint glyphLinked = 0;
            glGetProgramiv(glyphProgram, GL_LINK_STATUS, &glyphLinked);
            if (!glyphLinked) {
                char log[512];
                glGetProgramInfoLog(glyphProgram, 512, nullptr, log);
                printf("Glyph shader program linking error: %s\n", log);
                glDeleteProgram(glyphProgram);
                glyphProgram = 0;
            }

            glDeleteShader(gv); glDeleteShader(gf);
        }

        if (glyphProgram != 0) {
            glyphMvpLoc = glGetUniformLocation(glyphProgram, "uMVP");
            glyphScaleLoc = glGetUniformLocation(glyphProgram, "uScale");
            glyphLightDirLoc = glGetUniformLocation(glyphProgram, "uLightDir");
            glyphViewPosLoc = glGetUniformLocation(glyphProgram, "uViewPos");
            glyphColorLoc = glGetUniformLocation(glyphProgram, "uColor");
            glyphUseColormapLoc = glGetUniformLocation(glyphProgram, "uUseColormap");
            glyphMagMinLoc = glGetUniformLocation(glyphProgram, "uMagMin");
            glyphMagMaxLoc = glGetUniformLocation(glyphProgram, "uMagMax");
            glyphLutLoc = glGetUniformLocation(glyphProgram, "uColormapLUT");
            glyphScaleByMagLoc = glGetUniformLocation(glyphProgram, "uScaleByMag");
            glyphMeshExtentLoc = glGetUniformLocation(glyphProgram, "uMeshExtent");
            glyphMagTransformLoc = glGetUniformLocation(glyphProgram, "uMagTransform");
        }
    }
}

void Renderer::initGrid() {
    auto load = [](const QString& p) -> std::string {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { qCritical() << "grid shader missing" << p; return ""; }
        return QTextStream(&f).readAll().toStdString();
    };
    std::string vs = load(":/SciRenderUI/src/shaders/grid.vert");
    std::string fs = load(":/SciRenderUI/src/shaders/grid.frag");
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

    const float q[8] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f, 1.0f, 1.0f };
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
    if (!m_state.showGrid || gridProgram == 0) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glUseProgram(gridProgram);

    glm::mat4 invView = glm::inverse(view);
    glm::mat4 invProj = glm::inverse(proj);
    glUniformMatrix4fv(gridInvViewLoc, 1, GL_FALSE, glm::value_ptr(invView));
    glUniformMatrix4fv(gridInvProjLoc, 1, GL_FALSE, glm::value_ptr(invProj));
    glUniformMatrix4fv(gridViewLoc, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(gridProjLoc, 1, GL_FALSE, glm::value_ptr(proj));
    glUniform3f(gridCamPosLoc, (float)m_state.camera.position.x, (float)m_state.camera.position.y, (float)m_state.camera.position.z);
    float bgLum = 0.299f * m_state.bgColor[0] + 0.587f * m_state.bgColor[1] + 0.114f * m_state.bgColor[2];
    glm::vec3 gridCol = (bgLum > 0.5f) ? glm::vec3(0.18f, 0.18f, 0.20f)
                                        : glm::vec3(0.78f, 0.78f, 0.82f);
    glUniform3f(gridColorLoc, gridCol.r, gridCol.g, gridCol.b);
    glUniform3f(gridBgLoc, m_state.bgColor[0], m_state.bgColor[1], m_state.bgColor[2]);
    glUniform1f(gridFalloffLoc, 0.02f);
    // Align ground plane to the loaded mesh's y-min; before load, keep y=0.
    gridPlaneY = m_state.hasMeshLoaded ? m_state.worldMinY : 0.0;
    glUniform1f(gridPlaneYLoc, static_cast<float>(gridPlaneY));

    glBindVertexArray(gridVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glUseProgram(0);
}

void Renderer::initGizmo() {
    gizmo.init();
    colorbarOverlay.init();
}

void Renderer::uploadMesh(std::shared_ptr<const RenderMesh> renderMesh) {
    if (!renderMesh) return;
    meshManager.upload(renderMesh);
    m_lastUploadedMesh = renderMesh;
    vectorGlyph.rebuild(*renderMesh, m_state.vectorStride, m_state.vectorField, m_state.vectorMagTransform);
}

void Renderer::setPendingMesh(std::shared_ptr<const RenderMesh> renderMesh) {
    std::lock_guard<std::mutex> lock(meshQueueMutex);
    m_pendingMesh = std::move(renderMesh);
    meshManager.meshChanged = true;
}

void Renderer::markCameraMoving() {
    cameraMoving = true;
    m_lastMotion = std::chrono::steady_clock::now();
}

void Renderer::computeLightDirections(glm::vec3& key, glm::vec3& fill, glm::vec3& back1, glm::vec3& back2, glm::vec3& head) {
    m_state.lighting.computeDirections(m_state.camera.position, m_state.camera.focalPoint, m_state.camera.viewUp,
                               key, fill, back1, back2, head);
}

void Renderer::applyLightingPreset(int preset) {
    m_state.lighting.applyPreset(preset);
}

void Renderer::resetLighting() {
    m_state.lighting.reset();
}

void Renderer::resetCamera() {
    Camera& camera = m_state.camera;
    camera.focalPoint = glm::dvec3(m_state.worldCenterX, m_state.worldCenterY, m_state.worldCenterZ);

    const double dx = m_state.worldMaxX - m_state.worldMinX;
    const double dy = m_state.worldMaxY - m_state.worldMinY;
    const double dz = m_state.worldMaxZ - m_state.worldMinZ;
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double fitRadius = diag * 0.5;

    const double aspect = (height > 0) ? (static_cast<double>(width) / static_cast<double>(height)) : 1.0;
    const double fov = glm::radians(45.0);
    const double vFov = fov;
    const double hFov = 2.0 * std::atan(std::tan(fov * 0.5) * aspect);
    const double effFov = std::min(vFov, hFov);
    double dist = fitRadius / std::tan(effFov * 0.5);
    dist *= 1.3;

    camera.distance = dist;
    if (camera.distance < 1.0) camera.distance = 1.0;
    camera.maxDistance = std::max(1000.0, camera.distance * 50.0);
    nearPlane = std::max(0.01, camera.distance * 0.01);
    farPlane  = std::max(100.0, camera.distance * 20.0);
    camera.position = camera.focalPoint + glm::dvec3(0.0, 0.0, camera.distance);
    camera.viewUp = glm::dvec3(0.0, 1.0, 0.0);
    camera.orthogonalizeViewUp();
}

void Renderer::snapToOrthoView(int axis) {
    m_state.camera.snapToOrthoView(axis);
}

void Renderer::snapToAxisView(int axis, bool flip) {
    int preset = flip ? (axis * 2 + 1) : (axis * 2);
    m_state.camera.snapToOrthoView(preset);
}

void Renderer::resizeViewport(int w, int h) {
    width = w;
    height = h;
}

void Renderer::clearGpuMeshes() {
    meshManager.clear();
    vectorGlyph = VectorGlyphSet{};
    m_lastUploadedMesh.reset();
    m_pendingMesh.reset();
    m_state.hasMeshLoaded = false;
}

bool Renderer::consumeScalarDirty() {
    return scalarDirty.exchange(false);
}

void Renderer::updateScalarsOnGPU(std::shared_ptr<const std::vector<float>> scalars) {
    m_pendingScalarSrc = scalars; // shared_ptr, no data copy
    meshManager.updateScalars(scalars);
}

bool Renderer::captureViewportToFile(const QString& path) {
    if (path.isEmpty()) return false;
    if (!m_viewportFbo || !m_viewportFbo->isValid()) {
        qWarning() << "Screenshot skipped: viewport FBO not available.";
        return false;
    }

    const bool isPng = path.endsWith(".png", Qt::CaseInsensitive);
    const bool transparent = isPng && m_state.screenshotTransparent;

    // ponytail: MSAA FBOs cannot be read back with glReadPixels (undefined);
    // resolve to a single-sample target first.
    QOpenGLFramebufferObject* live = m_viewportFbo;
    QOpenGLFramebufferObject* readFbo = live;
    std::unique_ptr<QOpenGLFramebufferObject> resolveHolder;
    if (live->format().samples() > 0) {
        QOpenGLFramebufferObjectFormat rf;
        rf.setInternalTextureFormat(GL_RGBA8);
        resolveHolder = std::make_unique<QOpenGLFramebufferObject>(live->size(), rf);
        QOpenGLFramebufferObject::blitFramebuffer(resolveHolder.get(), live, GL_COLOR_BUFFER_BIT);
        readFbo = resolveHolder.get();
    }

    const int w = readFbo->width();
    const int h = readFbo->height();
    const int channels = transparent ? 4 : 3;
    const GLenum fmt = transparent ? GL_RGBA : GL_RGB;
    std::vector<unsigned char> raw(static_cast<size_t>(w) * h * channels);

    glBindFramebuffer(GL_FRAMEBUFFER, readFbo->handle());
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, fmt, GL_UNSIGNED_BYTE, raw.data());

    // Flip vertically (GL origin is bottom-left).
    std::vector<unsigned char> flipped(raw.size());
    const size_t row = static_cast<size_t>(w) * channels;
    for (int y = 0; y < h; ++y)
        std::memcpy(flipped.data() + static_cast<size_t>(y) * row,
                    raw.data() + static_cast<size_t>(h - 1 - y) * row, row);

    QImage::Format qf = transparent ? QImage::Format_RGBA8888 : QImage::Format_RGB888;
    QImage img = QImage(flipped.data(), w, h, static_cast<int>(row), qf).copy();

    const char* token = isPng ? "PNG"
                               : (path.endsWith(".bmp", Qt::CaseInsensitive) ? "BMP" : "JPG");
    const bool ok = img.save(path, token, -1);
    if (!ok) qWarning() << "Screenshot save failed:" << path;
    return ok;
}

void Renderer::drawGizmo() {
    glDisable(GL_DEPTH_TEST);
    gizmo.draw(m_state.camera.getViewMatrix(), static_cast<float>(devicePixelRatio),
               static_cast<int>(height * devicePixelRatio));
    if (m_state.lighting.lightKitEnabled && m_state.lighting.showLightMarkers) {
        glm::vec3 kitDirs[5] = {
            LightingModel::kitDirection(m_state.lighting.lightKeyAzimuth,  m_state.lighting.lightKeyElevation),
            LightingModel::kitDirection(m_state.lighting.lightFillAzimuth, m_state.lighting.lightFillElevation),
            LightingModel::kitDirection(m_state.lighting.lightBackAzimuth,  m_state.lighting.lightBackElevation),
            LightingModel::kitDirection(m_state.lighting.lightBackAzimuth + 180.0f, -m_state.lighting.lightBackElevation),
            LightingModel::kitDirection(m_state.lighting.lightHeadAzimuth,  m_state.lighting.lightHeadElevation),
        };
        auto warmTint = [](float w) -> glm::vec3 {
            if (w < 0.5f) return glm::mix(glm::vec3(0.6f,0.7f,1.0f), glm::vec3(1.0f), w/0.5f);
            return glm::mix(glm::vec3(1.0f), glm::vec3(1.0f,0.85f,0.7f), (w-0.5f)/0.5f);
        };
        glm::vec3 tint = warmTint(m_state.lighting.lightWarm);
        glm::vec3 cols[5] = { tint, tint * 0.9f, tint * 0.95f, tint * 0.95f, glm::vec3(1.0f, 1.0f, 1.0f) };
        gizmo.drawLights(kitDirs, cols, static_cast<float>(devicePixelRatio),
                         static_cast<int>(height * devicePixelRatio));
    }
    glEnable(GL_DEPTH_TEST);
}

void Renderer::drawColorbarLegends(int deviceW, int deviceH) {
    if (deviceW <= 0 || deviceH <= 0) return;
    const float dpr = static_cast<float>(devicePixelRatio);

    const int maxLegendW = static_cast<int>(width * dpr);
    const int maxLegendH = static_cast<int>(height * dpr);
    if (deviceW > maxLegendW) deviceW = maxLegendW;
    if (deviceH > maxLegendH) deviceH = maxLegendH;
    if (deviceW <= 0 || deviceH <= 0) return;

    const auto stopsFor = [&](int choice, bool reversed) {
        QVariantList out;
        const int steps = 16;
        for (int i = 0; i <= steps; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(steps);
            float s = reversed ? (1.0f - t) : t;
            glm::vec3 c = Colormaps::evaluate(s, static_cast<ColormapType>(choice));
            QVariantList stop;
            stop << t << c.r << c.g << c.b;
            out.append(QVariant(stop));
        }
        return out;
    };

    // Scalar colorbar: bottom-right (corner 0).
    if (m_state.hasMeshLoaded && m_state.meshHasScalars && m_state.showScalarColorbar) {
        ColorbarData data;
        data.visible = true;
        data.title = QString::fromStdString(m_state.activeScalarName);
        data.stops = stopsFor(m_state.colormapChoice, m_state.colormapReversed);
        const int tickCount = m_state.colorbarTicks;
        const float range = m_state.dataScalarMax - m_state.dataScalarMin;
        for (int i = 0; i < tickCount; ++i) {
            const float frac = tickCount > 1 ? static_cast<float>(i) / static_cast<float>(tickCount - 1) : 0.0f;
            const float v = m_state.dataScalarMax - range * frac;
            data.tickLabels.append(QString::number(v, 'f', 3));
        }
        colorbarOverlay.draw(dpr, deviceW, deviceH, data, 0);
    }

    // Vector magnitude colorbar: top-right (corner 1).
    if (m_state.showVectors && m_state.vectorUseColormap && m_state.hasMeshLoaded) {
        ColorbarData data;
        data.visible = true;
        data.title = QString::fromStdString(m_state.vectorField) + QChar(0x27A1);
        data.stops = stopsFor(m_state.vectorColormapChoice, m_state.vectorColormapReversed);
        const int tickCount = m_state.colorbarTicks;
        // The glyph shader maps color through txMag() (renderer state
        // vectorMagTransform), so the LUT gradient is linear in TRANSFORMED
        // magnitude. Tick labels must therefore invert the transform to show
        // raw magnitudes that line up with the arrow colors.
        auto txMag = [&](float m) -> float {
            if (m_state.vectorMagTransform == 1) return std::sqrt(std::max(m, 0.0f));
            if (m_state.vectorMagTransform == 2) return std::log(1.0f + std::max(m, 0.0f));
            return m;
        };
        auto invTxMag = [&](float t) -> float {
            if (m_state.vectorMagTransform == 1) return t * t;
            if (m_state.vectorMagTransform == 2) return std::exp(t) - 1.0f;
            return t;
        };
        const float tMin = txMag(vectorGlyph.magMin);
        const float tMax = txMag(vectorGlyph.magMax);
        const float tRange = tMax - tMin;
        for (int i = 0; i < tickCount; ++i) {
            const float frac = tickCount > 1 ? static_cast<float>(i) / static_cast<float>(tickCount - 1) : 0.0f;
            // frac = 0 is the top of the bar (max), frac = 1 the bottom (min).
            const float t = tMax - tRange * frac;
            const float v = invTxMag(t);
            data.tickLabels.append(QString::number(v, 'f', 3));
        }
        colorbarOverlay.draw(dpr, deviceW, deviceH, data, 1);
    }
}

void Renderer::renderFrame() {
    // LOD debounce: once 140 ms have elapsed since the last camera motion, clear
    // the moving flag so the next frame uses the full-resolution mesh.
    if (cameraMoving.load()) {
        auto now = std::chrono::steady_clock::now();
        auto dt = std::chrono::duration<double>(now - m_lastMotion).count();
        if (dt >= 0.14) cameraMoving = false;
    }

    // Consume a pending mesh handoff from the GUI thread (shared_ptr; no copy).
    // Uploading here keeps all GL work inside render() with the context current.
    {
        std::lock_guard<std::mutex> lock(meshQueueMutex);
        if (m_pendingMesh) {
            uploadMesh(m_pendingMesh);
            m_pendingMesh.reset();
        }
    }

    if (vectorGlyphDirty.exchange(false)) {
        if (m_lastUploadedMesh) vectorGlyph.rebuild(*m_lastUploadedMesh, m_state.vectorStride, m_state.vectorField, m_state.vectorMagTransform);
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);

    const float clearAlpha = m_state.screenshotTransparent ? 0.0f : 1.0f;
    glClearColor(m_state.bgColor[0], m_state.bgColor[1], m_state.bgColor[2], clearAlpha);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    int deviceW = static_cast<int>(width * devicePixelRatio);
    int deviceH = static_cast<int>(height * devicePixelRatio);
    glViewport(0, 0, deviceW, deviceH);

    glm::mat4 view = m_state.camera.getViewMatrix();

    double camDist = m_state.camera.distance;
    nearPlane = std::max(0.01, camDist * 0.001);
    farPlane  = std::max(farPlane, camDist + m_state.worldRadius + 250.0);

    glm::mat4 proj = m_state.orthographic
        ? [&]() { // ponytail: ortho frustum tracks camera.distance so dolly() zooms it
            if (m_orthoRefDist <= 0.0) m_orthoRefDist = m_state.camera.distance;
            float half = static_cast<float>(m_state.worldRadius * (m_state.camera.distance / m_orthoRefDist));
            float aspect = (deviceH > 0) ? static_cast<float>(deviceW) / static_cast<float>(deviceH) : 1.0f;
            return glm::ortho(-half * aspect, half * aspect, -half, half,
                              static_cast<float>(nearPlane), static_cast<float>(farPlane));
          }()
        : glm::perspective(
            glm::radians(45.0f),
            static_cast<float>(deviceW) / static_cast<float>(deviceH),
            static_cast<float>(nearPlane),
            static_cast<float>(farPlane)
            );

    glm::mat4 model = glm::mat4(1.0f);
    glm::mat4 mvp = proj * view * model;

    // Push the colormap choice/reversed snapshot into the GPU LUT manager.
    colormap.setScalarChoice(m_state.colormapChoice);
    colormap.setScalarReversed(m_state.colormapReversed);
    colormap.setVectorChoice(m_state.vectorColormapChoice);
    colormap.setVectorReversed(m_state.vectorColormapReversed);
    colormap.update();

    const bool useLod = true; // LOD handled internally by snapshot propagation
    if (meshManager.hasMeshes() && shaderProgram != 0) { // ponytail: also admits point clouds (no surface/wireframe flag)
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

        glm::vec3 camPos = glm::vec3(m_state.camera.position);
        glUniform3fv(viewPosLoc, 1, glm::value_ptr(camPos));

        glUniform3fv(colorLoc, 1, m_state.meshColor);
        glUniform3fv(surfaceColorLoc, 1, m_state.surfaceColor);
        glUniform1f(matAmbientLoc, m_state.lighting.matAmbient);
        glUniform1f(matDiffuseLoc, m_state.lighting.matDiffuse);
        glUniform1f(matSpecularLoc, m_state.lighting.matSpecular);
        glUniform1f(matShininessLoc, m_state.lighting.matShininess);

        float keyI = m_state.lighting.lightKitEnabled ? m_state.lighting.lightKeyIntensity : 0.0f;
        glUniform1f(keyIntensityLoc,  keyI);
        glUniform1f(fillIntensityLoc, m_state.lighting.lightKitEnabled ? keyI / m_state.lighting.lightKF : 0.0f);
        glUniform1f(headIntensityLoc, m_state.lighting.lightKitEnabled ? keyI / m_state.lighting.lightKH : 0.0f);
        glUniform1f(backIntensityLoc, m_state.lighting.lightKitEnabled ? keyI / m_state.lighting.lightKB : 0.0f);

        auto warmTint = [](float w) -> glm::vec3 {
            if (w < 0.5f) return glm::mix(glm::vec3(0.6f,0.7f,1.0f), glm::vec3(1.0f), w/0.5f);
            return glm::mix(glm::vec3(1.0f), glm::vec3(1.0f,0.85f,0.7f), (w-0.5f)/0.5f);
        };
        glm::vec3 tint = warmTint(m_state.lighting.lightWarm);
        const float keyCol[3]  = { tint.r,       tint.g,       tint.b };
        const float fillCol[3] = { tint.r*0.90f, tint.g*0.92f, tint.b*1.00f };
        const float backCol[3] = { tint.r*0.95f, tint.g*0.95f, tint.b*0.98f };
        const float headCol[3] = { 1.0f, 1.0f, 1.0f };
        glUniform3fv(keyColorLoc, 1, keyCol);
        glUniform3fv(fillColorLoc, 1, fillCol);
        glUniform3fv(backColorLoc, 1, backCol);
        glUniform3fv(headColorLoc, 1, headCol);

        glUniform1f(sliceHeightXLoc, m_state.sliceHeightX);
        glUniform1f(sliceHeightYLoc, m_state.sliceHeightY);
        glUniform1f(sliceHeightZLoc, m_state.sliceHeightZ);
        glUniform1i(invertXLoc, m_state.invertX ? 1 : 0);
        glUniform1i(invertYLoc, m_state.invertY ? 1 : 0);
        glUniform1i(invertZLoc, m_state.invertZ ? 1 : 0);
        glUniform1f(filterMinLoc, m_state.filterMin);
        glUniform1f(filterMaxLoc, m_state.filterMax);
        glUniform1i(clipEnabledLoc, m_state.clipEnabled ? 1 : 0);

        glUniform1f(scalarMinLoc, m_state.scalarMin);
        glUniform1f(scalarMaxLoc, m_state.scalarMax);
        glUniform1i(hasScalarsLoc, m_state.meshHasScalars ? 1 : 0);

        if (m_state.meshHasScalars && colormap.scalarTexture() != 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_1D, colormap.scalarTexture());
            glUniform1i(lutTextureLoc, 0);
        }

        std::vector<std::pair<GLuint, int>> drawList;
        std::vector<int> drawMode;
        std::vector<int> drawVerts;
        meshManager.snapshotDrawList(drawList, m_state.useLod, cameraMoving.load(), drawMode, drawVerts);

        for (size_t di = 0; di < drawList.size(); ++di) {
            glBindVertexArray(drawList[di].first);
            glUniform1i(isPointLoc, 0); // ponytail: reset; point block re-enables

            if (m_state.showSurface) {
                glUniform1i(wireframeLoc, 0);
                glUniform1f(surfaceOpacityLoc, m_state.surfaceOpacity);
                if (m_state.surfaceOpacity < 0.999f) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                glDrawElements(GL_TRIANGLES, drawList[di].second, GL_UNSIGNED_INT, 0);
                if (m_state.surfaceOpacity < 0.999f) glDisable(GL_BLEND);
            }

            if (m_state.showWireframe) {
                glUniform1i(wireframeLoc, 1);
                glLineWidth(m_state.lineWidth); // ponytail: clamped to driver GL_ALIASED_LINE_WIDTH_RANGE
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                glEnable(GL_POLYGON_OFFSET_LINE);
                glPolygonOffset(-1.0f, -1.0f);
                glDrawElements(GL_TRIANGLES, drawList[di].second, GL_UNSIGNED_INT, 0);
                glDisable(GL_POLYGON_OFFSET_LINE);
                glLineWidth(1.0f);
            }

            // ponytail: points overlay — works for STL + VTK + POLYDATA alike
            if (m_state.showPoints && drawVerts[di] > 0) {
                glEnable(GL_PROGRAM_POINT_SIZE);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glUniform1f(pointSizeLoc, m_state.pointSize);
                glUniform1i(isPointLoc, 1); // ponytail: frag carves sprite into sphere
                glUniform1i(pointUseScalarLoc, m_state.pointUseScalar ? 1 : 0);
                glUniform1f(pointOpacityLoc, m_state.pointOpacity);
                glUniform1i(wireframeLoc, 0);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                glDrawArrays(GL_POINTS, 0, drawVerts[di]);
                glDisable(GL_BLEND);
                glDisable(GL_PROGRAM_POINT_SIZE);
            }
        }
        glBindVertexArray(0);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        // ponytail: AABB wireframe overlay (reuses mesh shader, wireframe color)
        if (m_state.showBounds && shaderProgram != 0) {
            static GLuint bboxVao = 0, bboxVbo = 0;
            if (bboxVao == 0) {
                // 12 edges of a unit cube centered at origin, coords -0.5..0.5
                static const float c[24 * 3] = {
                    -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,
                     0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f,
                     0.5f, 0.5f,-0.5f, -0.5f, 0.5f,-0.5f,
                    -0.5f, 0.5f,-0.5f, -0.5f,-0.5f,-0.5f,
                    -0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,
                     0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f,
                     0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f,
                    -0.5f, 0.5f, 0.5f, -0.5f,-0.5f, 0.5f,
                    -0.5f,-0.5f,-0.5f, -0.5f,-0.5f, 0.5f,
                     0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f,
                     0.5f, 0.5f,-0.5f,  0.5f, 0.5f, 0.5f,
                    -0.5f, 0.5f,-0.5f, -0.5f, 0.5f, 0.5f
                };
                glGenVertexArrays(1, &bboxVao);
                glGenBuffers(1, &bboxVbo);
                glBindVertexArray(bboxVao);
                glBindBuffer(GL_ARRAY_BUFFER, bboxVbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(c), c, GL_STATIC_DRAW);
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
                glEnableVertexAttribArray(0);
                glBindVertexArray(0);
            }
            glm::vec3 center(static_cast<float>(m_state.worldCenterX),
                             static_cast<float>(m_state.worldCenterY),
                             static_cast<float>(m_state.worldCenterZ));
            glm::vec3 diag(static_cast<float>(m_state.worldMaxX - m_state.worldMinX),
                           static_cast<float>(m_state.worldMaxY - m_state.worldMinY),
                           static_cast<float>(m_state.worldMaxZ - m_state.worldMinZ));
            glm::mat4 model = glm::translate(glm::mat4(1.0f), center)
                            * glm::scale(glm::mat4(1.0f), diag);
            glm::mat4 mvp = proj * view * model;
            glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
            glUniform1i(wireframeLoc, 1);
            glUniform1i(isPointLoc, 0);
            glUniform3f(meshColorLoc, m_state.meshColor[0], m_state.meshColor[1], m_state.meshColor[2]);
            glBindVertexArray(bboxVao);
            glDrawArrays(GL_LINES, 0, 24);
            glBindVertexArray(0);
        }

        // ponytail: mesh-quality highlight overlay — degenerate faces (red fill)
        // + open edges (amber) + non-manifold edges (magenta), drawn ON TOP of
        // the mesh. Depth test + cull are disabled so interior defects (coplanar
        // with the surface) are not z-rejected and hidden.
        if (m_state.showQualityOverlay && shaderProgram != 0) {
            // ponytail: save state; restore to prev (mesh pass leaves cull OFF,
            // gizmo text quads rely on that — don't hard-enable cull here).
            GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
            GLboolean cullWas  = glIsEnabled(GL_CULL_FACE);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            auto drawList = [&](const std::vector<float>& verts, int mode, const float col[3]) {
                if (verts.empty()) return;
                GLuint vao = 0, vbo = 0;
                glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
                glBindVertexArray(vao);
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
                glEnableVertexAttribArray(0);
                glBindVertexArray(0);
                glm::mat4 mvp = proj * view * glm::mat4(1.0f);
                glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
                glUniform1i(wireframeLoc, (mode == GL_LINES) ? 1 : 0);
                glUniform1i(isPointLoc, 0);
                glUniform3f(meshColorLoc, col[0], col[1], col[2]);
                glBindVertexArray(vao);
                glDrawArrays(mode, 0, static_cast<GLsizei>(verts.size() / 3));
                glBindVertexArray(0);
                glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao);
            };
            const float red[3]     = {1.0f, 0.2f, 0.2f};    // ponytail: degenerate = deleted geometry
            const float amber[3]   = {1.0f, 0.6f, 0.1f};    // ponytail: open edge = boundary (expected on clips)
            const float magenta[3] = {1.0f, 0.2f, 1.0f};    // ponytail: non-manifold = topology error
            // ponytail: degenerate tris have ~zero area, so a FILL paints nothing.
            // Draw their 3 edges as red lines instead (visible + consistent with the
            // other two edge-based defect classes).
            drawList(m_state.qualityOpenEdges, GL_LINES, amber);
            drawList(m_state.qualityNonManifoldEdges, GL_LINES, magenta);
            drawList(m_state.qualityDegenerateTris, GL_LINES, red);
            if (depthWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
            if (cullWas)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
        }

        glUseProgram(0);


    if (m_state.showVectors && vectorGlyph.instanceCount > 0 && glyphProgram != 0) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glUseProgram(glyphProgram);
        glUniformMatrix4fv(glyphMvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
        glUniform1f(glyphScaleLoc, m_state.vectorScale);
        glm::vec3 kDir, fDir, b1Dir, b2Dir, hDir;
        computeLightDirections(kDir, fDir, b1Dir, b2Dir, hDir);
        glUniform3fv(glyphLightDirLoc, 1, glm::value_ptr(kDir));
        glm::vec3 camPos = glm::vec3(m_state.camera.position);
        glUniform3fv(glyphViewPosLoc, 1, glm::value_ptr(camPos));
        glUniform3fv(glyphColorLoc, 1, m_state.vectorColor);
        glUniform1i(glyphUseColormapLoc, m_state.vectorUseColormap ? 1 : 0);
        glUniform1f(glyphMagMinLoc, vectorGlyph.magMin);
        glUniform1f(glyphMagMaxLoc, vectorGlyph.magMax);
        glUniform1f(glyphScaleByMagLoc, m_state.vectorScaleByMagnitude ? 1.0f : 0.0f);
        glUniform1f(glyphMeshExtentLoc, vectorGlyph.meshExtent);
        glUniform1i(glyphMagTransformLoc, m_state.vectorMagTransform);
        if (m_state.vectorUseColormap && colormap.vectorTexture() != 0) {
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

    if (!m_state.screenshotTransparent) drawGrid(view, proj);

    if (m_state.showGizmo) drawGizmo();

    drawColorbarLegends(deviceW, deviceH);

    QQuickOpenGLUtils::resetOpenGLState();
}
}
