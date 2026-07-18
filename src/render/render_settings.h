#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QColor>
#include <QTimer>
#include <QFutureWatcher>

#include <memory>
#include <atomic>

#include "render/renderer.h"
#include "core/mesh_loader.h"
#include "core/Camera.h"

// ---------------------------------------------------------------------------
// RenderSettings — the GUI-THREAD facade.
//
// This is the ONLY object QML binds to (as the `backendSettings` context
// property). It owns the pure-C++ Renderer backend and exposes every
// UI-exposed setting as a Q_PROPERTY / Q_INVOKABLE. It mutates ONLY its own
// plain-C++ state (camera, scalars, flags) — never the Renderer's members.
//
// The render thread reads nothing here directly. ViewportFboRenderer::
// synchronize() asks this object for buildRenderState(), which produces a
// deep-copied RenderRenderState snapshot that is then handed to the Renderer.
// This is the strict state-copy boundary that isolates the GUI thread from
// the QSG render thread.
// ---------------------------------------------------------------------------
class RenderSettings : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool isWireframe READ isWireframe WRITE setWireframe NOTIFY viewChanged)
    Q_PROPERTY(bool useLod READ getUseLod WRITE setUseLod NOTIFY viewChanged)
    Q_PROPERTY(bool isSurfaceVisible READ isSurfaceVisible WRITE toggleSurface NOTIFY viewChanged)
    Q_PROPERTY(bool isGridVisible READ isGridVisible WRITE toggleGrid NOTIFY viewChanged)
    Q_PROPERTY(bool hasMeshLoaded READ getHasMeshLoaded NOTIFY meshLoadStateChanged)
    Q_PROPERTY(bool meshHasScalars READ hasMeshScalars NOTIFY meshLoadStateChanged)
    Q_PROPERTY(QString currentMeshName READ getCurrentMeshNameQStr NOTIFY meshLoadStateChanged)

    Q_PROPERTY(float lightKeyAzimuth READ getLightKeyAzimuth WRITE setLightKeyAzimuth NOTIFY lightingParametersChanged)
    Q_PROPERTY(float lightKeyElevation READ getLightKeyElevation WRITE setLightKeyElevation NOTIFY lightingParametersChanged)
    Q_PROPERTY(float lightFillAzimuth READ getLightFillAzimuth WRITE setLightFillAzimuth NOTIFY lightingParametersChanged)
    Q_PROPERTY(float lightFillElevation READ getLightFillElevation WRITE setLightFillElevation NOTIFY lightingParametersChanged)
    Q_PROPERTY(float lightBackAzimuth READ getLightBackAzimuth WRITE setLightBackAzimuth NOTIFY lightingParametersChanged)
    Q_PROPERTY(float lightBackElevation READ getLightBackElevation WRITE setLightBackElevation NOTIFY lightingParametersChanged)
    Q_PROPERTY(float lightHeadAzimuth READ getLightHeadAzimuth WRITE setLightHeadAzimuth NOTIFY lightingParametersChanged)
    Q_PROPERTY(float lightHeadElevation READ getLightHeadElevation WRITE setLightHeadElevation NOTIFY lightingParametersChanged)
    Q_PROPERTY(float matAmbient READ getMatAmbient WRITE setMatAmbient NOTIFY lightingParametersChanged)
    Q_PROPERTY(float matDiffuse READ getMatDiffuse WRITE setMatDiffuse NOTIFY lightingParametersChanged)
    Q_PROPERTY(float matSpecular READ getMatSpecular WRITE setMatSpecular NOTIFY lightingParametersChanged)
    Q_PROPERTY(float matShininess READ getMatShininess WRITE setMatShininess NOTIFY lightingParametersChanged)
    Q_PROPERTY(float lightKeyIntensity READ getLightKeyIntensity WRITE setLightKeyIntensity NOTIFY lightingParametersChanged)
    Q_PROPERTY(float lightKF READ getLightKF WRITE setLightKF NOTIFY lightingParametersChanged)
    Q_PROPERTY(float lightKB READ getLightKB WRITE setLightKB NOTIFY lightingParametersChanged)
    Q_PROPERTY(float lightKH READ getLightKH WRITE setLightKH NOTIFY lightingParametersChanged)
    Q_PROPERTY(bool lightKitEnabled READ getLightKitEnabled WRITE setLightKitEnabled NOTIFY lightingParametersChanged)
    Q_PROPERTY(bool showLightMarkers READ getShowLightMarkers WRITE setShowLightMarkers NOTIFY lightingParametersChanged)
    Q_PROPERTY(float lightWarm READ getLightWarm WRITE setLightWarm NOTIFY lightingParametersChanged)
    Q_PROPERTY(int triangleCount READ getTriangleCount NOTIFY meshLoadStateChanged)
    Q_PROPERTY(int pointCount READ getPointCount NOTIFY meshLoadStateChanged)
    Q_PROPERTY(QString meshDataType READ getMeshDataType NOTIFY meshLoadStateChanged)
    Q_PROPERTY(QString meshFormat READ getMeshFormat NOTIFY meshLoadStateChanged)
    Q_PROPERTY(QColor bgColor READ getBgColorQml WRITE setBgColorQml NOTIFY viewChanged)
    Q_PROPERTY(QStringList availableScalars READ getAvailableScalars NOTIFY meshDataUpdated)
    Q_PROPERTY(bool showScalarColorbar READ getShowScalarColorbar WRITE setShowScalarColorbar NOTIFY viewChanged)
    Q_PROPERTY(bool showVectors READ getShowVectors WRITE setShowVectors NOTIFY viewChanged)
    Q_PROPERTY(float vectorScale READ getVectorScale WRITE setVectorScale NOTIFY viewChanged)
    Q_PROPERTY(int vectorStride READ getVectorStride WRITE setVectorStride NOTIFY viewChanged)
    Q_PROPERTY(bool vectorScaleByMagnitude READ getVectorScaleByMagnitude WRITE setVectorScaleByMagnitude NOTIFY viewChanged)
    Q_PROPERTY(QColor vectorColor READ getVectorColorQml WRITE setVectorColorQml NOTIFY viewChanged)
    Q_PROPERTY(QString vectorField READ getVectorField WRITE setActiveVectorField NOTIFY meshDataUpdated)
    Q_PROPERTY(QStringList availableVectors READ getAvailableVectors NOTIFY meshDataUpdated)
    Q_PROPERTY(bool vectorUseColormap READ getVectorUseColormap WRITE setVectorUseColormap NOTIFY viewChanged)
    Q_PROPERTY(int vectorColormapChoice READ getVectorColormapChoice WRITE setVectorColormapChoice NOTIFY viewChanged)
    Q_PROPERTY(bool vectorColormapReversed READ getVectorColormapReversed WRITE setVectorColormapReversed NOTIFY vectorColormapChanged)
    Q_PROPERTY(QStringList recentFiles READ getRecentFiles NOTIFY meshLoadStateChanged)
    Q_PROPERTY(QString activeScalarName READ getActiveScalarNameQml NOTIFY meshDataUpdated)

    Q_PROPERTY(int colormapChoice READ getColormapChoice WRITE setColormapChoice NOTIFY colormapChanged)
    Q_PROPERTY(bool colormapReversed READ getColormapReversed WRITE setColormapReversed NOTIFY colormapChanged)
    Q_PROPERTY(QVariantList colormapStops READ getColormapStops NOTIFY colormapChanged)
    Q_PROPERTY(QVariantList vectorColormapStops READ getVectorColormapStops NOTIFY vectorColormapChanged)
    Q_PROPERTY(float vectorMagMin READ getVectorMagMin NOTIFY vectorColormapChanged)
    Q_PROPERTY(float vectorMagMax READ getVectorMagMax NOTIFY vectorColormapChanged)
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
    Q_PROPERTY(bool invertX READ getInvertX WRITE setInvertX NOTIFY viewChanged)
    Q_PROPERTY(bool invertY READ getInvertY WRITE setInvertY NOTIFY viewChanged)
    Q_PROPERTY(bool invertZ READ getInvertZ WRITE setInvertZ NOTIFY viewChanged)
    Q_PROPERTY(float filterMin READ getFilterMin WRITE setFilterMin NOTIFY viewChanged)
    Q_PROPERTY(float filterMax READ getFilterMax WRITE setFilterMax NOTIFY viewChanged)

    Q_PROPERTY(bool isGizmoVisible READ isGizmoVisible WRITE setGizmoVisible NOTIFY viewChanged)
    Q_PROPERTY(bool autoRotate READ getAutoRotate WRITE setAutoRotate NOTIFY viewChanged)
    Q_PROPERTY(QColor meshColor READ getMeshColorQml WRITE setMeshColorQml NOTIFY viewChanged)
    Q_PROPERTY(QColor surfaceColor READ getSurfaceColorQml WRITE setSurfaceColorQml NOTIFY viewChanged)
    Q_PROPERTY(bool screenshotTransparent READ getScreenshotTransparent WRITE setScreenshotTransparent NOTIFY viewChanged)
    Q_PROPERTY(int screenshotQuality READ getScreenshotQuality WRITE setScreenshotQuality NOTIFY viewChanged)
    Q_PROPERTY(int screenshotScale READ getScreenshotScale WRITE setScreenshotScale NOTIFY viewChanged)
    Q_PROPERTY(QString statusMessage READ getStatusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(bool showFps READ getShowFps WRITE setShowFps NOTIFY viewChanged)
    Q_PROPERTY(bool quickBarCollapsed READ getQuickBarCollapsed WRITE setQuickBarCollapsed NOTIFY quickBarCollapsedChanged)
    Q_PROPERTY(QString fpsText READ getFpsText NOTIFY fpsChanged)

public:
    explicit RenderSettings(QObject* parent = nullptr);
    ~RenderSettings() override;

    // ---- backend access (render thread owns the Renderer) ----
    Renderer* backend() { return &m_renderer; }

    // Assembles the per-frame snapshot into m_renderSnapshot. Called only from
    // publishRenderState() (GUI thread, exclusive access). Non-const: writes
    // directly into the cached double-buffer member.
    void buildRenderState();

    // Double-buffered publish: re-assembles the snapshot ONLY when the GUI
    // state is dirty, then hands the (already-built) buffer to the Renderer.
    // Skips the ~75-field assembly entirely on idle frames.
    void publishRenderState(::Renderer* scene);

    // ---- view state accessors ----
    bool isWireframe() const { return showWireframe; }
    void setWireframe(bool enabled);
    bool getUseLod() const { return useLod; }
    void setUseLod(bool enabled);

    bool isSurfaceVisible() const { return showSurface; }
    bool isGridVisible() const { return showGrid; }
    void toggleGrid(bool visible);

    bool getHasMeshLoaded() const { return hasMeshLoaded; }
    int getTriangleCount() const { return triangleCount; }
    int getPointCount() const { return pointCount; }
    QString getMeshDataType() const { return QString::fromStdString(meshDataType); }
    QString getMeshFormat() const { return QString::fromStdString(meshFormat); }
    QColor getBgColorQml() const { return QColor::fromRgbF(bgColor[0], bgColor[1], bgColor[2]); }
    void setBgColorQml(const QColor& c) { bgColor[0] = c.redF(); bgColor[1] = c.greenF(); bgColor[2] = c.blueF(); markStateDirty(); emit viewChanged(); }

    int getColormapChoice() const { return colormapChoice; }
    void setColormapChoice(int choice);
    bool getColormapReversed() const { return colormapReversed; }
    void setColormapReversed(bool reversed);
    bool getVectorColormapReversed() const { return vectorColormapReversed; }
    void setVectorColormapReversed(bool reversed);

    bool getQuickBarCollapsed() const { return quickBarCollapsed; }
    void setQuickBarCollapsed(bool collapsed);

    float getLightKeyAzimuth() const { return lighting.lightKeyAzimuth; }
    void setLightKeyAzimuth(float v) { lighting.lightKeyAzimuth = v; markStateDirty(); emit lightingParametersChanged(); }
    float getLightKeyElevation() const { return lighting.lightKeyElevation; }
    void setLightKeyElevation(float v) { lighting.lightKeyElevation = v; markStateDirty(); emit lightingParametersChanged(); }
    float getLightFillAzimuth() const { return lighting.lightFillAzimuth; }
    void setLightFillAzimuth(float v) { lighting.lightFillAzimuth = v; markStateDirty(); emit lightingParametersChanged(); }
    float getLightFillElevation() const { return lighting.lightFillElevation; }
    void setLightFillElevation(float v) { lighting.lightFillElevation = v; markStateDirty(); emit lightingParametersChanged(); }
    float getLightBackAzimuth() const { return lighting.lightBackAzimuth; }
    void setLightBackAzimuth(float v) { lighting.lightBackAzimuth = v; markStateDirty(); emit lightingParametersChanged(); }
    float getLightBackElevation() const { return lighting.lightBackElevation; }
    void setLightBackElevation(float v) { lighting.lightBackElevation = v; markStateDirty(); emit lightingParametersChanged(); }
    float getLightHeadAzimuth() const { return lighting.lightHeadAzimuth; }
    void setLightHeadAzimuth(float v) { lighting.lightHeadAzimuth = v; markStateDirty(); emit lightingParametersChanged(); }
    float getLightHeadElevation() const { return lighting.lightHeadElevation; }
    void setLightHeadElevation(float v) { lighting.lightHeadElevation = v; markStateDirty(); emit lightingParametersChanged(); }
    float getMatAmbient() const { return lighting.matAmbient; }
    void setMatAmbient(float v) { lighting.matAmbient = v; markStateDirty(); emit lightingParametersChanged(); }
    float getMatDiffuse() const { return lighting.matDiffuse; }
    void setMatDiffuse(float v) { lighting.matDiffuse = v; markStateDirty(); emit lightingParametersChanged(); }
    float getMatSpecular() const { return lighting.matSpecular; }
    void setMatSpecular(float v) { lighting.matSpecular = v; markStateDirty(); emit lightingParametersChanged(); }
    float getMatShininess() const { return lighting.matShininess; }
    void setMatShininess(float v) { lighting.matShininess = v; markStateDirty(); emit lightingParametersChanged(); }
    float getLightKeyIntensity() const { return lighting.lightKeyIntensity; }
    void setLightKeyIntensity(float v) { lighting.lightKeyIntensity = v; markStateDirty(); emit lightingParametersChanged(); }
    float getLightKF() const { return lighting.lightKF; }
    void setLightKF(float v) { lighting.lightKF = v; markStateDirty(); emit lightingParametersChanged(); }
    float getLightKB() const { return lighting.lightKB; }
    void setLightKB(float v) { lighting.lightKB = v; markStateDirty(); emit lightingParametersChanged(); }
    float getLightKH() const { return lighting.lightKH; }
    void setLightKH(float v) { lighting.lightKH = v; markStateDirty(); emit lightingParametersChanged(); }
    bool getLightKitEnabled() const { return lighting.lightKitEnabled; }
    void setLightKitEnabled(bool v) { lighting.lightKitEnabled = v; markStateDirty(); emit lightingParametersChanged(); }
    bool getShowLightMarkers() const { return lighting.showLightMarkers; }
    void setShowLightMarkers(bool v) { lighting.showLightMarkers = v; markStateDirty(); emit lightingParametersChanged(); }
    float getLightWarm() const { return lighting.lightWarm; }
    void setLightWarm(float v) { lighting.lightWarm = v; markStateDirty(); emit lightingParametersChanged(); }

    QString getCurrentMeshNameQStr() const { return QString::fromStdString(currentMeshName); }
    const std::string& getCurrentMeshName() const { return currentMeshName; }

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

    Q_INVOKABLE QStringList getColormapNames() const;
    Q_INVOKABLE QString getColormapPreviewUri(int index) const;

signals:
    void wireframeChanged();
    void surfaceVisibilityChanged();
    void gridVisibilityChanged();
    void meshLoadStateChanged();
    void meshDataUpdated();
    void lightingParametersChanged();
    void colormapChanged();
    void colorbarChanged();
    void vectorColormapChanged();
    void viewChanged();
    void quickBarCollapsedChanged();
    void screenshotCaptured(const QString& targetSavedPath);
    void screenshotRequested(const QString& targetPath);
    void fpsChanged();
    void statusMessageChanged();

public:
    // VTK Camera forwarders (QML-invokable). Mutate the GUI-side Camera; the
    // next synchronize() copies it into the render-thread snapshot.
    Q_INVOKABLE void azimuth(double angle) { camera.azimuth(angle); m_renderer.markCameraMoving(); markStateDirty(); emit viewChanged(); }
    Q_INVOKABLE void elevation(double angle) { camera.elevation(angle); m_renderer.markCameraMoving(); markStateDirty(); emit viewChanged(); }
    Q_INVOKABLE void roll(double angle) { camera.roll(angle); m_renderer.markCameraMoving(); markStateDirty(); emit viewChanged(); }
    Q_INVOKABLE void pan(double dx, double dy) { camera.pan(dx, dy); m_renderer.markCameraMoving(); markStateDirty(); emit viewChanged(); }
    Q_INVOKABLE void dolly(double factor) { camera.dolly(factor); m_renderer.markCameraMoving(); markStateDirty(); emit viewChanged(); }

    void orthogonalizeViewUp() { camera.orthogonalizeViewUp(); }
    glm::dvec3 directionOfProjection() const { return camera.directionOfProjection(); }

    float* getMeshColor() { return meshColor; }
    float* getSurfaceColor() { return surfaceColor; }
    float* getBgColor() { return bgColor; }

    Q_INVOKABLE void setSidebarWidth(float w) { sidebarWidth = w; }

    bool isGizmoVisible() const { return showGizmo; }
    void setGizmoVisible(bool v) { if (showGizmo != v) { showGizmo = v; markStateDirty(); emit viewChanged(); } }
    bool getAutoRotate() const { return autoRotate; }
    void setAutoRotate(bool v) { if (autoRotate != v) { autoRotate = v; markStateDirty(); emit viewChanged(); } }
    QColor getMeshColorQml() const { return QColor::fromRgbF(meshColor[0], meshColor[1], meshColor[2]); }
    void setMeshColorQml(const QColor& c) { meshColor[0] = c.redF(); meshColor[1] = c.greenF(); meshColor[2] = c.blueF(); markStateDirty(); emit viewChanged(); }
    QColor getSurfaceColorQml() const { return QColor::fromRgbF(surfaceColor[0], surfaceColor[1], surfaceColor[2]); }
    void setSurfaceColorQml(const QColor& c) { surfaceColor[0] = c.redF(); surfaceColor[1] = c.greenF(); surfaceColor[2] = c.blueF(); markStateDirty(); emit viewChanged(); }

    bool getShowVectors() const { return showVectors; }
    void setShowVectors(bool v) { if (showVectors != v) { showVectors = v; markStateDirty(); emit viewChanged(); } }
    bool getVectorScaleByMagnitude() const { return vectorScaleByMagnitude; }
    void setVectorScaleByMagnitude(bool v) { if (vectorScaleByMagnitude != v) { vectorScaleByMagnitude = v; markStateDirty(); emit viewChanged(); } }
    float getVectorScale() const { return vectorScale; }
    void setVectorScale(float v) { if (vectorScale != v) { vectorScale = v; markStateDirty(); emit viewChanged(); } }
    int getVectorStride() const { return vectorStride; }
    void setVectorStride(int v) { int s = v < 1 ? 1 : v; if (vectorStride != s) { vectorStride = s; m_renderer.markCameraMoving(); markStateDirty(); emit viewChanged(); } }
    QColor getVectorColorQml() const { return QColor::fromRgbF(vectorColor[0], vectorColor[1], vectorColor[2]); }
    void setVectorColorQml(const QColor& c) { vectorColor[0] = c.redF(); vectorColor[1] = c.greenF(); vectorColor[2] = c.blueF(); markStateDirty(); emit viewChanged(); }
    bool getVectorUseColormap() const { return vectorUseColormap; }
    void setVectorUseColormap(bool v) { if (vectorUseColormap != v) { vectorUseColormap = v; markStateDirty(); emit viewChanged(); } }
    int getVectorColormapChoice() const { return vectorColormapChoice; }
    void setVectorColormapChoice(int c) { if (vectorColormapChoice != c) { vectorColormapChoice = c; markStateDirty(); emit viewChanged(); } }
    QStringList getAvailableVectors() const { QStringList l; for (const auto& n : m_guiMeta.availableVectorNames) l.append(QString::fromStdString(n)); return l; }
    QString getVectorField() const { return QString::fromStdString(m_guiMeta.vectorName); }
    Q_INVOKABLE void setActiveVectorField(const QString& fieldName);
    bool getScreenshotTransparent() const { return screenshotTransparent; }
    void setScreenshotTransparent(bool v) { screenshotTransparent = v; }
    int getScreenshotQuality() const { return screenshotQuality; }
    void setScreenshotQuality(int v) { screenshotQuality = (v < 1 ? 1 : (v > 100 ? 100 : v)); }
    int getScreenshotScale() const { return screenshotScale; }
    void setScreenshotScale(int v) { int c = (v < 1 ? 1 : (v > 4 ? 4 : v)); if (screenshotScale != c) { screenshotScale = c; markStateDirty(); emit viewChanged(); } }
    QString getStatusMessage() const { return statusMessage; }
    Q_INVOKABLE QString generateScreenshotFilename() const {
        return ScreenshotExporter::generateFilename(QString::fromStdString(currentMeshName), ExportFormat::PNG);
    }

    Q_INVOKABLE void applyLightingPreset(int preset);
    Q_INVOKABLE void resetLighting();
    static constexpr int PRESET_STUDIO = 0;
    static constexpr int PRESET_CADFLAT = 1;
    static constexpr int PRESET_SOFT = 2;

    bool getShowFps() const { return showFps; }
    void setShowFps(bool v) { if (showFps != v) { showFps = v; markStateDirty(); emit viewChanged(); } }
    QString getFpsText() const { return fpsText; }

    void setScalarRange(float min, float max) { scalarMin = min; scalarMax = max; }

    bool hasMeshScalars() const { return meshHasScalars; }
    const std::string& getActiveScalarName() const { return activeScalarName; }
    Q_INVOKABLE QString getActiveScalarNameQml() const { return QString::fromStdString(activeScalarName); }

    Q_INVOKABLE bool hasMeshScalarsQml() const { return meshHasScalars; }
    Q_INVOKABLE float getDataScalarMinQml() const { return dataScalarMin; }
    Q_INVOKABLE float getDataScalarMaxQml() const { return dataScalarMax; }
    int getColorbarTicks() const { return colorbarTicks; }
    void setColorbarTicks(int v) { int c = v < 2 ? 2 : v; if (colorbarTicks != c) { colorbarTicks = c; markStateDirty(); emit colorbarChanged(); } }
    bool getShowScalarColorbar() const { return showScalarColorbar; }
    void setShowScalarColorbar(bool v) { if (showScalarColorbar != v) { showScalarColorbar = v; markStateDirty(); emit viewChanged(); } }

    bool getClipEnabled() const { return clipEnabled; }
    void setClipEnabled(bool v) { if (clipEnabled != v) { clipEnabled = v; markStateDirty(); emit viewChanged(); } }
    float getSliceX() const { return sliceHeightX; }
    void setSliceX(float v) { if (sliceHeightX != v) { sliceHeightX = v; markStateDirty(); emit viewChanged(); } }
    float getSliceY() const { return sliceHeightY; }
    void setSliceY(float v) { if (sliceHeightY != v) { sliceHeightY = v; markStateDirty(); emit viewChanged(); } }
    float getSliceZ() const { return sliceHeightZ; }
    void setSliceZ(float v) { if (sliceHeightZ != v) { sliceHeightZ = v; markStateDirty(); emit viewChanged(); } }
    bool getInvertX() const { return invertX; }
    void setInvertX(bool v) { if (invertX != v) { invertX = v; markStateDirty(); emit viewChanged(); } }
    bool getInvertY() const { return invertY; }
    void setInvertY(bool v) { if (invertY != v) { invertY = v; markStateDirty(); emit viewChanged(); } }
    bool getInvertZ() const { return invertZ; }
    void setInvertZ(bool v) { if (invertZ != v) { invertZ = v; markStateDirty(); emit viewChanged(); } }
    float getFilterMin() const { return filterMin; }
    void setFilterMin(float v) { if (filterMin != v) { filterMin = v; markStateDirty(); emit viewChanged(); } }
    float getFilterMax() const { return filterMax; }
    void setFilterMax(float v) { if (filterMax != v) { filterMax = v; markStateDirty(); emit viewChanged(); } }

    double getWorldMinX() const { return worldMinX; }
    double getWorldMaxX() const { return worldMaxX; }
    double getWorldMinY() const { return worldMinY; }
    double getWorldMaxY() const { return worldMaxY; }
    double getWorldMinZ() const { return worldMinZ; }
    double getWorldMaxZ() const { return worldMaxZ; }

    Q_INVOKABLE QVariantList getColormapStops() const;
    Q_INVOKABLE QVariantList getVectorColormapStops() const;
    float getVectorMagMin() const { return m_renderer.vectorMagMin(); }
    float getVectorMagMax() const { return m_renderer.vectorMagMax(); }

    float getScalarMin() const { return scalarMin; }
    float getScalarMax() const { return scalarMax; }
    float getDataScalarMin() const { return dataScalarMin; }
    float getDataScalarMax() const { return dataScalarMax; }

private:
    void setStatus(const QString& msg);
    void recomputeScalarRange();

    // Marks the render-state snapshot stale; the next publishRenderState()
    // re-assembles it. Called from every GUI-state mutation (see emit sites).
    void markStateDirty() { m_stateDirty = true; }

    // ---- render-state double buffer ----
    // m_uiState is the GUI-thread source of truth (the plain members below).
    // m_renderSnapshot is the published copy handed to the render thread.
    // Because synchronize() (GUI thread) and renderFrame() (render thread)
    // never run concurrently on these buffers, we swap/rebuild without a
    // runtime mutex — a pure structural copy only when m_stateDirty is set.
    RenderRenderState m_renderSnapshot;
    bool m_stateDirty = true;

    // ---- backend (render thread) ----
    Renderer m_renderer;

    // ---- GUI-side view / state ----
    Camera camera;

    float sidebarWidth = 0.0f;
    bool showSurface = true;
    bool showGrid = false;
    bool showWireframe = false;
    bool showGizmo = true;
    bool autoRotate = false;
    bool useLod = true;

    float meshColor[3] = { 0.4f, 0.9f, 0.4f };
    float surfaceColor[3] = { 1.0f, 1.0f, 1.0f };
    float bgColor[3] = { 0.12f, 0.12f, 0.12f };

    bool hasMeshLoaded = false;
    bool meshHasScalars = false;
    int triangleCount = 0;
    int pointCount = 0;
    std::string currentMeshName;
    std::string meshDataType;
    std::string meshFormat;
    std::string activeScalarName;
    std::shared_ptr<const RenderMesh> m_loadedMesh; // single heavy CPU payload (immutable)
    RenderMesh m_guiMeta;                           // light copy: attributes + active scalars + names + metadata

    double worldCenterX = 0, worldCenterY = 0, worldCenterZ = 0;
    double worldRadius = 1.0;
    double worldMinX = -10.0, worldMaxX = 10.0;
    double worldMinY = -10.0, worldMaxY = 10.0;
    double worldMinZ = -10.0, worldMaxZ = 10.0;

    float scalarMin = 0.0f;
    float scalarMax = 1.0f;
    float dataScalarMin = 0.0f;
    float dataScalarMax = 1.0f;
    int colorbarTicks = 6;
    bool showScalarColorbar = true;

    // lighting (mirrors LightingModel; copied into snapshot)
    LightingModel lighting;

    int colormapChoice = 3;
    bool colormapReversed = false;
    int vectorColormapChoice = 3;
    bool vectorColormapReversed = false;

    bool showVectors = false;
    float vectorScale = 1.0f;
    int vectorStride = 1;
    float vectorColor[3] = { 0.2f, 0.6f, 1.0f };
    bool vectorUseColormap = false;
    bool vectorScaleByMagnitude = false;

    bool clipEnabled = false;
    float sliceHeightX = 0.0f;
    float sliceHeightY = 0.0f;
    float sliceHeightZ = 0.0f;
    bool invertX = false;
    bool invertY = false;
    bool invertZ = false;
    float filterMin = 0.0f;
    float filterMax = 1.0f;

    bool screenshotTransparent = false;
    int screenshotQuality = 95;
    int screenshotScale = 1;
    QString statusMessage;

    bool showFps = false;
    QString fpsText = "FPS: --";

    bool quickBarCollapsed = false;
    QStringList recentFiles;

    mutable std::map<int, QString> m_colormapPreviewCache;

    // Async parse watcher (GUI thread). Parsing runs off the GUI thread via
    // QtConcurrent::run; the finished result is delivered here and handed to the
    // render thread as a shared_ptr (no copy). Parented to this object so it is
    // destroyed on the GUI thread.
    QFutureWatcher<std::shared_ptr<const RenderMesh>> m_meshWatcher;
    std::string m_loadingPath; // normalized path for filename/recent-files in the continuation
    // Monotonic generation counter: each loadMesh() increments it and stamps the
    // async task. onMeshParsed() ignores results whose token != current, so a
    // cancelled/superseded parse never misreports a load error or clobbers state.
    uint64_t m_loadToken = 0;
    // Generation stamp of the in-flight async parse. The worker writes its token
    // on completion; onMeshParsed() compares it to m_loadToken to drop stale
    // (cancelled/superseded) results.
    std::shared_ptr<std::atomic<uint64_t>> m_taskToken;
};
