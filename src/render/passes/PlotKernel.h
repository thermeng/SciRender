#pragma once

#include "render/foundation/gl_raii.h"
#include <glad/gl.h>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <cstdint>

// GPU histogram kernel for MVP.
// Design: scalar field (CPU materialized via FieldResolver incl. derived) is
// uploaded to a dedicated SSBO mirror (not the VAO VBO) so the histogram
// compute is decoupled from the mesh draw path. Histogram bins are computed
// via atomicAdd on GPU and read back as a tiny (bins*4 bytes) result.
// Counting is per-source-point (unique topological points) for ParaView
// parity: vertex duplicates at sharp edges (vertexSourceIndex) are deduped
// on the CPU before dispatch so histogram n == sourcePointCount.
// Cell placement bins raw per-cell values. Filter window is optionally
// applied via PlotParams filterEnabled.
class PlotKernel {
public:
    PlotKernel() = default;
    ~PlotKernel() = default;

    // Must be called with a current GL context. Safe to call multiple times (idempotent).
    void init(const std::string& histogramSrc);
    void shutdown();
    bool isReady() const { return m_ready; }
    const std::string& lastError() const { return m_lastError; }

    // Enqueue a new histogram request. The actual dispatch happens on the
    // next tryDispatch() call (which must run with a current GL context,
    // typically from ViewportWidget::paintGL / Renderer::renderFrame).
    // dataMin/dataMax define binning range; if degenerate (|max-min|<1e-12)
    // the kernel will produce zero bins and the caller should show a single-bin
    // fallback. CPU materialization of derived fields is caller's responsibility.
    void requestHistogram(const std::vector<float>& scalars, int numBins, float dataMin, float dataMax);

    // Lightweight CPU fallback when compute unavailable (e.g. headless CI).
    void requestHistogramCpu(const std::vector<float>& scalars, int numBins, float dataMin, float dataMax);

    // Dispatch pending work if any. Returns true if bins were produced (result
    // available via bins()). Must have current GL context if GPU path.
    // If GPU not ready, falls back to already-computed CPU result.
    bool tryDispatch();

    // Result access (thread-safe snapshot). Empty until first successful dispatch.
    std::vector<uint32_t> bins() const;
    int numBins() const { std::lock_guard<std::mutex> lk(m_mutex); return m_lastNumBins; }
    float dataMin() const { std::lock_guard<std::mutex> lk(m_mutex); return m_lastDataMin; }
    float dataMax() const { std::lock_guard<std::mutex> lk(m_mutex); return m_lastDataMax; }
    size_t numValues() const { std::lock_guard<std::mutex> lk(m_mutex); return m_lastNumValues; }
    bool hasResult() const { std::lock_guard<std::mutex> lk(m_mutex); return m_hasResult; }

    // Set histogram compute shader source for lazy init (mirrors MeshGLManager pattern)
    void setHistogramSource(const std::string& src) { m_histogramSrc = src; }

private:
    bool dispatchGpu(const std::vector<float>& scalars, int numBins, float dataMin, float dataMax,
                     std::vector<uint32_t>& outBins);
    static void computeHistogramCpu(const std::vector<float>& scalars, int numBins,
                                    float dataMin, float dataMax, std::vector<uint32_t>& out);

    mutable std::mutex m_mutex;
    std::string m_histogramSrc;
    GlProgram m_progHist;
    GlBuffer m_scalarSsbo; // upload mirror for scalar array
    GlBuffer m_histSsbo;   // histogram bins
    GlBuffer m_paramsUbo;  // PlotParams
    bool m_ready = false;
    std::string m_lastError;

    // Pending request (set by requestHistogram under lock, consumed by tryDispatch)
    std::vector<float> m_pendingScalars;
    int m_pendingBins = 0;
    float m_pendingMin = 0.0f, m_pendingMax = 1.0f;
    bool m_hasPending = false;

    // Last result
    std::vector<uint32_t> m_lastBins;
    int m_lastNumBins = 0;
    float m_lastDataMin = 0.0f, m_lastDataMax = 1.0f;
    size_t m_lastNumValues = 0;
    bool m_hasResult = false;
};
