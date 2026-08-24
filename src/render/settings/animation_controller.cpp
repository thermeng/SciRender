#include "render/settings/animation_controller.h"
#include "QtConcurrent/QtConcurrentRun"

#include <algorithm>
#include <cmath>
#include <iostream>

// Frame parse payload: load every part file of one timestep through the
// extension dispatcher and merge multi-part frames into a single mesh.
// Runs entirely off the GUI thread; returns nullptr on failure so the caller
// can report a per-frame error without aborting playback.
// If outError is non-null, it is filled with the exception message for UI.
static std::shared_ptr<const RenderMesh> loadFrameFiles(const std::vector<std::string>& files,
                                                        std::string* outError = nullptr) {
    try {
        if (files.empty()) {
            if (outError) *outError = "no files for frame";
            return nullptr;
        }
        std::vector<RenderMesh> pieces;
        pieces.reserve(files.size());
        std::string firstError;
        for (const std::string& f : files) {
            try {
                RenderMesh m = loadMeshFile(f);
                if (!m.vertices.empty()) pieces.push_back(std::move(m));
                else if (firstError.empty()) firstError = "empty mesh: " + f;
            } catch (const std::exception& e) {
                if (firstError.empty()) firstError = std::string(e.what()) + " (" + f + ")";
                std::cerr << "AnimationController: part load failed: " << f << " — " << e.what() << std::endl;
            }
        }
        if (pieces.empty()) {
            if (outError) *outError = firstError.empty() ? "all parts failed" : firstError;
            return nullptr;
        }
        if (pieces.size() == 1)
            return std::make_shared<const RenderMesh>(std::move(pieces[0]));
        return std::make_shared<const RenderMesh>(mergeRenderMeshes(pieces));
    } catch (const std::exception& e) {
        std::string msg = e.what();
        std::cerr << "AnimationController: frame load failed: " << msg << std::endl;
        if (outError) *outError = msg;
        return nullptr;
    } catch (...) {
        if (outError) *outError = "unknown error";
        return nullptr;
    }
}

AnimationController::AnimationController(QObject* parent)
    : QObject(parent) {
    m_tickTimer.setInterval(33);
    connect(&m_tickTimer, &QTimer::timeout, this, &AnimationController::onTick);
    connect(&m_watcher, &QFutureWatcher<FrameLoadResult>::finished,
            this, &AnimationController::onFrameLoaded);
}

QString AnimationController::sequenceName() const {
    if (m_sourcePath.isEmpty()) return QString();
    int slash = m_sourcePath.lastIndexOf('/');
    if (slash < 0) slash = m_sourcePath.lastIndexOf('\\');
    return slash >= 0 ? m_sourcePath.mid(slash + 1) : m_sourcePath;
}

void AnimationController::setFps(double v) {
    v = std::clamp(v, 0.25, 120.0);
    if (m_fps == v) return;
    m_fps = v;
    emit stateChanged();
}

void AnimationController::setLoop(bool v) {
    if (m_loop == v) return;
    m_loop = v;
    emit stateChanged();
}

void AnimationController::loadPvd(const QString& filePath) {
    if (filePath.isEmpty()) return;

    PvdParseDiagnostics diag;
    PvdSequence seq = parsePVD(filePath.toStdString(), &diag);
    if (seq.frameCount() <= 0) {
        QString msg = diag.error.empty()
            ? QString("Could not read PVD collection:\n%1").arg(filePath)
            : QString::fromStdString(diag.error);
        emit errorOccurred(msg);
        return;
    }
    if (!diag.warning.empty()) {
        emit errorOccurred(QString::fromStdString(diag.warning));
    }

    // Fresh sequence: bump the generation so any in-flight parse from the
    // previous sequence is dropped on arrival (no GUI-thread wait), then
    // drain loader state.
    ++m_generation;
    m_loadQueue.clear();
    m_queuedSet.clear();
    m_loadInFlight = false;
    m_cache.clear();
    m_lastAccess.clear();
    m_cacheBytesMap.clear();
    m_cacheBytes = 0;
    m_accessCounter = 0;

    m_sequence = std::move(seq);
    m_sourcePath = filePath;
    m_errorContext = sequenceName();
    m_displayFrame = -1;
    m_playhead = 0.0;

    // Kick the pipeline: queue the opening window, keep paused.
    // Do not autoplay — user must press Play. First frame will be
    // published via onFrameLoaded even while paused.
    updatePrefetch();
    pumpQueue();
    m_clock.restart();
    m_lastTickMs = 0;
    m_playing = false;
    m_tickTimer.stop();
    emit stateChanged();
}

void AnimationController::play() {
    if (!hasSequence() || m_playing) return;
    m_clock.restart();
    m_lastTickMs = 0;
    m_playing = true;
    m_tickTimer.start();
    updatePrefetch();
    pumpQueue();
    emit stateChanged();
}

void AnimationController::pause() {
    if (!m_playing) return;
    m_playing = false;
    m_tickTimer.stop();
    emit stateChanged();
}

void AnimationController::togglePlay() {
    if (m_playing) pause();
    else play();
}

void AnimationController::clear() {
    if (!hasSequence() && m_cache.empty() && !m_playing && !m_loadInFlight) return;
    ++m_generation;
    m_loadQueue.clear();
    m_queuedSet.clear();
    m_loadInFlight = false;
    m_cache.clear();
    m_lastAccess.clear();
    m_cacheBytesMap.clear();
    m_cacheBytes = 0;
    m_sequence = PvdSequence();
    m_sourcePath.clear();
    m_errorContext.clear();
    m_displayFrame = -1;
    m_playhead = 0.0;
    m_playing = false;
    m_tickTimer.stop();
    emit stateChanged();
}

void AnimationController::stepForward() {
    if (!hasSequence()) return;
    pause();
    seek(m_displayFrame + 1);
}

void AnimationController::stepBackward() {
    if (!hasSequence()) return;
    pause();
    seek(m_displayFrame - 1);
}

void AnimationController::seek(int frameIndex) {
    if (!hasSequence()) return;
    const int n = frameCount();
    frameIndex = std::clamp(frameIndex, 0, n - 1);
    m_playhead = static_cast<double>(frameIndex);

    auto it = m_cache.find(frameIndex);
    if (it != m_cache.end()) {
        publishFrame(it->second, frameIndex);
        return;
    }
    // Uncached: put the target at the FRONT of the loader queue.
    if (!m_queuedSet.count(frameIndex)) {
        m_loadQueue.push_front(frameIndex);
        m_queuedSet.insert(frameIndex);
    }
    updatePrefetch();
    pumpQueue();
    emit stateChanged();
}

void AnimationController::onTick() {
    if (!hasSequence() || frameCount() <= 0) return;
    const qint64 now = m_clock.elapsed();
    double dtSec = static_cast<double>(now - m_lastTickMs) / 1000.0;
    m_lastTickMs = now;
    // Clamp dt to avoid playhead jump after GUI stall (dialog, heavy render).
    // Without this, a multi-second stall would fast-forward past transients.
    dtSec = std::clamp(dtSec, 0.0, 0.25);

    if (m_playing && dtSec > 0.0) {
        m_playhead += dtSec * m_fps;
        const int n = frameCount();
        if (n > 0 && m_playhead >= n) {
            if (m_loop) {
                m_playhead = std::fmod(m_playhead, static_cast<double>(n));
                if (!std::isfinite(m_playhead) || m_playhead < 0) m_playhead = 0;
            } else {
                m_playhead = n - 1;
                m_playing = false;
                m_tickTimer.stop();
                emit stateChanged();
            }
        }
    }

    const int target = std::clamp(static_cast<int>(std::floor(m_playhead)), 0, frameCount() - 1);
    auto it = m_cache.find(target);
    if (it != m_cache.end() && target != m_displayFrame) {
        publishFrame(it->second, target);
        return; // publishFrame already refreshed prefetch + queue
    }

    updatePrefetch();
    pumpQueue();
}

void AnimationController::onFrameLoaded() {
    // Safe: QFutureWatcher::result() is valid after finished() on the GUI thread,
    // and the watcher is never reset between finished and this slot (all state
    // mutations are GUI-thread serialized). No race with clear()/loadPvd().
    m_loadInFlight = false;
    FrameLoadResult res = m_watcher.result();
    MeshPtr mesh = res.mesh;
    const int index = m_inFlightIndex;
    const uint64_t gen = m_inFlightGen;

    // A newer loadPvd()/clear() invalidated everything queued before it.
    if (gen != m_generation) {
        pumpQueue();
        return;
    }

    if (mesh) {
        // Track bytes and LRU (use actual bytes stored in map)
        size_t bytes = mesh->estimatedBytes;
        if (bytes == 0) bytes = mesh->vertices.size() * sizeof(float) + mesh->indices.size() * sizeof(uint32_t) + 1024;
        auto itOld = m_cache.find(index);
        if (itOld != m_cache.end()) {
            auto bit = m_cacheBytesMap.find(index);
            if (bit != m_cacheBytesMap.end()) {
                m_cacheBytes -= bit->second;
                m_cacheBytesMap.erase(bit);
            } else if (itOld->second) {
                m_cacheBytes -= itOld->second->estimatedBytes ? itOld->second->estimatedBytes : 0;
            }
        }
        m_cache[index] = std::move(mesh);
        m_cacheBytes += bytes;
        m_cacheBytesMap[index] = bytes;
        m_lastAccess[index] = ++m_accessCounter;
        // The frame we just parsed MUST survive this pass — it is about to be
        // published. Without protection, a loop-wrap/seek target (e.g. frame 0
        // while m_displayFrame is still 23) falls outside the stale window and
        // is erased here, after which the publish below would read a
        // default-constructed null entry.
        evictOutsideWindow(index);
        // Paused scrubbing has no ticks — publish here when the requested
        // frame arrives. While playing, onTick publishes; publishing from
        // both is safe because both check against the playhead target.
        // find() (NOT operator[]): operator[] would insert a null MeshPtr if
        // the entry vanished, poisoning the cache for the eviction derefs.
        const int target = std::clamp(
            static_cast<int>(std::floor(m_playhead)), 0, frameCount() - 1);
        if (target != m_displayFrame && index == target) {
            auto itPub = m_cache.find(target);
            if (itPub != m_cache.end() && itPub->second)
                publishFrame(itPub->second, target);
        }
    } else if (!m_sequence.frameCount()) {
        // Sequence went away mid-load; nothing to report.
    } else {
        QString detail = res.error.empty() ? QString()
            : QString(" (%1)").arg(QString::fromStdString(res.error));
        emit errorOccurred(QString("Frame %1 of %2 could not be loaded — skipped%3.")
                               .arg(index).arg(m_errorContext).arg(detail));
    }

    pumpQueue();
}

void AnimationController::publishFrame(MeshPtr mesh, int idx) {
    if (!mesh || idx < 0 || idx >= frameCount()) return;
    m_displayFrame = idx;
    // Update LRU timestamp
    m_lastAccess[idx] = ++m_accessCounter;
    emit frameReady(mesh, idx, m_sequence.frameTime(idx));
    emit stateChanged();
    updatePrefetch();
    pumpQueue();
}

void AnimationController::updatePrefetch() {
    if (!hasSequence()) return;
    const int n = frameCount();
    const int base = std::max(m_displayFrame, 0);
    const int target = std::clamp(static_cast<int>(std::floor(m_playhead)), 0, n - 1);

    // Priority order: exact playhead target first, then forward window
    // (playback direction), then a small look-behind for scrubs back.
    std::deque<int> wanted;
    auto pushWanted = [&](int i) {
        if (i < 0 || i >= n) return;
        auto it = m_cache.find(i);
        if (it != m_cache.end()) {
            if (it->second) return;   // genuinely cached
            // Null entry (defensive): drop it so the frame can be re-queued.
            m_cache.erase(it);
            m_lastAccess.erase(i);
            m_cacheBytesMap.erase(i);
        }
        if (m_queuedSet.count(i)) return;
        wanted.push_back(i);
    };
    pushWanted(target);
    for (int k = 1; k <= kPrefetchAhead; ++k) pushWanted(base + k);
    for (int k = 1; k <= kKeepBehind; ++k) pushWanted(base - k);

    for (int i : wanted) {
        m_loadQueue.push_back(i);
        m_queuedSet.insert(i);
    }
    evictOutsideWindow();
}

void AnimationController::evictOutsideWindow(int protectedIndex) {
    if (m_cache.empty()) return;
    const int n = frameCount();
    // Anchor the keep-window on BOTH the last displayed frame and the playhead
    // target. During a loop-wrap or distant seek the two diverge (display=23,
    // target=0); anchoring only on display would immediately evict the frame
    // the user is about to see — the crash at "move to first frame".
    const int anchor = std::clamp(
        static_cast<int>(std::floor(m_playhead)), 0, std::max(0, n - 1));
    auto isProtected = [&](int i) {
        return i == m_displayFrame || i == anchor || i == protectedIndex;
    };
    auto inWindow = [&](int i) {
        if (isProtected(i)) return true;
        const bool nearDisplay = i >= m_displayFrame - kKeepBehind && i <= m_displayFrame + kPrefetchAhead;
        const bool nearAnchor  = i >= anchor - kKeepBehind        && i <= anchor + kPrefetchAhead;
        return nearDisplay || nearAnchor;
    };
    // Byte accounting helper — null entries contribute 0 and are erased.
    auto dropEntry = [&](std::map<int, MeshPtr>::iterator it) {
        auto bit = m_cacheBytesMap.find(it->first);
        if (bit != m_cacheBytesMap.end()) {
            m_cacheBytes -= bit->second;
            m_cacheBytesMap.erase(bit);
        } else if (it->second) {
            m_cacheBytes -= it->second->estimatedBytes ? it->second->estimatedBytes : 0;
        }
        m_lastAccess.erase(it->first);
        m_cache.erase(it);
    };
    auto it = m_cache.begin();
    while (it != m_cache.end()) {
        // Defensive: a null MeshPtr must never be dereferenced (would segfault
        // on ->estimatedBytes). Erase it; updatePrefetch re-queues the frame.
        if (!it->second) { dropEntry(it++); continue; }
        if (!inWindow(it->first)) { dropEntry(it++); continue; }
        ++it;
    }
    // Hard cap for pathological seeks inside the window bounds.
    // Evict farthest-from-anchor, never the protected frames, so a
    // just-seeked/just-parsed frame is not dropped while a stale prefetch
    // survives.
    while (static_cast<int>(m_cache.size()) > kCacheCap) {
        auto farthest = m_cache.end();
        int maxDist = -1;
        for (auto it2 = m_cache.begin(); it2 != m_cache.end(); ++it2) {
            if (!it2->second) { farthest = it2; break; }   // drop nulls first
            if (isProtected(it2->first)) continue;
            int dist = std::max(std::abs(it2->first - anchor),
                                std::abs(it2->first - m_displayFrame));
            if (dist > maxDist) {
                maxDist = dist;
                farthest = it2;
            }
        }
        if (farthest != m_cache.end()) dropEntry(farthest);
        else break;
    }
    // Byte budget: evict LRU until under budget (protected frames survive).
    while (m_cacheBytes > kCacheBudgetBytes && m_cache.size() > 1) {
        auto lru = m_cache.end();
        qint64 oldest = std::numeric_limits<qint64>::max();
        for (auto it2 = m_cache.begin(); it2 != m_cache.end(); ++it2) {
            if (!it2->second) { lru = it2; break; }        // drop nulls first
            if (isProtected(it2->first)) continue;
            auto ait = m_lastAccess.find(it2->first);
            qint64 ts = (ait != m_lastAccess.end()) ? ait->second : 0;
            if (ts < oldest) {
                oldest = ts;
                lru = it2;
            }
        }
        if (lru == m_cache.end()) break;
        dropEntry(lru);
    }
}

void AnimationController::pumpQueue() {
    if (!hasSequence()) { m_loadQueue.clear(); m_queuedSet.clear(); return; }
    // Drop entries already cached (or null-poisoned) while they waited in line.
    while (!m_loadQueue.empty()) {
        const int front = m_loadQueue.front();
        auto it = m_cache.find(front);
        if (it != m_cache.end()) {
            if (it->second) {
                m_loadQueue.pop_front();
                m_queuedSet.erase(front);
                continue;
            }
            // Null entry: drop it so the frame is genuinely re-loaded.
            m_cache.erase(it);
            m_lastAccess.erase(front);
            m_cacheBytesMap.erase(front);
        }
        break;
    }
    // Single in-flight parse: m_loadInFlight guards setFuture() so a running
    // future is never overwritten mid-flight (except via clear()/loadPvd(),
    // whose generation guard drops the stale result on arrival).
    if (m_loadInFlight || m_loadQueue.empty()) return;

    const int next = m_loadQueue.front();
    m_loadQueue.pop_front();
    m_queuedSet.erase(next);

    const uint64_t gen = m_generation;
    std::vector<std::string> files = m_sequence.filesForFrame(next);

    m_inFlightIndex = next;
    m_inFlightGen = gen;
    m_loadInFlight = true;

    auto future = QtConcurrent::run([gen, files]() -> FrameLoadResult {
        std::string err;
        MeshPtr mesh = loadFrameFiles(files, &err);
        return FrameLoadResult{mesh, err};
    });
    m_watcher.setFuture(future);
}
