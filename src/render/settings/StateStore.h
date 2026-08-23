#pragma once
#include "render/foundation/renderer.h"
#include <atomic>

// Deep module for RenderSnapshot seam. Owns snapshot + dirty, hides copy
// optimization and thread handoff. One interface, N consumers (UI, viewport, Renderer).
class StateStore {
public:
    StateStore() = default;

    RenderRenderState& state() { return m_state; }
    const RenderRenderState& state() const { return m_state; }
    const RenderRenderState snapshot() const { return m_state; }

    void markDirty() { m_dirty.store(true); }
    bool consumeDirty() { return m_dirty.exchange(false); }
    bool isDirty() const { return m_dirty.load(); }

    // Deep-copy snapshot to render thread; skips copy when clean.
    // Returns true if copy happened.
    bool publish(::Renderer* scene) {
        if (!scene || !m_dirty.load()) return false;
        // quality overlay optimization: when hidden, avoid copying large float vectors
        RenderRenderState s = m_state;
        if (!s.showQualityOverlay) {
            s.qualityDegenerateTris.reset();
            s.qualityOpenEdges.reset();
            s.qualityNonManifoldEdges.reset();
        }
        scene->setState(s);
        m_dirty.store(false);
        return true;
    }

private:
    RenderRenderState m_state;
    std::atomic<bool> m_dirty{true};
};
