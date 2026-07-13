#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

// Qt Core & Object Meta-Architecture
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QColor>

// Core Graphics Dependencies (Replacing GLFW windows management entirely)
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

#include "mesh/mesh_loader.h"
#include "gizmo/gizmo.h"
#include "camera/Camera.h"

struct Mesh {
    GLuint vao;
    GLuint vbo;
    GLuint nbo;
    GLuint ebo;
    GLuint sbo;
    int indexCount;
};

class Renderer : public QObject {
    Q_OBJECT

    // -----------------------------------------------------------------------
    // QML Data Binding Interop Properties Matrix
    // -----------------------------------------------------------------------
    Q_PROPERTY(bool isWireframe READ isWireframe WRITE setWireframe NOTIFY wireframeChanged)
    Q_PROPERTY(bool isGridVisible READ isGridVisible WRITE toggleGrid NOTIFY gridVisibilityChanged)
    Q_PROPERTY(bool isSurfaceVisible READ isSurfaceVisible WRITE toggleSurface NOTIFY surfaceVisibilityChanged)
    Q_PROPERTY(bool hasMeshLoaded READ getHasMeshLoaded NOTIFY meshLoadStateChanged)
    Q_PROPERTY(QString currentMeshName READ getCurrentMeshNameQStr NOTIFY meshLoadStateChanged)

    Q_PROPERTY(float lightInt READ getLightInt WRITE setLightInt NOTIFY lightingParametersChanged)
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
    Q_PROPERTY(float lightFillIntensity READ getLightFillIntensity WRITE setLightFillIntensity NOTIFY lightingParametersChanged)
    Q_PROPERTY(float lightHeadIntensity READ getLightHeadIntensity WRITE setLightHeadIntensity NOTIFY lightingParametersChanged)
    Q_PROPERTY(int triangleCount READ getTriangleCount NOTIFY meshLoadStateChanged)
    Q_PROPERTY(int pointCount READ getPointCount NOTIFY meshLoadStateChanged)
    Q_PROPERTY(QColor bgColor READ getBgColorQml WRITE setBgColorQml NOTIFY viewChanged)
    Q_PROPERTY(int gizmoSize READ getGizmoSize WRITE setGizmoSize NOTIFY meshLoadStateChanged)
    Q_PROPERTY(float devicePixelRatio READ getDevicePixelRatio NOTIFY meshLoadStateChanged)
    Q_PROPERTY(int colormapChoice READ getColormapChoice WRITE setColormapChoice NOTIFY colormapChanged)
    // ponytail: property so QML Repeater re-reads on colormapChanged (Q_INVOKABLE alone doesn't notify)
    Q_PROPERTY(QVariantList colormapStops READ getColormapStops NOTIFY colormapChanged)
    // ponytail: world bounds exposed so the slice-panel sliders can set their from/to ranges
    Q_PROPERTY(double worldMinX READ getWorldMinX NOTIFY meshLoadStateChanged)
    Q_PROPERTY(double worldMaxX READ getWorldMaxX NOTIFY meshLoadStateChanged)
    Q_PROPERTY(double worldMinY READ getWorldMinY NOTIFY meshLoadStateChanged)
    Q_PROPERTY(double worldMaxY READ getWorldMaxY NOTIFY meshLoadStateChanged)
    Q_PROPERTY(double worldMinZ READ getWorldMinZ NOTIFY meshLoadStateChanged)
    Q_PROPERTY(double worldMaxZ READ getWorldMaxZ NOTIFY meshLoadStateChanged)
    // ponytail: data scalar range exposed so the scalar-filter sliders + colorbar use the real range (raw vScalar, not 0..1)
    Q_PROPERTY(double dataScalarMinQml READ getDataScalarMinQml NOTIFY meshLoadStateChanged)
    Q_PROPERTY(double dataScalarMaxQml READ getDataScalarMaxQml NOTIFY meshLoadStateChanged)
    // ponytail: slice/clip exposed as props so QML can set them AND trigger a repaint
    Q_PROPERTY(bool clipEnabled READ getClipEnabled WRITE setClipEnabled NOTIFY viewChanged)
    Q_PROPERTY(float sliceHeightX READ getSliceX WRITE setSliceX NOTIFY viewChanged)
    Q_PROPERTY(float sliceHeightY READ getSliceY WRITE setSliceY NOTIFY viewChanged)
    Q_PROPERTY(float sliceHeightZ READ getSliceZ WRITE setSliceZ NOTIFY viewChanged)
    Q_PROPERTY(bool invertX READ getInvertX WRITE setInvertX NOTIFY viewChanged)
    Q_PROPERTY(bool invertY READ getInvertY WRITE setInvertY NOTIFY viewChanged)
    Q_PROPERTY(bool invertZ READ getInvertZ WRITE setInvertZ NOTIFY viewChanged)
    Q_PROPERTY(float filterMin READ getFilterMin WRITE setFilterMin NOTIFY viewChanged)
    Q_PROPERTY(float filterMax READ getFilterMax WRITE setFilterMax NOTIFY viewChanged)

public:
    explicit Renderer(QObject* parent = nullptr);
    virtual ~Renderer();

    // Core Initialization & Graphics Lifecycle Routines called by Qt Context
    void initGLAD();
    void initShaders();
    void initGrid();
    void initGizmo();
    void renderFrame();

    // Uploads CPU geometry to the GPU. Safe to call on the render thread.
    void uploadMesh(const RenderMesh& renderMesh);
    void resizeViewport(int width, int height);

    // Modern Qt Property Accessors & Mutators
    bool isWireframe() const { return showWireframe; }
    void setWireframe(bool enabled);

    bool isGridVisible() const { return showGrid; }

    bool isSurfaceVisible() const { return showSurface; }

    bool getHasMeshLoaded() const { return hasMeshLoaded; }
    int getTriangleCount() const { return triangleCount; }
    int getPointCount() const { return pointCount; }
    QColor getBgColorQml() const { return QColor::fromRgbF(bgColor[0], bgColor[1], bgColor[2]); }
    void setBgColorQml(const QColor& c) { bgColor[0] = c.redF(); bgColor[1] = c.greenF(); bgColor[2] = c.blueF(); emit viewChanged(); }

    int getColormapChoice() const { return colormapChoice; }
    void setColormapChoice(int choice);

    float getLightInt() const { return lightInt; }
    void setLightInt(float intensity) { lightInt = intensity; emit lightingParametersChanged(); }

    float getLightKeyAzimuth() const { return lightKeyAzimuth; }
    void setLightKeyAzimuth(float v) { lightKeyAzimuth = v; emit lightingParametersChanged(); }
    float getLightKeyElevation() const { return lightKeyElevation; }
    void setLightKeyElevation(float v) { lightKeyElevation = v; emit lightingParametersChanged(); }
    float getLightFillAzimuth() const { return lightFillAzimuth; }
    void setLightFillAzimuth(float v) { lightFillAzimuth = v; emit lightingParametersChanged(); }
    float getLightFillElevation() const { return lightFillElevation; }
    void setLightFillElevation(float v) { lightFillElevation = v; emit lightingParametersChanged(); }
    float getLightBackAzimuth() const { return lightBackAzimuth; }
    void setLightBackAzimuth(float v) { lightBackAzimuth = v; emit lightingParametersChanged(); }
    float getLightBackElevation() const { return lightBackElevation; }
    void setLightBackElevation(float v) { lightBackElevation = v; emit lightingParametersChanged(); }
    float getLightHeadAzimuth() const { return lightHeadAzimuth; }
    void setLightHeadAzimuth(float v) { lightHeadAzimuth = v; emit lightingParametersChanged(); }
    float getLightHeadElevation() const { return lightHeadElevation; }
    void setLightHeadElevation(float v) { lightHeadElevation = v; emit lightingParametersChanged(); }
    float getMatAmbient() const { return matAmbient; }
    void setMatAmbient(float v) { matAmbient = v; emit lightingParametersChanged(); }
    float getMatDiffuse() const { return matDiffuse; }
    void setMatDiffuse(float v) { matDiffuse = v; emit lightingParametersChanged(); }
    float getMatSpecular() const { return matSpecular; }
    void setMatSpecular(float v) { matSpecular = v; emit lightingParametersChanged(); }
    float getMatShininess() const { return matShininess; }
    void setMatShininess(float v) { matShininess = v; emit lightingParametersChanged(); }
    float getLightKeyIntensity() const { return lightKF; }
    void setLightKeyIntensity(float v) { lightKF = v; emit lightingParametersChanged(); }
    float getLightFillIntensity() const { return lightKB; }
    void setLightFillIntensity(float v) { lightKB = v; emit lightingParametersChanged(); }
    float getLightHeadIntensity() const { return lightKH; }
    void setLightHeadIntensity(float v) { lightKH = v; emit lightingParametersChanged(); }

    // Direct String Mapper adjustments for Qt Engines
    QString getCurrentMeshNameQStr() const { return QString::fromStdString(currentMeshName); }
    const std::string& getCurrentMeshName() const { return currentMeshName; }

    // -----------------------------------------------------------------------
    // UI Callable Actions (Slots invoked directly by QML buttons/sliders)
    // -----------------------------------------------------------------------
public slots:
    void loadMesh(const QString& filePath);
    void clearMeshes();
    void resetCamera();
    void snapToOrthoView(int axis);
    bool captureScreenshotWithDialog();
    // Performs the actual GL pixel read + file save. MUST be called on the
    // render thread while the OpenGL context is current (see CustomViewportItem).
    bool captureScreenshotToFile(const QString& path);
    void toggleGrid(bool visible);
    void snapToAxisView(int axis, bool flip); // gizmo-driven snap: axis 0/1/2, flip=true -> negative side
    void toggleSurface(bool visible);

    // Multi-Scalar Dynamic Interop
    QStringList getAvailableScalars() const;
    void setActiveScalarField(const QString& fieldName);

    // Returns the list of colormap display names in ColormapType enum order,
    // suitable for binding directly to a QML ComboBox model.
    Q_INVOKABLE QStringList getColormapNames() const;

signals:
    void wireframeChanged();
    void gridVisibilityChanged();
    void surfaceVisibilityChanged();
    void meshLoadStateChanged();
    void meshDataUpdated();
    void lightingParametersChanged();
    void colormapChanged();
    void viewChanged(); // gizmo hover/snap -> viewport repaint
    void screenshotCaptured(const QString& targetSavedPath);
    void screenshotRequested(const QString& targetPath);

public:
    // VTK Camera Inline Forwarders
    void azimuth(double angle) { camera.azimuth(angle); }
    void elevation(double angle) { camera.elevation(angle); }
    void roll(double angle) { camera.roll(angle); }
    void pan(double dx, double dy) { camera.pan(dx, dy); }
    void dolly(double factor) { camera.dolly(factor); camDistance = camera.distance; }
    void orthogonalizeViewUp() { camera.orthogonalizeViewUp(); }
    glm::dvec3 directionOfProjection() const { return camera.directionOfProjection(); }

    // Vector adjustments & Bounds queries
    float* getMeshColor() { return meshColor; }
    float* getSurfaceColor() { return surfaceColor; }
    float* getBgColor() { return bgColor; }
    void getWorldCenter(float& x, float& y, float& z) const { x = static_cast<float>(worldCenterX); y = static_cast<float>(worldCenterY); z = static_cast<float>(worldCenterZ); }
    float getWorldRadius() const { return static_cast<float>(worldRadius); }
    void getGizmoAxisEndpoints(float& xEndX, float& xEndY, float& yEndX, float& yEndY, float& zEndX, float& zEndY);
    void setHoveredAxis(int axis) { gizmo.setHoveredAxis(axis); emit viewChanged(); }
    int getGizmoSize() const { return gizmo.getSize(); }
    Q_INVOKABLE void setGizmoSize(int size) { gizmo.setSize(size); }
    float getDevicePixelRatio() const { return devicePixelRatio; }

    // Gizmo interaction helpers (called from the QML MouseArea over the gizmo rect)
    Q_INVOKABLE int pickGizmoAxis(float px, float py) {
        // px,py are device pixels in GL (bottom-left) origin.
        float xX, xY, yX, yY, zX, zY;
        gizmo.getAxisEndpoints(static_cast<int>(width * devicePixelRatio),
                               static_cast<int>(height * devicePixelRatio),
                               camera.computeGizmoQuat(),
                               sidebarWidth * devicePixelRatio,
                               xX, xY, yX, yY, zX, zY);
        auto near = [&](float ax, float ay) -> bool {
            float dx = ax - px, dy = ay - py;
            return (dx * dx + dy * dy) <= (18.0f * 18.0f);
        };
        if (near(xX, xY)) return 0;
        if (near(yX, yY)) return 1;
        if (near(zX, zY)) return 2;
        return -1;
    }
    void setSidebarWidth(float w) { sidebarWidth = w; }
    float getSidebarWidth() const { return sidebarWidth; }


    // Device-pixel-ratio aware sizing so the GL viewport matches the real
    // framebuffer on HiDPI displays.
    void setDevicePixelRatio(float dpr) { devicePixelRatio = dpr; }
    std::vector<std::string> getAvailableScalarNames() const;
    void setActiveScalarFieldStd(const std::string& fieldName);

    void getBounds(float& outMinX, float& outMaxX, float& outMinY, float& outMaxY, float& outMinZ, float& outMaxZ) const {
        outMinX = static_cast<float>(worldMinX); outMaxX = static_cast<float>(worldMaxX);
        outMinY = static_cast<float>(worldMinY); outMaxY = static_cast<float>(worldMaxY);
        outMinZ = static_cast<float>(worldMinZ); outMaxZ = static_cast<float>(worldMaxZ);
    }

    enum class GridPlane { XY, XZ, YZ, AutoLowest };
    GridPlane activeGridPlane = GridPlane::AutoLowest;

    void setScalarRange(float min, float max) { scalarMin = min; scalarMax = max; }
    bool consumeMeshChanged() { return meshChanged.exchange(false); }

    // Thread-safe extraction of the queued mesh produced on the UI thread by loadMesh().
    // Copies the geometry out under the mesh queue mutex so the caller can safely
    // perform GL upload on the render thread.
    void takeQueuedMesh(RenderMesh& out) {
        std::lock_guard<std::mutex> lock(meshQueueMutex);
        out = dynamicMeshQueue;
    }
    float getScalarMin() const { return scalarMin; }
    float getScalarMax() const { return scalarMax; }
    float getDataScalarMin() const { return dataScalarMin; }
    float getDataScalarMax() const { return dataScalarMax; }
    bool hasMeshScalars() const { return meshHasScalars; }
    const std::string& getActiveScalarName() const { return activeScalarName; }
    // ponytail: QString accessor so QML bindings can read the active scalar name (std::string isn't a QML metatype)
    Q_INVOKABLE QString getActiveScalarNameQml() const { return QString::fromStdString(activeScalarName); }

    // QML-visible accessors for the colorbar legend overlay.
    Q_INVOKABLE bool hasMeshScalarsQml() const { return meshHasScalars; }
    Q_INVOKABLE float getDataScalarMinQml() const { return dataScalarMin; }
    Q_INVOKABLE float getDataScalarMaxQml() const { return dataScalarMax; }

    // ponytail: slice/clip getters/setters — setters emit viewChanged (cheap repaint)
    bool getClipEnabled() const { return clipEnabled; }
    void setClipEnabled(bool v) { if (clipEnabled != v) { clipEnabled = v; emit viewChanged(); } }
    float getSliceX() const { return sliceHeightX; }
    void setSliceX(float v) { if (sliceHeightX != v) { sliceHeightX = v; emit viewChanged(); } }
    float getSliceY() const { return sliceHeightY; }
    void setSliceY(float v) { if (sliceHeightY != v) { sliceHeightY = v; emit viewChanged(); } }
    float getSliceZ() const { return sliceHeightZ; }
    void setSliceZ(float v) { if (sliceHeightZ != v) { sliceHeightZ = v; emit viewChanged(); } }
    bool getInvertX() const { return invertX; }
    void setInvertX(bool v) { if (invertX != v) { invertX = v; emit viewChanged(); } }
    bool getInvertY() const { return invertY; }
    void setInvertY(bool v) { if (invertY != v) { invertY = v; emit viewChanged(); } }
    bool getInvertZ() const { return invertZ; }
    void setInvertZ(bool v) { if (invertZ != v) { invertZ = v; emit viewChanged(); } }
    float getFilterMin() const { return filterMin; }
    void setFilterMin(float v) { if (filterMin != v) { filterMin = v; emit viewChanged(); } }
    float getFilterMax() const { return filterMax; }
    void setFilterMax(float v) { if (filterMax != v) { filterMax = v; emit viewChanged(); } }

    double getWorldMinX() const { return worldMinX; }
    double getWorldMaxX() const { return worldMaxX; }
    double getWorldMinY() const { return worldMinY; }
    double getWorldMaxY() const { return worldMaxY; }
    double getWorldMinZ() const { return worldMinZ; }
    double getWorldMaxZ() const { return worldMaxZ; }

    // Returns a list of [t, r, g, b] stops (t in 0..1, rgb in 0..1) sampling the
    // active colormap, suitable for building a QML gradient/legend.
    Q_INVOKABLE QVariantList getColormapStops() const;

    // Dynamic Slices & Iso-Filter Threshold Registers
    float sliceHeightX = 0.0f;
    float sliceHeightY = 0.0f;
    float sliceHeightZ = 0.0f;
    bool invertX = false;
    bool invertY = false;
    bool invertZ = false;
    float filterMin = 0.0f;
    float filterMax = 1.0f;
    bool clipEnabled = false;

    // 4-Point Light Parameter Set registers
    // ponytail: warmth fields deleted — dead (never read by computeLightDirections/drawMesh)
    float lightKF = 3.0f;
    float lightKB = 3.5f;
    float lightKH = 3.0f;
    float lightKeyAzimuth = 10.0f;
    float lightKeyElevation = 50.0f;
    float lightFillAzimuth = -10.0f;
    float lightFillElevation = -75.0f;
    float lightBackAzimuth = 110.0f;
    float lightBackElevation = 0.0f;
    float lightHeadAzimuth = 0.0f;
    float lightHeadElevation = 0.0f;

    float matAmbient = 0.08f;
    float matDiffuse = 0.75f;
    float matSpecular = 0.15f;
    float matShininess = 0.5f;

private:
    void drawGizmo();
    void updateColormapTexture();
    void destroyMesh(Mesh& mesh);
    void computeLightDirections(glm::vec3& key, glm::vec3& fill, glm::vec3& back1, glm::vec3& back2, glm::vec3& head);
    std::string readShaderFile(const std::string& filePath);

    // Display Dimension Registers
    int width = 800;
    int height = 600;
    float devicePixelRatio = 1.0f; // device pixels per logical pixel (HiDPI)

    // Viewport Core Transform Tracking Classes
    Camera camera;
    Gizmo gizmo;

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

    double camDistance = 3.0;
    double nearPlane = 0.1;
    double farPlane = 100.0;
    bool hasMeshLoaded = false;
    std::vector<Mesh> meshes;

    RenderMesh cachedMeshSource;
    std::string currentMeshName;
    std::string activeScalarName;
    float meshColor[3] = { 0.4f, 0.9f, 0.4f };
    float surfaceColor[3] = { 1.0f, 1.0f, 1.0f };
    float bgColor[3] = { 0.0f, 0.0f, 0.0f };

    double worldCenterX = 0, worldCenterY = 0, worldCenterZ = 0;
    double worldRadius = 0;
    double worldMinX = -10.0, worldMaxX = 10.0;
    double worldMinY = -10.0, worldMaxY = 10.0;
    double worldMinZ = -10.0, worldMaxZ = 10.0;

    float sidebarWidth = 0.0f;
    GLuint gridVAO = 0, gridVBO = 0;
    GLuint gridShaderProgram = 0;
    GLint scalarAttribLoc = -1;
    GLint gridMVPLoc = -1;
    bool showGrid = false;
    bool showSurface = true;
    int gridVertexCount = 0;

    int colormapChoice = 0;
    int lastUploadedChoice = -1;
    GLuint colormapTex = 0;
    std::atomic<bool> meshChanged{true};
    float scalarMin = 0.0f;
    float scalarMax = 1.0f;
    float dataScalarMin = 0.0f;
    float dataScalarMax = 1.0f;
    bool meshHasScalars = false;

    std::mutex meshGLMutex; // guards meshes GPU-handle teardown/uploads across threads
    bool m_destroying = false; // set in ~Renderer to suppress signals during teardown
    bool showWireframe = false;
    int triangleCount = 0;
    int pointCount = 0;
    float lightInt = 0.2f;

    RenderMesh dynamicMeshQueue;
    std::mutex meshQueueMutex;
};