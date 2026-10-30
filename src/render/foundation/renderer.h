#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif





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
#include <cmath>
#include <thread>
#include <memory>
#include <cstddef>

#include "core/mesh_loader.h"
#include "core/unstructured_volume.h"
#include "render/overlays/gizmo.h"
#include "render/overlays/light_markers.h"
#include "render/overlays/colorbar_overlay.h"
#include "core/Camera.h"

#include "render/foundation/LightingModel.h"
#include "render/passes/ColormapManager.h"
#include "render/streamlines/VectorGlyphSet.h"
#include "render/streamlines/StreamlineSet.h"
#include "render/passes/MeshGLManager.h"
#include "render/overlays/BBoxOverlay.h"
#include "render/overlays/QualityOverlayRenderer.h"
#include "render/overlays/LineProbeOverlay.h"
#include "render/overlays/ProbeOverlay.h"
#include "render/streamlines/StreamlineController.h"
#include "render/passes/MeshPass.h"
#include "render/passes/GlyphPass.h"
#include "render/passes/ParticlePass.h"
#include "render/passes/VolumePass.h"
#include "render/passes/VolumeTextureCache.h"
#include "render/passes/VectorTextureCache.h"
#include "render/overlays/VolumeSliceOverlay.h"
#include "render/passes/LodScheduler.h"
#include "render/passes/DepthPeelPass.h"
#include "core/post_fields.h"
#include "core/transfer_function.h"

#include <QOpenGLFramebufferObject>

class PlotKernel;



struct ShaderSources {
    std::string meshVert;
    std::string meshFrag;
    std::string meshClipGeo;
    std::string meshWireVert;
    std::string meshWireGeo;
    std::string meshWireFrag;
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
    std::string compositeVert;   // fullscreen triangle, also used by depth peel
    std::string peelBlitFrag;    // dual depth peel: seed depth ranges + resolve
    std::string volumeVert;
    std::string volumeFrag;
    std::string volumeSliceVert;
    std::string volumeSliceFrag;
    std::string surfaceLicFrag;

    std::string pbrFragCommon;
    std::string plotHistogramComp;
};




// LIC clamping constants — single source for UI sliders, CPU upload, and GPU guards.
// Setter bounds (product) are tight; shader bounds are looser (safety net for direct state injection).
namespace LicLimits {
    constexpr int   StepsMin = 4;
    constexpr int   StepsMax = 128;
    constexpr int   StepsGpuMin = 0;   // shader tolerates 0 (single-sample fallback) even though UI min is 4
    constexpr float StepSizeMin = 0.001f;   // fraction of scene diagonal
    constexpr float StepSizeMax = 2.0f;
    constexpr float NoiseFreqMin = 0.5f;
    constexpr float NoiseFreqMax = 64.0f;
    // Lower bound of the world-space step as a FRACTION of the scene diagonal.
    // An absolute floor (the old 1e-6) made a sub-micron scene step thousands
    // of scene widths per tap.
    constexpr float WorldStepMinFraction = 1e-4f;
    constexpr float WorldStepMaxFactor = 2.0f; // cap worldStep at diag * factor
    // Upper bound on integrator taps per direction. The LIC cost is
    // steps * (integrator taps) * (2 when enhanced); UI max settings would
    // otherwise reach ~1024 vector fetches per fragment per direction.
    constexpr int   MaxTapsPerDirection = 256;
}

inline float applyVectorMagTransform(float m, int mode) {
    if (mode == 1) return std::sqrt(std::max(m, 0.0f));
    if (mode == 2) return std::log(1.0f + std::max(m, 0.0f));
    return m;
}









struct RenderRenderState {

    Camera camera;


    bool showWireframe = false;
    bool showSurface = true;
    bool showGizmo = true;
    bool autoRotate = false;
    bool showFps = false;
    bool useLod = true;
    float pointSize = 4.0f;
    float lineWidth = 1.0f;
    bool showPoints = false;
    bool pointUseScalar = true;
    float pointOpacity = 1.0f;
    float surfaceOpacity = 1.0f;
    // Dual depth peeling iterations. SAFETY CEILING ONLY.
    //
    // This is NOT a transparency control: the result is pass-count independent
    // because the ALPHA mop-up pass blends whatever the peel loop leaves
    // unresolved. It exists only to bound worst-case CPU, and the loop also
    // terminates itself via occlusion query. Verified numerically: 1..64 passes
    // give identical alpha for 2/4/8/20/40-layer stacks.
    //
    // Default 4 covers virtually every real scene, and the mop-up pass means the
    // result is correct at any setting above 1 anyway.
    int maxPeelLayers = 4;
    // Fraction of the viewport a peel pass must still cover to justify another
    // pass. 0 = never stop early (exact, slowest). Small values let the loop
    // exit once remaining layers are sub-pixel. Clamped to [0, 0.5].
    float peelOcclusionRatio = 0.0f;
    int cullMode = 0;
    bool showBounds = false;
    int gizmoCorner = 0;
    int gizmoSizeChoice = 1;
    bool showQualityOverlay = false;


    std::shared_ptr<const std::vector<float>> qualityDegenerateTris;
    std::shared_ptr<const std::vector<float>> qualityOpenEdges;
    std::shared_ptr<const std::vector<float>> qualityNonManifoldEdges;
    bool orthographic = false;


    float meshColor[3] = { 0.4f, 0.9f, 0.4f };
    float surfaceColor[3] = { 1.0f, 1.0f, 1.0f };
    float bgColor[3] = { 0.12f, 0.12f, 0.12f };
    float textColor[3] = { 1.0f, 1.0f, 1.0f };


    double worldCenterX = 0, worldCenterY = 0, worldCenterZ = 0;
    double worldRadius = 1.0;
    double worldMinX = -10.0, worldMaxX = 10.0;
    double worldMinY = -10.0, worldMaxY = 10.0;
    double worldMinZ = -10.0, worldMaxZ = 10.0;


    LightingModel lighting;



    int colormapChoice = 3;
    bool colormapReversed = false;
    int vectorColormapChoice = 3;
    bool vectorColormapReversed = false;
    int scalarColorBands = 0;
    int vectorColorBands = 0;
    int streamlineColorBands = 0;
    int volumeColorBands = 0;
    int volumeSliceColorBands = 0;


    bool meshHasScalars = false;
    float scalarMin = 0.0f;
    float scalarMax = 1.0f;
    float dataScalarMin = 0.0f;
    float dataScalarMax = 1.0f;
    float filterMin = 0.0f;
    float filterMax = 1.0f;
    bool filterEnabled = false;

    float volumeFilterMin = 0.0f;
    float volumeFilterMax = 1.0f;
    bool volumeFilterEnabled = false;





    float colorRangeLo = 0.0f;
    float colorRangeHi = 1.0f;
    bool colorRangeOverrideEnabled = false;


    float colorMapMin() const { return colorRangeOverrideEnabled ? colorRangeLo : scalarMin; }
    float colorMapMax() const { return colorRangeOverrideEnabled ? colorRangeHi : scalarMax; }






    float volumeColorRangeLo = 0.0f;
    float volumeColorRangeHi = 1.0f;
    bool volumeColorRangeOverrideEnabled = false;
    float sliceColorRangeLo = 0.0f;
    float sliceColorRangeHi = 1.0f;
    bool sliceColorRangeOverrideEnabled = false;
    float glyphMagRangeLo = 0.0f;
    float glyphMagRangeHi = -1.0f;
    bool glyphMagRangeOverrideEnabled = false;
    float glyphCompRangeLo[3] = { 0.0f, 0.0f, 0.0f };
    float glyphCompRangeHi[3] = { -1.0f, -1.0f, -1.0f };
    bool glyphCompRangeOverrideEnabled[3] = { false, false, false };
    float streamlineMagRangeLo = 0.0f;
    float streamlineMagRangeHi = -1.0f;
    bool streamlineMagRangeOverrideEnabled = false;
    float streamlineCompRangeLo[3] = { 0.0f, 0.0f, 0.0f };
    float streamlineCompRangeHi[3] = { -1.0f, -1.0f, -1.0f };
    bool streamlineCompRangeOverrideEnabled[3] = { false, false, false };
    bool showScalarColorbar = true;
    bool meshUseScalarColor = false;
    int colorbarTicks = 6;

    QString colorbarFontFamily;
    bool colorbarFontBold = false;
    bool colorbarFontItalic = false;
    float colorbarFontScale = 1.0f;
    float colorbarTickFontScale = 1.0f;
    float colorbarLengthScale = 1.0f;
    float colorbarThicknessScale = 1.0f;
    bool colorbarPanelEnabled = false;
    float colorbarPanelOpacity = 0.55f;
    bool colorbarShowAnnotation = true;
    std::string activeScalarName;
    int scalarPlacement = 0; // 0=Vertex (point data), 1=Cell Center (cell data)


    bool clipEnabled = false;
    bool crinkleClipMode = false;
    float clipHeightX = 0.0f;
    float clipHeightY = 0.0f;
    float clipHeightZ = 0.0f;
    bool clipEnabledX = false;
    bool clipEnabledY = false;
    bool clipEnabledZ = false;
    bool invertX = false;
    bool invertY = false;
    bool invertZ = false;


    bool showVectors = false;
    float vectorScale = 1.0f;
    int vectorStride = 1;
    float vectorColor[3] = { 0.2f, 0.6f, 1.0f };
    int vectorColorMode = 1;
    bool vectorScaleByMagnitude = false;
    int vectorMagTransform = 0;
    std::string vectorField;
    int vectorPlacement = 0;
    float vectorCompMin[3] = { 0.0f, 0.0f, 0.0f };
    float vectorCompMax[3] = { 0.0f, 0.0f, 0.0f };

    bool showLic = false;
    int vectorVisMode = 0;
    int licSteps = 24;          // kernel half-length (total taps = 1+2*steps); UI [4..128], GPU tolerates [0..128]
    // Fraction of the scene diagonal per LIC step. The kernel is
    // 2*steps*licStepSize long, while the noise it convolves has a
    // correlation length of 1/licNoiseFreq - a kernel of a few correlation
    // lengths is what makes an LIC read as streamlines. At the old default
    // (0.02) the kernel was ~10 correlation lengths long and smeared the
    // whole field into a blur; 0.005 puts it at ~2.7 for the default
    // steps/frequency.
    float licStepSize = 0.005f;  // fraction of scene diagonal; MeshPass converts to worldStep = licStepSize*diag, clamped to [1e-6, diag*2]
    float licNoiseFreq = 8.0f;  // UI [0.5..64], upload clamped to same
    int licNoiseGrain = 64;     // 3D noise resolution; supported set {64,128}
    int licBoundaryMode = 1;    // deprecated: fixed to Clamp (GL_CLAMP_TO_EDGE). Persisted values coerced to 1; not used for texture key.
    bool licEnhanced = false;   // Enhanced LIC: 2-pass (second convolution sharpens coherence)
    bool licOnly = false;       // Skip PBR lighting, output unshaded LIC color
    int licIntegrator = 2;      // 0=Euler, 1=Midpoint, 2=RK4


    std::string streamlineVectorField;
    float streamlineCompMin[3] = { 0.0f, 0.0f, 0.0f };
    float streamlineCompMax[3] = { 0.0f, 0.0f, 0.0f };


    bool showStreamlines = false;
    int streamlineSeedCount = 25;
    float streamlineStepSize = 0.02f;
    int streamlineMaxSteps = 100;
    bool streamlineUseColormap = false;
    int streamlineColormapChoice = 3;
    bool streamlineColormapReversed = false;
    float streamlineColor[3] = { 0.2f, 0.6f, 1.0f };
    int streamlineColorMode = 1;

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
    float streamlineArrowSpacingFrac = 0.15f;
    float streamlineArrowSize = 0.05f;


    bool showParticles = false;
    int particleCount = 500;
    float particleSpeed = 1.0f;
    float particleSize = 4.0f;
    bool particleAdditive = true;


    bool showVolume = false;
    bool volumeUseColormap = true;
    int  volumeColormapChoice = 3;
    bool volumeColormapReversed = false;
    float volumeStepSize = 0.01f;
    float volumeOpacity = 1.0f;
    tf::Curve volumeTransferCurve = tf::defaultCurve();
    bool volumeTFEnabled = false;
    float fovY = glm::radians(45.0f);


    bool slicePlaneEnabled[3] = {false, false, false};
    float slicePlanePos[3] = {0.5f, 0.5f, 0.5f};
    float slicePlaneOpacity[3] = {0.35f, 0.35f, 0.35f};
    bool slicePlaneShowColorbar[3] = {false, false, false};



    bool volumeSliceUseColormap = true;
    int  volumeSliceColormapChoice = 3;
    bool volumeSliceColormapReversed = false;
    float sliceScalarMin[3] = {0.0f, 0.0f, 0.0f};
    float sliceScalarMax[3] = {1.0f, 1.0f, 1.0f};
    std::string sliceScalarName[3];

    bool anySlicePlaneEnabled() const {
        return slicePlaneEnabled[0] || slicePlaneEnabled[1] || slicePlaneEnabled[2];
    }


    bool screenshotTransparent = false;

     bool hasMeshLoaded = false;
    bool meshHasVectors = false;
    bool meshHasCellVectors = false;
    bool flatShading = true;







    bool showIsosurface = false;
    float isovalue = 0.0f; // legacy single value; kept for compat — isovalues is authoritative
    std::vector<float> isovalues; // multi-contour list; when non-empty isovalue==isovalues[0]
    int isosurfacePlacement = 0; // 0=Vertex (point data), 1=Cell Center (cell data)
    std::string isosurfaceField; // independent contour field (may differ from activeScalarName)
    // Flat iso color (light blue default). The isosurface is NEVER colormapped.
    float isosurfaceColor[3] = { 0.68f, 0.85f, 0.90f };

    // Post-processed fields snapshot (vorticity/helicity/Q/grad-p). Immutable
    // shared payloads — safe to read from the render thread. Null = none.
    std::shared_ptr<const postfields::ComputedSnapshot> computed;

    bool showLineProbe = false;
    glm::vec3 lineP0 = glm::vec3(0.0f);
    glm::vec3 lineP1 = glm::vec3(1.0f, 0.0f, 0.0f);

    bool showProbe = false;
    glm::vec3 probePos = glm::vec3(0.0f);
    int probeFormat = 1; // 0=Integer Rounding, 1=Float 4 sig, 2=Exponent
    int probePlacement = 0; // 0=Vertex, 1=Cell Center
};



struct MeshUBOData {
    glm::mat4 mvp;
    glm::mat4 model;
    glm::vec4 viewPos_ps;
    glm::vec4 meshColor_wire;
    glm::vec4 surfaceColor_sop;
    glm::vec4 point_clip;
    glm::vec4 lightDir;
    glm::vec4 lightFill;
    glm::vec4 lightBack1;
    glm::vec4 lightBack2;
    glm::vec4 lightHead;
    glm::vec4 keyColor;
    glm::vec4 fillColor;
    glm::vec4 backColor;
    glm::vec4 headColor;
    glm::vec4 scalars;
    glm::vec4 clipY;
    glm::vec4 clipEn;
    glm::vec4 invert;
    glm::vec4 filter;
    glm::vec4 material;
    glm::vec4 intensities;
    glm::vec4 pbr;
    glm::vec4 shadingMode;
};
static_assert(sizeof(MeshUBOData) % 16 == 0, "MeshUBOData must be std140-aligned");

static_assert(offsetof(MeshUBOData, meshColor_wire) == 144, "UBO offset drift");
static_assert(offsetof(MeshUBOData, point_clip) == 176, "UBO offset drift");


struct GlyphUBOData {
    glm::mat4 mvp;
    glm::vec4 scale_magMin_magMax_scaleByMag;
    glm::vec4 meshExtent_magTransform_viewPosY_colorR;
    glm::vec4 lightDir_colorGB;
    glm::vec4 colorB_colormode;
    glm::vec4 compMin;
    glm::vec4 compMax;
    glm::vec4 pbr;
};
static_assert(sizeof(GlyphUBOData) % 16 == 0, "GlyphUBOData must be std140-aligned");


struct StreamlineUBOData {
    glm::mat4 mvp;
    glm::mat4 model;
    glm::vec4 viewPos;
    glm::vec4 lightDir;
    glm::vec4 time_opacity;
    glm::vec4 color_useColormap;
    glm::vec4 magRange;
    glm::vec4 compMin;
    glm::vec4 compMax;
    glm::vec4 colorMode;
    glm::vec4 material;
    glm::vec4 ribbon;
    glm::vec4 arrowParams;
    glm::vec4 pbr;
};
static_assert(sizeof(StreamlineUBOData) % 16 == 0, "StreamlineUBOData must be std140-aligned");















class Renderer {
public:
    Renderer();
    ~Renderer();


    void initGLAD();
    void initShaders(const ShaderSources& sources);
    void initGizmo();
    void renderFrame();


    void setState(const RenderRenderState& state);


    bool autoRotate() const { return m_state.autoRotate; }
    bool showFps() const { return m_state.showFps; }
    bool isParticlesAnimating() const { return m_state.showParticles; }


    void uploadMesh(std::shared_ptr<const RenderMesh> renderMesh);




    void setPendingMesh(std::shared_ptr<const RenderMesh> renderMesh);





    void setPendingIsosurface(std::shared_ptr<const RenderMesh> isoMesh);

    // Derived voxel grid for volume rendering / axis-aligned slices of an
    // unstructured mesh. The GUI thread builds it asynchronously (topology-only)
    // and hands the immutable result over; the render thread consumes it at the
    // start of the next frame and drops the cached volume textures.
    void setPendingDerivedVolume(std::shared_ptr<const volume::DerivedGrid> grid);

    void consumePendingMesh();

    void markCameraMoving();
    void markVectorGlyphDirty() { vectorGlyphDirty = true; }
    void markStreamlineDirty() { m_streamlines.streamlineDirty = true; }
    void markParticleCountDirty() { m_streamlines.particleCountDirty = true; }
    void resizeViewport(int width, int height);

    void setDevicePixelRatio(float dpr) { devicePixelRatio = dpr; }



    void setViewportOverride(int deviceW, int deviceH) { m_overrideDeviceW = deviceW; m_overrideDeviceH = deviceH; }
    void clearViewportOverride() { m_overrideDeviceW = 0; m_overrideDeviceH = 0; colorbarOverlay.markDirty(); }


    int colorbarIndexAt(int px, int py, const std::vector<ColorbarData>& bars) const;
    void setColorbarPosition(int index, float fracX, float fracY);
    void setColorbarOrientation(int index, ColorbarStyle::Orientation orient);
    void setColorbarVisible(int index, bool vis);
    void commitColorbarPositions();
    void markColorbarDirty() { colorbarOverlay.markDirty(); }
    QRectF colorbarBarRect(float dpr, int deviceW, int deviceH, const ColorbarData& bar) const;
    const std::vector<ColorbarData>& colorbarBars() const { return m_colorbarBars; }





    int effectiveDeviceW() const { return m_overrideDeviceW > 0 ? m_overrideDeviceW : static_cast<int>(width * devicePixelRatio); }
    int effectiveDeviceH() const { return m_overrideDeviceH > 0 ? m_overrideDeviceH : static_cast<int>(height * devicePixelRatio); }




    bool consumeScalarDirty();
    void markScalarDirty(std::shared_ptr<const std::vector<float>> src) {
        {
            std::lock_guard<std::mutex> lock(meshQueueMutex);
            m_pendingScalarSrc = std::move(src);
        }
        scalarDirty = true;
    }
    void markVectorDirty(std::shared_ptr<const RenderMesh> mesh) {
        {
            std::lock_guard<std::mutex> lock(meshQueueMutex);
            m_pendingVectorMesh = std::move(mesh);
        }
        vectorDirty = true;
    }
    void updateScalarsOnGPU(std::shared_ptr<const std::vector<float>> scalars);

    // Plot kernel (GPU histogram MVP) — owns compute program + SSBOs
    PlotKernel& plotKernel();
    const PlotKernel& plotKernel() const;
    const RenderMesh* lastUploadedMeshForPlot() const { return m_dataMesh.get(); }




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
    bool hasVolumeData() const {
        if (!m_dataMesh) return false;
        if (m_dataMesh->hasVolumeData()) return true;
        // Unstructured 3D CFD mesh: no IJK grid, but volume/slice can be derived
        // from the cells as soon as a scalar exists.
        return volume::eligible(*m_dataMesh) && m_dataMesh->hasScalarData();
    }
    void uploadVolumeFromScalarDirty(const RenderRenderState& state,
        std::shared_ptr<const std::vector<float>> scalars,
        std::shared_ptr<const RenderMesh> mesh);



    void clearGpuMeshes();




    void reinitForNewContext();



    void reinitMeshData();

    void setClipControlAvailable(bool available) { m_clipControlAvailable = available; }
    bool clipControlAvailable() const { return m_clipControlAvailable; }


    bool hasGpuMeshes() const { return meshManager.hasMeshes(); }


    std::shared_ptr<const std::vector<float>> cachedScalars() const {
        std::lock_guard<std::mutex> lock(meshQueueMutex);
        return m_pendingScalarSrc;
    }


    void applyLightingPreset(int preset);
    void resetLighting();




    void resetCamera();

    void snapToOrthoView(int axis);
    void snapToAxisView(int axis, bool flip);





    int gizmoAxisAt(int pxDev, int pyDev) const;
    void setGizmoHoverAxis(int axis) { m_gizmoHoverAxis.store(axis, std::memory_order_relaxed); }


    float vectorMagMin() const { return vectorGlyph.magMin; }
    float vectorMagMax() const { return vectorGlyph.magMax; }
    float vectorCompMin(int comp) const { return vectorGlyph.compMin[comp]; }
    float vectorCompMax(int comp) const { return vectorGlyph.compMax[comp]; }


    float streamlineMagMin() const { return streamlineSet.magMin; }
    float streamlineMagMax() const { return streamlineSet.magMax; }
    float streamlineCompMin(int comp) const { return streamlineSet.compMin[comp]; }
    float streamlineCompMax(int comp) const { return streamlineSet.compMax[comp]; }

private:
    void drawGizmo(int deviceW, int deviceH);
    void drawColorbarLegends(int deviceW, int deviceH);
    static std::string vectorGlyphTitle(const RenderRenderState& state, const RenderMesh* mesh);
    void computeLightDirections(glm::vec3& key, glm::vec3& fill, glm::vec3& back1, glm::vec3& back2, glm::vec3& head);
    void updateSliceScalarRange(const VolumeTexture* sliceTextures);

    void ensureLicNoiseTexture(int grain);
    void shutdownLic();
  public:
    const std::string& getLastLicError() const { return m_lastLicError; }
  private:


    int width = 800;
    int height = 600;
    float devicePixelRatio = 1.0f;
    int m_overrideDeviceW = 0;
    int m_overrideDeviceH = 0;


    Gizmo gizmo;
    LightMarkerOverlay m_lightMarkers;
    ColorbarOverlay colorbarOverlay;
    std::vector<ColorbarData> m_colorbarBars;
    std::mutex m_colorbarCacheMutex;
    std::map<QString, std::pair<float, float>> m_colorbarPosCache;
    std::map<QString, int> m_colorbarOrientCache;
    std::map<QString, bool> m_colorbarVisibleCache;

    double m_orthoRefDist = 0.0;
    double m_lastOrthoRadius = 1.0;

    double camDistance = 3.0;
    double nearPlane = 0.1;
    double farPlane = 100.0;

    std::atomic<bool> vectorGlyphDirty{false};
    std::atomic<int>  m_gizmoHoverAxis{-1};

    bool m_destroying = false;


    double m_animationTime = 0.0;
    double m_lastFrameDt = 0.0;
    std::chrono::steady_clock::time_point m_lastFrameTime;


    std::atomic<bool> scalarDirty{false};
    std::atomic<bool> vectorDirty{false};



    std::atomic<bool> lodSettleDirty{false};

  public:
    bool consumeLodSettle() { return lodSettleDirty.exchange(false); }

  private:

      std::shared_ptr<const RenderMesh> m_pendingMesh;
    std::shared_ptr<const RenderMesh> m_pendingVectorMesh;
    // m_lastUploadedMesh is ONLY ever set where geometry is actually uploaded,
    // so it always describes what the GPU holds. m_dataMesh is the newest mesh
    // carrying field data (vectors/scalars may advance without a re-upload) and
    // is what data consumers (glyphs, LIC, volume, streamlines, plots) read.
    std::shared_ptr<const RenderMesh> m_lastUploadedMesh;
    std::shared_ptr<const RenderMesh> m_dataMesh;
    std::shared_ptr<const RenderMesh> m_pendingIsosurface;
    std::shared_ptr<const RenderMesh> m_lastIsosurfaceMesh;
    std::atomic<bool> isosurfaceDirty{false};

    // Derived volume grid (unstructured meshes) + the pending handoff slot.
    std::shared_ptr<const volume::DerivedGrid> m_derivedVolume;
    std::shared_ptr<const volume::DerivedGrid> m_pendingDerivedVolume;
    std::atomic<bool> derivedVolumeDirty{false};

    mutable std::mutex meshQueueMutex;
    std::shared_ptr<const std::vector<float>> m_pendingScalarSrc;




    std::atomic<bool> volumeDirty{false};
    std::shared_ptr<const RenderMesh> m_pendingVolumeMesh;


    RenderRenderState m_state;

    bool m_clipControlAvailable = false;


    ColormapManager colormap;
    VectorGlyphSet vectorGlyph;
    StreamlineSet streamlineSet;
    MeshGLManager meshManager;
    MeshPass meshPass;
    GlyphPass glyphPass;
    ParticlePass particlePass;
    LodScheduler lodScheduler;
    BBoxOverlay m_bbox;
    QualityOverlayRenderer m_qualityOverlay;
    LineProbeOverlay m_lineProbe;
    ProbeOverlay m_probe;
    StreamlineController m_streamlines;
    VolumePass m_volume;
    VolumeTextureCache m_volumeCache;
    VectorTextureCache m_vectorCache;
    VolumeSliceOverlay m_volumeSliceOverlay;
    GlTexture m_licNoiseTex;
    int m_licNoiseGrain = 0;
    std::string m_lastLicError;      // surfaced via getLastLicError() for status bar
    std::string m_loggedLicError;    // last message sent to qWarning(), to stop per-frame log spam


    DepthPeelPass m_depthPeel;
    void renderTransparent(const glm::mat4& view, const glm::mat4& proj,
                             GLuint meshUbo,
                             const std::vector<std::pair<GLuint, int>>& transparentMeshes);

    // GPU histogram kernel (MVP: bins computed on GPU, rendered by custom QPainter widget)
    std::unique_ptr<PlotKernel> m_plotKernel;
};
