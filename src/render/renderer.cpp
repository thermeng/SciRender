#include "render/renderer.h"
#include "render/render_config.h"
#include "render/StreamlineSet.h"
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
    if (QOpenGLContext::currentContext()) {
        if (meshUbo) glDeleteBuffers(1, &meshUbo);
        if (gridUbo) glDeleteBuffers(1, &gridUbo);
        if (glyphUbo) glDeleteBuffers(1, &glyphUbo);
        if (shaderProgram) glDeleteProgram(shaderProgram);
        if (gridProgram) glDeleteProgram(gridProgram);
        if (gridVAO) glDeleteVertexArrays(1, &gridVAO);
        if (gridVBO) glDeleteBuffers(1, &gridVBO);
        if (bboxProgram) glDeleteProgram(bboxProgram);
        if (bboxVao) glDeleteVertexArrays(1, &bboxVao);
        if (bboxVbo) glDeleteBuffers(1, &bboxVbo);
        if (qualityOpenEdgesVao) glDeleteVertexArrays(1, &qualityOpenEdgesVao);
        if (qualityOpenEdgesVbo) glDeleteBuffers(1, &qualityOpenEdgesVbo);
        if (qualityNonManifoldVao) glDeleteVertexArrays(1, &qualityNonManifoldVao);
        if (qualityNonManifoldVbo) glDeleteBuffers(1, &qualityNonManifoldVbo);
        if (qualityDegenerateVao) glDeleteVertexArrays(1, &qualityDegenerateVao);
        if (qualityDegenerateVbo) glDeleteBuffers(1, &qualityDegenerateVbo);
        colormap.shutdown();
        vectorGlyph.shutdown();
        streamlineSet.shutdown();
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

    if (!gladLoaderLoadGL()) {
        qFatal("Fatal: GLAD failed to load core OpenGL functions.");
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

void Renderer::initShaders(const ShaderSources& sources) {
    const std::string& vertSrcStr = sources.meshVert;
    const std::string& fragSrcStr = sources.meshFrag;

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
        glDeleteProgram(shaderProgram);
        shaderProgram = 0;
        glDeleteShader(vert);
        glDeleteShader(frag);
        return;
    }

    glDeleteShader(vert);
    glDeleteShader(frag);

    meshUboIndex = glGetUniformBlockIndex(shaderProgram, "MeshUBO");
    glUniformBlockBinding(shaderProgram, meshUboIndex, 0);

    lutTextureLoc = glGetUniformLocation(shaderProgram, "uColormapLUT");

    // instanced vector glyph program
    const std::string& gvert = sources.glyphVert;
    const std::string& gfrag = sources.glyphFrag;
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
            glyphLutLoc = glGetUniformLocation(glyphProgram, "uColormapLUT");
            glyphUboIndex = glGetUniformBlockIndex(glyphProgram, "GlyphUBO");
            if (glyphUboIndex != GL_INVALID_INDEX) {
                glUniformBlockBinding(glyphProgram, glyphUboIndex, 1);
                glCreateBuffers(1, &glyphUbo);
                glNamedBufferData(glyphUbo, sizeof(GlyphUBOData), nullptr, GL_DYNAMIC_DRAW);
                glBindBufferBase(GL_UNIFORM_BUFFER, 1, glyphUbo);
            }
        }
    }

    // bbox overlay program
    const std::string& bvert = sources.bboxVert;
    const std::string& bfrag = sources.bboxFrag;
    if (!bvert.empty() && !bfrag.empty()) {
        GLuint bv = glCreateShader(GL_VERTEX_SHADER);
        GLuint bf = glCreateShader(GL_FRAGMENT_SHADER);
        if (!compileShader(bv, bvert.c_str(), "BBOX_VERT") || !compileShader(bf, bfrag.c_str(), "BBOX_FRAG")) {
            glDeleteShader(bv); glDeleteShader(bf);
            bboxProgram = 0;
        } else {
            bboxProgram = glCreateProgram();
            glAttachShader(bboxProgram, bv); glAttachShader(bboxProgram, bf);
            glLinkProgram(bboxProgram);
            GLint bboxLinked = 0;
            glGetProgramiv(bboxProgram, GL_LINK_STATUS, &bboxLinked);
            if (!bboxLinked) {
                char log[512];
                glGetProgramInfoLog(bboxProgram, 512, nullptr, log);
                printf("BBox shader program linking error: %s\n", log);
                glDeleteProgram(bboxProgram);
                bboxProgram = 0;
            }
            glDeleteShader(bv); glDeleteShader(bf);
            if (bboxProgram != 0) {
                bboxMvpLoc = glGetUniformLocation(bboxProgram, "uMVP");
                bboxColorLoc = glGetUniformLocation(bboxProgram, "uColor");
            }
        }
    }

    // streamline line-list program
    const std::string& svert = sources.streamlineVert;
    const std::string& sfrag = sources.streamlineFrag;
    if (!svert.empty() && !sfrag.empty()) {
        GLuint sv = glCreateShader(GL_VERTEX_SHADER);
        GLuint sf = glCreateShader(GL_FRAGMENT_SHADER);
        if (!compileShader(sv, svert.c_str(), "STREAMLINE_VERT") || !compileShader(sf, sfrag.c_str(), "STREAMLINE_FRAG")) {
            glDeleteShader(sv); glDeleteShader(sf);
            streamlineProgram = 0;
        } else {
            streamlineProgram = glCreateProgram();
            glAttachShader(streamlineProgram, sv); glAttachShader(streamlineProgram, sf);
            glLinkProgram(streamlineProgram);

            GLint linked = 0;
            glGetProgramiv(streamlineProgram, GL_LINK_STATUS, &linked);
            if (!linked) {
                char log[512];
                glGetProgramInfoLog(streamlineProgram, 512, nullptr, log);
                printf("Streamline shader program linking error: %s\n", log);
                glDeleteProgram(streamlineProgram);
                streamlineProgram = 0;
            }
            glDeleteShader(sv); glDeleteShader(sf);
            if (streamlineProgram != 0) {
                streamlineLutLoc = glGetUniformLocation(streamlineProgram, "uColormapLUT");
                if (streamlineUbo == 0) {
                    glCreateBuffers(1, &streamlineUbo);
                    glNamedBufferData(streamlineUbo, sizeof(StreamlineUBOData), nullptr, GL_DYNAMIC_DRAW);
                }
            }
        }
    }

    // seed point program
    const std::string& sdvert = sources.seedVert;
    const std::string& sdfrag = sources.seedFrag;
    if (!sdvert.empty() && !sdfrag.empty()) {
        GLuint sdv = glCreateShader(GL_VERTEX_SHADER);
        GLuint sdf = glCreateShader(GL_FRAGMENT_SHADER);
        if (!compileShader(sdv, sdvert.c_str(), "SEED_VERT") || !compileShader(sdf, sdfrag.c_str(), "SEED_FRAG")) {
            glDeleteShader(sdv); glDeleteShader(sdf);
            seedProgram = 0;
        } else {
            seedProgram = glCreateProgram();
            glAttachShader(seedProgram, sdv); glAttachShader(seedProgram, sdf);
            glLinkProgram(seedProgram);

            GLint linked = 0;
            glGetProgramiv(seedProgram, GL_LINK_STATUS, &linked);
            if (!linked) {
                char log[512];
                glGetProgramInfoLog(seedProgram, 512, nullptr, log);
                printf("Seed shader program linking error: %s\n", log);
                glDeleteProgram(seedProgram);
                seedProgram = 0;
            }
            glDeleteShader(sdv); glDeleteShader(sdf);
            if (seedProgram != 0) {
                seedMvpLoc = glGetUniformLocation(seedProgram, "uMVP");
                seedColorLoc = glGetUniformLocation(seedProgram, "uColor");
                seedPointSizeLoc = glGetUniformLocation(seedProgram, "uPointSize");
            }
        }
    }
}

void Renderer::initGrid(const ShaderSources& sources) {
    const std::string& vs = sources.gridVert;
    const std::string& fs = sources.gridFrag;
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
    GLint gridLinked = 0;
    glGetProgramiv(gridProgram, GL_LINK_STATUS, &gridLinked);
    if (!gridLinked) {
        char log[512];
        glGetProgramInfoLog(gridProgram, 512, nullptr, log);
        printf("Grid shader program linking error: %s\n", log);
        glDeleteProgram(gridProgram);
        gridProgram = 0;
    }

    const float q[8] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f, 1.0f, 1.0f };
    glCreateVertexArrays(1, &gridVAO);
    glCreateBuffers(1, &gridVBO);
    glEnableVertexArrayAttrib(gridVAO, 0);
    glVertexArrayAttribFormat(gridVAO, 0, 2, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(gridVAO, 0, 0);
    glNamedBufferData(gridVBO, sizeof(q), q, GL_STATIC_DRAW);
    glVertexArrayVertexBuffer(gridVAO, 0, gridVBO, 0, 2 * sizeof(float));
}

void Renderer::updateGridUbo(const glm::mat4& view, const glm::mat4& proj) {
    if (gridProgram == 0) return;
    if (gridUbo == 0) {
        gridUboIndex = glGetUniformBlockIndex(gridProgram, "GridUBO");
        if (gridUboIndex != GL_INVALID_INDEX) {
            glUniformBlockBinding(gridProgram, gridUboIndex, 2);
            glCreateBuffers(1, &gridUbo);
            glNamedBufferData(gridUbo, sizeof(GridUBOData), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_UNIFORM_BUFFER, 2, gridUbo);
        }
    }
    if (gridUbo == 0) return;
    GridUBOData ubo{};
    ubo.invView = glm::inverse(view);
    ubo.invProj = glm::inverse(proj);
    ubo.view = view;
    ubo.proj = proj;
    ubo.camPos_colorR = glm::vec4(glm::vec3(m_state.camera.position), 0.0f);
    float bgLum = 0.299f * m_state.bgColor[0] + 0.587f * m_state.bgColor[1] + 0.114f * m_state.bgColor[2];
    glm::vec3 gridCol = (bgLum > 0.5f) ? glm::vec3(0.18f, 0.18f, 0.20f) : glm::vec3(0.78f, 0.78f, 0.82f);
    ubo.colorBG_falloff = glm::vec4(gridCol.r, gridCol.g, gridCol.b, 0.02f);
    gridPlaneY = m_state.hasMeshLoaded ? m_state.worldMinY : 0.0;
    ubo.planeY_pad = glm::vec4(static_cast<float>(gridPlaneY), 0.0f, 0.0f, 0.0f);
    glNamedBufferSubData(gridUbo, 0, sizeof(GridUBOData), &ubo);
}

void Renderer::drawGrid(const glm::mat4& view, const glm::mat4& proj) {
    if (!m_state.showGrid || gridProgram == 0) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glUseProgram(gridProgram);

    updateGridUbo(view, proj);

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
    streamlineSet.rebuild(*renderMesh, m_state.streamlineSeedCount, m_state.streamlineStepSize, m_state.streamlineMaxSteps, m_state.vectorField, m_state.seedMode, m_state.seedPlanePos, m_state.seedJitter, m_state.showStreamlineArrows, m_state.streamlineArrowSpacing, m_state.streamlineArrowSize);
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
    dist *= RenderConfig::defaults().cameraFitMultiplier;

    camera.distance = dist;
    // ponytail: keep ortho zoom baseline in sync with the fit distance so
    // dolly-zoom tracks correctly after a reset (m_orthoRefDist is seeded once).
    m_orthoRefDist = camera.distance;
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
    qualityOverlayDirty = true;
    if (qualityOpenEdgesVao) { glDeleteVertexArrays(1, &qualityOpenEdgesVao); qualityOpenEdgesVao = 0; }
    if (qualityOpenEdgesVbo) { glDeleteBuffers(1, &qualityOpenEdgesVbo); qualityOpenEdgesVbo = 0; }
    if (qualityNonManifoldVao) { glDeleteVertexArrays(1, &qualityNonManifoldVao); qualityNonManifoldVao = 0; }
    if (qualityNonManifoldVbo) { glDeleteBuffers(1, &qualityNonManifoldVbo); qualityNonManifoldVbo = 0; }
    if (qualityDegenerateVao) { glDeleteVertexArrays(1, &qualityDegenerateVao); qualityDegenerateVao = 0; }
    if (qualityDegenerateVbo) { glDeleteBuffers(1, &qualityDegenerateVbo); qualityDegenerateVbo = 0; }
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
    GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_TEST);
    gizmo.draw(m_state.camera.getViewMatrix(), static_cast<float>(devicePixelRatio));
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
        gizmo.drawLights(kitDirs, cols, static_cast<float>(devicePixelRatio));
    }
    if (depthWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
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
    if (m_state.hasMeshLoaded && m_state.meshHasScalars && m_state.meshUseScalarColor && m_state.showScalarColorbar) {
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

void Renderer::drawBoundingBox(const glm::mat4& view, const glm::mat4& proj) {
    if (!m_state.showBounds || bboxProgram == 0) return;
    if (!meshManager.hasMeshes()) return;

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
    if (bboxVao == 0) {
        glCreateVertexArrays(1, &bboxVao);
        glCreateBuffers(1, &bboxVbo);
        glEnableVertexArrayAttrib(bboxVao, 0);
        glVertexArrayAttribFormat(bboxVao, 0, 3, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(bboxVao, 0, 0);
        glNamedBufferData(bboxVbo, sizeof(c), c, GL_STATIC_DRAW);
        glVertexArrayVertexBuffer(bboxVao, 0, bboxVbo, 0, 3 * sizeof(float));
    }

    glm::vec3 center(static_cast<float>(m_state.worldCenterX),
                     static_cast<float>(m_state.worldCenterY),
                     static_cast<float>(m_state.worldCenterZ));
    glm::vec3 diag(static_cast<float>(m_state.worldMaxX - m_state.worldMinX),
                   static_cast<float>(m_state.worldMaxY - m_state.worldMinY),
                   static_cast<float>(m_state.worldMaxZ - m_state.worldMinZ));
    glm::mat4 model = glm::translate(glm::mat4(1.0f), center) *
                      glm::scale(glm::mat4(1.0f), diag);
    glm::mat4 mvp = proj * view * model;

    GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_TEST);
    glLineWidth(1.0f);

    glUseProgram(bboxProgram);
    glUniformMatrix4fv(bboxMvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform4f(bboxColorLoc,
                m_state.meshColor[0],
                m_state.meshColor[1],
                m_state.meshColor[2],
                1.0f);

    glBindVertexArray(bboxVao);
    glDrawArrays(GL_LINES, 0, 24);
    glBindVertexArray(0);

    if (depthWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glUseProgram(0);
}

void Renderer::buildQualityOverlayVAOs() {
    auto buildOne = [&](GLuint& vao, GLuint& vbo, const std::vector<float>& verts) {
        if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
        if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
        if (verts.empty()) return;
        glCreateVertexArrays(1, &vao);
        glCreateBuffers(1, &vbo);
        glEnableVertexArrayAttrib(vao, 0);
        glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(vao, 0, 0);
        glNamedBufferData(vbo, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
        glVertexArrayVertexBuffer(vao, 0, vbo, 0, 3 * sizeof(float));
    };
    buildOne(qualityOpenEdgesVao, qualityOpenEdgesVbo, m_state.qualityOpenEdges);
    buildOne(qualityNonManifoldVao, qualityNonManifoldVbo, m_state.qualityNonManifoldEdges);
    buildOne(qualityDegenerateVao, qualityDegenerateVbo, m_state.qualityDegenerateTris);
    qualityOverlayDirty = false;
}

void Renderer::renderFrame() {
    // LOD debounce: once 140 ms have elapsed since the last camera motion, clear
    // the moving flag so the next frame uses the full-resolution mesh.
    if (cameraMoving.load()) {
        auto now = std::chrono::steady_clock::now();
        auto dt = std::chrono::duration<double>(now - m_lastMotion).count();
        if (dt >= RenderConfig::defaults().lodDebounceSeconds) cameraMoving = false;
    }

    // Compute LOD throttle: only re-dispatch once per camera-motion burst.
    // Without this, every frame during a 140 ms camera-moving window would
    // run the full 3-pass compute pipeline plus read-back, causing driver stalls.
    if (cameraMoving.load() && !m_wasCameraMoving) {
        gpuDecimationDirty = true;
    }
    m_wasCameraMoving = cameraMoving.load();

    // Consume a pending mesh handoff from the GUI thread (shared_ptr; no copy).
    // Uploading here keeps all GL work inside render() with the context current.
    {
        std::lock_guard<std::mutex> lock(meshQueueMutex);
        if (m_pendingMesh) {
            uploadMesh(m_pendingMesh);
            m_pendingMesh.reset();
            qualityOverlayDirty = true;
        }
    }

    if (vectorGlyphDirty.exchange(false)) {
        if (m_lastUploadedMesh) vectorGlyph.rebuild(*m_lastUploadedMesh, m_state.vectorStride, m_state.vectorField, m_state.vectorMagTransform);
    }

    if (streamlineDirty.exchange(false)) {
        if (m_lastUploadedMesh) streamlineSet.rebuild(*m_lastUploadedMesh, m_state.streamlineSeedCount, m_state.streamlineStepSize, m_state.streamlineMaxSteps, m_state.vectorField, m_state.seedMode, m_state.seedPlanePos, m_state.seedJitter, m_state.showStreamlineArrows, m_state.streamlineArrowSpacing, m_state.streamlineArrowSize);
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
    farPlane  = camDist + m_state.worldRadius + 250.0;

    // Clip control: switch post-projection NDC to Vulkan-style [0,1] depth so the
    // grid shader can skip manual gl_FragDepth remap and gain 24-bit extra precision.
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

    glm::mat4 proj = m_state.orthographic
        ? [&]() {
            if (m_orthoRefDist <= 0.0) m_orthoRefDist = std::max(m_state.camera.distance, 1e-6);
            float d = static_cast<float>(m_state.camera.distance / m_orthoRefDist);
            float half = static_cast<float>(m_state.worldRadius * d);
            float aspect = (deviceH > 0) ? static_cast<float>(deviceW) / static_cast<float>(deviceH) : 1.0f;
            float n = static_cast<float>(nearPlane);
            float f = static_cast<float>(farPlane);
            const float r = half * aspect;
            const float t = half;
            glm::mat4 p(1.0f);
            p[0][0] = 2.0f / (r + r);
            p[1][1] = 2.0f / (t + t);
            p[2][2] = -1.0f / (f - n);
            p[2][3] = -n / (f - n);
            p[3][3] = 1.0f;
            return p;
          }()
        : [&]() {
            float aspect = (deviceH > 0) ? static_cast<float>(deviceW) / static_cast<float>(deviceH) : 1.0f;
            float n = static_cast<float>(nearPlane);
            float f = static_cast<float>(farPlane);
            float fov = glm::radians(45.0f);
            float tanHalf = std::tan(fov * 0.5f);
            float r = 1.0f / (aspect * tanHalf);
            float t = 1.0f / tanHalf;
            glm::mat4 p(1.0f);
            p[0][0] = r;
            p[1][1] = t;
            p[2][2] = f / (n - f);
            p[2][3] = -1.0f;
            p[3][2] = n * f / (n - f);
            return p;
          }();

    glm::mat4 model = glm::mat4(1.0f);
    glm::mat4 mvp = proj * view * model;

    // Push the colormap choice/reversed snapshot into the GPU LUT manager.
    colormap.setScalarChoice(m_state.colormapChoice);
    colormap.setScalarReversed(m_state.colormapReversed);
    colormap.setVectorChoice(m_state.vectorColormapChoice);
    colormap.setVectorReversed(m_state.vectorColormapReversed);
    colormap.update();

    const bool useLod = m_state.useLod;
    if (meshManager.hasMeshes() && shaderProgram != 0) {
        glUseProgram(shaderProgram);

        if (meshUbo == 0) {
            glCreateBuffers(1, &meshUbo);
            glNamedBufferData(meshUbo, sizeof(MeshUBOData), nullptr, GL_DYNAMIC_DRAW);
            if (meshUboIndex != GL_INVALID_INDEX)
                glBindBufferBase(GL_UNIFORM_BUFFER, 0, meshUbo);
        }
        MeshUBOData ubo{};
        ubo.mvp = mvp;
        ubo.model = model;
        ubo.viewPos_ps = glm::vec4(glm::vec3(m_state.camera.position), m_state.pointSize);
        ubo.meshColor_wire = glm::vec4(m_state.meshColor[0], m_state.meshColor[1], m_state.meshColor[2], 0.0f);
        ubo.surfaceColor_sop = glm::vec4(m_state.surfaceColor[0], m_state.surfaceColor[1], m_state.surfaceColor[2], m_state.surfaceOpacity);
        ubo.point_clip = glm::vec4(0.0f, m_state.pointUseScalar ? 1.0f : 0.0f, m_state.pointOpacity, m_state.clipEnabled ? 1.0f : 0.0f);
        glm::vec3 kDir, fDir, b1Dir, b2Dir, hDir;
        computeLightDirections(kDir, fDir, b1Dir, b2Dir, hDir);
        ubo.lightDir = glm::vec4(kDir, 0.0f);
        ubo.lightFill = glm::vec4(fDir, 0.0f);
        ubo.lightBack1 = glm::vec4(b1Dir, 0.0f);
        ubo.lightBack2 = glm::vec4(b2Dir, 0.0f);
        ubo.lightHead = glm::vec4(hDir, 0.0f);
        auto warmTint = [](float w) -> glm::vec3 {
            if (w < 0.5f) return glm::mix(glm::vec3(0.6f,0.7f,1.0f), glm::vec3(1.0f), w/0.5f);
            return glm::mix(glm::vec3(1.0f), glm::vec3(1.0f,0.85f,0.7f), (w-0.5f)/0.5f);
        };
        glm::vec3 tint = warmTint(m_state.lighting.lightWarm);
        ubo.keyColor = glm::vec4(tint, 0.0f);
        ubo.fillColor = glm::vec4(tint * glm::vec3(0.90f, 0.92f, 1.00f), 0.0f);
        ubo.backColor = glm::vec4(tint * glm::vec3(0.95f, 0.95f, 0.98f), 0.0f);
        ubo.headColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
        ubo.scalars = glm::vec4(m_state.scalarMin, m_state.scalarMax, (m_state.meshHasScalars && m_state.meshUseScalarColor) ? 1.0f : 0.0f, 0.0f);
        ubo.sliceY = glm::vec4(m_state.sliceHeightX, m_state.sliceHeightY, m_state.sliceHeightZ, 0.0f);
        ubo.sliceEn = glm::vec4(m_state.sliceEnabledX ? 1.0f : 0.0f, m_state.sliceEnabledY ? 1.0f : 0.0f, m_state.sliceEnabledZ ? 1.0f : 0.0f, 0.0f);
        ubo.invert = glm::vec4(m_state.invertX ? 1.0f : 0.0f, m_state.invertY ? 1.0f : 0.0f, m_state.invertZ ? 1.0f : 0.0f, 0.0f);
        ubo.filter = glm::vec4(m_state.filterMin, m_state.filterMax, 0.0f, 0.0f);
        float keyI = m_state.lighting.lightKitEnabled ? m_state.lighting.lightKeyIntensity : 0.0f;
        float kf = std::max(m_state.lighting.lightKF, 0.001f);
        float kh = std::max(m_state.lighting.lightKH, 0.001f);
        float kb = std::max(m_state.lighting.lightKB, 0.001f);
        ubo.intensities = glm::vec4(keyI, m_state.lighting.lightKitEnabled ? keyI / kf : 0.0f, m_state.lighting.lightKitEnabled ? keyI / kb : 0.0f, m_state.lighting.lightKitEnabled ? keyI / kh : 0.0f);
        ubo.material = glm::vec4(m_state.lighting.matAmbient, m_state.lighting.matDiffuse, m_state.lighting.matSpecular, m_state.lighting.matShininess);
        glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);

        if (m_state.meshHasScalars && m_state.meshUseScalarColor && colormap.scalarTexture() != 0) {
            glBindTextureUnit(0, colormap.scalarTexture());
            glUniform1i(lutTextureLoc, 0);
        }

        if (useLod && gpuDecimationDirty.load() && meshManager.hasDecimated() && meshManager.hasFullSource()) {
            Mesh newDec;
            if (meshManager.dispatchLodCompute(*meshManager.getFullSource(), newDec)) {
                meshManager.replaceDecimatedMesh(0, newDec);
            } else {
                qWarning() << "[LOD] GPU compute decimation failed, using CPU fallback";
            }
            gpuDecimationDirty = false;
        }

        std::vector<std::pair<GLuint, int>> drawList;
        std::vector<int> drawMode;
        std::vector<int> drawVerts;
        meshManager.snapshotDrawList(drawList, m_state.useLod, cameraMoving.load(), drawMode, drawVerts);

        for (size_t di = 0; di < drawList.size(); ++di) {
            glBindVertexArray(drawList[di].first);

            if (m_state.showSurface) {
                // ponytail: user cullMode toggle (0=off, 1=back, 2=front). Cull only
                // when opaque — culling + alpha blend gives wrong results.
                const bool opaque = m_state.surfaceOpacity >= 0.999f;
                const bool cull = m_state.cullMode != 0 && opaque;
                if (cull) { glEnable(GL_CULL_FACE); glCullFace(m_state.cullMode == 2 ? GL_FRONT : GL_BACK); }
                else glDisable(GL_CULL_FACE);
                if (!opaque) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                // ponytail: push the FILL surface slightly back in depth so the
                // coincident cell-edge GL_LINES (drawn later) win the depth test
                // without z-fighting. Polygon offset is a no-op for GL_LINES, so
                // offsetting the lines themselves (the old -2,-2) did nothing and
                // the lines interleaved with the surface. FILL offset only affects
                // triangles, leaving the wireframe GL_LINE pass untouched.
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(1.0f, 1.0f);
                glDrawElements(GL_TRIANGLES, drawList[di].second, GL_UNSIGNED_INT, 0);
                glDisable(GL_POLYGON_OFFSET_FILL);
                if (cull) glDisable(GL_CULL_FACE);
                if (!opaque) glDisable(GL_BLEND);
            }

            if (m_state.showWireframe) {
                glLineWidth(m_state.lineWidth); // ponytail: clamped to driver GL_ALIASED_LINE_WIDTH_RANGE
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                glEnable(GL_POLYGON_OFFSET_LINE);
                glPolygonOffset(-1.0f, -1.0f);
                ubo.meshColor_wire.w = 1.0f;
                glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);
                glDrawElements(GL_TRIANGLES, drawList[di].second, GL_UNSIGNED_INT, 0);
                glDisable(GL_POLYGON_OFFSET_LINE);
                glLineWidth(1.0f);
                ubo.meshColor_wire.w = 0.0f;
                glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);
            }

            // ponytail: points overlay — works for STL + VTK + POLYDATA alike
            if (m_state.showPoints && drawVerts[di] > 0) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                ubo.point_clip.x = 1.0f;
                glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);
                glDrawArrays(GL_POINTS, 0, drawVerts[di]);
                ubo.point_clip.x = 0.0f;
                glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);
                glDisable(GL_BLEND);
            }
        }
        glBindVertexArray(0);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        // ponytail: ParaView-style cell edges — true per-cell boundaries (quads
        // without the triangle diagonal). Drawn from the per-mesh line VBO built
        // at load from globalCellToVertices. Reuses mesh shader + wireframe color.
        if (m_state.showCellEdges && shaderProgram != 0) {
            auto ce = meshManager.getCellEdgeLine();
            if (ce.first != 0 && ce.second > 0) {
                glEnable(GL_DEPTH_TEST);
                // ponytail: surface is pushed back via GL_POLYGON_OFFSET_FILL in
                // the fill pass, so these lines (at true depth) win cleanly.
                // A polygon offset here would be a no-op for GL_LINES.
                glLineWidth(m_state.cellEdgeLineWidth); // ponytail: own thickness, not wireframe's
                glBindVertexArray(ce.first);
                glDrawArrays(GL_LINES, 0, ce.second);
                glBindVertexArray(0);
                glLineWidth(1.0f);
                // depth test left enabled (it was on for the surface pass above);
                // no GL_LINES polygon offset to clear.
            }
        }

        // ponytail: mesh-quality highlight overlay — degenerate faces (red fill)
        // + open edges (amber) + non-manifold edges (magenta), drawn ON TOP of
        // the mesh. Depth test + cull are disabled so interior defects (coplanar
        // with the surface) are not z-rejected and hidden.
        if (m_state.showQualityOverlay && shaderProgram != 0) {
            GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
            GLboolean cullWas  = glIsEnabled(GL_CULL_FACE);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            if (qualityOverlayDirty) buildQualityOverlayVAOs();
            auto drawCached = [&](GLuint vao, GLsizei count) {
                if (vao == 0 || count <= 0) return;
                glBindVertexArray(vao);
                glDrawArrays(GL_LINES, 0, count);
                glBindVertexArray(0);
            };
            drawCached(qualityOpenEdgesVao, static_cast<GLsizei>(m_state.qualityOpenEdges.size() / 3));
            drawCached(qualityNonManifoldVao, static_cast<GLsizei>(m_state.qualityNonManifoldEdges.size() / 3));
            drawCached(qualityDegenerateVao, static_cast<GLsizei>(m_state.qualityDegenerateTris.size() / 3));
            if (depthWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
            if (cullWas)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
        }

        glUseProgram(0);

    drawBoundingBox(view, proj);

    if (m_state.showVectors && vectorGlyph.instanceCount > 0 && glyphProgram != 0) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glUseProgram(glyphProgram);
        if (glyphUbo == 0 && glyphUboIndex != GL_INVALID_INDEX) {
            glCreateBuffers(1, &glyphUbo);
            glNamedBufferData(glyphUbo, sizeof(GlyphUBOData), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_UNIFORM_BUFFER, 1, glyphUbo);
        }
        glm::vec3 kDir, fDir, b1Dir, b2Dir, hDir;
        computeLightDirections(kDir, fDir, b1Dir, b2Dir, hDir);
        glm::vec3 camPos = glm::vec3(m_state.camera.position);
        GlyphUBOData ubo{};
        ubo.mvp = mvp;
        ubo.scale_magMin_magMax_scaleByMag = glm::vec4(m_state.vectorScale, vectorGlyph.magMin, vectorGlyph.magMax, m_state.vectorScaleByMagnitude ? 1.0f : 0.0f);
        ubo.meshExtent_magTransform_viewPosY_colorR = glm::vec4(vectorGlyph.meshExtent, float(m_state.vectorMagTransform), camPos.y, m_state.vectorColor[0]);
        ubo.lightDir_colorGB = glm::vec4(kDir, m_state.vectorColor[1]);
        ubo.colorB_useColormap = glm::vec4(m_state.vectorColor[2], m_state.vectorUseColormap ? 1.0f : 0.0f, 0.0f, 0.0f);
        glNamedBufferSubData(glyphUbo, 0, sizeof(GlyphUBOData), &ubo);
        if (m_state.vectorUseColormap && colormap.vectorTexture() != 0) {
            glBindTextureUnit(1, colormap.vectorTexture());
            glUniform1i(glyphLutLoc, 1);
        }
        glBindVertexArray(vectorGlyph.vao);
        glDrawElementsInstanced(GL_TRIANGLES, vectorGlyph.glyphIndexCount, GL_UNSIGNED_INT, 0, vectorGlyph.instanceCount);
        glBindVertexArray(0);
        glUseProgram(0);
    }

    if (m_state.showStreamlines && !streamlineSet.empty() && streamlineProgram != 0) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glUseProgram(streamlineProgram);
        if (streamlineUbo == 0) {
            glCreateBuffers(1, &streamlineUbo);
            glNamedBufferData(streamlineUbo, sizeof(StreamlineUBOData), nullptr, GL_DYNAMIC_DRAW);
        }
        glm::vec3 kDir, fDir, b1Dir, b2Dir, hDir;
        computeLightDirections(kDir, fDir, b1Dir, b2Dir, hDir);
        glm::vec3 camPos = glm::vec3(m_state.camera.position);
        StreamlineUBOData ubo{};
        ubo.mvp = mvp;
        ubo.model = glm::mat4(1.0f);
        ubo.viewPos = glm::vec4(camPos, 0.0f);
        ubo.lightDir = glm::vec4(kDir, 0.0f);
        static float timeAccum = 0.0f;
        timeAccum += 0.016f;
        ubo.time_opacity = glm::vec4(timeAccum, 1.0f, 0.0f, 0.0f);
        ubo.color_useColormap = glm::vec4(m_state.streamlineColor[0], m_state.streamlineColor[1], m_state.streamlineColor[2], m_state.streamlineUseColormap ? 1.0f : 0.0f);
        ubo.magRange = glm::vec4(streamlineSet.magMin, streamlineSet.magMax, 0.0f, 0.0f);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, streamlineUbo);
        glNamedBufferSubData(streamlineUbo, 0, sizeof(StreamlineUBOData), &ubo);
        if (m_state.streamlineUseColormap && colormap.vectorTexture() != 0) {
            glBindTextureUnit(1, colormap.vectorTexture());
            glUniform1i(streamlineLutLoc, 1);
        }
        glBindVertexArray(streamlineSet.vao);
        glDrawArrays(GL_TRIANGLES, 0, streamlineSet.lineCount);
        glBindVertexArray(0);
        glUseProgram(0);
    }

    if (m_state.showSeeds && !streamlineSet.seedsEmpty() && seedProgram != 0) {
        glUseProgram(seedProgram);
        glm::vec4 seedColor(1.0f, 0.2f, 0.2f, 1.0f);
        glUniformMatrix4fv(seedMvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
        glUniform4fv(seedColorLoc, 1, glm::value_ptr(seedColor));
        glUniform1f(seedPointSizeLoc, 6.0f);
        glBindVertexArray(streamlineSet.seedVao);
        glDrawArrays(GL_POINTS, 0, streamlineSet.seedCount);
        glBindVertexArray(0);
        glUseProgram(0);
    }

    if (m_state.showStreamlineArrows && streamlineSet.arrowCount > 0 && streamlineProgram != 0) {
        glUseProgram(streamlineProgram);
        if (streamlineUbo != 0) {
            glm::vec3 kDir, fDir, b1Dir, b2Dir, hDir;
            computeLightDirections(kDir, fDir, b1Dir, b2Dir, hDir);
            glm::vec3 camPos = glm::vec3(m_state.camera.position);
            StreamlineUBOData ubo{};
            ubo.mvp = mvp;
            ubo.model = glm::mat4(1.0f);
            ubo.viewPos = glm::vec4(camPos, 0.0f);
            ubo.lightDir = glm::vec4(kDir, 0.0f);
            static float timeAccum = 0.0f;
            timeAccum += 0.016f;
            ubo.time_opacity = glm::vec4(timeAccum, 1.0f, 0.0f, 0.0f);
            ubo.color_useColormap = glm::vec4(m_state.streamlineColor[0], m_state.streamlineColor[1], m_state.streamlineColor[2], 0.0f);
            ubo.magRange = glm::vec4(streamlineSet.magMin, streamlineSet.magMax, 0.0f, 0.0f);
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, streamlineUbo);
            glNamedBufferSubData(streamlineUbo, 0, sizeof(StreamlineUBOData), &ubo);
        }
        glBindVertexArray(streamlineSet.arrowVao);
        glDrawArrays(GL_TRIANGLES, 0, streamlineSet.arrowCount);
        glBindVertexArray(0);
        glUseProgram(0);
    }

    if (!m_state.screenshotTransparent) drawGrid(view, proj);

    if (m_state.showGizmo) drawGizmo();

    drawColorbarLegends(deviceW, deviceH);

    QQuickOpenGLUtils::resetOpenGLState();
}
}

