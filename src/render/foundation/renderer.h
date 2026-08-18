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
// produced on the GUI thread by RenderSettings::publishRenderState().
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
#include <thread>
#include <memory>

#include "core/mesh_loader.h"
#include "render/overlays/gizmo.h"
#include "render/overlays/colorbar_overlay.h"
#include "core/Camera.h"

#include "render/foundation/LightingModel.h"
#include "render/passes/ColormapManager.h"
#include "render/streamlines/VectorGlyphSet.h"
#include "render/streamlines/StreamlineSet.h"
#include "render/passes/MeshGLManager.h"
#include "render/overlays/GridRenderer.h"
#include "render/overlays/BBoxOverlay.h"
#include "render/overlays/QualityOverlayRenderer.h"
#include "render/streamlines/StreamlineController.h"
#include "render/passes/MeshPass.h"
#include "render/passes/GlyphPass.h"
#include "render/passes/ParticlePass.h"
#include "render/passes/VolumePass.h"
#include "render/overlays/VolumeSliceOverlay.h"
#include "render/passes/ColormapSync.h"
#include "render/passes/LodScheduler.h"

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
    std::string particleVert;
    std::string particleFrag;
    std::string lodComp;
    std::string lodOutputComp;
    std::string lodTrisComp;
    std::string qualityOverlayVert;
    std::string qualityOverlayFrag;
    std::string depthPeelVert;
    std::string depthPeelFrag;
    std::string compositeVert;
    std::string compositeFrag;
    std::string volumeVert;
    std::string volumeFrag;
    std::string volumeSliceVert;
    std::string volumeSliceFrag;
    std::string shadowVert;
    std::string shadowFrag;
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
    int gridAxis = 1; // 0=X Min, 1=X Max, 2=Y Min, 3=Y Max, 4=Z Min, 5=Z Max
    bool showGizmo = true;
    bool autoRotate = false;
    bool showFps = false;
    bool useLod = true;
    float pointSize = 4.0f; // ponytail: CPU-driven gl_PointSize for point clouds
    float lineWidth = 1.0f; // ponytail: wireframe glLineWidth in px
    bool showPoints = false; // ponytail: draw vertices as GL_POINTS
    bool pointUseScalar = true;  // ponytail: color points by scalar; else solid
    float pointOpacity = 1.0f;   // ponytail: point sprite alpha
    float surfaceOpacity = 1.0f; // ponytail: surface fill alpha
    int cullMode = 0;              // ponytail: 0=off by default — mirror of settings default
    bool showBounds = false;     // ponytail: AABB wireframe overlay
    bool showQualityOverlay = false;     // ponytail: highlight degenerate faces + bad edges
    bool gridShadows = false;            // ponytail: shadow mapping for reference grid
    glm::mat4 lightMVP = glm::mat4(1.0f); // ponytail: shadow light view-projection
    // ponytail: overlay geometry (xyz floats), copied from RenderSettings at load
    // shared_ptr so RenderRenderState copies are O(1) instead of O(n)
    std::shared_ptr<const std::vector<float>> qualityDegenerateTris;
    std::shared_ptr<const std::vector<float>> qualityOpenEdges;
    std::shared_ptr<const std::vector<float>> qualityNonManifoldEdges;
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
    int vectorPlacement = 0; // 0 = vertex glyphs (per-vertex vectors), 1 = cell-center glyphs

    // Streamline vector field (independent from vector glyphs)
    std::string streamlineVectorField;

    // Streamlines
    bool showStreamlines = false;
    int streamlineSeedCount = 25;
    float streamlineStepSize = 0.02f;
    int streamlineMaxSteps = 100;
    bool streamlineUseColormap = false;
    int streamlineColormapChoice = 3;
    bool streamlineColormapReversed = false;
    float streamlineColor[3] = { 0.2f, 0.6f, 1.0f };

    float streamlineOpacity = 1.0f;
    float streamlineRibbonWidth = 0.005f;
    float streamlineTaperFactor = 0.3f;
    float streamlineAmbient = 0.35f;
    float streamlineDiffuse = 0.55f;
    float streamlineSpecular = 0.25f;
    int streamlineSpecularPower = 32;
    float seedPointSize = 6.0f;
    float seedPointColor[3] = { 1.0f, 0.2f, 0.2f };

    std::string seedMode = "Volume";
    std::string streamlineDirection = "Both";
    double seedPlanePos = 0.5;
    int seedPlaneCountU = 10;
    int seedPlaneCountV = 10;
    double seedJitter = 0.0;
    bool showSeeds = false;
    bool showStreamlineArrows = false;
    int streamlineArrowSpacing = 4;
    float streamlineArrowSize = 0.05f;

    // Particles
    bool showParticles = false;
    int particleCount = 500;
    float particleSpeed = 1.0f;
    float particleSize = 4.0f;

    // Volume rendering
    bool showVolume = false;
    bool volumeUseColormap = true;
    int  volumeColormapChoice = 3;
    bool volumeColormapReversed = false;
    float volumeStepSize = 0.01f;
    float volumeOpacity = 1.0f;
    float fovY = glm::radians(45.0f);  // vertical field of view in radians

    // Volume slice overlay
    bool showVolumeSlice = false;
    int  volumeSliceAxis = 1;
    float volumeSlicePos = 0.5f;
    float volumeSliceOpacity = 0.35f;
    // The slice plane renders with its own colormap (independent of the full-volume
    // one) and a value range computed from the data actually visible on the slice,
    // so colors remap as the plane moves through the volume.
    bool volumeSliceUseColormap = true;
    int  volumeSliceColormapChoice = 3;
    bool volumeSliceColormapReversed = false;
    float sliceScalarMin = 0.0f;
    float sliceScalarMax = 1.0f;

    // Screenshot export options
    bool screenshotTransparent = false;

     bool hasMeshLoaded = false;
    bool meshHasVectors = false;
    bool meshHasCellVectors = false;
    bool flatShading = true;

    // Isosurface (marching cubes). The extracted surface is rendered as an
    // additional mesh through the existing MeshPass pipeline (colormap LUT +
    // PBR lighting + depth-peel transparency), so no dedicated shader is
    // needed. `isovalue` is an absolute scalar threshold in data units;
    // `showIsosurface` gates its visibility (independent of showSurface, so a
    // surface shell can be hidden while the isosurface stays visible).
    bool showIsosurface = false;
    float isovalue = 0.0f;
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
    glm::vec4 material;         // x = matAmbient, y = matDiffuse, z = matSpecular
    glm::vec4 intensities;      // x = keyIntensity, y = fillIntensity, z = backIntensity, w = headIntensity
    glm::vec4 pbr;              // x = matRoughness, y = matMetallic, z = pad, w = pad (Phase 1 PBR)
    glm::vec4 shadingMode;      // x = 0.0 smooth, 1.0 flat
};
static_assert(sizeof(MeshUBOData) % 16 == 0, "MeshUBOData must be std140-aligned");

// CPU-side UBO layout matching the std140 GridUBO block in grid.vert/frag.
struct GridUBOData {
    glm::mat4 invView;
    glm::mat4 invProj;
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 camPos_colorR;    // xyz = camPos, w = colorR
    glm::vec4 colorBG_falloff;  // xyz = colorG+B, w = falloff
    glm::vec4 gridAxis_planePos;  // x = normalized axis (0=X,1=Y,2=Z), y = planePos, zw = pad
    glm::vec4 flags;            // x = useZeroToOne (1.0 or 0.0)
    glm::mat4 lightMVP;         // shadow light view-projection matrix
    glm::vec4 shadowParams;     // x = shadowsEnabled, y = bias, zw = pad
};
static_assert(sizeof(GridUBOData) % 16 == 0, "GridUBOData must be std140-aligned");

// CPU-side UBO layout matching the std140 GlyphUBO block in glyph.vert/frag.
struct GlyphUBOData {
    glm::mat4 mvp;
    glm::vec4 scale_magMin_magMax_scaleByMag; // x=scale, y=magMin, z=magMax, w=scaleByMag
    glm::vec4 meshExtent_magTransform_viewPosY_colorR; // x=meshExtent, y=magTransform, z=viewPos.y, w=colorR
    glm::vec4 lightDir_colorGB; // xyz=lightDir, w=colorG
    glm::vec4 colorB_useColormap; // x=colorB, y=useColormap(0/1), zw=pad
    glm::vec4 pbr;              // x = matRoughness, y = matMetallic, z = pad, w = pad
};
static_assert(sizeof(GlyphUBOData) % 16 == 0, "GlyphUBOData must be std140-aligned");

// CPU-side UBO layout matching the std140 StreamlineUBO block in streamline.vert/frag.
struct StreamlineUBOData {
    glm::mat4 mvp;
    glm::mat4 model;
    glm::vec4 viewPos;           // xyz = viewPos
    glm::vec4 lightDir;          // xyz = lightDir
    glm::vec4 time_opacity;      // x = uTime, y = opacity
    glm::vec4 color_useColormap; // xyz = color, w = useColormap(0/1)
    glm::vec4 magRange;          // x = magMin, y = magMax, zw = pad
    glm::vec4 material;          // x = ambient, y = diffuse, z = specular, w = specularPower
    glm::vec4 ribbon;            // x = ribbonWidth, y = taperFactor, zw = pad
    glm::vec4 arrowParams;       // x = arrowAnimSpeed, yzw = pad
    glm::vec4 pbr;               // x = matRoughness, y = matMetallic, z = pad, w = pad
};
static_assert(sizeof(StreamlineUBOData) % 16 == 0, "StreamlineUBOData must be std140-aligned");

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
    void initGizmo();
    void renderFrame();

    // Deep-copy the GUI-thread snapshot into this render-thread instance.
    void setState(const RenderRenderState& state);

    // Snapshot accessors used by the FBO renderer (drawn state only).
    bool autoRotate() const { return m_state.autoRotate; }
    bool showFps() const { return m_state.showFps; }
    bool isParticlesAnimating() const { return m_state.showParticles; }

    // Uploads CPU geometry to the GPU. Safe to call on the render thread.
    void uploadMesh(std::shared_ptr<const RenderMesh> renderMesh);

    // Pending mesh handoff (GUI -> render thread). setPendingMesh() stores a
    // shared_ptr (no copy) plus a dirty flag; renderFrame() consumes it and
    // uploads on the render thread under the GL context.
    void setPendingMesh(std::shared_ptr<const RenderMesh> renderMesh);

    // Isosurface handoff (GUI -> render thread). Same zero-copy shared_ptr
    // pattern as setPendingMesh(); renderFrame() consumes it and uploads only
    // the surface VAO (no vector/streamline/volume rebuild, since the isosurface
    // is a plain triangle mesh, not a structured grid).
    void setPendingIsosurface(std::shared_ptr<const RenderMesh> isoMesh);

    // Mark the camera as moving and (re)start the LOD debounce timer.
    void markCameraMoving();
    void markVectorGlyphDirty() { vectorGlyphDirty = true; }
    void markStreamlineDirty() { m_streamlines.streamlineDirty = true; }
    void markParticleCountDirty() { m_streamlines.particleCountDirty = true; }
    void resizeViewport(int width, int height);

    void setDevicePixelRatio(float dpr) { devicePixelRatio = dpr; }

    // Temporary viewport override for screenshot re-render at arbitrary resolution.
    // Pass {0,0} to clear the override and revert to the widget dimensions.
    void setViewportOverride(int deviceW, int deviceH) { m_overrideDeviceW = deviceW; m_overrideDeviceH = deviceH; }
    void clearViewportOverride() { m_overrideDeviceW = 0; m_overrideDeviceH = 0; colorbarOverlay.markDirty(); }

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

    // Volume scalar-switch handoff. When the active scalar field changes and the
    // mesh has structured-grid volume data, the render loop re-uploads the 3D
    // texture in the same tick (same GL context) as the surface scalar SBO update.
    bool consumeVolumeDirty();
    void markVolumeDirty(std::shared_ptr<const RenderMesh> mesh) {
        {
            std::lock_guard<std::mutex> lock(meshQueueMutex);
            m_pendingVolumeMesh = mesh;
        }
        volumeDirty = true;
    }
    std::shared_ptr<const RenderMesh> cachedVolumeMesh() const {
        std::lock_guard<std::mutex> lock(meshQueueMutex);
        return m_pendingVolumeMesh;
    }
    bool hasVolumeData() const { return m_lastUploadedMesh && m_lastUploadedMesh->hasVolumeData(); }
    void uploadVolumeFromScalarDirty(const RenderRenderState& state,
        std::shared_ptr<const std::vector<float>> scalars,
        std::shared_ptr<const RenderMesh> mesh);

    // Drop all GPU meshes (GUI-thread request, safe to call any time; real GL
    // teardown happens in meshManager with a current context on the render thread).
    void clearGpuMeshes();

    // Re-initialize after a GL context change (e.g. MSAA viewport recreation).
    // Zeros stale handles and shuts down subsystems so the next initShaders()
    // / initGizmo() call creates fresh resources.
    void reinitForNewContext();

    // Re-upload mesh geometry, vector glyphs, and colormap textures from
    // CPU-side copies.  Call after reinitForNewContext() + initShaders().
    void reinitMeshData();

    void setClipControlAvailable(bool available) { m_clipControlAvailable = available; }
    bool clipControlAvailable() const { return m_clipControlAvailable; }

    // Render-thread accessors used by ViewportFboRenderer.
    bool hasGpuMeshes() const { return meshManager.hasMeshes(); }
    // Returns the shared scalar payload (no copy); may be null if none queued.
    // Guarded by meshQueueMutex to pair with markScalarDirty().
    std::shared_ptr<const std::vector<float>> cachedScalars() const {
        std::lock_guard<std::mutex> lock(meshQueueMutex);
        return m_pendingScalarSrc;
    }

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
    static std::string vectorGlyphTitle(const RenderRenderState& state, const RenderMesh* mesh);
    void computeLightDirections(glm::vec3& key, glm::vec3& fill, glm::vec3& back1, glm::vec3& back2, glm::vec3& head);
    void updateSliceScalarRange();

    // Display Dimension Registers
    int width = 800;
    int height = 600;
    float devicePixelRatio = 1.0f;
    int m_overrideDeviceW = 0;
    int m_overrideDeviceH = 0;

    // Viewport Core Transform Tracking
    Gizmo gizmo;
    ColorbarOverlay colorbarOverlay;

    double m_orthoRefDist = 0.0; // ponytail: baseline camera.distance for ortho dolly zoom
    double m_lastOrthoRadius = 1.0;

    double camDistance = 3.0;
    double nearPlane = 0.1;
    double farPlane = 100.0;

    std::atomic<bool> vectorGlyphDirty{false};

    bool m_destroying = false;

    // Animation clock (drives arrow animation independent of frame rate)
    double m_animationTime = 0.0;
    double m_lastFrameDt = 0.0;
    std::chrono::steady_clock::time_point m_lastFrameTime;

    // Scalar-field switch signal: set on the GUI thread and consumed here.
    std::atomic<bool> scalarDirty{false};

      std::shared_ptr<const RenderMesh> m_pendingMesh;        // handoff from GUI (shared, no copy)
    std::shared_ptr<const RenderMesh> m_lastUploadedMesh;   // kept for deferred vector-glyph rebuilds
    std::shared_ptr<const RenderMesh> m_pendingIsosurface;  // isosurface handoff (shared, no copy)
    std::shared_ptr<const RenderMesh> m_lastIsosurfaceMesh; // kept for deferred re-upload across GL context resets
    std::atomic<bool> isosurfaceDirty{false};

    mutable std::mutex meshQueueMutex;
    std::shared_ptr<const std::vector<float>> m_pendingScalarSrc; // scalar handoff (zero-copy)


    // Volume-specific scalar-switch handoff. Paired with markVolumeDirty() /
    // consumeVolumeDirty() and guarded by the same meshQueueMutex.
    std::atomic<bool> volumeDirty{false};
    std::shared_ptr<const RenderMesh> m_pendingVolumeMesh;

    // Deep-copied snapshot; the ONLY source of truth renderFrame() reads.
    RenderRenderState m_state;

    bool m_clipControlAvailable = false;

    // --- extracted responsibility helpers -------------------------------------
    ColormapManager colormap;     // scalar + vector LUT textures & choices
    ColormapSync colormapSync;     // batches colormap choice→LUT updates per frame
    VectorGlyphSet vectorGlyph;   // instanced arrow GPU resources + mag range
    StreamlineSet streamlineSet;  // GL_LINES streamline GPU resources + mag range
    MeshGLManager meshManager;     // full + decimated GPU meshes & upload
    MeshPass meshPass;             // mesh shader program + UBO + surface draw passes
    GlyphPass glyphPass;           // vector glyph shader program + UBO + draw
    ParticlePass particlePass;     // particle shader program + VAO/VBO + draw
    LodScheduler lodScheduler;     // LOD debounce + GPU compute dispatch
    GridRenderer m_grid;           // procedural ray-cast ground plane
    BBoxOverlay m_bbox;            // AABB wireframe overlay
    QualityOverlayRenderer m_qualityOverlay; // mesh defect highlights
    StreamlineController m_streamlines;      // streamline compute + draw + seeds
    VolumePass m_volume;                      // volume ray-march pass
    VolumeSliceOverlay m_volumeSliceOverlay;  // volume slice plane overlay

    // --- Shadow mapping for reference grid ---
    GlProgram m_shadowProgram;
    GlFramebuffer m_shadowFbo;
    GlTexture m_shadowDepthTex;
    GLuint m_shadowUbo = 0;
    GLint m_shadowUboIndex = -1;
    static constexpr int kShadowMapSize = 1024;

    void drawShadowPass(const std::vector<std::pair<GLuint, int>>& drawList, const glm::mat4& lightMVP);
    void ensureShadowFbo();
    void destroyShadowFbo();

    // --- Depth peeling for transparent surfaces ---
    GlProgram m_peelProgram;
    GLint  m_peelPrevDepthLoc = -1;
    GLint  m_peelLayerLoc = -1;
    GlProgram m_compositeProgram;

    // FBOs for two-layer peeling: [0] = front layer, [1] = back layer
    GlFramebuffer m_peelFbo[2];
    GlTexture m_peelColorTex[2];
    GlTexture m_peelDepthTex[2];
    GlTexture m_peelDummyDepth;  // 1x1 initialized to 1.0 for first pass
    GlTexture m_peelMainDepth;   // opaque geometry depth copied from main FBO
    GlVao m_peelDummyVao;    // empty VAO for fullscreen triangle
    int m_peelFboW = 0, m_peelFboH = 0;

    void ensurePeelFbos(int w, int h);
    void destroyPeelFbos();
    void renderTransparent(const glm::mat4& view, const glm::mat4& proj,
                            GLuint meshUbo,
                            const std::vector<std::pair<GLuint, int>>& transparentMeshes);
};


