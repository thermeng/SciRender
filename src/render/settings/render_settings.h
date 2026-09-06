#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QColor>
#include <QTimer>
#include <QFutureWatcher>
#include <QDateTime>
#include <QRegularExpression>

#include <memory>
#include <atomic>
#include <functional>
#include <vector>

#include "render/foundation/renderer.h"
#include "core/mesh_loader.h"
#include "core/mesh_quality.h"
#include "core/Camera.h"
#include "core/camera_math.h"
#include "core/isosurface.h"
#include "render/settings/isosurface_controller.h"
#include "render/settings/animation_controller.h"
#include "render/settings/StateStore.h"
#include "core/FieldResolver.h"

#include <QElapsedTimer>

class AnimationExporter;

// Change flags carried by the consolidated viewChanged signal so receivers
// can distinguish which domain changed (e.g. lighting vs. colormap) without
// subscribing to a dozen fine-grained signals.
enum class ChangeFlag {
    Camera      = 1 << 0,
    Lighting    = 1 << 1,
    Colormap    = 1 << 2,
    Display     = 1 << 3,
    Slicing     = 1 << 4,
    Vectors     = 1 << 5,
    All         = 0xFF,
};
Q_DECLARE_FLAGS(ChangeFlags, ChangeFlag)
Q_DECLARE_OPERATORS_FOR_FLAGS(ChangeFlags)

// ponytail: bundles parse + quality so both run on the worker thread; the GUI
// callback only publishes. Quality analysis is pure CPU (reads flatVerts), no
// GL context, so it is safe off-thread.
struct MeshLoadResult {
    std::shared_ptr<const RenderMesh> mesh;
    MeshQuality quality;
};

// ---------------------------------------------------------------------------
// MeshData — all mesh-derived state (payload + metadata + quality).
// Belongs to the mesh, not to the user's display preferences. Separating it
// from RenderSettings means the QML facade stays small and adding a new
// mesh-derived field doesn't touch the render-state copy.
//
// Fields that affect rendering output (colors, toggles, scalar ranges, world
// bounds, quality overlay geometry) live in RenderRenderState (m_state) and
// are NOT duplicated here.
// ---------------------------------------------------------------------------
struct MeshData {
    // Parsed geometry (heavy — shared_ptr, never copied)
    std::shared_ptr<const RenderMesh> loadedMesh;
    // Light GUI meta copy (cheap: names, bounds, active scalar array)
    RenderMesh guiMeta;

    // Mesh metadata (read-only informational)
    std::string fileName;
    std::string datasetType;
    std::string meshFormat;
    int triangleCount = 0;
    int pointCount = 0;

    // Quality counts
    int degenerateFaces = 0;
    int openEdges = 0;
    int nonManifoldEdges = 0;
    int nonManifoldVerts = 0;
    bool watertight = false;

    // Available fields for QML combo boxes
    QStringList availableScalars;
    QStringList availableVectors;
};

// ---------------------------------------------------------------------------
// RenderSettings — the GUI-THREAD facade.
//
// This is the ONLY object QML binds to (as the `backendSettings` context
// property). It owns the pure-C++ Renderer backend and exposes every
// UI-exposed setting as a Q_PROPERTY / Q_INVOKABLE.
//
// m_state (RenderRenderState) is the SINGLE SOURCE OF truth for all visual
// / camera state. GUI-thread getters/setters read/write m_state.* directly.
// publishRenderState() hands a const reference to the Renderer, which
// deep-copies it. No intermediate snapshot assembly is needed.
// ---------------------------------------------------------------------------

// Collapses the boilerplate getter+setter body shared by ~100 QML-bound
// properties: getters read the member, setters guard against redundant writes,
// mark the snapshot stale, and emit the consolidated viewChanged(flag).
// The preprocessor runs before moc, so the macro is safe in a Q_OBJECT class.
#define STATE_PROP(GETTER, SETTER, TYPE, MEMBER, FLAG)                \
    TYPE GETTER() const { return MEMBER; }                            \
    void SETTER(TYPE v) {                                             \
        if (MEMBER != v) { MEMBER = v; markStateDirty(); emit viewChanged(ChangeFlag::FLAG); } \
    }

// Variant for QML `double` setters that store into a float member (the
// render state uses single precision; QML/QVariant traffic is double).
#define STATE_PROP_CAST(GETTER, SETTER, MEMBERTYPE, MEMBER, FLAG)     \
    double GETTER() const { return MEMBER; }                          \
    void SETTER(double v) {                                           \
        if (MEMBER != v) { MEMBER = static_cast<MEMBERTYPE>(v); markStateDirty(); emit viewChanged(ChangeFlag::FLAG); } \
    }

class RenderSettings : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool isWireframe READ isWireframe WRITE setWireframe NOTIFY viewChanged)
    Q_PROPERTY(bool useLod READ getUseLod WRITE setUseLod NOTIFY viewChanged)
    Q_PROPERTY(int msaaSamples READ getMsaaSamples WRITE setMsaaSamples NOTIFY viewChanged)
    Q_PROPERTY(bool isSurfaceVisible READ isSurfaceVisible WRITE toggleSurface NOTIFY viewChanged)
    Q_PROPERTY(bool hasMeshLoaded READ getHasMeshLoaded NOTIFY meshLoadStateChanged)
    Q_PROPERTY(bool meshHasScalars READ hasMeshScalars NOTIFY meshLoadStateChanged)
    Q_PROPERTY(bool hasMeshVectors READ hasMeshVectors NOTIFY meshLoadStateChanged)
    Q_PROPERTY(bool hasMeshCellVectors READ hasMeshCellVectors NOTIFY meshLoadStateChanged)
    Q_PROPERTY(QString currentMeshName READ getCurrentMeshNameQStr NOTIFY meshLoadStateChanged)

    Q_PROPERTY(float lightKeyAzimuth READ getLightKeyAzimuth WRITE setLightKeyAzimuth NOTIFY viewChanged)
    Q_PROPERTY(float lightKeyElevation READ getLightKeyElevation WRITE setLightKeyElevation NOTIFY viewChanged)
    Q_PROPERTY(float lightFillAzimuth READ getLightFillAzimuth WRITE setLightFillAzimuth NOTIFY viewChanged)
    Q_PROPERTY(float lightFillElevation READ getLightFillElevation WRITE setLightFillElevation NOTIFY viewChanged)
    Q_PROPERTY(float lightBackAzimuth READ getLightBackAzimuth WRITE setLightBackAzimuth NOTIFY viewChanged)
    Q_PROPERTY(float lightBackElevation READ getLightBackElevation WRITE setLightBackElevation NOTIFY viewChanged)
    Q_PROPERTY(float lightHeadAzimuth READ getLightHeadAzimuth WRITE setLightHeadAzimuth NOTIFY viewChanged)
    Q_PROPERTY(float lightHeadElevation READ getLightHeadElevation WRITE setLightHeadElevation NOTIFY viewChanged)
    Q_PROPERTY(float matAmbient READ getMatAmbient WRITE setMatAmbient NOTIFY viewChanged)
    Q_PROPERTY(float matDiffuse READ getMatDiffuse WRITE setMatDiffuse NOTIFY viewChanged)
    Q_PROPERTY(float matSpecular READ getMatSpecular WRITE setMatSpecular NOTIFY viewChanged)
    Q_PROPERTY(float matRoughness READ getMatRoughness WRITE setMatRoughness NOTIFY viewChanged)
    Q_PROPERTY(float matMetallic  READ getMatMetallic  WRITE setMatMetallic  NOTIFY viewChanged)
    Q_PROPERTY(float lightKeyIntensity READ getLightKeyIntensity WRITE setLightKeyIntensity NOTIFY viewChanged)
    Q_PROPERTY(float lightKF READ getLightKF WRITE setLightKF NOTIFY viewChanged)
    Q_PROPERTY(float lightKB READ getLightKB WRITE setLightKB NOTIFY viewChanged)
    Q_PROPERTY(float lightKH READ getLightKH WRITE setLightKH NOTIFY viewChanged)
    Q_PROPERTY(bool lightKitEnabled READ getLightKitEnabled WRITE setLightKitEnabled NOTIFY viewChanged)
    Q_PROPERTY(bool showLightMarkers READ getShowLightMarkers WRITE setShowLightMarkers NOTIFY viewChanged)
    Q_PROPERTY(float lightWarm READ getLightWarm WRITE setLightWarm NOTIFY viewChanged)
    Q_PROPERTY(int triangleCount READ getTriangleCount NOTIFY meshLoadStateChanged)
    Q_PROPERTY(int pointCount READ getPointCount NOTIFY meshLoadStateChanged)
    Q_PROPERTY(int degenerateFaces READ getDegenerateFaces NOTIFY meshLoadStateChanged)
    Q_PROPERTY(int openEdges READ getOpenEdges NOTIFY meshLoadStateChanged)
    Q_PROPERTY(int nonManifoldEdges READ getNonManifoldEdges NOTIFY meshLoadStateChanged)
    Q_PROPERTY(int nonManifoldVerts READ getNonManifoldVerts NOTIFY meshLoadStateChanged)
    Q_PROPERTY(bool watertight READ getWatertight NOTIFY meshLoadStateChanged)

    Q_PROPERTY(QString meshFormat READ getMeshFormat NOTIFY meshLoadStateChanged)
    Q_PROPERTY(QString meshDataType READ getMeshDataType NOTIFY meshLoadStateChanged)
    Q_PROPERTY(QColor bgColor READ getBgColorQml WRITE setBgColorQml NOTIFY viewChanged)
    Q_PROPERTY(QColor textColor READ getTextColorQml WRITE setTextColorQml NOTIFY viewChanged)
    Q_PROPERTY(QStringList availableScalars READ getAvailableScalars NOTIFY meshDataUpdated)
    Q_PROPERTY(bool showScalarColorbar READ getShowScalarColorbar WRITE setShowScalarColorbar NOTIFY viewChanged)
    Q_PROPERTY(bool meshUseScalarColor READ getMeshUseScalarColor WRITE setMeshUseScalarColor NOTIFY viewChanged)
    Q_PROPERTY(bool showVectors READ getShowVectors WRITE setShowVectors NOTIFY viewChanged)
    Q_PROPERTY(float vectorScale READ getVectorScale WRITE setVectorScale NOTIFY viewChanged)
    Q_PROPERTY(int vectorStride READ getVectorStride WRITE setVectorStride NOTIFY viewChanged)
    Q_PROPERTY(bool vectorScaleByMagnitude READ getVectorScaleByMagnitude WRITE setVectorScaleByMagnitude NOTIFY viewChanged)
    Q_PROPERTY(int vectorMagTransform READ getVectorMagTransform WRITE setVectorMagTransform NOTIFY viewChanged)
    Q_PROPERTY(QColor vectorColor READ getVectorColorQml WRITE setVectorColorQml NOTIFY viewChanged)
    Q_PROPERTY(QString vectorField READ getVectorField WRITE setActiveVectorField NOTIFY meshDataUpdated)
    Q_PROPERTY(QStringList availableVectors READ getAvailableVectors NOTIFY meshDataUpdated)
    Q_PROPERTY(int vectorColorMode READ getVectorColorMode WRITE setVectorColorMode NOTIFY viewChanged)
    Q_PROPERTY(int vectorColormapChoice READ getVectorColormapChoice WRITE setVectorColormapChoice NOTIFY viewChanged)
    Q_PROPERTY(bool vectorColormapReversed READ getVectorColormapReversed WRITE setVectorColormapReversed NOTIFY viewChanged)
    Q_PROPERTY(int vectorPlacement READ getVectorPlacement WRITE setVectorPlacement NOTIFY viewChanged)
    Q_PROPERTY(QStringList vectorPlacementOptions READ getVectorPlacementOptions CONSTANT)
    Q_PROPERTY(int vectorVisMode READ getVectorVisMode WRITE setVectorVisMode NOTIFY viewChanged)
    Q_PROPERTY(bool showLic READ getShowLic WRITE setShowLic NOTIFY viewChanged)
    Q_PROPERTY(int licSteps READ getLicSteps WRITE setLicSteps NOTIFY viewChanged)
    Q_PROPERTY(double licStepSize READ getLicStepSize WRITE setLicStepSize NOTIFY viewChanged)
    Q_PROPERTY(double licNoiseFreq READ getLicNoiseFreq WRITE setLicNoiseFreq NOTIFY viewChanged)
    Q_PROPERTY(int licNoiseGrain READ getLicNoiseGrain WRITE setLicNoiseGrain NOTIFY viewChanged)
    Q_PROPERTY(int licBoundaryMode READ getLicBoundaryMode WRITE setLicBoundaryMode NOTIFY viewChanged)
    Q_PROPERTY(bool licEnhanced READ getLicEnhanced WRITE setLicEnhanced NOTIFY viewChanged)
    Q_PROPERTY(QStringList recentFiles READ getRecentFiles NOTIFY meshLoadStateChanged)
    Q_PROPERTY(QString activeScalarName READ getActiveScalarNameQml NOTIFY meshDataUpdated)

    Q_PROPERTY(int colormapChoice READ getColormapChoice WRITE setColormapChoice NOTIFY viewChanged)
    Q_PROPERTY(bool colormapReversed READ getColormapReversed WRITE setColormapReversed NOTIFY viewChanged)
    Q_PROPERTY(int scalarColorBands READ getScalarColorBands WRITE setScalarColorBands NOTIFY viewChanged)
    Q_PROPERTY(int vectorColorBands READ getVectorColorBands WRITE setVectorColorBands NOTIFY viewChanged)
    Q_PROPERTY(int streamlineColorBands READ getStreamlineColorBands WRITE setStreamlineColorBands NOTIFY viewChanged)
    Q_PROPERTY(int volumeColorBands READ getVolumeColorBands WRITE setVolumeColorBands NOTIFY viewChanged)
    Q_PROPERTY(int volumeSliceColorBands READ getVolumeSliceColorBands WRITE setVolumeSliceColorBands NOTIFY viewChanged)
    Q_PROPERTY(double worldMinX READ getWorldMinX NOTIFY meshLoadStateChanged)
    Q_PROPERTY(double worldMaxX READ getWorldMaxX NOTIFY meshLoadStateChanged)
    Q_PROPERTY(double worldMinY READ getWorldMinY NOTIFY meshLoadStateChanged)
    Q_PROPERTY(double worldMaxY READ getWorldMaxY NOTIFY meshLoadStateChanged)
    Q_PROPERTY(double worldMinZ READ getWorldMinZ NOTIFY meshLoadStateChanged)
    Q_PROPERTY(double worldMaxZ READ getWorldMaxZ NOTIFY meshLoadStateChanged)
    Q_PROPERTY(double dataScalarMinQml READ getDataScalarMinQml NOTIFY meshLoadStateChanged)
    Q_PROPERTY(double dataScalarMaxQml READ getDataScalarMaxQml NOTIFY meshLoadStateChanged)
    Q_PROPERTY(int colorbarTicks READ getColorbarTicks WRITE setColorbarTicks NOTIFY viewChanged)
    Q_PROPERTY(bool clipEnabled READ getClipEnabled WRITE setClipEnabled NOTIFY viewChanged)
    Q_PROPERTY(bool crinkleClipMode READ getCrinkleClipMode WRITE setCrinkleClipMode NOTIFY viewChanged)
    Q_PROPERTY(float clipHeightX READ getClipX WRITE setClipX NOTIFY viewChanged)
    Q_PROPERTY(float clipHeightY READ getClipY WRITE setClipY NOTIFY viewChanged)
    Q_PROPERTY(float clipHeightZ READ getClipZ WRITE setClipZ NOTIFY viewChanged)
    Q_PROPERTY(bool clipEnabledX READ getClipEnabledX WRITE setClipEnabledX NOTIFY viewChanged)
    Q_PROPERTY(bool clipEnabledY READ getClipEnabledY WRITE setClipEnabledY NOTIFY viewChanged)
    Q_PROPERTY(bool clipEnabledZ READ getClipEnabledZ WRITE setClipEnabledZ NOTIFY viewChanged)
    Q_PROPERTY(bool invertX READ getInvertX WRITE setInvertX NOTIFY viewChanged)
    Q_PROPERTY(bool invertY READ getInvertY WRITE setInvertY NOTIFY viewChanged)
    Q_PROPERTY(bool invertZ READ getInvertZ WRITE setInvertZ NOTIFY viewChanged)
    Q_PROPERTY(float filterMin READ getFilterMin WRITE setFilterMin NOTIFY viewChanged)
    Q_PROPERTY(float filterMax READ getFilterMax WRITE setFilterMax NOTIFY viewChanged)
    Q_PROPERTY(bool filterEnabled READ getFilterEnabled WRITE setFilterEnabled NOTIFY viewChanged)
    Q_PROPERTY(bool colorRangeOverrideEnabled READ getColorRangeOverrideEnabled WRITE setColorRangeOverrideEnabled NOTIFY viewChanged)
    Q_PROPERTY(float colorRangeLo READ getColorRangeLo WRITE setColorRangeLo NOTIFY viewChanged)
    Q_PROPERTY(float colorRangeHi READ getColorRangeHi WRITE setColorRangeHi NOTIFY viewChanged)
    Q_PROPERTY(bool volumeColorRangeOverrideEnabled READ getVolumeColorRangeOverrideEnabled WRITE setVolumeColorRangeOverrideEnabled NOTIFY viewChanged)
    Q_PROPERTY(float volumeColorRangeLo READ getVolumeColorRangeLo WRITE setVolumeColorRangeLo NOTIFY viewChanged)
    Q_PROPERTY(float volumeColorRangeHi READ getVolumeColorRangeHi WRITE setVolumeColorRangeHi NOTIFY viewChanged)
    Q_PROPERTY(bool sliceColorRangeOverrideEnabled READ getSliceColorRangeOverrideEnabled WRITE setSliceColorRangeOverrideEnabled NOTIFY viewChanged)
    Q_PROPERTY(float sliceColorRangeLo READ getSliceColorRangeLo WRITE setSliceColorRangeLo NOTIFY viewChanged)
    Q_PROPERTY(float sliceColorRangeHi READ getSliceColorRangeHi WRITE setSliceColorRangeHi NOTIFY viewChanged)
    Q_PROPERTY(bool glyphMagRangeOverrideEnabled READ getGlyphMagRangeOverrideEnabled WRITE setGlyphMagRangeOverrideEnabled NOTIFY viewChanged)
    Q_PROPERTY(float glyphMagRangeLo READ getGlyphMagRangeLo WRITE setGlyphMagRangeLo NOTIFY viewChanged)
    Q_PROPERTY(float glyphMagRangeHi READ getGlyphMagRangeHi WRITE setGlyphMagRangeHi NOTIFY viewChanged)
    Q_PROPERTY(bool streamlineMagRangeOverrideEnabled READ getStreamlineMagRangeOverrideEnabled WRITE setStreamlineMagRangeOverrideEnabled NOTIFY viewChanged)
    Q_PROPERTY(float streamlineMagRangeLo READ getStreamlineMagRangeLo WRITE setStreamlineMagRangeLo NOTIFY viewChanged)
    Q_PROPERTY(float streamlineMagRangeHi READ getStreamlineMagRangeHi WRITE setStreamlineMagRangeHi NOTIFY viewChanged)
    Q_PROPERTY(bool streamlineCompRangeOverrideEnabledX READ getStreamlineCompRangeOverrideEnabledX NOTIFY viewChanged)
    Q_PROPERTY(float streamlineCompRangeLoX READ getStreamlineCompRangeLoX WRITE setStreamlineCompRangeLoX NOTIFY viewChanged)
    Q_PROPERTY(float streamlineCompRangeHiX READ getStreamlineCompRangeHiX WRITE setStreamlineCompRangeHiX NOTIFY viewChanged)
    Q_PROPERTY(bool streamlineCompRangeOverrideEnabledY READ getStreamlineCompRangeOverrideEnabledY NOTIFY viewChanged)
    Q_PROPERTY(float streamlineCompRangeLoY READ getStreamlineCompRangeLoY WRITE setStreamlineCompRangeLoY NOTIFY viewChanged)
    Q_PROPERTY(float streamlineCompRangeHiY READ getStreamlineCompRangeHiY WRITE setStreamlineCompRangeHiY NOTIFY viewChanged)
    Q_PROPERTY(bool streamlineCompRangeOverrideEnabledZ READ getStreamlineCompRangeOverrideEnabledZ NOTIFY viewChanged)
    Q_PROPERTY(float streamlineCompRangeLoZ READ getStreamlineCompRangeLoZ WRITE setStreamlineCompRangeLoZ NOTIFY viewChanged)
    Q_PROPERTY(float streamlineCompRangeHiZ READ getStreamlineCompRangeHiZ WRITE setStreamlineCompRangeHiZ NOTIFY viewChanged)
    Q_PROPERTY(float vectorCompMinX READ getVectorCompMinX NOTIFY meshDataUpdated)
    Q_PROPERTY(float vectorCompMaxX READ getVectorCompMaxX NOTIFY meshDataUpdated)
    Q_PROPERTY(float vectorCompMinY READ getVectorCompMinY NOTIFY meshDataUpdated)
    Q_PROPERTY(float vectorCompMaxY READ getVectorCompMaxY NOTIFY meshDataUpdated)
    Q_PROPERTY(float vectorCompMinZ READ getVectorCompMinZ NOTIFY meshDataUpdated)
    Q_PROPERTY(float vectorCompMaxZ READ getVectorCompMaxZ NOTIFY meshDataUpdated)

    Q_PROPERTY(bool showStreamlines READ getShowStreamlines WRITE setShowStreamlines NOTIFY viewChanged)
    Q_PROPERTY(QString streamlineVectorField READ getStreamlineVectorField WRITE setStreamlineVectorField NOTIFY meshDataUpdated)
    Q_PROPERTY(int streamlineSeedCount READ getStreamlineSeedCount WRITE setStreamlineSeedCount NOTIFY viewChanged)
    Q_PROPERTY(double streamlineStepSize READ getStreamlineStepSize WRITE setStreamlineStepSize NOTIFY viewChanged)
    Q_PROPERTY(int streamlineMaxSteps READ getStreamlineMaxSteps WRITE setStreamlineMaxSteps NOTIFY viewChanged)
    Q_PROPERTY(bool streamlineUseColormap READ getStreamlineUseColormap WRITE setStreamlineUseColormap NOTIFY viewChanged)
    Q_PROPERTY(int streamlineColorMode READ getStreamlineColorMode WRITE setStreamlineColorMode NOTIFY viewChanged)
    Q_PROPERTY(int streamlineColormapChoice READ getStreamlineColormapChoice WRITE setStreamlineColormapChoice NOTIFY viewChanged)
    Q_PROPERTY(bool streamlineColormapReversed READ getStreamlineColormapReversed WRITE setStreamlineColormapReversed NOTIFY viewChanged)
    Q_PROPERTY(QColor streamlineColor READ getStreamlineColorQml WRITE setStreamlineColorQml NOTIFY viewChanged)
    Q_PROPERTY(QString seedMode READ getSeedMode WRITE setSeedMode NOTIFY viewChanged)
    Q_PROPERTY(double seedPlanePos READ getSeedPlanePos WRITE setSeedPlanePos NOTIFY viewChanged)
    Q_PROPERTY(int seedPlaneCountU READ getSeedPlaneCountU WRITE setSeedPlaneCountU NOTIFY viewChanged)
    Q_PROPERTY(int seedPlaneCountV READ getSeedPlaneCountV WRITE setSeedPlaneCountV NOTIFY viewChanged)
    Q_PROPERTY(double seedJitter READ getSeedJitter WRITE setSeedJitter NOTIFY viewChanged)
    Q_PROPERTY(bool showSeeds READ getShowSeeds WRITE setShowSeeds NOTIFY viewChanged)
    Q_PROPERTY(bool showStreamlineArrows READ getShowStreamlineArrows WRITE setShowStreamlineArrows NOTIFY viewChanged)
    Q_PROPERTY(double streamlineArrowSpacingFrac READ getStreamlineArrowSpacingFrac WRITE setStreamlineArrowSpacingFrac NOTIFY viewChanged)
    Q_PROPERTY(double streamlineArrowSize READ getStreamlineArrowSize WRITE setStreamlineArrowSize NOTIFY viewChanged)
    Q_PROPERTY(double streamlineOpacity READ getStreamlineOpacity WRITE setStreamlineOpacity NOTIFY viewChanged)
    Q_PROPERTY(double streamlineRibbonWidth READ getStreamlineRibbonWidth WRITE setStreamlineRibbonWidth NOTIFY viewChanged)
    Q_PROPERTY(double streamlineTaperFactor READ getStreamlineTaperFactor WRITE setStreamlineTaperFactor NOTIFY viewChanged)
    Q_PROPERTY(QString streamlineDirection READ getStreamlineDirection WRITE setStreamlineDirection NOTIFY viewChanged)
    Q_PROPERTY(double streamlineAmbient READ getStreamlineAmbient WRITE setStreamlineAmbient NOTIFY viewChanged)
    Q_PROPERTY(double streamlineDiffuse READ getStreamlineDiffuse WRITE setStreamlineDiffuse NOTIFY viewChanged)
    Q_PROPERTY(double streamlineSpecular READ getStreamlineSpecular WRITE setStreamlineSpecular NOTIFY viewChanged)
    Q_PROPERTY(int streamlineSpecularPower READ getStreamlineSpecularPower WRITE setStreamlineSpecularPower NOTIFY viewChanged)
    Q_PROPERTY(double seedPointSize READ getSeedPointSize WRITE setSeedPointSize NOTIFY viewChanged)
    Q_PROPERTY(QColor seedPointColor READ getSeedPointColorQml WRITE setSeedPointColorQml NOTIFY viewChanged)
    Q_PROPERTY(bool showParticles READ getShowParticles WRITE setShowParticles NOTIFY viewChanged)
    Q_PROPERTY(int particleCount READ getParticleCount WRITE setParticleCount NOTIFY viewChanged)
    Q_PROPERTY(double particleSpeed READ getParticleSpeed WRITE setParticleSpeed NOTIFY viewChanged)
    Q_PROPERTY(double particleSize READ getParticleSize WRITE setParticleSize NOTIFY viewChanged)
    Q_PROPERTY(bool particleAdditive READ getParticleAdditive WRITE setParticleAdditive NOTIFY viewChanged)
    Q_PROPERTY(bool showVolume READ getShowVolume WRITE setShowVolume NOTIFY viewChanged)
    Q_PROPERTY(bool volumeUseColormap READ getVolumeUseColormap WRITE setVolumeUseColormap NOTIFY viewChanged)
    Q_PROPERTY(int volumeColormapChoice READ getVolumeColormapChoice WRITE setVolumeColormapChoice NOTIFY viewChanged)
    Q_PROPERTY(bool volumeColormapReversed READ getVolumeColormapReversed WRITE setVolumeColormapReversed NOTIFY viewChanged)
    Q_PROPERTY(double volumeStepSize READ getVolumeStepSize WRITE setVolumeStepSize NOTIFY viewChanged)
    Q_PROPERTY(double volumeOpacity READ getVolumeOpacity WRITE setVolumeOpacity NOTIFY viewChanged)
    Q_PROPERTY(bool slicePlaneEnabledX READ getSlicePlaneEnabledX WRITE setSlicePlaneEnabledX NOTIFY viewChanged)
    Q_PROPERTY(bool slicePlaneEnabledY READ getSlicePlaneEnabledY WRITE setSlicePlaneEnabledY NOTIFY viewChanged)
    Q_PROPERTY(bool slicePlaneEnabledZ READ getSlicePlaneEnabledZ WRITE setSlicePlaneEnabledZ NOTIFY viewChanged)
    Q_PROPERTY(double slicePlanePosX READ getSlicePlanePosX WRITE setSlicePlanePosX NOTIFY viewChanged)
    Q_PROPERTY(double slicePlanePosY READ getSlicePlanePosY WRITE setSlicePlanePosY NOTIFY viewChanged)
    Q_PROPERTY(double slicePlanePosZ READ getSlicePlanePosZ WRITE setSlicePlanePosZ NOTIFY viewChanged)
    Q_PROPERTY(double slicePlaneOpacityX READ getSlicePlaneOpacityX WRITE setSlicePlaneOpacityX NOTIFY viewChanged)
    Q_PROPERTY(double slicePlaneOpacityY READ getSlicePlaneOpacityY WRITE setSlicePlaneOpacityY NOTIFY viewChanged)
    Q_PROPERTY(double slicePlaneOpacityZ READ getSlicePlaneOpacityZ WRITE setSlicePlaneOpacityZ NOTIFY viewChanged)
    Q_PROPERTY(bool slicePlaneShowColorbarX READ getSlicePlaneShowColorbarX WRITE setSlicePlaneShowColorbarX NOTIFY viewChanged)
    Q_PROPERTY(bool slicePlaneShowColorbarY READ getSlicePlaneShowColorbarY WRITE setSlicePlaneShowColorbarY NOTIFY viewChanged)
    Q_PROPERTY(bool slicePlaneShowColorbarZ READ getSlicePlaneShowColorbarZ WRITE setSlicePlaneShowColorbarZ NOTIFY viewChanged)
    Q_PROPERTY(bool volumeSliceUseColormap READ getVolumeSliceUseColormap WRITE setVolumeSliceUseColormap NOTIFY viewChanged)
    Q_PROPERTY(int volumeSliceColormapChoice READ getVolumeSliceColormapChoice WRITE setVolumeSliceColormapChoice NOTIFY viewChanged)
    Q_PROPERTY(bool volumeSliceColormapReversed READ getVolumeSliceColormapReversed WRITE setVolumeSliceColormapReversed NOTIFY viewChanged)

    Q_PROPERTY(bool isGizmoVisible READ isGizmoVisible WRITE setGizmoVisible NOTIFY viewChanged)
    Q_PROPERTY(int gizmoCorner READ getGizmoCorner WRITE setGizmoCorner NOTIFY viewChanged)
    Q_PROPERTY(int gizmoSizeChoice READ getGizmoSizeChoice WRITE setGizmoSizeChoice NOTIFY viewChanged)
    Q_PROPERTY(bool showPoints READ getShowPoints WRITE setShowPoints NOTIFY viewChanged)
    Q_PROPERTY(float pointSize READ getPointSize WRITE setPointSize NOTIFY viewChanged)
    Q_PROPERTY(float lineWidth READ getLineWidth WRITE setLineWidth NOTIFY viewChanged)
    Q_PROPERTY(bool pointUseScalar READ getPointUseScalar WRITE setPointUseScalar NOTIFY viewChanged)
    Q_PROPERTY(float pointOpacity READ getPointOpacity WRITE setPointOpacity NOTIFY viewChanged)
    Q_PROPERTY(float surfaceOpacity READ getSurfaceOpacity WRITE setSurfaceOpacity NOTIFY viewChanged)
    Q_PROPERTY(int maxPeelLayers READ getMaxPeelLayers WRITE setMaxPeelLayers NOTIFY viewChanged) // depth peeling layer count (1-8)
    Q_PROPERTY(int cullMode READ getCullMode WRITE setCullMode NOTIFY viewChanged) // 0=off 1=back 2=front
    Q_PROPERTY(bool showBounds READ getShowBounds WRITE setShowBounds NOTIFY viewChanged)
    Q_PROPERTY(bool showQualityOverlay READ getShowQualityOverlay WRITE setShowQualityOverlay NOTIFY viewChanged)
    Q_PROPERTY(bool orthographic READ getOrthographic WRITE setOrthographic NOTIFY viewChanged)
    Q_PROPERTY(bool autoRotate READ getAutoRotate WRITE setAutoRotate NOTIFY viewChanged)
    Q_PROPERTY(QColor meshColor READ getMeshColorQml WRITE setMeshColorQml NOTIFY viewChanged)
    Q_PROPERTY(QColor surfaceColor READ getSurfaceColorQml WRITE setSurfaceColorQml NOTIFY viewChanged)
    Q_PROPERTY(bool screenshotTransparent READ getScreenshotTransparent WRITE setScreenshotTransparent NOTIFY viewChanged)
    Q_PROPERTY(int screenshotResolution READ getScreenshotResolution WRITE setScreenshotResolution NOTIFY viewChanged)
    Q_PROPERTY(int screenshotAASamples READ getScreenshotAASamples WRITE setScreenshotAASamples NOTIFY viewChanged)
     Q_PROPERTY(bool flatShading READ getFlatShading WRITE setFlatShading NOTIFY viewChanged)

     Q_PROPERTY(bool showIsosurface READ getShowIsosurface WRITE setShowIsosurface NOTIFY viewChanged)
     Q_PROPERTY(double isovalue READ getIsovalue WRITE setIsovalue NOTIFY viewChanged)
     Q_PROPERTY(bool isosurfaceAvailable READ getIsosurfaceAvailable NOTIFY meshDataUpdated)

     Q_PROPERTY(QString statusMessage READ getStatusMessage NOTIFY statusMessageChanged)

    Q_PROPERTY(bool showFps READ getShowFps WRITE setShowFps NOTIFY viewChanged)
    Q_PROPERTY(bool quickBarCollapsed READ getQuickBarCollapsed WRITE setQuickBarCollapsed NOTIFY quickBarCollapsedChanged)
    Q_PROPERTY(QString fpsText READ getFpsText NOTIFY fpsChanged)

public:
    explicit RenderSettings(QObject* parent = nullptr);
    ~RenderSettings() override;

    // ---- backend access (render thread owns the Renderer) ----
    Renderer* backend() { return &m_renderer; }

    // PVD animation playback engine (see AnimationController). The MainWindow
    // animation page binds directly to the controller; RenderSettings only
    // routes .pvd loads into it and consumes frameReady() in onAnimationFrame().
    AnimationController* anim() { return &m_animController; }

    // .pvd frame-sequence export (AVI/PNG); capture callback lives in the viewport.
    AnimationExporter* animationExporter() const { return m_animationExporter; }

    // Assembles the per-frame snapshot. No-op now that m_state IS the
    // single source of truth; kept as a trivial inline in the header.
    const RenderRenderState& snapshot() const { return m_state; }

    // Double-buffered publish: re-assembles the snapshot ONLY when the GUI
    // state is dirty, then hands the (already-built) buffer to the Renderer.
    // Skips the ~75-field assembly entirely on idle frames.
    void publishRenderState(::Renderer* scene);

    // ---- view state accessors ----
    bool isWireframe() const { return m_state.showWireframe; }
    void setWireframe(bool enabled);
    bool getUseLod() const { return m_state.useLod; }
    void setUseLod(bool enabled);
    int getMsaaSamples() const { return msaaSamples; }
    void setMsaaSamples(int n);

    bool isSurfaceVisible() const { return m_state.showSurface; }

    bool getHasMeshLoaded() const { return m_state.hasMeshLoaded; }
    bool isLoading() const { return m_meshWatcher.isRunning(); }
    int getTriangleCount() const { return m_meshData.triangleCount; }
    int getPointCount() const { return m_meshData.pointCount; }
    int getDegenerateFaces() const { return m_meshData.degenerateFaces; }
    int getOpenEdges() const { return m_meshData.openEdges; }
    int getNonManifoldEdges() const { return m_meshData.nonManifoldEdges; }
    int getNonManifoldVerts() const { return m_meshData.nonManifoldVerts; }
    bool getWatertight() const { return m_meshData.watertight; }
    QString getMeshDataType() const { return QString::fromStdString(m_meshData.datasetType); }
    QString getMeshFormat() const { return QString::fromStdString(m_meshData.meshFormat); }
    QColor getBgColorQml() const { return QColor::fromRgbF(m_state.bgColor[0], m_state.bgColor[1], m_state.bgColor[2]); }
    void setBgColorQml(const QColor& c) { m_state.bgColor[0] = c.redF(); m_state.bgColor[1] = c.greenF(); m_state.bgColor[2] = c.blueF(); markStateDirty(); emit viewChanged(ChangeFlag::Display); }
    QColor getTextColorQml() const { return QColor::fromRgbF(m_state.textColor[0], m_state.textColor[1], m_state.textColor[2]); }
    void setTextColorQml(const QColor& c) { m_state.textColor[0] = c.redF(); m_state.textColor[1] = c.greenF(); m_state.textColor[2] = c.blueF(); markStateDirty(); emit viewChanged(ChangeFlag::Display); }

    int getColormapChoice() const { return m_state.colormapChoice; }
    void setColormapChoice(int choice);
    bool getColormapReversed() const { return m_state.colormapReversed; }
    void setColormapReversed(bool reversed);
    bool getVectorColormapReversed() const { return m_state.vectorColormapReversed; }
    void setVectorColormapReversed(bool reversed);
    int getScalarColorBands() const { return m_state.scalarColorBands; }
    void setScalarColorBands(int bands);
    int getVectorColorBands() const { return m_state.vectorColorBands; }
    void setVectorColorBands(int bands);
    int getStreamlineColorBands() const { return m_state.streamlineColorBands; }
    void setStreamlineColorBands(int bands);
    int getVolumeColorBands() const { return m_state.volumeColorBands; }
    void setVolumeColorBands(int bands);
    int getVolumeSliceColorBands() const { return m_state.volumeSliceColorBands; }
    void setVolumeSliceColorBands(int bands);

    bool getQuickBarCollapsed() const { return quickBarCollapsed; }
    void setQuickBarCollapsed(bool collapsed);

    STATE_PROP(getLightKeyAzimuth, setLightKeyAzimuth, float, m_state.lighting.lightKeyAzimuth, Lighting)
    STATE_PROP(getLightKeyElevation, setLightKeyElevation, float, m_state.lighting.lightKeyElevation, Lighting)
    STATE_PROP(getLightFillAzimuth, setLightFillAzimuth, float, m_state.lighting.lightFillAzimuth, Lighting)
    STATE_PROP(getLightFillElevation, setLightFillElevation, float, m_state.lighting.lightFillElevation, Lighting)
    STATE_PROP(getLightBackAzimuth, setLightBackAzimuth, float, m_state.lighting.lightBackAzimuth, Lighting)
    STATE_PROP(getLightBackElevation, setLightBackElevation, float, m_state.lighting.lightBackElevation, Lighting)
    STATE_PROP(getLightHeadAzimuth, setLightHeadAzimuth, float, m_state.lighting.lightHeadAzimuth, Lighting)
    STATE_PROP(getLightHeadElevation, setLightHeadElevation, float, m_state.lighting.lightHeadElevation, Lighting)
    STATE_PROP(getMatAmbient, setMatAmbient, float, m_state.lighting.matAmbient, Lighting)
    STATE_PROP(getMatDiffuse, setMatDiffuse, float, m_state.lighting.matDiffuse, Lighting)
    STATE_PROP(getMatSpecular, setMatSpecular, float, m_state.lighting.matSpecular, Lighting)
    STATE_PROP(getMatRoughness, setMatRoughness, float, m_state.lighting.matRoughness, Lighting)
    STATE_PROP(getMatMetallic, setMatMetallic, float, m_state.lighting.matMetallic, Lighting)
    STATE_PROP(getLightKeyIntensity, setLightKeyIntensity, float, m_state.lighting.lightKeyIntensity, Lighting)
    STATE_PROP(getLightKF, setLightKF, float, m_state.lighting.lightKF, Lighting)
    STATE_PROP(getLightKB, setLightKB, float, m_state.lighting.lightKB, Lighting)
    STATE_PROP(getLightKH, setLightKH, float, m_state.lighting.lightKH, Lighting)
    STATE_PROP(getLightKitEnabled, setLightKitEnabled, bool, m_state.lighting.lightKitEnabled, Lighting)
    STATE_PROP(getShowLightMarkers, setShowLightMarkers, bool, m_state.lighting.showLightMarkers, Lighting)
    STATE_PROP(getLightWarm, setLightWarm, float, m_state.lighting.lightWarm, Lighting)

    QString getCurrentMeshNameQStr() const { return QString::fromStdString(m_meshData.fileName); }

    // ---- UI callable actions (slots invoked by QML) ----
public slots:
    void loadMesh(const QString& filePath);
    void onMeshParsed(); // GUI-thread continuation after async parse
    void onAnimationFrame(std::shared_ptr<const RenderMesh> mesh, int frameIndex, double time); // PVD frame publish
    void openRecent(const QString& filePath);
    void clearMeshes();
    void resetCamera();
    void resetCameraInstant();   // no animation — used when the data itself changed (mesh load)
    void snapToOrthoView(int axis);
    Q_INVOKABLE void requestScreenshot(const QString& path);
    void snapToAxisView(int axis, bool flip);
    void snapGizmoAxis(int axis);   // ParaView-style: align to axis; second click flips to the opposite face
    void toggleSurface(bool visible);

    QStringList getAvailableScalars() const;
    QStringList getRecentFiles() const { return recentFiles; }
    void loadRecentFromSettings();
    void saveRecentToSettings() const;
    void clearRecentFiles();
    void saveStateToSettings() const;
    void restoreStateFromSettings();
    Q_INVOKABLE void setActiveScalarField(const QString& fieldName);

    Q_INVOKABLE void setFpsText(const QString& text);

    signals:
    void viewChanged(ChangeFlags flags = ChangeFlag::All);
    void meshLoadStateChanged();
    void meshDataUpdated();
    void quickBarCollapsedChanged();
    void screenshotCaptured(const QString& targetSavedPath);
    void screenshotRequested(const QString& targetPath);
    void fpsChanged();
    void statusMessageChanged();

public:
    // VTK Camera forwarders (QML-invokable). Mutate the GUI-side Camera; the
    // next synchronize() copies it into the render-thread snapshot. Manual
    // input always cancels an in-flight fly-to-face transition first.
    Q_INVOKABLE void azimuth(double angle) { cancelCameraTransition(); m_state.camera.azimuth(angle); m_renderer.markCameraMoving(); markStateDirty(); emit viewChanged(ChangeFlag::Camera); }
    Q_INVOKABLE void elevation(double angle) { cancelCameraTransition(); m_state.camera.elevation(angle); m_renderer.markCameraMoving(); markStateDirty(); emit viewChanged(ChangeFlag::Camera); }
    Q_INVOKABLE void pan(double dx, double dy) { cancelCameraTransition(); m_state.camera.pan(dx, dy); m_renderer.markCameraMoving(); markStateDirty(); emit viewChanged(ChangeFlag::Camera); }
    Q_INVOKABLE void dolly(double factor) { cancelCameraTransition(); m_state.camera.dolly(factor); m_renderer.markCameraMoving(); markStateDirty(); emit viewChanged(ChangeFlag::Camera); }

    float* getMeshColor() { return m_state.meshColor; }
    float* getSurfaceColor() { return m_state.surfaceColor; }
    float* getBgColor() { return m_state.bgColor; }

    Q_INVOKABLE void setSidebarWidth(float w) { sidebarWidth = w; }

    STATE_PROP(isGizmoVisible, setGizmoVisible, bool, m_state.showGizmo, Display)
    STATE_PROP(getGizmoCorner, setGizmoCorner, int, m_state.gizmoCorner, Display)
    STATE_PROP(getGizmoSizeChoice, setGizmoSizeChoice, int, m_state.gizmoSizeChoice, Display)
    STATE_PROP(getAutoRotate, setAutoRotate, bool, m_state.autoRotate, Display)
    STATE_PROP(getShowPoints, setShowPoints, bool, m_state.showPoints, Display)
    STATE_PROP(getPointSize, setPointSize, float, m_state.pointSize, Display)
    STATE_PROP(getLineWidth, setLineWidth, float, m_state.lineWidth, Display)
    STATE_PROP(getPointUseScalar, setPointUseScalar, bool, m_state.pointUseScalar, Display)
    STATE_PROP(getPointOpacity, setPointOpacity, float, m_state.pointOpacity, Display)
    STATE_PROP(getSurfaceOpacity, setSurfaceOpacity, float, m_state.surfaceOpacity, Display)

    int getMaxPeelLayers() const { return m_state.maxPeelLayers; }
    void setMaxPeelLayers(int v) { int t = (v < 1) ? 1 : (v > 8 ? 8 : v); if (m_state.maxPeelLayers != t) { m_state.maxPeelLayers = t; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    STATE_PROP(getCullMode, setCullMode, int, m_state.cullMode, Display)
    STATE_PROP(getShowBounds, setShowBounds, bool, m_state.showBounds, Display)
    STATE_PROP(getShowQualityOverlay, setShowQualityOverlay, bool, m_state.showQualityOverlay, Display)
    // Manual (not STATE_PROP): entering/leaving parallel projection must cancel
    // any in-flight fly-to-face, since the two projection modes interpolate differently.
    bool getOrthographic() const { return m_state.orthographic; }
    void setOrthographic(bool v) {
        cancelCameraTransition();
        if (m_state.orthographic != v) { m_state.orthographic = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); }
    }
    QColor getMeshColorQml() const { return QColor::fromRgbF(m_state.meshColor[0], m_state.meshColor[1], m_state.meshColor[2]); }
    void setMeshColorQml(const QColor& c) { m_state.meshColor[0] = c.redF(); m_state.meshColor[1] = c.greenF(); m_state.meshColor[2] = c.blueF(); markStateDirty(); emit viewChanged(ChangeFlag::Display); }
    QColor getSurfaceColorQml() const { return QColor::fromRgbF(m_state.surfaceColor[0], m_state.surfaceColor[1], m_state.surfaceColor[2]); }
    void setSurfaceColorQml(const QColor& c) { m_state.surfaceColor[0] = c.redF(); m_state.surfaceColor[1] = c.greenF(); m_state.surfaceColor[2] = c.blueF(); markStateDirty(); emit viewChanged(ChangeFlag::Display); }

    int getVectorVisMode() const { return m_state.vectorVisMode; }
    void setVectorVisMode(int v) {
        int t = (v < 0) ? 0 : (v > 2 ? 2 : v);
        if (m_state.vectorVisMode != t) {
            m_state.vectorVisMode = t;
            m_state.showVectors = (t == 1);
            m_state.showLic = (t == 2);
            m_renderer.markVectorGlyphDirty();
            markStateDirty();
            emit viewChanged(ChangeFlag::Vectors);
            emit viewChanged(ChangeFlag::Display);
        }
    }
    bool getShowVectors() const { return m_state.showVectors; }
    void setShowVectors(bool v) {
        int desired = v ? 1 : 0;
        if (v && m_state.vectorVisMode == 2) desired = 1;
        if (!v && m_state.vectorVisMode == 1) desired = 0;
        if (m_state.vectorVisMode != desired) {
            setVectorVisMode(desired);
        } else if (m_state.showVectors != v) {
            m_state.showVectors = v;
            markStateDirty();
            emit viewChanged(ChangeFlag::Display);
        }
    }
    STATE_PROP(getVectorScaleByMagnitude, setVectorScaleByMagnitude, bool, m_state.vectorScaleByMagnitude, Vectors)
    int getVectorMagTransform() const { return m_state.vectorMagTransform; }
    void setVectorMagTransform(int v) { int t = (v < 0) ? 0 : (v > 2 ? 2 : v); if (m_state.vectorMagTransform != t) { m_state.vectorMagTransform = t; markStateDirty(); emit viewChanged(ChangeFlag::Colormap); } }
    STATE_PROP(getVectorScale, setVectorScale, float, m_state.vectorScale, Vectors)
    int getVectorStride() const { return m_state.vectorStride; }
    void setVectorStride(int v) { int s = v < 1 ? 1 : v; if (m_state.vectorStride != s) { m_state.vectorStride = s; m_renderer.markVectorGlyphDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Vectors); } }
    int getVectorPlacement() const { return m_state.vectorPlacement; }
    void setVectorPlacement(int v) { int p = (v < 0) ? 0 : (v > 1 ? 1 : v); if (m_state.vectorPlacement != p) { m_state.vectorPlacement = p; m_renderer.markVectorGlyphDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Vectors); } }
    QStringList getVectorPlacementOptions() const { return {"Vertex", "Cell Center"}; }
    bool getShowLic() const { return m_state.showLic; }
    void setShowLic(bool v) {
        int desired = v ? 2 : 0;
        if (v && m_state.vectorVisMode == 1) desired = 2;
        if (!v && m_state.vectorVisMode == 2) desired = 0;
        if (m_state.vectorVisMode != desired) {
            setVectorVisMode(desired);
        } else if (m_state.showLic != v) {
            m_state.showLic = v;
            markStateDirty();
            emit viewChanged(ChangeFlag::Display);
        }
    }
    int getLicSteps() const { return m_state.licSteps; }
    void setLicSteps(int v) {
        int s = std::clamp(v, LicLimits::StepsMin, LicLimits::StepsMax);
        if (m_state.licSteps != s) { m_state.licSteps = s; markStateDirty(); emit viewChanged(ChangeFlag::Display); }
    }
    double getLicStepSize() const { return m_state.licStepSize; }
    void setLicStepSize(double v) {
        float f = std::clamp(static_cast<float>(v), LicLimits::StepSizeMin, LicLimits::StepSizeMax);
        if (m_state.licStepSize != f) { m_state.licStepSize = f; markStateDirty(); emit viewChanged(ChangeFlag::Display); }
    }
    double getLicNoiseFreq() const { return m_state.licNoiseFreq; }
    void setLicNoiseFreq(double v) {
        float f = std::clamp(static_cast<float>(v), LicLimits::NoiseFreqMin, LicLimits::NoiseFreqMax);
        if (m_state.licNoiseFreq != f) { m_state.licNoiseFreq = f; markStateDirty(); emit viewChanged(ChangeFlag::Display); }
    }
    int getLicNoiseGrain() const { return m_state.licNoiseGrain; }
    void setLicNoiseGrain(int v) {
        int g = (v <= 64) ? 64 : (v <= 128) ? 128 : (v <= 256) ? 256 : 512;
        if (m_state.licNoiseGrain != g) { m_state.licNoiseGrain = g; markStateDirty(); emit viewChanged(ChangeFlag::Display); }
    }
    // Deprecated: fixed to Repeat (GL_REPEAT). Legacy setter coerces any persisted value to 1.
    // Texture key no longer includes boundary mode; VectorTextureCache ignores the argument.
    int getLicBoundaryMode() const { return 1; }
    void setLicBoundaryMode(int) {
        if (m_state.licBoundaryMode != 1) { m_state.licBoundaryMode = 1; markStateDirty(); emit viewChanged(ChangeFlag::Display); }
    }
    QStringList getLicBoundaryModeOptions() const { return {"Clamp (Repeat)"}; }
    bool getLicEnhanced() const { return m_state.licEnhanced; }
    void setLicEnhanced(bool v) {
        if (m_state.licEnhanced != v) { m_state.licEnhanced = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); }
    }
    QColor getVectorColorQml() const { return QColor::fromRgbF(m_state.vectorColor[0], m_state.vectorColor[1], m_state.vectorColor[2]); }
    void setVectorColorQml(const QColor& c) { m_state.vectorColor[0] = c.redF(); m_state.vectorColor[1] = c.greenF(); m_state.vectorColor[2] = c.blueF(); markStateDirty(); emit viewChanged(ChangeFlag::Vectors); }
    bool getShowStreamlines() const { return m_state.showStreamlines; }
    void setShowStreamlines(bool v) { if (m_state.showStreamlines != v) { m_state.showStreamlines = v; if (v) { m_renderer.markStreamlineDirty(); m_renderer.markParticleCountDirty(); } markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    int getStreamlineSeedCount() const { return m_state.streamlineSeedCount; }
    void setStreamlineSeedCount(int v) { if (m_state.streamlineSeedCount != v) { m_state.streamlineSeedCount = v; m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getStreamlineStepSize() const { return m_state.streamlineStepSize; }
    void setStreamlineStepSize(double v) { if (m_state.streamlineStepSize != v) { m_state.streamlineStepSize = static_cast<float>(v); m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    int getStreamlineMaxSteps() const { return m_state.streamlineMaxSteps; }
    void setStreamlineMaxSteps(int v) { if (m_state.streamlineMaxSteps != v) { m_state.streamlineMaxSteps = v; m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    STATE_PROP(getStreamlineUseColormap, setStreamlineUseColormap, bool, m_state.streamlineUseColormap, Display)
    STATE_PROP(getStreamlineColormapChoice, setStreamlineColormapChoice, int, m_state.streamlineColormapChoice, Colormap)
    STATE_PROP(getStreamlineColormapReversed, setStreamlineColormapReversed, bool, m_state.streamlineColormapReversed, Colormap)
    QColor getStreamlineColorQml() const { return QColor::fromRgbF(m_state.streamlineColor[0], m_state.streamlineColor[1], m_state.streamlineColor[2]); }
    void setStreamlineColorQml(const QColor& c) { m_state.streamlineColor[0] = c.redF(); m_state.streamlineColor[1] = c.greenF(); m_state.streamlineColor[2] = c.blueF(); markStateDirty(); emit viewChanged(ChangeFlag::Display); }
    QString getSeedMode() const { return QString::fromStdString(m_state.seedMode); }
    void setSeedMode(const QString& v) { if (m_state.seedMode != v.toStdString()) { m_state.seedMode = v.toStdString(); m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getSeedPlanePos() const { return m_state.seedPlanePos; }
    void setSeedPlanePos(double v) { if (m_state.seedPlanePos != v) { m_state.seedPlanePos = v; m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    int getSeedPlaneCountU() const { return m_state.seedPlaneCountU; }
    void setSeedPlaneCountU(int v) { if (v < 1) v = 1; if (m_state.seedPlaneCountU != v) { m_state.seedPlaneCountU = v; m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    int getSeedPlaneCountV() const { return m_state.seedPlaneCountV; }
    void setSeedPlaneCountV(int v) { if (v < 1) v = 1; if (m_state.seedPlaneCountV != v) { m_state.seedPlaneCountV = v; m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getSeedJitter() const { return m_state.seedJitter; }
    void setSeedJitter(double v) { if (m_state.seedJitter != v) { m_state.seedJitter = v; m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    STATE_PROP(getShowSeeds, setShowSeeds, bool, m_state.showSeeds, Display)
    bool getShowStreamlineArrows() const { return m_state.showStreamlineArrows; }
    void setShowStreamlineArrows(bool v) { if (m_state.showStreamlineArrows != v) { m_state.showStreamlineArrows = v; m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getStreamlineArrowSpacingFrac() const { return m_state.streamlineArrowSpacingFrac; }
    void setStreamlineArrowSpacingFrac(double v) {
        float f = static_cast<float>(v);
        if (f < 0.02f) f = 0.02f;   // clamps stale persisted int values from older builds
        if (f > 0.50f) f = 0.50f;
        if (m_state.streamlineArrowSpacingFrac != f) { m_state.streamlineArrowSpacingFrac = f; m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); }
    }
    double getStreamlineArrowSize() const { return m_state.streamlineArrowSize; }
    void setStreamlineArrowSize(double v) { if (m_state.streamlineArrowSize != v) { m_state.streamlineArrowSize = static_cast<float>(v); m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    STATE_PROP_CAST(getStreamlineOpacity, setStreamlineOpacity, float, m_state.streamlineOpacity, Display)
    double getStreamlineRibbonWidth() const { return m_state.streamlineRibbonWidth; }
    void setStreamlineRibbonWidth(double v) { if (m_state.streamlineRibbonWidth != v) { m_state.streamlineRibbonWidth = static_cast<float>(v); m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getStreamlineTaperFactor() const { return m_state.streamlineTaperFactor; }
    void setStreamlineTaperFactor(double v) { if (m_state.streamlineTaperFactor != v) { m_state.streamlineTaperFactor = static_cast<float>(v); m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    QString getStreamlineDirection() const { return QString::fromStdString(m_state.streamlineDirection); }
    void setStreamlineDirection(const QString& v) { if (m_state.streamlineDirection != v.toStdString()) { m_state.streamlineDirection = v.toStdString(); m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    STATE_PROP_CAST(getStreamlineAmbient, setStreamlineAmbient, float, m_state.streamlineAmbient, Display)
    STATE_PROP_CAST(getStreamlineDiffuse, setStreamlineDiffuse, float, m_state.streamlineDiffuse, Display)
    STATE_PROP_CAST(getStreamlineSpecular, setStreamlineSpecular, float, m_state.streamlineSpecular, Display)
    STATE_PROP(getStreamlineSpecularPower, setStreamlineSpecularPower, int, m_state.streamlineSpecularPower, Display)
    STATE_PROP_CAST(getSeedPointSize, setSeedPointSize, float, m_state.seedPointSize, Display)
    QColor getSeedPointColorQml() const { return QColor::fromRgbF(m_state.seedPointColor[0], m_state.seedPointColor[1], m_state.seedPointColor[2]); }
    void setSeedPointColorQml(const QColor& c) { m_state.seedPointColor[0] = c.redF(); m_state.seedPointColor[1] = c.greenF(); m_state.seedPointColor[2] = c.blueF(); markStateDirty(); emit viewChanged(ChangeFlag::Display); }
    STATE_PROP(getShowParticles, setShowParticles, bool, m_state.showParticles, Display)
    int getParticleCount() const { return m_state.particleCount; }
    void setParticleCount(int v) { if (m_state.particleCount != v) { m_state.particleCount = v; m_renderer.markParticleCountDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    STATE_PROP_CAST(getParticleSpeed, setParticleSpeed, float, m_state.particleSpeed, Display)
    STATE_PROP_CAST(getParticleSize, setParticleSize, float, m_state.particleSize, Display)
    bool getParticleAdditive() const { return m_state.particleAdditive; }
    void setParticleAdditive(bool v) { if (m_state.particleAdditive != v) { m_state.particleAdditive = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    STATE_PROP(getShowVolume, setShowVolume, bool, m_state.showVolume, Display)
    STATE_PROP(getVolumeUseColormap, setVolumeUseColormap, bool, m_state.volumeUseColormap, Colormap)
    STATE_PROP(getVolumeColormapChoice, setVolumeColormapChoice, int, m_state.volumeColormapChoice, Colormap)
    STATE_PROP(getVolumeColormapReversed, setVolumeColormapReversed, bool, m_state.volumeColormapReversed, Colormap)
    STATE_PROP_CAST(getVolumeStepSize, setVolumeStepSize, float, m_state.volumeStepSize, Display)
    STATE_PROP_CAST(getVolumeOpacity, setVolumeOpacity, float, m_state.volumeOpacity, Display)
    bool getSlicePlaneEnabledX() const { return m_state.slicePlaneEnabled[0]; }
    void setSlicePlaneEnabledX(bool v) { if (m_state.slicePlaneEnabled[0] != v) { m_state.slicePlaneEnabled[0] = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getSlicePlaneEnabledY() const { return m_state.slicePlaneEnabled[1]; }
    void setSlicePlaneEnabledY(bool v) { if (m_state.slicePlaneEnabled[1] != v) { m_state.slicePlaneEnabled[1] = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getSlicePlaneEnabledZ() const { return m_state.slicePlaneEnabled[2]; }
    void setSlicePlaneEnabledZ(bool v) { if (m_state.slicePlaneEnabled[2] != v) { m_state.slicePlaneEnabled[2] = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    STATE_PROP_CAST(getSlicePlanePosX, setSlicePlanePosX, float, m_state.slicePlanePos[0], Display)
    STATE_PROP_CAST(getSlicePlanePosY, setSlicePlanePosY, float, m_state.slicePlanePos[1], Display)
    STATE_PROP_CAST(getSlicePlanePosZ, setSlicePlanePosZ, float, m_state.slicePlanePos[2], Display)
    STATE_PROP_CAST(getSlicePlaneOpacityX, setSlicePlaneOpacityX, float, m_state.slicePlaneOpacity[0], Display)
    STATE_PROP_CAST(getSlicePlaneOpacityY, setSlicePlaneOpacityY, float, m_state.slicePlaneOpacity[1], Display)
    STATE_PROP_CAST(getSlicePlaneOpacityZ, setSlicePlaneOpacityZ, float, m_state.slicePlaneOpacity[2], Display)
    bool getSlicePlaneShowColorbarX() const { return m_state.slicePlaneShowColorbar[0]; }
    void setSlicePlaneShowColorbarX(bool v) { if (m_state.slicePlaneShowColorbar[0] != v) { m_state.slicePlaneShowColorbar[0] = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getSlicePlaneShowColorbarY() const { return m_state.slicePlaneShowColorbar[1]; }
    void setSlicePlaneShowColorbarY(bool v) { if (m_state.slicePlaneShowColorbar[1] != v) { m_state.slicePlaneShowColorbar[1] = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getSlicePlaneShowColorbarZ() const { return m_state.slicePlaneShowColorbar[2]; }
    void setSlicePlaneShowColorbarZ(bool v) { if (m_state.slicePlaneShowColorbar[2] != v) { m_state.slicePlaneShowColorbar[2] = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    void setSlicePlaneField(int axis, const QString& fieldName);
    QString getSlicePlaneField(int axis) const { return QString::fromStdString(m_state.sliceScalarName[axis]); }
    STATE_PROP(getVolumeSliceUseColormap, setVolumeSliceUseColormap, bool, m_state.volumeSliceUseColormap, Colormap)
    STATE_PROP(getVolumeSliceColormapChoice, setVolumeSliceColormapChoice, int, m_state.volumeSliceColormapChoice, Colormap)
    STATE_PROP(getVolumeSliceColormapReversed, setVolumeSliceColormapReversed, bool, m_state.volumeSliceColormapReversed, Colormap)

     // ---- isosurface (marching cubes) ----
     // Contours the active scalar field at `isovalue` (absolute, in data units).
     // The extracted surface is re-colored by the colormap LUT and lit by the
     // PBR model via the shared surface pass -- no per-iso shading state.
     // The lifecycle (async extract, debounce, stale-guard) lives in
     // IsosurfaceController; these are thin QML-binding forwarders.
     bool getShowIsosurface() const { return m_state.showIsosurface; }
     void setShowIsosurface(bool v) { m_isoController.setShowIsosurface(v); }
     double getIsovalue() const { return static_cast<double>(m_state.isovalue); }
     void setIsovalue(double v) {
         m_isoController.setIsovalue(static_cast<float>(v),
                                      m_state.dataScalarMin, m_state.dataScalarMax);
     }
     bool getIsosurfaceAvailable() const { return m_isoController.isAvailable(); }
     Q_INVOKABLE void recomputeIsosurface() { m_isoController.recompute(); }

     // LIC diagnostics: surfaced from Renderer::m_lastLicError after each frame.
     QString lastLicError() const {
         std::string s = m_renderer.getLastLicError();
         return s.empty() ? QString() : QString::fromStdString(s);
     }


    int getVectorColorMode() const { return m_state.vectorColorMode; }
    void setVectorColorMode(int v) { int t = (v < 0) ? 0 : (v > 4 ? 4 : v); if (m_state.vectorColorMode != t) { m_state.vectorColorMode = t; markStateDirty(); emit viewChanged(ChangeFlag::Vectors); } }
    STATE_PROP(getVectorColormapChoice, setVectorColormapChoice, int, m_state.vectorColormapChoice, Vectors)
    QStringList getAvailableVectors() const {
        if (m_meshData.loadedMesh) {
            QStringList l;
            for (auto& n : FieldResolver::availableVectorNames(*m_meshData.loadedMesh))
                l.append(QString::fromStdString(n));
            if (!l.isEmpty()) return l;
        }
        QStringList l;
        for (auto& n : m_meshData.guiMeta.availableVectorNames) l.append(QString::fromStdString(n));
        if (l.isEmpty()) for (auto& n : m_meshData.guiMeta.availableCellVectorNames) l.append(QString::fromStdString(n));
        return l;
    }
    QString getVectorField() const { return QString::fromStdString(m_meshData.guiMeta.vectorName); }
    Q_INVOKABLE void setActiveVectorField(const QString& fieldName);
    QString getStreamlineVectorField() const { return QString::fromStdString(m_state.streamlineVectorField); }
    Q_INVOKABLE void setStreamlineVectorField(const QString& fieldName);
    STATE_PROP(getScreenshotTransparent, setScreenshotTransparent, bool, m_state.screenshotTransparent, Display)
    int getScreenshotResolution() const { return m_screenshotResolution; }
    void setScreenshotResolution(int v);
    int getScreenshotAASamples() const { return m_screenshotAASamples; }
    void setScreenshotAASamples(int v);
    STATE_PROP(getFlatShading, setFlatShading, bool, m_state.flatShading, Display)
    QString getStatusMessage() const { return statusMessage; }
    Q_INVOKABLE QString generateScreenshotFilename() const {
        QString base = m_meshData.fileName.empty() ? "scene" : QString::fromStdString(m_meshData.fileName);
        base.replace(QRegularExpression("[^a-zA-Z0-9_]"), "_");
        QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss");
        return QString("%1_%2.png").arg(base, timestamp);
    }

    Q_INVOKABLE void applyLightingPreset(int preset);
    Q_INVOKABLE void resetLighting();
    static constexpr int PRESET_STUDIO = 0;
    static constexpr int PRESET_CADFLAT = 1;
    static constexpr int PRESET_SOFT = 2;

    STATE_PROP(getShowFps, setShowFps, bool, m_state.showFps, Display)
    QString getFpsText() const { return fpsText; }

    bool hasMeshScalars() const { return m_state.meshHasScalars; }
    bool hasMeshVectors() const { return m_state.meshHasVectors; }
    bool hasMeshCellVectors() const { return m_state.meshHasCellVectors; }
    bool hasVolumeData() const { return m_meshData.loadedMesh && m_meshData.loadedMesh->hasVolumeData(); }
    Q_INVOKABLE QString getActiveScalarNameQml() const { return QString::fromStdString(m_state.activeScalarName); }
    Q_INVOKABLE float getDataScalarMinQml() const { return m_state.dataScalarMin; }
    Q_INVOKABLE float getDataScalarMaxQml() const { return m_state.dataScalarMax; }
    int getColorbarTicks() const { return m_state.colorbarTicks; }
    void setColorbarTicks(int v) { int c = v < 2 ? 2 : v; if (m_state.colorbarTicks != c) { m_state.colorbarTicks = c; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    STATE_PROP(getShowScalarColorbar, setShowScalarColorbar, bool, m_state.showScalarColorbar, Display)
    STATE_PROP(getMeshUseScalarColor, setMeshUseScalarColor, bool, m_state.meshUseScalarColor, Display)
    STATE_PROP(getColorbarFontFamily, setColorbarFontFamily, QString, m_state.colorbarFontFamily, Display)
    STATE_PROP(getColorbarFontBold, setColorbarFontBold, bool, m_state.colorbarFontBold, Display)
    STATE_PROP(getColorbarFontItalic, setColorbarFontItalic, bool, m_state.colorbarFontItalic, Display)
    STATE_PROP(getColorbarFontScale, setColorbarFontScale, float, m_state.colorbarFontScale, Display)
    STATE_PROP(getColorbarTickFontScale, setColorbarTickFontScale, float, m_state.colorbarTickFontScale, Display)
    STATE_PROP(getColorbarLengthScale, setColorbarLengthScale, float, m_state.colorbarLengthScale, Display)
    STATE_PROP(getColorbarThicknessScale, setColorbarThicknessScale, float, m_state.colorbarThicknessScale, Display)
    STATE_PROP(getColorbarPanelEnabled, setColorbarPanelEnabled, bool, m_state.colorbarPanelEnabled, Display)
    STATE_PROP(getColorbarPanelOpacity, setColorbarPanelOpacity, float, m_state.colorbarPanelOpacity, Display)
    STATE_PROP(getColorbarShowAnnotation, setColorbarShowAnnotation, bool, m_state.colorbarShowAnnotation, Display)

    STATE_PROP(getClipEnabled, setClipEnabled, bool, m_state.clipEnabled, Slicing)
    STATE_PROP(getCrinkleClipMode, setCrinkleClipMode, bool, m_state.crinkleClipMode, Slicing)
    STATE_PROP(getClipX, setClipX, float, m_state.clipHeightX, Slicing)
    STATE_PROP(getClipY, setClipY, float, m_state.clipHeightY, Slicing)
    STATE_PROP(getClipZ, setClipZ, float, m_state.clipHeightZ, Slicing)
    STATE_PROP(getClipEnabledX, setClipEnabledX, bool, m_state.clipEnabledX, Slicing)
    STATE_PROP(getClipEnabledY, setClipEnabledY, bool, m_state.clipEnabledY, Slicing)
    STATE_PROP(getClipEnabledZ, setClipEnabledZ, bool, m_state.clipEnabledZ, Slicing)
    STATE_PROP(getInvertX, setInvertX, bool, m_state.invertX, Slicing)
    STATE_PROP(getInvertY, setInvertY, bool, m_state.invertY, Slicing)
    STATE_PROP(getInvertZ, setInvertZ, bool, m_state.invertZ, Slicing)
    float getFilterMin() const { return m_state.filterMin; }
    void setFilterMin(float v);
    float getFilterMax() const { return m_state.filterMax; }
    void setFilterMax(float v);
    bool getFilterEnabled() const { return m_state.filterEnabled; }
    void setFilterEnabled(bool v);

    // Fixed custom colormap range (surface/point scalar coloring). Values
    // outside [lo, hi] clamp to the LUT ends. Setters clamp into the current
    // data range and keep lo <= hi.
    bool getColorRangeOverrideEnabled() const { return m_state.colorRangeOverrideEnabled; }
    void setColorRangeOverrideEnabled(bool v);
    float getColorRangeLo() const { return m_state.colorRangeLo; }
    void setColorRangeLo(float v);
    float getColorRangeHi() const { return m_state.colorRangeHi; }
    void setColorRangeHi(float v);
    // Disable the override and snap lo/hi to a data range — used on mesh load /
    // field switch so a stale range never colors a new field.
    void resetColorRangeOverride();                       // snap to current data range
    void resetColorRangeOverride(float lo, float hi);     // snap to explicit values

    // Per-pass fixed colormap windows (fully independent). Volume/slice clamp
    // into the scalar data range; glyph/streamline magnitude pairs enforce
    // ordering only (the GUI holds no copy of mag ranges — enables seed from
    // the renderer's current scan values).
    bool getVolumeColorRangeOverrideEnabled() const { return m_state.volumeColorRangeOverrideEnabled; }
    void setVolumeColorRangeOverrideEnabled(bool v);
    float getVolumeColorRangeLo() const { return m_state.volumeColorRangeLo; }
    void setVolumeColorRangeLo(float v);
    float getVolumeColorRangeHi() const { return m_state.volumeColorRangeHi; }
    void setVolumeColorRangeHi(float v);
    void resetVolumeColorRangeOverride();
    bool getSliceColorRangeOverrideEnabled() const { return m_state.sliceColorRangeOverrideEnabled; }
    void setSliceColorRangeOverrideEnabled(bool v);
    float getSliceColorRangeLo() const { return m_state.sliceColorRangeLo; }
    void setSliceColorRangeLo(float v);
    float getSliceColorRangeHi() const { return m_state.sliceColorRangeHi; }
    void setSliceColorRangeHi(float v);
    void resetSliceColorRangeOverride();
    bool getGlyphMagRangeOverrideEnabled() const { return m_state.glyphMagRangeOverrideEnabled; }
    void setGlyphMagRangeOverrideEnabled(bool v);
    float getGlyphMagRangeLo() const { return m_state.glyphMagRangeLo; }
    void setGlyphMagRangeLo(float v);
    float getGlyphMagRangeHi() const { return m_state.glyphMagRangeHi; }
    void setGlyphMagRangeHi(float v);
    void resetGlyphMagRangeOverride();
    bool getStreamlineMagRangeOverrideEnabled() const { return m_state.streamlineMagRangeOverrideEnabled; }
    void setStreamlineMagRangeOverrideEnabled(bool v);
    float getStreamlineMagRangeLo() const { return m_state.streamlineMagRangeLo; }
    void setStreamlineMagRangeLo(float v);
    float getStreamlineMagRangeHi() const { return m_state.streamlineMagRangeHi; }
    void setStreamlineMagRangeHi(float v);
    void resetStreamlineMagRangeOverride();

    int getStreamlineColorMode() const { return m_state.streamlineColorMode; }
    void setStreamlineColorMode(int v) { int t = (v < 0) ? 0 : (v > 4 ? 4 : v); if (m_state.streamlineColorMode != t) { m_state.streamlineColorMode = t; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getStreamlineCompRangeOverrideEnabled(int comp) const { return m_state.streamlineCompRangeOverrideEnabled[comp]; }
    void setStreamlineCompRangeOverrideEnabled(int comp, bool v);
    float getStreamlineCompRangeLo(int comp) const { return m_state.streamlineCompRangeLo[comp]; }
    void setStreamlineCompRangeLo(int comp, float v);
    float getStreamlineCompRangeHi(int comp) const { return m_state.streamlineCompRangeHi[comp]; }
    void setStreamlineCompRangeHi(int comp, float v);
    void resetStreamlineCompRangeOverride(int comp);
    bool getStreamlineCompRangeOverrideEnabledX() const { return getStreamlineCompRangeOverrideEnabled(0); }
    bool getStreamlineCompRangeOverrideEnabledY() const { return getStreamlineCompRangeOverrideEnabled(1); }
    bool getStreamlineCompRangeOverrideEnabledZ() const { return getStreamlineCompRangeOverrideEnabled(2); }
    float getStreamlineCompRangeLoX() const { return m_state.streamlineCompRangeLo[0]; }
    void setStreamlineCompRangeLoX(float v) { setStreamlineCompRangeLo(0, v); }
    float getStreamlineCompRangeHiX() const { return m_state.streamlineCompRangeHi[0]; }
    void setStreamlineCompRangeHiX(float v) { setStreamlineCompRangeHi(0, v); }
    float getStreamlineCompRangeLoY() const { return m_state.streamlineCompRangeLo[1]; }
    void setStreamlineCompRangeLoY(float v) { setStreamlineCompRangeLo(1, v); }
    float getStreamlineCompRangeHiY() const { return m_state.streamlineCompRangeHi[1]; }
    void setStreamlineCompRangeHiY(float v) { setStreamlineCompRangeHi(1, v); }
    float getStreamlineCompRangeLoZ() const { return m_state.streamlineCompRangeLo[2]; }
    void setStreamlineCompRangeLoZ(float v) { setStreamlineCompRangeLo(2, v); }
    float getStreamlineCompRangeHiZ() const { return m_state.streamlineCompRangeHi[2]; }
    void setStreamlineCompRangeHiZ(float v) { setStreamlineCompRangeHi(2, v); }

    bool getGlyphCompRangeOverrideEnabled(int comp) const { return m_state.glyphCompRangeOverrideEnabled[comp]; }
    void setGlyphCompRangeOverrideEnabled(int comp, bool v);
    float getGlyphCompRangeLo(int comp) const { return m_state.glyphCompRangeLo[comp]; }
    void setGlyphCompRangeLo(int comp, float v);
    float getGlyphCompRangeHi(int comp) const { return m_state.glyphCompRangeHi[comp]; }
    void setGlyphCompRangeHi(int comp, float v);
    void resetGlyphCompRangeOverride(int comp);
    float getVectorCompMinX() const { return m_state.vectorCompMin[0]; }
    float getVectorCompMaxX() const { return m_state.vectorCompMax[0]; }
    float getVectorCompMinY() const { return m_state.vectorCompMin[1]; }
    float getVectorCompMaxY() const { return m_state.vectorCompMax[1]; }
    float getVectorCompMinZ() const { return m_state.vectorCompMin[2]; }
    float getVectorCompMaxZ() const { return m_state.vectorCompMax[2]; }

    // Animation colormap scaling: true = whole-sequence range, false = per-frame.
    bool getAnimScaleGlobal() const { return m_animRange.global; }
    void setAnimScaleGlobal(bool global);

    double getWorldMinX() const { return m_state.worldMinX; }
    double getWorldMaxX() const { return m_state.worldMaxX; }
    double getWorldMinY() const { return m_state.worldMinY; }
    double getWorldMaxY() const { return m_state.worldMaxY; }
    double getWorldMinZ() const { return m_state.worldMinZ; }
    double getWorldMaxZ() const { return m_state.worldMaxZ; }

  private:
    void setStatus(const QString& msg);
    void recomputeScalarRange();

    // Marks the render-state snapshot stale; the next publishRenderState()
    // re-assembles it. Called from every GUI-state mutation (see emit sites).
    void markStateDirty() { m_stateDirty = true; }

    // ---- animated camera transitions (fly-to-face) ----
    // Snap paths (triad clicks, quick-bar ortho buttons, shortcuts) hand the
    // target pose here instead of assigning it; a 16 ms timer interpolates the
    // camera over ~400 ms. Any manual camera input cancels immediately.
    void beginCameraTransition(const CameraPose& to);
    void cancelCameraTransition();
    bool isCameraTransitioning() const { return m_camAnimating; }
    void tickCameraTransition();

    // One delayed repaint after camera motion stops (~postMotionRedrawMs).
    // LOD recovery needs a frame rendered after the debounce window so
    // LodScheduler can drop the moving flag and present the full-res swap;
    // without it the last decimated frame stays on screen indefinitely.
    void schedulePostMotionRepaint();

    // Computes the pose for an ortho preset (0..5 = +X,-X,+Y,-Y,+Z,-Z) against
    // the current camera and starts a transition toward it.
    void snapToPresetAnimated(int preset);

    // Fit-all isometric target: writes distance/maxDistance guards into the
    // camera immediately and returns the iso corner pose for interpolation.
    CameraPose computeFitAllIsoPose();

    // Shared body of every *ColorRangeOverride reset: disable + snap to
    // [snapLo, snapHi], emitting only when something actually changed.
    void resetColorRangeOverrideImpl(bool& enabled, float& lo, float& hi,
                                     float snapLo, float snapHi);

    // ---- table-driven QSettings persistence ----
    // One row per scalar setting; saveStateToSettings() and
    // restoreStateFromSettings() share the table. Camera / color-list /
    // clamped members are persisted inline (they have bespoke formats).
    struct StateEntry {
        const char* key;
        std::function<QVariant(const RenderSettings&)> get;
        std::function<void(RenderSettings&, const QVariant&)> set;
    };
    static const std::vector<StateEntry>& persistenceTable();

private:


    // ---- render-state double buffer ----
    // m_state is the single source of truth for all render visual/camera state.
    // publishRenderState() hands a const reference to the Renderer, which
    // deep-copies it. GUI-thread mutations write directly to m_state.*.
    RenderRenderState m_state;
    bool m_stateDirty = true;
    StateStore m_store; // deep seam — snapshot + dirty behind one interface

    // ---- camera transition state (GUI thread only) ----
    QTimer* m_camAnimTimer = nullptr;
    QElapsedTimer m_camAnimClock;
    CameraPose m_camAnimFrom{};
    CameraPose m_camAnimTo{};
    bool m_camAnimating = false;

    // ---- backend (render thread) ----
    Renderer m_renderer;

    // ---- GUI-side view / state ----
    float sidebarWidth = 0.0f;
    int msaaSamples = 0;         // ponytail: FBO MSAA (0=off, 2, 4); 0 default for iGPU
    int m_screenshotResolution = 0; // 0=Current Viewport, 1=HD, 2=FHD, 3=2K, 4=4K
    int m_screenshotAASamples = 0;  // 0=Off, 2=2x, 4=4x

    QString statusMessage;
    QString fpsText = "FPS: --";

    bool quickBarCollapsed = false;
    QStringList recentFiles;

    // Mesh-derived data (separate from display settings so the
    // QML facade stays focused on user preferences).
    MeshData m_meshData;

     // Async parse watcher (GUI thread). Parsing runs off the GUI thread via
    // QtConcurrent::run; the finished result is delivered here and handed to the
    // render thread as a shared_ptr (no copy). Parented to this object so it is
    // destroyed on the GUI thread.
    QFutureWatcher<MeshLoadResult> m_meshWatcher;
    std::string m_loadingPath; // normalized path for filename/recent-files in the continuation
    // Monotonic generation counter: each loadMesh() increments it and stamps the
    // async task. onMeshParsed() ignores results whose token != current, so a
    // cancelled/superseded parse never misreports a load error or clobbers state.
    uint64_t m_loadToken = 0;
    // Generation stamp of the in-flight async parse. The worker writes its token
    // on completion; onMeshParsed() compares it to m_loadToken to drop stale
    // (cancelled/superseded) results.
    std::shared_ptr<std::atomic<uint64_t>> m_taskToken;

    // ---- isosurface lifecycle ----
    // Owns the debounced async extraction, stale-task guard, and the
    // zero-copy shared_ptr handoff to the Renderer. RenderSettings delegates
    // the 3 isosurface Q_PROPERTYs to it via thin forwarders.
    IsosurfaceController m_isoController;

    // ---- PVD animation ----
    AnimationController m_animController;
    AnimationExporter* m_animationExporter = nullptr;
    // True while frames are flowing from the CURRENT sequence; reset by
    // loadMesh()/clearMeshes() so a new sequence re-initializes scalar range
    // and isosurface bounds on its first frame.
    bool m_animSequenceActive = false;
    // Colormap range across frames: whole-sequence union (default) or
    // per-frame rescale. Rules live in FieldResolver::AnimRangeState.
    FieldResolver::AnimRangeState m_animRange;
};


