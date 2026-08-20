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
#include "core/isosurface.h"
#include "render/settings/isosurface_controller.h"

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

    STATE_PROP(isGizmoVisible, setGizmoVisible, bool, m_state.showGizmo, Display)
    STATE_PROP(getAutoRotate, setAutoRotate, bool, m_state.autoRotate, Display)
    STATE_PROP(getShowPoints, setShowPoints, bool, m_state.showPoints, Display)
    STATE_PROP(getPointSize, setPointSize, float, m_state.pointSize, Display)
    STATE_PROP(getLineWidth, setLineWidth, float, m_state.lineWidth, Display)
    STATE_PROP(getPointUseScalar, setPointUseScalar, bool, m_state.pointUseScalar, Display)
    STATE_PROP(getPointOpacity, setPointOpacity, float, m_state.pointOpacity, Display)
    STATE_PROP(getSurfaceOpacity, setSurfaceOpacity, float, m_state.surfaceOpacity, Display)
    STATE_PROP(getCullMode, setCullMode, int, m_state.cullMode, Display)
    STATE_PROP(getShowBounds, setShowBounds, bool, m_state.showBounds, Display)
    STATE_PROP(getShowQualityOverlay, setShowQualityOverlay, bool, m_state.showQualityOverlay, Display)
    STATE_PROP(getOrthographic, setOrthographic, bool, m_state.orthographic, Display)
    QColor getMeshColorQml() const { return QColor::fromRgbF(m_state.meshColor[0], m_state.meshColor[1], m_state.meshColor[2]); }
    void setMeshColorQml(const QColor& c) { m_state.meshColor[0] = c.redF(); m_state.meshColor[1] = c.greenF(); m_state.meshColor[2] = c.blueF(); markStateDirty(); emit viewChanged(ChangeFlag::Display); }
    QColor getSurfaceColorQml() const { return QColor::fromRgbF(m_state.surfaceColor[0], m_state.surfaceColor[1], m_state.surfaceColor[2]); }
    void setSurfaceColorQml(const QColor& c) { m_state.surfaceColor[0] = c.redF(); m_state.surfaceColor[1] = c.greenF(); m_state.surfaceColor[2] = c.blueF(); markStateDirty(); emit viewChanged(ChangeFlag::Display); }

    STATE_PROP(getShowVectors, setShowVectors, bool, m_state.showVectors, Vectors)
    STATE_PROP(getVectorScaleByMagnitude, setVectorScaleByMagnitude, bool, m_state.vectorScaleByMagnitude, Vectors)
    int getVectorMagTransform() const { return m_state.vectorMagTransform; }
    void setVectorMagTransform(int v) { int t = (v < 0) ? 0 : (v > 2 ? 2 : v); if (m_state.vectorMagTransform != t) { m_state.vectorMagTransform = t; markStateDirty(); emit viewChanged(ChangeFlag::Colormap); } }
    STATE_PROP(getVectorScale, setVectorScale, float, m_state.vectorScale, Vectors)
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
    int getStreamlineArrowSpacing() const { return m_state.streamlineArrowSpacing; }
    void setStreamlineArrowSpacing(int v) { if (m_state.streamlineArrowSpacing != v) { m_state.streamlineArrowSpacing = v; m_renderer.markStreamlineDirty(); markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
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
    STATE_PROP(getShowVolume, setShowVolume, bool, m_state.showVolume, Display)
    STATE_PROP(getVolumeUseColormap, setVolumeUseColormap, bool, m_state.volumeUseColormap, Colormap)
    STATE_PROP(getVolumeColormapChoice, setVolumeColormapChoice, int, m_state.volumeColormapChoice, Colormap)
    STATE_PROP(getVolumeColormapReversed, setVolumeColormapReversed, bool, m_state.volumeColormapReversed, Colormap)
    STATE_PROP_CAST(getVolumeStepSize, setVolumeStepSize, float, m_state.volumeStepSize, Display)
    STATE_PROP_CAST(getVolumeOpacity, setVolumeOpacity, float, m_state.volumeOpacity, Display)
    STATE_PROP(getShowVolumeSlice, setShowVolumeSlice, bool, m_state.showVolumeSlice, Display)
    int getVolumeSliceAxis() const { return m_state.volumeSliceAxis; }
    void setVolumeSliceAxis(int a) { int ax = std::clamp(a, 0, 2); if (m_state.volumeSliceAxis != ax) { m_state.volumeSliceAxis = ax; markStateDirty(); emit viewChanged(ChangeFlag::Display); } }
    STATE_PROP_CAST(getVolumeSlicePos, setVolumeSlicePos, float, m_state.volumeSlicePos, Display)
    STATE_PROP_CAST(getVolumeSliceOpacity, setVolumeSliceOpacity, float, m_state.volumeSliceOpacity, Display)
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


    STATE_PROP(getVectorUseColormap, setVectorUseColormap, bool, m_state.vectorUseColormap, Vectors)
    STATE_PROP(getVectorColormapChoice, setVectorColormapChoice, int, m_state.vectorColormapChoice, Vectors)
    QStringList getAvailableVectors() const {
        QStringList l;
        for (const auto& n : m_meshData.guiMeta.availableVectorNames)
            l.append(QString::fromStdString(n));
        if (l.isEmpty()) {
            for (const auto& n : m_meshData.guiMeta.availableCellVectorNames)
                l.append(QString::fromStdString(n));
        }
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

    STATE_PROP(getClipEnabled, setClipEnabled, bool, m_state.clipEnabled, Slicing)
    STATE_PROP(getSliceX, setSliceX, float, m_state.sliceHeightX, Slicing)
    STATE_PROP(getSliceY, setSliceY, float, m_state.sliceHeightY, Slicing)
    STATE_PROP(getSliceZ, setSliceZ, float, m_state.sliceHeightZ, Slicing)
    STATE_PROP(getSliceEnabledX, setSliceEnabledX, bool, m_state.sliceEnabledX, Slicing)
    STATE_PROP(getSliceEnabledY, setSliceEnabledY, bool, m_state.sliceEnabledY, Slicing)
    STATE_PROP(getSliceEnabledZ, setSliceEnabledZ, bool, m_state.sliceEnabledZ, Slicing)
    STATE_PROP(getInvertX, setInvertX, bool, m_state.invertX, Slicing)
    STATE_PROP(getInvertY, setInvertY, bool, m_state.invertY, Slicing)
    STATE_PROP(getInvertZ, setInvertZ, bool, m_state.invertZ, Slicing)
    STATE_PROP(getFilterMin, setFilterMin, float, m_state.filterMin, Display)
    STATE_PROP(getFilterMax, setFilterMax, float, m_state.filterMax, Display)

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
};


