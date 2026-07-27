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
#include <glad/gl.h>
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

#include "render/LightingModel.h"
#include "render/ColormapManager.h"
#include "render/VectorGlyphSet.h"
#include "render/StreamlineSet.h"
#include "render/MeshGLManager.h"

#include <QOpenGLFramebufferObject>

// Shader source bundle — loaded by the caller (which has Qt resource access)
// and passed to Renderer::initShaders() so the Renderer stays Qt-free.
struct ShaderSources {
    std::string meshVert;
    std::string meshFrag;
    std::string gridVert;
    std::string gridFrag;
    std::string glyphVert;
    std::string glyphFrag;
    std::string bboxVert;
    std::string bboxFrag;
    std::string streamlineVert;
    std::string streamlineFrag;
    std::string seedVert;
    std::string seedFrag;
};

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
    float pointSize = 4.0f; // ponytail: CPU-driven gl_PointSize for point clouds
    float lineWidth = 1.0f; // ponytail: wireframe glLineWidth in px
    float cellEdgeLineWidth = 1.0f; // ponytail: separate thickness for cell-edge overlay
    bool showPoints = false; // ponytail: draw vertices as GL_POINTS
    bool pointUseScalar = true;  // ponytail: color points by scalar; else solid
    float pointOpacity = 1.0f;   // ponytail: point sprite alpha
    float surfaceOpacity = 1.0f; // ponytail: surface fill alpha
    int cullMode = 0;              // ponytail: 0=off by default — mirror of settings default
    bool showBounds = false;     // ponytail: AABB wireframe overlay
    bool showQualityOverlay = false; // ponytail: highlight degenerate faces + bad edges
    bool showCellEdges = false;      // ponytail: ParaView-style per-cell boundary edges
    // ponytail: overlay geometry (xyz floats), copied from RenderSettings at load
    std::vector<float> qualityDegenerateTris;
    std::vector<float> qualityOpenEdges;
    std::vector<float> qualityNonManifoldEdges;
    bool orthographic = false;    // ponytail: orthographic (parallel) projection

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
    bool meshUseScalarColor = false; // ponytail: gate surface colormap; off until user enables
    int colorbarTicks = 6;
    std::string activeScalarName;

    // Slice / clip
    bool clipEnabled = false;
    float sliceHeightX = 0.0f;
    float sliceHeightY = 0.0f;
    float sliceHeightZ = 0.0f;
    bool sliceEnabledX = false;
    bool sliceEnabledY = false;
    bool sliceEnabledZ = false;
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
    int vectorMagTransform = 0; // 0 = linear, 1 = sqrt, 2 = log
    std::string vectorField;

    // Streamlines
    bool showStreamlines = false;
    int streamlineSeedCount = 50;
    float streamlineStepSize = 0.02f;
    int streamlineMaxSteps = 100;
    bool streamlineUseColormap = false;
    float streamlineColor[3] = { 0.2f, 0.6f, 1.0f };

    std::string seedMode = "Volume";
    double seedPlanePos = 0.5;
    double seedJitter = 0.0;
    bool showSeeds = false;
    bool showStreamlineArrows = false;
    int streamlineArrowSpacing = 5;
    float streamlineArrowSize = 0.05f;

    // Screenshot export options
    bool screenshotTransparent = false;

    bool hasMeshLoaded = false;
};

// CPU-side UBO layout matching the std140 MeshUBO block in mesh.vert/frag.
// Members are padded to 16 bytes per std140 rules by using glm::vec4.
struct MeshUBOData {
    glm::mat4 mvp;
    glm::mat4 model;
    glm::vec4 viewPos_ps;       // xyz = viewPos, w = pointSize
    glm::vec4 meshColor_wire;   // xyz = meshColor, w = wireframe(0/1)
    glm::vec4 surfaceColor_sop; // xyz = surfaceColor, w = surfaceOpacity
    glm::vec4 point_clip;       // x = isPoint, y = pointUseScalar, z = pointOpacity, w = clipEnabled
    glm::vec4 lightDir;
    glm::vec4 lightFill;
    glm::vec4 lightBack1;
    glm::vec4 lightBack2;
    glm::vec4 lightHead;
    glm::vec4 keyColor;
    glm::vec4 fillColor;
    glm::vec4 backColor;
    glm::vec4 headColor;
    glm::vec4 scalars;          // x = scalarMin, y = scalarMax, z = hasScalars(0/1), w = 0
    glm::vec4 sliceY;           // x = sliceHeightX, y = sliceHeightY, z = sliceHeightZ, w = 0
    glm::vec4 sliceEn;          // x = sliceEnabledX, y = sliceEnabledY, z = sliceEnabledZ, w = 0
    glm::vec4 invert;           // x = invertX, y = invertY, z = invertZ, w = 0
    glm::vec4 filter;           // x = filterMin, y = filterMax, z = 0, w = 0
    glm::vec4 material;         // x = matAmbient, y = matDiffuse, z = matSpecular, w = matShininess
    glm::vec4 intensities;      // x = keyIntensity, y = fillIntensity, z = backIntensity, w = headIntensity
};

// CPU-side UBO layout matching the std140 GridUBO block in grid.vert/frag.
struct GridUBOData {
    glm::mat4 invView;
    glm::mat4 invProj;
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 camPos_colorR;    // xyz = camPos, w = colorR
    glm::vec4 colorBG_falloff;  // xyz = colorG+B, w = falloff
    glm::vec4 planeY_pad;       // x = planeY, yzw = pad
};

// CPU-side UBO layout matching the std140 GlyphUBO block in glyph.vert/frag.
struct GlyphUBOData {
    glm::mat4 mvp;
    glm::vec4 scale_magMin_magMax_scaleByMag; // x=scale, y=magMin, z=magMax, w=scaleByMag
    glm::vec4 meshExtent_magTransform_viewPosY_colorR; // x=meshExtent, y=magTransform, z=viewPos.y, w=colorR
    glm::vec4 lightDir_colorGB; // xyz=lightDir, w=colorG
    glm::vec4 colorB_useColormap; // x=colorB, y=useColormap(0/1), zw=pad
};

// CPU-side UBO layout matching the std140 StreamlineUBO block in streamline.vert/frag.
struct StreamlineUBOData {
    glm::mat4 mvp;
    glm::mat4 model;
    glm::vec4 viewPos;           // xyz = viewPos
    glm::vec4 lightDir;          // xyz = lightDir
    glm::vec4 time_opacity;      // x = uTime, y = opacity, zw = pad
    glm::vec4 color_useColormap; // xyz = color, w = useColormap(0/1)
    glm::vec4 magRange;          // x = magMin, y = magMax, zw = pad
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
    void initShaders(const ShaderSources& sources);
    void initGrid(const ShaderSources& sources);
    void updateGridUbo(const glm::mat4& view, const glm::mat4& proj);
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
    void markStreamlineDirty() { streamlineDirty = true; }
    void resizeViewport(int width, int height);

    void setDevicePixelRatio(float dpr) { devicePixelRatio = dpr; }

    // Scalar-only re-upload handoff. The payload is a shared_ptr (zero-copy).
    // m_pendingScalarSrc is guarded by meshQueueMutex so the GUI-thread write
    // and the render-thread read in cachedScalars() cannot race.
    bool consumeScalarDirty();
    void markScalarDirty(std::shared_ptr<const std::vector<float>> src) {
        {
            std::lock_guard<std::mutex> lock(meshQueueMutex);
            m_pendingScalarSrc = std::move(src);
        }
        scalarDirty = true;
    }
    void updateScalarsOnGPU(std::shared_ptr<const std::vector<float>> scalars);

    // Drop all GPU meshes (GUI-thread request, safe to call any time; real GL
    // teardown happens in meshManager with a current context on the render thread).
    void clearGpuMeshes();

    // Render-thread accessors used by ViewportFboRenderer.
    bool hasGpuMeshes() const { return meshManager.hasMeshes(); }
    // Returns the shared scalar payload (no copy); may be null if none queued.
    // Guarded by meshQueueMutex to pair with markScalarDirty().
    std::shared_ptr<const std::vector<float>> cachedScalars() const {
        std::lock_guard<std::mutex> lock(meshQueueMutex);
        return m_pendingScalarSrc;
    }

    // Screenshot capture (render thread, GL context current). The viewport FBO
    // is supplied by the QQuickFramebufferObject renderer just before capture.
    void setViewportFbo(QOpenGLFramebufferObject* fbo) { m_viewportFbo = fbo; }
    bool captureViewportToFile(const QString& path);

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

    // Streamline magnitude range (rebuilt by StreamlineSet).
    float streamlineMagMin() const { return streamlineSet.magMin; }
    float streamlineMagMax() const { return streamlineSet.magMax; }

private:
    void drawGizmo();
    void drawColorbarLegends(int deviceW, int deviceH);
    void drawBoundingBox(const glm::mat4& view, const glm::mat4& proj);
    void buildQualityOverlayVAOs(); // ponytail: rebuild cached defect VAOs/VBOs
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
    GLuint meshUbo = 0;
    GLuint meshUboIndex = GL_INVALID_INDEX;
    GLuint gridUbo = 0;
    GLuint gridUboIndex = GL_INVALID_INDEX;
    GLuint glyphUbo = 0;
    GLuint glyphUboIndex = GL_INVALID_INDEX;
    GLint lutTextureLoc = -1;

    double m_orthoRefDist = 0.0; // ponytail: baseline camera.distance for ortho dolly zoom

    // glyph program
    GLuint glyphProgram = 0;
    GLint glyphLutLoc = -1;

    // bbox overlay program
    GLuint bboxProgram = 0;
    GLint bboxMvpLoc = -1;
    GLint bboxColorLoc = -1;
    GLuint bboxVao = 0;
    GLuint bboxVbo = 0;

    // streamline program
    GLuint streamlineProgram = 0;
    GLuint streamlineUbo = 0;
    GLint streamlineLutLoc = -1;

    // seed point program
    GLuint seedProgram = 0;
    GLint seedMvpLoc = -1;
    GLint seedColorLoc = -1;
    GLint seedPointSizeLoc = -1;

    // quality overlay cached VAOs/VBOs (one per defect class)
    GLuint qualityOpenEdgesVao = 0, qualityOpenEdgesVbo = 0;
    GLuint qualityNonManifoldVao = 0, qualityNonManifoldVbo = 0;
    GLuint qualityDegenerateVao = 0, qualityDegenerateVbo = 0;
    bool qualityOverlayDirty = true;

    double camDistance = 3.0;
    double nearPlane = 0.1;
    double farPlane = 100.0;

    std::atomic<bool> cameraMoving{false};
    std::atomic<bool> gpuDecimationDirty{false};
    bool m_wasCameraMoving = false;

    // grid (procedural ray-cast ground plane)
    GLuint gridVAO = 0, gridVBO = 0;
    GLuint gridProgram = 0;
    double gridPlaneY = 0.0;

    std::atomic<bool> vectorGlyphDirty{false};
    std::atomic<bool> streamlineDirty{false};

    bool m_destroying = false;

    // Scalar-field switch signal: set on the GUI thread and consumed here.
    std::atomic<bool> scalarDirty{false};

    std::shared_ptr<const RenderMesh> m_pendingMesh;        // handoff from GUI (shared, no copy)
    std::shared_ptr<const RenderMesh> m_lastUploadedMesh;   // kept for deferred vector-glyph rebuilds
    mutable std::mutex meshQueueMutex;
    std::shared_ptr<const std::vector<float>> m_pendingScalarSrc; // scalar handoff (zero-copy)

    std::chrono::steady_clock::time_point m_lastMotion;

    // Deep-copied snapshot; the ONLY source of truth renderFrame() reads.
    RenderRenderState m_state;

    // Lifetime: transient pointer to the Qt-owned viewport FBO, set only
    // during screenshot capture via setViewportFbo(). Not dereferenced
    // outside captureViewportToFile() on the render thread. Null-checked
    // before use (see renderer.cpp:419).
    QOpenGLFramebufferObject* m_viewportFbo = nullptr;

    // --- extracted responsibility helpers -------------------------------------
    ColormapManager colormap;     // scalar + vector LUT textures & choices
    VectorGlyphSet vectorGlyph;   // instanced arrow GPU resources + mag range
    StreamlineSet streamlineSet;  // GL_LINES streamline GPU resources + mag range
    MeshGLManager meshManager;     // full + decimated GPU meshes & upload
};
