#pragma once

#include <QObject>
#include <QFutureWatcher>
#include <QTimer>
#include <memory>
#include <string>

#include "render/foundation/renderer.h"
#include "core/mesh_loader.h"
#include "core/isosurface.h"

// IsosurfaceController — owns the isosurface lifecycle for the GUI thread:
// debounced async marching-cubes extraction, stale-task guarding, and the
// zero-copy shared_ptr handoff to the render thread.
//
// RenderSettings holds 3 Q_PROPERTYs (showIsosurface, isovalue,
// isosurfaceAvailable) that delegate to this controller. The controller
// calls setPendingIsosurface() on the Renderer directly (same thread-safe
// zero-copy pattern as the mesh/upload handoff) and signals RenderSettings
// only when m_state or signal emission is needed.
class IsosurfaceController : public QObject {
    Q_OBJECT
public:
    explicit IsosurfaceController(Renderer& renderer, QObject* parent = nullptr);

    bool showIsosurface() const { return m_showIsosurface; }
    void setShowIsosurface(bool v);

    float isovalue() const { return m_isovalue; }
    void setIsovalue(float v, float lo, float hi);

    bool isAvailable() const;

    // Stores the current mesh + active scalar field name so that a later
    // recompute (from toggle or debounce) doesn't need them passed again.
    void setCurrentMesh(std::shared_ptr<const RenderMesh> mesh, const std::string& field);
    void setCurrentField(const std::string& field) { m_currentField = field; }
    void recompute();
    void reset(float dataMin, float dataMax);
    void clear();

signals:
    void displayDirty();
    void needsScalarColor();
    void showIsosurfaceChanged(bool);
    void isovalueChanged(float);

private slots:
    void onDebounceTimeout();
    void onComputed();

private:
    void launchAsync();

    Renderer& m_renderer;
    bool m_showIsosurface = false;
    float m_isovalue = 0.0f;
    std::shared_ptr<const RenderMesh> m_currentMesh;
    std::string m_currentField;

    QFutureWatcher<RenderMesh> m_watcher;
    QTimer m_debounceTimer;

    uint64_t m_loadToken = 0;
    std::shared_ptr<std::atomic<uint64_t>> m_taskToken;
};


