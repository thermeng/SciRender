#include "PlotKernel.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace {
GLuint compileComputeSrc(const char* src, const char* label, std::string& errOut) {
    GLuint s = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
        errOut += std::string("[PlotKernel] ") + label + " compile error: " + log + "\n";
        glDeleteShader(s);
        return 0;
    }
    return s;
}
GLuint linkComputeProg(GLuint s, const char* label, std::string& errOut) {
    GLuint p = glCreateProgram();
    glAttachShader(p, s);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
        errOut += std::string("[PlotKernel] ") + label + " link error: " + log + "\n";
        glDeleteProgram(p);
        return 0;
    }
    glDeleteShader(s);
    return p;
}
} // anon

void PlotKernel::init(const std::string& histogramSrc) {
    if (m_ready) return;
    if (!histogramSrc.empty()) m_histogramSrc = histogramSrc;
    if (m_histogramSrc.empty()) {
        m_lastError = "[PlotKernel] histogram source empty";
        return;
    }
    std::string err;
    GLuint s = compileComputeSrc(m_histogramSrc.c_str(), "plot_histogram", err);
    if (!s) { m_lastError = err; return; }
    GLuint p = linkComputeProg(s, "plot_histogram", err);
    if (!p) { m_lastError = err; return; }
    m_progHist.reset(p);
    // Bind uniform block "PlotParams" to binding 2 for GL_UNIFORM_BUFFER (shader declares binding=2)
    GLuint blockIdx = glGetUniformBlockIndex(p, "PlotParams");
    if (blockIdx != GL_INVALID_INDEX) glUniformBlockBinding(p, blockIdx, 2);
    glCreateBuffers(1, m_scalarSsbo.ptr());
    glCreateBuffers(1, m_histSsbo.ptr());
    glCreateBuffers(1, m_paramsUbo.ptr());
    m_ready = true;
    m_lastError.clear();
}

void PlotKernel::shutdown() {
    m_progHist.reset();
    m_scalarSsbo.reset();
    m_histSsbo.reset();
    m_paramsUbo.reset();
    m_ready = false;
}

void PlotKernel::requestHistogram(const std::vector<float>& scalars, int numBins, float dataMin, float dataMax) {
    if (numBins < 8) numBins = 8;
    if (numBins > 512) numBins = 512;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_pendingScalars = scalars; // copy (scalars may be large but histogram request is infrequent)
    m_pendingBins = numBins;
    m_pendingMin = dataMin;
    m_pendingMax = dataMax;
    m_hasPending = true;
}

void PlotKernel::requestHistogramCpu(const std::vector<float>& scalars, int numBins, float dataMin, float dataMax) {
    if (numBins < 8) numBins = 8;
    if (numBins > 512) numBins = 512;
    std::vector<uint32_t> out;
    computeHistogramCpu(scalars, numBins, dataMin, dataMax, out);
    std::lock_guard<std::mutex> lk(m_mutex);
    m_lastBins = std::move(out);
    m_lastNumBins = numBins;
    m_lastDataMin = dataMin;
    m_lastDataMax = dataMax;
    m_lastNumValues = scalars.size();
    m_hasResult = true;
    m_hasPending = false;
}

std::vector<uint32_t> PlotKernel::bins() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_lastBins;
}

bool PlotKernel::tryDispatch() {
    std::vector<float> scalars;
    int bins = 0;
    float mn = 0, mx = 1;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_hasPending) return false;
        scalars = std::move(m_pendingScalars);
        bins = m_pendingBins;
        mn = m_pendingMin;
        mx = m_pendingMax;
        m_hasPending = false;
    }
    if (scalars.empty() || bins <= 0) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_lastBins.assign(bins > 0 ? bins : 64, 0);
        m_lastNumBins = bins;
        m_lastDataMin = mn;
        m_lastDataMax = mx;
        m_lastNumValues = scalars.size();
        m_hasResult = true;
        return true;
    }
    // Degenerate range -> CPU single-bin fallback (GPU kernel would leave zeros)
    if (std::abs(mx - mn) < 1e-12f) {
        std::vector<uint32_t> out(bins, 0);
        // Count finite values into the middle bin (or bin 0 if you prefer)
        size_t finite = 0;
        for (float v : scalars) if (std::isfinite(v)) ++finite;
        if (!out.empty()) out[out.size() / 2] = static_cast<uint32_t>(finite);
        std::lock_guard<std::mutex> lk(m_mutex);
        m_lastBins = std::move(out);
        m_lastNumBins = bins;
        m_lastDataMin = mn;
        m_lastDataMax = mx;
        m_lastNumValues = scalars.size();
        m_hasResult = true;
        return true;
    }

    if (!m_ready) {
        // Lazy init if source was set but init not yet called (needs GL context)
        if (!m_histogramSrc.empty()) init(m_histogramSrc);
    }
    if (!m_ready) {
        // GPU not available -> CPU fallback (e.g. in tests / headless)
        std::vector<uint32_t> out;
        computeHistogramCpu(scalars, bins, mn, mx, out);
        std::lock_guard<std::mutex> lk(m_mutex);
        m_lastBins = std::move(out);
        m_lastNumBins = bins;
        m_lastDataMin = mn;
        m_lastDataMax = mx;
        m_lastNumValues = scalars.size();
        m_hasResult = true;
        return true;
    }

    std::vector<uint32_t> out;
    if (!dispatchGpu(scalars, bins, mn, mx, out)) {
        // GPU dispatch failed -> fallback to CPU and surface the error
        computeHistogramCpu(scalars, bins, mn, mx, out);
        std::lock_guard<std::mutex> lk(m_mutex);
        m_lastError += " [PlotKernel] GPU dispatch failed, fell back to CPU\n";
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_lastBins = std::move(out);
        m_lastNumBins = bins;
        m_lastDataMin = mn;
        m_lastDataMax = mx;
        m_lastNumValues = scalars.size();
        m_hasResult = true;
    }
    return true;
}

bool PlotKernel::dispatchGpu(const std::vector<float>& scalars, int numBins, float dataMin, float dataMax,
                              std::vector<uint32_t>& outBins) {
    // Clear any prior error so dispatch error is isolated
    while (glGetError() != GL_NO_ERROR) {}
    // Unbind before orphaning (same pattern as MeshGLManager::dispatchLodCompute)
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, 0);

    const size_t numValues = scalars.size();
    // Upload scalar mirror
    glNamedBufferData(m_scalarSsbo, numValues * sizeof(float), scalars.data(), GL_STATIC_DRAW);
    // Allocate + zero histogram
    glNamedBufferData(m_histSsbo, numBins * sizeof(uint32_t), nullptr, GL_DYNAMIC_DRAW);
    {
        std::vector<uint32_t> zeros(numBins, 0);
        glNamedBufferSubData(m_histSsbo, 0, zeros.size() * sizeof(uint32_t), zeros.data());
    }
    struct Params {
        int numValues;
        int numBins;
        float dataMin;
        float dataMax;
        float filterMin;
        float filterMax;
        int filterEnabled;
        int pad0;
    } params{};
    params.numValues = static_cast<int>(numValues);
    params.numBins = numBins;
    params.dataMin = dataMin;
    params.dataMax = dataMax;
    params.filterMin = 0.0f;
    params.filterMax = 0.0f;
    params.filterEnabled = 0;
    params.pad0 = 0;
    glNamedBufferData(m_paramsUbo, sizeof(Params), &params, GL_STATIC_DRAW);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_scalarSsbo.get());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_histSsbo.get());
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_paramsUbo.get()); // note: shader declares binding=2 uniform

    // The shader declares `layout(std140, binding=2) uniform PlotParams` — that's a UBO binding.
    // glBindBufferBase(GL_UNIFORM_BUFFER, 2, ...) matches it. For SSBO the shader uses
    // binding=0,1 which we bound as SHADER_STORAGE_BUFFER above.

    GLuint groups = static_cast<GLuint>((numValues + 255u) / 256u);
    glUseProgram(m_progHist.get());
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glDispatchCompute(groups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glFlush();

    // Poll for completion — histogram is tiny so synchronous readback is fine for MVP.
    // In a later optimization this can be fenced async.
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        m_lastError = "[PlotKernel] gl error after dispatch: 0x" + std::to_string(err);
        return false;
    }
    outBins.assign(numBins, 0);
    glGetNamedBufferSubData(m_histSsbo, 0, numBins * sizeof(uint32_t), outBins.data());
    return true;
}

void PlotKernel::computeHistogramCpu(const std::vector<float>& scalars, int numBins,
                                     float dataMin, float dataMax, std::vector<uint32_t>& out) {
    out.assign(numBins, 0);
    float d = dataMax - dataMin;
    if (std::abs(d) < 1e-12f) {
        size_t finite = 0;
        for (float v : scalars) if (std::isfinite(v)) ++finite;
        if (!out.empty()) out[out.size() / 2] = static_cast<uint32_t>(finite);
        return;
    }
    for (float v : scalars) {
        if (!std::isfinite(v)) continue;
        float t = (v - dataMin) / d;
        t = std::clamp(t, 0.0f, 0.9999999f);
        int b = static_cast<int>(std::floor(t * numBins));
        b = std::clamp(b, 0, numBins - 1);
        ++out[b];
    }
}
