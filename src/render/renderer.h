#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

// Raw OpenGL / math dependencies only. NO Qt Object macros: this class runs
// strictly on the QSG render thread and must never be touched from the GUI
// thread. All view/visual state arrives via a deep-copied RenderRenderState
// produced on the GUI thread by RenderSettings::buildRenderState().
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <optional>
#include <chrono>
#include <map>

#include "core/mesh_loader.h"
#include "render/gizmo.h"
#include "render/colorbar_overlay.h"
#include "core/Camera.h"
#include "export/screenshot.h"

#include "render/LightingModel.h"
#include "render/ColormapManager.h"
#include "render/VectorGlyphSet.h"
#include "render/MeshGLManager.h"

class QOpenGLFramebufferObject;

// ---------------------------------------------------------------------------
// RenderRenderState
//
// Plain-C++ snapshot of every visual / camera parameter the render thread
// needs to draw one frame. RenderSettings (GUI thread) produces a copy of
// this; ViewportFboRenderer::synchronize() deep-copies it into the backend
// Renderer. No QObject, no shared pointers across threads — a value copy.
// ---------------------------------------------------------------------------
struct RenderRenderState {
    // View / camera (value copy, no aliasing)
    Camera camera;

    // Display toggles
    bool showWireframe = false;
    bool showSurface = true;
    bool showGrid = false;
    bool showGizmo = true;
    bool autoRotate = false;
    bool showFps = false;
    bool useLod = true;

    // Colors
    float meshColor[3] = { 0.4f, 0.9f, 0.4f };
    float surfaceColor[3] = { 1.0f, 1.0f, 1.0f };
    float bgColor[3] = { 0.12f, 0.12f, 0.12f };

    // World bounds / extents (for camera fit + clip range context)
    double worldCenterX = 0, worldCenterY = 0, worldCenterZ = 0;
    double worldRadius = 1.0;
    double worldMinX = -10.0, worldMaxX = 10.0;
    double worldMinY = -10.0, worldMaxY = 10.0;
    double worldMinZ = -10.0, worldMaxZ = 10.0;

    // Lighting (value copy)
    LightingModel lighting;

    // Colormap choices/reversed only; the LUT textures themselves are built on
    // the render thread by ColormapManager (GL resources cannot cross threads).
    int colormapChoice = 3;
    bool colormapReversed = false;
    int vectorColormapChoice = 3;
    bool vectorColormapReversed = false;

    // Scalar field
    bool meshHasScalars = false;
    float scalarMin = 0.0f;
    float scalarMax = 1.0f;
    float dataScalarMin = 0.0f;
    float dataScalarMax = 1.0f;
    float filterMin = 0.0f;
    float filterMax = 1.0f;
    bool showScalarColorbar = true;
    int colorbarTicks = 6;
    std::string activeScalarName;

    // Slice / clip
    bool clipEnabled = false;
    float sliceHeightX = 0.0f;
    float sliceHeightY = 0.0f;
    float sliceHeightZ = 0.0f;
    bool invertX = false;
    bool invertY = false;
    bool invertZ = false;

    // Vector glyphs
    bool showVectors = false;
    float vectorScale = 1.0f;
    int vectorStride = 1;
    float vectorColor[3] = { 0.2f, 0.6f, 1.0f };
    bool vectorUseColormap = false;
    bool vectorScaleByMagnitude = false;
    std::string vectorField;

    // Screenshot export options
    bool screenshotTransparent = false;
    int screenshotQuality = 95;
    int screenshotScale = 1;

    bool hasMeshLoaded = false;
};

// ---------------------------------------------------------------------------
// Renderer — PURE C++ backend.
//
// Owns the high-level GPU/state responsibilities and delegates to four
// cohesive helpers:
//   - LightingModel    : 4-point light kit params, presets, direction math
//   - ColormapManager  : scalar + vector-magnitude LUT textures & choices
//   - VectorGlyphSet   : instanced arrow glyph GPU resources + magnitude range
//   - MeshGLManager    : full + decimated (LOD) GPU meshes & upload/teardown
//
// It holds NO QObject / Q_PROPERTY / Q_INVOKABLE / signals. The GUI thread
// never reads or writes its members; the render thread drives it exclusively
// through setState(), the mesh/scalar handoff queues, and renderFrame().
// ---------------------------------------------------------------------------
class Renderer {
public:
    Renderer();
    ~Renderer();

    // Core Initialization & Graphics Lifecycle Routines (render thread).
    void initGLAD();
    void initShaders();
    void initGrid();
    void initGizmo();
    void renderFrame();

    // Deep-copy the GUI-thread snapshot into this render-thread instance.
    void setState(const RenderRenderState& state) { m_state = state; }

    // Snapshot accessors used by the FBO renderer (drawn state only).
    bool autoRotate() const { return m_state.autoRotate; }
    bool showFps() const { return m_state.showFps; }

    // Uploads CPU geometry to the GPU. Safe to call on the render thread.
    void uploadMesh(std::shared_ptr<const RenderMesh> renderMesh);
    void drawGrid(const glm::mat4& view, const glm::mat4& proj);

    // Pending mesh handoff (GUI -> render thread). setPendingMesh() stores a
    // shared_ptr (no copy) plus a dirty flag; renderFrame() consumes it and
    // uploads on the render thread under the GL context.
    void setPendingMesh(std::shared_ptr<const RenderMesh> renderMesh);

    // Mark the camera as moving and (re)start the LOD debounce timer.
    void markCameraMoving();
    void markVectorGlyphDirty() { vectorGlyphDirty = true; }
    void onLodTimer();
    void resizeViewport(int width, int height);

    void setDevicePixelRatio(float dpr) { devicePixelRatio = dpr; }

    // Scalar-only re-upload handoff.
    bool consumeScalarDirty();
    void markScalarDirty() { scalarDirty = true; }
    void updateScalarsOnGPU(const std::vector<float>& scalars);

    // Drop all GPU meshes (GUI-thread request, safe to call any time; real GL
    // teardown happens in meshManager with a current context on the render thread).
    void clearGpuMeshes();

    // Render-thread accessors used by ViewportFboRenderer.
    bool hasGpuMeshes() const { return meshManager.hasMeshes(); }
    const std::vector<float>& cachedScalars() const { return m_pendingScalarSrc; }

    // Screenshot capture (render thread, GL context current).
    bool captureScreenshotToFile(const QString& path, QOpenGLFramebufferObject* fbo = nullptr);

    // Lighting presets resolve in pure data (no signals needed on backend).
    void applyLightingPreset(int preset);
    void resetLighting();

    // Camera reset needs world bounds — done on the backend from the snapshot
    // so the GUI camera and the render camera stay consistent. Returns nothing;
    // the new camera lives in m_state.camera until the next snapshot.
    void resetCamera();

    void snapToOrthoView(int axis);
    void snapToAxisView(int axis, bool flip);

    // Vector magnitude range (rebuilt on upload by VectorGlyphSet).
    float vectorMagMin() const { return vectorGlyph.magMin; }
    float vectorMagMax() const { return vectorGlyph.magMax; }

private:
    void drawGizmo();
    void drawColorbarLegends(int deviceW, int deviceH);
    void computeLightDirections(glm::vec3& key, glm::vec3& fill, glm::vec3& back1, glm::vec3& back2, glm::vec3& head);
    std::string readShaderFile(const std::string& filePath);

    // Display Dimension Registers
    int width = 800;
    int height = 600;
    float devicePixelRatio = 1.0f;

    // Viewport Core Transform Tracking
    Gizmo gizmo;
    ColorbarOverlay colorbarOverlay;

    GLuint shaderProgram = 0;
    GLuint vao = 0, vbo = 0, ebo = 0;

    // Shader uniform cache registry
    GLint mvpLoc = -1;
    GLint modelLoc = -1;
    GLint viewLoc = -1;
    GLint lightDirLoc = -1;
    GLint viewPosLoc = -1;
    GLint wireframeLoc = -1;
    GLint colorLoc = -1;
    GLint surfaceColorLoc = -1;
    GLint lightFillLoc = -1;
    GLint lightBack1Loc = -1;
    GLint lightBack2Loc = -1;
    GLint lightHeadLoc = -1;
    GLint matAmbientLoc = -1;
    GLint matDiffuseLoc = -1;
    GLint matSpecularLoc = -1;
    GLint matShininessLoc = -1;
    GLint keyIntensityLoc = -1;
    GLint fillIntensityLoc = -1;
    GLint headIntensityLoc = -1;
    GLint backIntensityLoc = -1;

    GLint keyColorLoc = -1;
    GLint fillColorLoc = -1;
    GLint backColorLoc = -1;
    GLint headColorLoc = -1;

    GLint sliceHeightXLoc = -1;
    GLint sliceHeightYLoc = -1;
    GLint sliceHeightZLoc = -1;
    GLint invertXLoc = -1;
    GLint invertYLoc = -1;
    GLint invertZLoc = -1;
    GLint filterMinLoc = -1;
    GLint filterMaxLoc = -1;
    GLint clipEnabledLoc = -1;
    GLint scalarMinLoc = -1;
    GLint scalarMaxLoc = -1;
    GLint hasScalarsLoc = -1;
    GLint lutTextureLoc = -1;

    // glyph program + uniform cache
    GLuint glyphProgram = 0;
    GLint glyphMvpLoc = -1;
    GLint glyphScaleLoc = -1;
    GLint glyphLightDirLoc = -1;
    GLint glyphViewPosLoc = -1;
    GLint glyphColorLoc = -1;
    GLint glyphUseColormapLoc = -1;
    GLint glyphMagMinLoc = -1;
    GLint glyphMagMaxLoc = -1;
    GLint glyphLutLoc = -1;
    GLint glyphScaleByMagLoc = -1;

    double camDistance = 3.0;
    double nearPlane = 0.1;
    double farPlane = 100.0;
    std::atomic<bool> cameraMoving{false};

    // grid (procedural ray-cast ground plane)
    GLuint gridVAO = 0, gridVBO = 0;
    GLuint gridProgram = 0;
    double gridPlaneY = 0.0;
    GLint gridInvViewLoc = -1, gridInvProjLoc = -1;
    GLint gridViewLoc = -1, gridProjLoc = -1;
    GLint gridCamPosLoc = -1, gridColorLoc = -1, gridBgLoc = -1, gridFalloffLoc = -1, gridPlaneYLoc = -1;

    std::atomic<bool> vectorGlyphDirty{false};

    bool m_destroying = false;

    // Scalar-field switch signal: set on the GUI thread and consumed here.
    std::atomic<bool> scalarDirty{false};

    std::shared_ptr<const RenderMesh> m_pendingMesh;        // handoff from GUI (shared, no copy)
    std::shared_ptr<const RenderMesh> m_lastUploadedMesh;   // kept for deferred vector-glyph rebuilds
    std::mutex meshQueueMutex;
    std::vector<float> m_pendingScalarSrc;

    std::chrono::steady_clock::time_point m_lastMotion;

    // Deep-copied snapshot; the ONLY source of truth renderFrame() reads.
    RenderRenderState m_state;

    // --- extracted responsibility helpers -------------------------------------
    LightingModel lighting;       // 4-point light kit params, presets, dir math
    ColormapManager colormap;     // scalar + vector LUT textures & choices
    VectorGlyphSet vectorGlyph;   // instanced arrow GPU resources + mag range
    MeshGLManager meshManager;     // full + decimated GPU meshes & upload
};
