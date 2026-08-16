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

#include "render/renderer.h"
#include "core/mesh_loader.h"
#include "core/mesh_quality.h"
#include "core/Camera.h"
#include "core/isosurface.h"

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
class RenderSettings : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool isWireframe READ isWireframe WRITE setWireframe NOTIFY viewChanged)
    Q_PROPERTY(bool useLod READ getUseLod WRITE setUseLod NOTIFY viewChanged)
    Q_PROPERTY(int msaaSamples READ getMsaaSamples WRITE setMsaaSamples NOTIFY viewChanged)
    Q_PROPERTY(bool isSurfaceVisible READ isSurfaceVisible WRITE toggleSurface NOTIFY viewChanged)
    Q_PROPERTY(bool isGridVisible READ isGridVisible WRITE toggleGrid NOTIFY viewChanged)
    Q_PROPERTY(int gridAxis READ getGridAxis WRITE setGridAxis NOTIFY viewChanged)
    Q_PROPERTY(bool gridShadows READ getGridShadows WRITE setGridShadows NOTIFY viewChanged)
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
    Q_PROPERTY(bool vectorUseColormap READ getVectorUseColormap WRITE setVectorUseColormap NOTIFY viewChanged)
    Q_PROPERTY(int vectorColormapChoice READ getVectorColormapChoice WRITE setVectorColormapChoice NOTIFY viewChanged)
    Q_PROPERTY(bool vectorColormapReversed READ getVectorColormapReversed WRITE setVectorColormapReversed NOTIFY viewChanged)
    Q_PROPERTY(int vectorPlacement READ getVectorPlacement WRITE setVectorPlacement NOTIFY viewChanged)
    Q_PROPERTY(QStringList vectorPlacementOptions READ getVectorPlacementOptions CONSTANT)
    Q_PROPERTY(QStringList recentFiles READ getRecentFiles NOTIFY meshLoadStateChanged)
    Q_PROPERTY(QString activeScalarName READ getActiveScalarNameQml NOTIFY meshDataUpdated)

    Q_PROPERTY(int colormapChoice READ getColormapChoice WRITE setColormapChoice NOTIFY viewChanged)
    Q_PROPERTY(bool colormapReversed READ getColormapReversed WRITE setColormapReversed NOTIFY viewChanged)
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
    Q_PROPERTY(float sliceHeightX READ getSliceX WRITE setSliceX NOTIFY viewChanged)
    Q_PROPERTY(float sliceHeightY READ getSliceY WRITE setSliceY NOTIFY viewChanged)
    Q_PROPERTY(float sliceHeightZ READ getSliceZ WRITE setSliceZ NOTIFY viewChanged)
    Q_PROPERTY(bool sliceEnabledX READ getSliceEnabledX WRITE setSliceEnabledX NOTIFY viewChanged)
    Q_PROPERTY(bool sliceEnabledY READ getSliceEnabledY WRITE setSliceEnabledY NOTIFY viewChanged)
    Q_PROPERTY(bool sliceEnabledZ READ getSliceEnabledZ WRITE setSliceEnabledZ NOTIFY viewChanged)
    Q_PROPERTY(bool invertX READ getInvertX WRITE setInvertX NOTIFY viewChanged)
    Q_PROPERTY(bool invertY READ getInvertY WRITE setInvertY NOTIFY viewChanged)
    Q_PROPERTY(bool invertZ READ getInvertZ WRITE setInvertZ NOTIFY viewChanged)
    Q_PROPERTY(float filterMin READ getFilterMin WRITE setFilterMin NOTIFY viewChanged)
    Q_PROPERTY(float filterMax READ getFilterMax WRITE setFilterMax NOTIFY viewChanged)

    Q_PROPERTY(bool showStreamlines READ getShowStreamlines WRITE setShowStreamlines NOTIFY viewChanged)
    Q_PROPERTY(QString streamlineVectorField READ getStreamlineVectorField WRITE setStreamlineVectorField NOTIFY meshDataUpdated)
    Q_PROPERTY(int streamlineSeedCount READ getStreamlineSeedCount WRITE setStreamlineSeedCount NOTIFY viewChanged)
    Q_PROPERTY(double streamlineStepSize READ getStreamlineStepSize WRITE setStreamlineStepSize NOTIFY viewChanged)
    Q_PROPERTY(int streamlineMaxSteps READ getStreamlineMaxSteps WRITE setStreamlineMaxSteps NOTIFY viewChanged)
    Q_PROPERTY(bool streamlineUseColormap READ getStreamlineUseColormap WRITE setStreamlineUseColormap NOTIFY viewChanged)
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
    Q_PROPERTY(int streamlineArrowSpacing READ getStreamlineArrowSpacing WRITE setStreamlineArrowSpacing NOTIFY viewChanged)
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
    Q_PROPERTY(bool showVolume READ getShowVolume WRITE setShowVolume NOTIFY viewChanged)
    Q_PROPERTY(bool volumeUseColormap READ getVolumeUseColormap WRITE setVolumeUseColormap NOTIFY viewChanged)
    Q_PROPERTY(int volumeColormapChoice READ getVolumeColormapChoice WRITE setVolumeColormapChoice NOTIFY viewChanged)
    Q_PROPERTY(bool volumeColormapReversed READ getVolumeColormapReversed WRITE setVolumeColormapReversed NOTIFY viewChanged)
    Q_PROPERTY(double volumeStepSize READ getVolumeStepSize WRITE setVolumeStepSize NOTIFY viewChanged)
    Q_PROPERTY(double volumeOpacity READ getVolumeOpacity WRITE setVolumeOpacity NOTIFY viewChanged)
    Q_PROPERTY(bool showVolumeSlice READ getShowVolumeSlice WRITE setShowVolumeSlice NOTIFY viewChanged)
    Q_PROPERTY(int volumeSliceAxis READ getVolumeSliceAxis WRITE setVolumeSliceAxis NOTIFY viewChanged)
    Q_PROPERTY(double volumeSlicePos READ getVolumeSlicePos WRITE setVolumeSlicePos NOTIFY viewChanged)
    Q_PROPERTY(double volumeSliceOpacity READ getVolumeSliceOpacity WRITE setVolumeSliceOpacity NOTIFY viewChanged)
    Q_PROPERTY(bool volumeSliceUseColormap READ getVolumeSliceUseColormap WRITE setVolumeSliceUseColormap NOTIFY viewChanged)
    Q_PROPERTY(int volumeSliceColormapChoice READ getVolumeSliceColormapChoice WRITE setVolumeSliceColormapChoice NOTIFY viewChanged)
    Q_PROPERTY(bool volumeSliceColormapReversed READ getVolumeSliceColormapReversed WRITE setVolumeSliceColormapReversed NOTIFY viewChanged)

    Q_PROPERTY(bool isGizmoVisible READ isGizmoVisible WRITE setGizmoVisible NOTIFY viewChanged)
    Q_PROPERTY(bool showPoints READ getShowPoints WRITE setShowPoints NOTIFY viewChanged)
    Q_PROPERTY(float pointSize READ getPointSize WRITE setPointSize NOTIFY viewChanged)
    Q_PROPERTY(float lineWidth READ getLineWidth WRITE setLineWidth NOTIFY viewChanged)
    Q_PROPERTY(bool pointUseScalar READ getPointUseScalar WRITE setPointUseScalar NOTIFY viewChanged)
    Q_PROPERTY(float pointOpacity READ getPointOpacity WRITE setPointOpacity NOTIFY viewChanged)
    Q_PROPERTY(float surfaceOpacity READ getSurfaceOpacity WRITE setSurfaceOpacity NOTIFY viewChanged)
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
    bool isGridVisible() const { return m_state.showGrid; }
    void toggleGrid(bool visible);
    int getGridAxis() const { return m_state.gridAxis; }
    void setGridAxis(int axis);
    bool getGridShadows() const { return m_state.gridShadows; }
    void setGridShadows(bool enabled);

    bool getHasMeshLoaded() const { return m_state.hasMeshLoaded; }
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

    int getColormapChoice() const { return m_state.colormapChoice; }
    void setColormapChoice(int choice);
    bool getColormapReversed() const { return m_state.colormapReversed; }
    void setColormapReversed(bool reversed);
    bool getVectorColormapReversed() const { return m_state.vectorColormapReversed; }
    void setVectorColormapReversed(bool reversed);

    bool getQuickBarCollapsed() const { return quickBarCollapsed; }
    void setQuickBarCollapsed(bool collapsed);

    float getLightKeyAzimuth() const { return m_state.lighting.lightKeyAzimuth; }
    void setLightKeyAzimuth(float v) { m_state.lighting.lightKeyAzimuth = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    float getLightKeyElevation() const { return m_state.lighting.lightKeyElevation; }
    void setLightKeyElevation(float v) { m_state.lighting.lightKeyElevation = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    float getLightFillAzimuth() const { return m_state.lighting.lightFillAzimuth; }
    void setLightFillAzimuth(float v) { m_state.lighting.lightFillAzimuth = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    float getLightFillElevation() const { return m_state.lighting.lightFillElevation; }
    void setLightFillElevation(float v) { m_state.lighting.lightFillElevation = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    float getLightBackAzimuth() const { return m_state.lighting.lightBackAzimuth; }
    void setLightBackAzimuth(float v) { m_state.lighting.lightBackAzimuth = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    float getLightBackElevation() const { return m_state.lighting.lightBackElevation; }
    void setLightBackElevation(float v) { m_state.lighting.lightBackElevation = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    float getLightHeadAzimuth() const { return m_state.lighting.lightHeadAzimuth; }
    void setLightHeadAzimuth(float v) { m_state.lighting.lightHeadAzimuth = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    float getLightHeadElevation() const { return m_state.lighting.lightHeadElevation; }
    void setLightHeadElevation(float v) { m_state.lighting.lightHeadElevation = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    float getMatAmbient() const { return m_state.lighting.matAmbient; }
    void setMatAmbient(float v) { m_state.lighting.matAmbient = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    float getMatDiffuse() const { return m_state.lighting.matDiffuse; }
    void setMatDiffuse(float v) { m_state.lighting.matDiffuse = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    float getMatSpecular() const { return m_state.lighting.matSpecular; }
    void setMatSpecular(float v) { m_state.lighting.matSpecular = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    float getMatRoughness() const { return m_state.lighting.matRoughness; }
    void setMatRoughness(float v) { if (m_state.lighting.matRoughness != v) { m_state.lighting.matRoughness = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); } }
    float getMatMetallic() const { return m_state.lighting.matMetallic; }
    void setMatMetallic(float v) { if (m_state.lighting.matMetallic != v) { m_state.lighting.matMetallic = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); } }
    float getLightKeyIntensity() const { return m_state.lighting.lightKeyIntensity; }
    void setLightKeyIntensity(float v) { m_state.lighting.lightKeyIntensity = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    float getLightKF() const { return m_state.lighting.lightKF; }
    void setLightKF(float v) { m_state.lighting.lightKF = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    float getLightKB() const { return m_state.lighting.lightKB; }
    void setLightKB(float v) { m_state.lighting.lightKB = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    float getLightKH() const { return m_state.lighting.lightKH; }
    void setLightKH(float v) { m_state.lighting.lightKH = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    bool getLightKitEnabled() const { return m_state.lighting.lightKitEnabled; }
    void setLightKitEnabled(bool v) { m_state.lighting.lightKitEnabled = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    bool getShowLightMarkers() const { return m_state.lighting.showLightMarkers; }
    void setShowLightMarkers(bool v) { m_state.lighting.showLightMarkers = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }
    float getLightWarm() const { return m_state.lighting.lightWarm; }
    void setLightWarm(float v) { m_state.lighting.lightWarm = v; markStateDirty(); emit viewChanged(ChangeFlag::Lighting); }

    QString getCurrentMeshNameQStr() const { return QString::fromStdString(m_meshData.fileName); }

    // ---- UI callable actions (slots invoked by QML) ----
public slots:
    void loadMesh(const QString& filePath);
    void onMeshParsed(); // GUI-thread continuation after async parse
    void openRecent(const QString& filePath);
    void clearMeshes();
    void resetCamera();
    void snapToOrthoView(int axis);
    Q_INVOKABLE void requestScreenshot(const QString& path);
    void snapToAxisView(int axis, bool flip);
    void toggleSurface(bool visible);

    QStringList getAvailableScalars() const;
    QStringList getRecentFiles() const { return recentFiles; }
    void loadRecentFromSettings();
    void saveRecentToSettings() const;
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
    // next synchronize() copies it into the render-thread snapshot.
    Q_INVOKABLE void azimuth(double angle) { m_state.camera.azimuth(angle); m_renderer.markCameraMoving(); markStateDirty(); emit viewChanged(ChangeFlag::Camera); }
    Q_INVOKABLE void elevation(double angle) { m_state.camera.elevation(angle); m_renderer.markCameraMoving(); markStateDirty(); emit viewChanged(ChangeFlag::Camera); }
    Q_INVOKABLE void pan(double dx, double dy) { m_state.camera.pan(dx, dy); m_renderer.markCameraMoving(); markStateDirty(); emit viewChanged(ChangeFlag::Camera); }
    Q_INVOKABLE void dolly(double factor) { m_state.camera.dolly(factor); m_renderer.markCameraMoving(); markStateDirty(); emit viewChanged(ChangeFlag::Camera); }

    float* getMeshColor() { return m_state.meshColor; }
    float* getSurfaceColor() { return m_state.surfaceColor; }
    float* getBgColor() { return m_state.bgColor; }

    Q_INVOKABLE void setSidebarWidth(float w) { sidebarWidth = w; }

    bool isGizmoVisible() const { return m_state.showGizmo; }
    void setGizmoVisible(bool v) { if (m_state.showGizmo != v) { m_state.showGizmo = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getAutoRotate() const { return m_state.autoRotate; }
    void setAutoRotate(bool v) { if (m_state.autoRotate != v) { m_state.autoRotate = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getShowPoints() const { return m_state.showPoints; }
    void setShowPoints(bool v) { if (m_state.showPoints != v) { m_state.showPoints = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    float getPointSize() const { return m_state.pointSize; }
    void setPointSize(float v) { if (m_state.pointSize != v) { m_state.pointSize = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    float getLineWidth() const { return m_state.lineWidth; }
    void setLineWidth(float v) { if (m_state.lineWidth != v) { m_state.lineWidth = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getPointUseScalar() const { return m_state.pointUseScalar; }
    void setPointUseScalar(bool v) { if (m_state.pointUseScalar != v) { m_state.pointUseScalar = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    float getPointOpacity() const { return m_state.pointOpacity; }
    void setPointOpacity(float v) { if (m_state.pointOpacity != v) { m_state.pointOpacity = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    float getSurfaceOpacity() const { return m_state.surfaceOpacity; }
    void setSurfaceOpacity(float v) { if (m_state.surfaceOpacity != v) { m_state.surfaceOpacity = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    int getCullMode() const { return m_state.cullMode; }
    void setCullMode(int v) { if (m_state.cullMode != v) { m_state.cullMode = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getShowBounds() const { return m_state.showBounds; }
    void setShowBounds(bool v) { if (m_state.showBounds != v) { m_state.showBounds = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getShowQualityOverlay() const { return m_state.showQualityOverlay; }
    void setShowQualityOverlay(bool v) { if (m_state.showQualityOverlay != v) { m_state.showQualityOverlay = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getOrthographic() const { return m_state.orthographic; }
    void setOrthographic(bool v) { if (m_state.orthographic != v) { m_state.orthographic = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    QColor getMeshColorQml() const { return QColor::fromRgbF(m_state.meshColor[0], m_state.meshColor[1], m_state.meshColor[2]); }
    void setMeshColorQml(const QColor& c) { m_state.meshColor[0] = c.redF(); m_state.meshColor[1] = c.greenF(); m_state.meshColor[2] = c.blueF(); markStateDirty(); emit viewChanged(ChangeFlag::Display); }
    QColor getSurfaceColorQml() const { return QColor::fromRgbF(m_state.surfaceColor[0], m_state.surfaceColor[1], m_state.surfaceColor[2]); }
    void setSurfaceColorQml(const QColor& c) { m_state.surfaceColor[0] = c.redF(); m_state.surfaceColor[1] = c.greenF(); m_state.surfaceColor[2] = c.blueF(); markStateDirty(); emit viewChanged(ChangeFlag::Display); }

    bool getShowVectors() const { return m_state.showVectors; }
    void setShowVectors(bool v) { if (m_state.showVectors != v) { m_state.showVectors = v; markStateDirty(); emit viewChanged(ChangeFlag::Vectors); } }
    bool getVectorScaleByMagnitude() const { return m_state.vectorScaleByMagnitude; }
    void setVectorScaleByMagnitude(bool v) { if (m_state.vectorScaleByMagnitude != v) { m_state.vectorScaleByMagnitude = v; markStateDirty(); emit viewChanged(ChangeFlag::Vectors); } }
    int getVectorMagTransform() const { return m_state.vectorMagTransform; }
    void setVectorMagTransform(int v) { int t = (v < 0) ? 0 : (v > 2 ? 2 : v); if (m_state.vectorMagTransform != t) { m_state.vectorMagTransform = t; m_renderer.markVectorGlyphDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Colormap); } }
    float getVectorScale() const { return m_state.vectorScale; }
    void setVectorScale(float v) { if (m_state.vectorScale != v) { m_state.vectorScale = v; markStateDirty(); emit viewChanged(ChangeFlag::Vectors); } }
    int getVectorStride() const { return m_state.vectorStride; }
    void setVectorStride(int v) { int s = v < 1 ? 1 : v; if (m_state.vectorStride != s) { m_state.vectorStride = s; m_renderer.markVectorGlyphDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Vectors); } }
    int getVectorPlacement() const { return m_state.vectorPlacement; }
    void setVectorPlacement(int v) { int p = (v < 0) ? 0 : (v > 1 ? 1 : v); if (m_state.vectorPlacement != p) { m_state.vectorPlacement = p; m_renderer.markVectorGlyphDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Vectors); } }
    QStringList getVectorPlacementOptions() const { return {"Vertex", "Cell Center"}; }
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
    bool getStreamlineUseColormap() const { return m_state.streamlineUseColormap; }
    void setStreamlineUseColormap(bool v) { if (m_state.streamlineUseColormap != v) { m_state.streamlineUseColormap = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    int getStreamlineColormapChoice() const { return m_state.streamlineColormapChoice; }
    void setStreamlineColormapChoice(int c) { if (m_state.streamlineColormapChoice != c) { m_state.streamlineColormapChoice = c; markStateDirty(); emit viewChanged(ChangeFlag::Colormap); } }
    bool getStreamlineColormapReversed() const { return m_state.streamlineColormapReversed; }
    void setStreamlineColormapReversed(bool v) { if (m_state.streamlineColormapReversed != v) { m_state.streamlineColormapReversed = v; markStateDirty(); emit viewChanged(ChangeFlag::Colormap); } }
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
    bool getShowSeeds() const { return m_state.showSeeds; }
    void setShowSeeds(bool v) { if (m_state.showSeeds != v) { m_state.showSeeds = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getShowStreamlineArrows() const { return m_state.showStreamlineArrows; }
    void setShowStreamlineArrows(bool v) { if (m_state.showStreamlineArrows != v) { m_state.showStreamlineArrows = v; m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    int getStreamlineArrowSpacing() const { return m_state.streamlineArrowSpacing; }
    void setStreamlineArrowSpacing(int v) { if (m_state.streamlineArrowSpacing != v) { m_state.streamlineArrowSpacing = v; m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getStreamlineArrowSize() const { return m_state.streamlineArrowSize; }
    void setStreamlineArrowSize(double v) { if (m_state.streamlineArrowSize != v) { m_state.streamlineArrowSize = static_cast<float>(v); m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getStreamlineOpacity() const { return m_state.streamlineOpacity; }
    void setStreamlineOpacity(double v) { if (m_state.streamlineOpacity != v) { m_state.streamlineOpacity = static_cast<float>(v); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getStreamlineRibbonWidth() const { return m_state.streamlineRibbonWidth; }
    void setStreamlineRibbonWidth(double v) { if (m_state.streamlineRibbonWidth != v) { m_state.streamlineRibbonWidth = static_cast<float>(v); m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getStreamlineTaperFactor() const { return m_state.streamlineTaperFactor; }
    void setStreamlineTaperFactor(double v) { if (m_state.streamlineTaperFactor != v) { m_state.streamlineTaperFactor = static_cast<float>(v); m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    QString getStreamlineDirection() const { return QString::fromStdString(m_state.streamlineDirection); }
    void setStreamlineDirection(const QString& v) { if (m_state.streamlineDirection != v.toStdString()) { m_state.streamlineDirection = v.toStdString(); m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getStreamlineAmbient() const { return m_state.streamlineAmbient; }
    void setStreamlineAmbient(double v) { if (m_state.streamlineAmbient != v) { m_state.streamlineAmbient = static_cast<float>(v); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getStreamlineDiffuse() const { return m_state.streamlineDiffuse; }
    void setStreamlineDiffuse(double v) { if (m_state.streamlineDiffuse != v) { m_state.streamlineDiffuse = static_cast<float>(v); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getStreamlineSpecular() const { return m_state.streamlineSpecular; }
    void setStreamlineSpecular(double v) { if (m_state.streamlineSpecular != v) { m_state.streamlineSpecular = static_cast<float>(v); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    int getStreamlineSpecularPower() const { return m_state.streamlineSpecularPower; }
    void setStreamlineSpecularPower(int v) { if (m_state.streamlineSpecularPower != v) { m_state.streamlineSpecularPower = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getSeedPointSize() const { return m_state.seedPointSize; }
    void setSeedPointSize(double v) { if (m_state.seedPointSize != v) { m_state.seedPointSize = static_cast<float>(v); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    QColor getSeedPointColorQml() const { return QColor::fromRgbF(m_state.seedPointColor[0], m_state.seedPointColor[1], m_state.seedPointColor[2]); }
    void setSeedPointColorQml(const QColor& c) { m_state.seedPointColor[0] = c.redF(); m_state.seedPointColor[1] = c.greenF(); m_state.seedPointColor[2] = c.blueF(); markStateDirty(); emit viewChanged(ChangeFlag::Display); }
    bool getShowParticles() const { return m_state.showParticles; }
    void setShowParticles(bool v) { if (m_state.showParticles != v) { m_state.showParticles = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    int getParticleCount() const { return m_state.particleCount; }
    void setParticleCount(int v) { if (m_state.particleCount != v) { m_state.particleCount = v; m_renderer.markParticleCountDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getParticleSpeed() const { return m_state.particleSpeed; }
    void setParticleSpeed(double v) { if (m_state.particleSpeed != v) { m_state.particleSpeed = static_cast<float>(v); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getParticleSize() const { return m_state.particleSize; }
    void setParticleSize(double v) { if (m_state.particleSize != v) { m_state.particleSize = static_cast<float>(v); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getShowVolume() const { return m_state.showVolume; }
    void setShowVolume(bool v) { if (m_state.showVolume != v) { m_state.showVolume = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getVolumeUseColormap() const { return m_state.volumeUseColormap; }
    void setVolumeUseColormap(bool v) { if (m_state.volumeUseColormap != v) { m_state.volumeUseColormap = v; markStateDirty(); emit viewChanged(ChangeFlag::Colormap); } }
    int getVolumeColormapChoice() const { return m_state.volumeColormapChoice; }
    void setVolumeColormapChoice(int c) { if (m_state.volumeColormapChoice != c) { m_state.volumeColormapChoice = c; markStateDirty(); emit viewChanged(ChangeFlag::Colormap); } }
    bool getVolumeColormapReversed() const { return m_state.volumeColormapReversed; }
    void setVolumeColormapReversed(bool v) { if (m_state.volumeColormapReversed != v) { m_state.volumeColormapReversed = v; markStateDirty(); emit viewChanged(ChangeFlag::Colormap); } }
    double getVolumeStepSize() const { return m_state.volumeStepSize; }
    void setVolumeStepSize(double v) { if (m_state.volumeStepSize != v) { m_state.volumeStepSize = static_cast<float>(v); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getVolumeOpacity() const { return m_state.volumeOpacity; }
    void setVolumeOpacity(double v) { if (m_state.volumeOpacity != v) { m_state.volumeOpacity = static_cast<float>(v); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getShowVolumeSlice() const { return m_state.showVolumeSlice; }
    void setShowVolumeSlice(bool v) { if (m_state.showVolumeSlice != v) { m_state.showVolumeSlice = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    int getVolumeSliceAxis() const { return m_state.volumeSliceAxis; }
    void setVolumeSliceAxis(int a) { int ax = std::clamp(a, 0, 2); if (m_state.volumeSliceAxis != ax) { m_state.volumeSliceAxis = ax; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getVolumeSlicePos() const { return m_state.volumeSlicePos; }
    void setVolumeSlicePos(double v) { if (m_state.volumeSlicePos != v) { m_state.volumeSlicePos = static_cast<float>(v); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    double getVolumeSliceOpacity() const { return m_state.volumeSliceOpacity; }
    void setVolumeSliceOpacity(double v) { if (m_state.volumeSliceOpacity != v) { m_state.volumeSliceOpacity = static_cast<float>(v); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getVolumeSliceUseColormap() const { return m_state.volumeSliceUseColormap; }
    void setVolumeSliceUseColormap(bool v) { if (m_state.volumeSliceUseColormap != v) { m_state.volumeSliceUseColormap = v; markStateDirty(); emit viewChanged(ChangeFlag::Colormap); } }
    int getVolumeSliceColormapChoice() const { return m_state.volumeSliceColormapChoice; }
    void setVolumeSliceColormapChoice(int c) { if (m_state.volumeSliceColormapChoice != c) { m_state.volumeSliceColormapChoice = c; markStateDirty(); emit viewChanged(ChangeFlag::Colormap); } }
     bool getVolumeSliceColormapReversed() const { return m_state.volumeSliceColormapReversed; }
     void setVolumeSliceColormapReversed(bool v) { if (m_state.volumeSliceColormapReversed != v) { m_state.volumeSliceColormapReversed = v; markStateDirty(); emit viewChanged(ChangeFlag::Colormap); } }

     // ---- isosurface (marching cubes) ----
     // Contours the active scalar field at `isovalue` (absolute, in data units).
     // The extracted surface is re-colored by the colormap LUT and lit by the
     // PBR model via the shared surface pass -- no per-iso shading state.
     bool getShowIsosurface() const { return m_state.showIsosurface; }
     void setShowIsosurface(bool v);
     double getIsovalue() const { return static_cast<double>(m_state.isovalue); }
     void setIsovalue(double v);
     bool getIsosurfaceAvailable() const {
         return m_meshData.loadedMesh && isosurface::canExtract(*m_meshData.loadedMesh);
     }
     Q_INVOKABLE void recomputeIsosurface();


    bool getVectorUseColormap() const { return m_state.vectorUseColormap; }
    void setVectorUseColormap(bool v) { if (m_state.vectorUseColormap != v) { m_state.vectorUseColormap = v; markStateDirty(); emit viewChanged(ChangeFlag::Vectors); } }
    int getVectorColormapChoice() const { return m_state.vectorColormapChoice; }
    void setVectorColormapChoice(int c) { if (m_state.vectorColormapChoice != c) { m_state.vectorColormapChoice = c; markStateDirty(); emit viewChanged(ChangeFlag::Vectors); } }
    QStringList getAvailableVectors() const { QStringList l; for (const auto& n : m_meshData.guiMeta.availableVectorNames) l.append(QString::fromStdString(n)); return l; }
    QString getVectorField() const { return QString::fromStdString(m_meshData.guiMeta.vectorName); }
    Q_INVOKABLE void setActiveVectorField(const QString& fieldName);
    QString getStreamlineVectorField() const { return QString::fromStdString(m_state.streamlineVectorField); }
    Q_INVOKABLE void setStreamlineVectorField(const QString& fieldName);
    bool getScreenshotTransparent() const { return m_state.screenshotTransparent; }
    void setScreenshotTransparent(bool v) { if (m_state.screenshotTransparent != v) { m_state.screenshotTransparent = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    int getScreenshotResolution() const { return m_screenshotResolution; }
    void setScreenshotResolution(int v);
    int getScreenshotAASamples() const { return m_screenshotAASamples; }
    void setScreenshotAASamples(int v);
    bool getFlatShading() const { return m_state.flatShading; }
    void setFlatShading(bool v) { if (m_state.flatShading != v) { m_state.flatShading = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
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

    bool getShowFps() const { return m_state.showFps; }
    void setShowFps(bool v) { if (m_state.showFps != v) { m_state.showFps = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    QString getFpsText() const { return fpsText; }

    bool hasMeshScalars() const { return m_state.meshHasScalars; }
    bool hasMeshVectors() const { return m_state.meshHasVectors; }
    bool hasMeshCellVectors() const { return m_state.meshHasCellVectors; }
    bool hasVolumeData() const { return m_meshData.loadedMesh && !m_meshData.loadedMesh->scalars.empty()
                                            && m_meshData.loadedMesh->gridDimX > 0; }
    Q_INVOKABLE QString getActiveScalarNameQml() const { return QString::fromStdString(m_state.activeScalarName); }
    Q_INVOKABLE float getDataScalarMinQml() const { return m_state.dataScalarMin; }
    Q_INVOKABLE float getDataScalarMaxQml() const { return m_state.dataScalarMax; }
    int getColorbarTicks() const { return m_state.colorbarTicks; }
    void setColorbarTicks(int v) { int c = v < 2 ? 2 : v; if (m_state.colorbarTicks != c) { m_state.colorbarTicks = c; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getShowScalarColorbar() const { return m_state.showScalarColorbar; }
    void setShowScalarColorbar(bool v) { if (m_state.showScalarColorbar != v) { m_state.showScalarColorbar = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    bool getMeshUseScalarColor() const { return m_state.meshUseScalarColor; }
    void setMeshUseScalarColor(bool v) { if (m_state.meshUseScalarColor != v) { m_state.meshUseScalarColor = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }

    bool getClipEnabled() const { return m_state.clipEnabled; }
    void setClipEnabled(bool v) { if (m_state.clipEnabled != v) { m_state.clipEnabled = v; markStateDirty(); emit viewChanged(ChangeFlag::Slicing); } }
    float getSliceX() const { return m_state.sliceHeightX; }
    void setSliceX(float v) { if (m_state.sliceHeightX != v) { m_state.sliceHeightX = v; markStateDirty(); emit viewChanged(ChangeFlag::Slicing); } }
    float getSliceY() const { return m_state.sliceHeightY; }
    void setSliceY(float v) { if (m_state.sliceHeightY != v) { m_state.sliceHeightY = v; markStateDirty(); emit viewChanged(ChangeFlag::Slicing); } }
    float getSliceZ() const { return m_state.sliceHeightZ; }
    void setSliceZ(float v) { if (m_state.sliceHeightZ != v) { m_state.sliceHeightZ = v; markStateDirty(); emit viewChanged(ChangeFlag::Slicing); } }
    bool getSliceEnabledX() const { return m_state.sliceEnabledX; }
    void setSliceEnabledX(bool v) { if (m_state.sliceEnabledX != v) { m_state.sliceEnabledX = v; markStateDirty(); emit viewChanged(ChangeFlag::Slicing); } }
    bool getSliceEnabledY() const { return m_state.sliceEnabledY; }
    void setSliceEnabledY(bool v) { if (m_state.sliceEnabledY != v) { m_state.sliceEnabledY = v; markStateDirty(); emit viewChanged(ChangeFlag::Slicing); } }
    bool getSliceEnabledZ() const { return m_state.sliceEnabledZ; }
    void setSliceEnabledZ(bool v) { if (m_state.sliceEnabledZ != v) { m_state.sliceEnabledZ = v; markStateDirty(); emit viewChanged(ChangeFlag::Slicing); } }
    bool getInvertX() const { return m_state.invertX; }
    void setInvertX(bool v) { if (m_state.invertX != v) { m_state.invertX = v; markStateDirty(); emit viewChanged(ChangeFlag::Slicing); } }
    bool getInvertY() const { return m_state.invertY; }
    void setInvertY(bool v) { if (m_state.invertY != v) { m_state.invertY = v; markStateDirty(); emit viewChanged(ChangeFlag::Slicing); } }
    bool getInvertZ() const { return m_state.invertZ; }
    void setInvertZ(bool v) { if (m_state.invertZ != v) { m_state.invertZ = v; markStateDirty(); emit viewChanged(ChangeFlag::Slicing); } }
    float getFilterMin() const { return m_state.filterMin; }
    void setFilterMin(float v) { if (m_state.filterMin != v) { m_state.filterMin = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    float getFilterMax() const { return m_state.filterMax; }
    void setFilterMax(float v) { if (m_state.filterMax != v) { m_state.filterMax = v; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }

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

 private slots:
    // GUI-thread continuation for the async isosurface (marching-cubes) task.
    void onIsosurfaceComputed();

 private:


    // ---- render-state double buffer ----
    // m_state is the single source of truth for all render visual/camera state.
    // publishRenderState() hands a const reference to the Renderer, which
    // deep-copies it. GUI-thread mutations write directly to m_state.*.
    RenderRenderState m_state;
    bool m_stateDirty = true;

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

    // ---- isosurface async recompute ----
    // Marching cubes runs off the GUI thread (QtConcurrent); the result is
    // delivered as a shared_ptr to the Renderer via setPendingIsosurface().
    QFutureWatcher<RenderMesh> m_isoSurfaceWatcher;
    // Single-shot debounce timer: slider scrubs reset it so we only recompute
    // after the user pauses (~150 ms), avoiding a task per tick.
    QTimer m_isoDebounceTimer;
    // Stale-result guard: onIsosurfaceComputed() checks m_state.showIsosurface
    // (and the watcher drops superseded futures on setFuture) so a compute that
    // finishes after the user toggled the isosurface off is discarded.
};
