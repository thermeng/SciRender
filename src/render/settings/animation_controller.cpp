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
    m_tickTimer.setInterval(std::max(4, int(1000.0 / m_fps)));
    connect(&m_tickTimer, &QTimer::timeout, this, &AnimationController::onTick);
}

QString AnimationController::sequenceName() const {
    if (m_sourcePath.isEmpty()) return QString();
    int slash = m_sourcePath.lastIndexOf('/');
    if (slash < 0) slash = m_sourcePath.lastIndexOf('\\');
    return slash >= 0 ? m_sourcePath.mid(slash + 1) : m_sourcePath;
}

float AnimationController::bufferingProgress() const {
    if (!hasSequence() || frameCount() <= 0) return 0.0f;
    const int target = std::clamp(static_cast<int>(std::floor(m_playhead)), 0, frameCount() - 1);
    int total = 0;
    int cached = 0;
    auto countFrame = [&](int i) {
        if (i < 0 || i >= frameCount()) return;
        ++total;
        if (m_cache.count(i)) ++cached;
    };
    countFrame(target);
    for (int k = 1; k <= kPrefetchAhead; ++k) countFrame(target + k);
    for (int k = 1; k <= kKeepBehind; ++k) countFrame(target - k);
    if (total <= 0) return 1.0f;
    return static_cast<float>(cached) / static_cast<float>(total);
}

void AnimationController::setFps(double v) {
    v = std::clamp(v, 0.25, 120.0);
    if (m_fps == v) return;
    m_fps = v;
    m_tickTimer.setInterval(std::max(4, int(1000.0 / effectiveFps())));
    emit stateChanged();
}

void AnimationController::setSpeedMultiplier(double v) {
    v = std::clamp(v, 0.25, 16.0);
    if (qFuzzyCompare(m_speedMultiplier + 1.0, v + 1.0)) return;
    m_speedMultiplier = v;
    m_tickTimer.setInterval(std::max(4, int(1000.0 / effectiveFps())));
    emit stateChanged();
}

void AnimationController::setLoop(bool v) {
    if (m_loop == v) return;
    m_loop = v;
    emit stateChanged();
}

void AnimationController::setLoopRange(int start, int end) {
    const int n = frameCount();
    m_loopStart = std::clamp(start, 0, std::max(0, n - 1));
    m_loopEnd = std::clamp(end, m_loopStart, std::max(0, n - 1));
    emit stateChanged();
}

void AnimationController::setLoopRangeActive(bool v) {
    if (m_loopRangeActive == v) return;
    m_loopRangeActive = v;
    emit stateChanged();
}

void AnimationController::seekToTime(double t) {
    if (!hasSequence() || frameCount() <= 0) return;
    const auto& ts = m_sequence.timesteps;
    auto it = std::lower_bound(ts.begin(), ts.end(), t);
    int best = 0;
    if (it != ts.end()) {
        best = static_cast<int>(it - ts.begin());
        if (it != ts.begin()) {
            auto prev = it - 1;
            if (std::abs(*prev - t) < std::abs(*it - t))
                best = static_cast<int>(prev - ts.begin());
        }
    }
    seek(best);
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
    // Parse warnings (e.g. skipped malformed entries) are logged to stderr
    // by the parser; do not surface them as user-facing errors here.

    // Fresh sequence: bump the generation so any in-flight parse from the
    // previous sequence is dropped on arrival (no GUI-thread wait), then
    // drain loader state.
    ++m_generation;
    for (auto &inf : m_inFlights) {
        if (inf.watcher) {
            disconnect(inf.watcher, nullptr, this, nullptr);
            inf.watcher->cancel();
            inf.watcher->deleteLater();
        }
    }
    m_inFlights.clear();
    m_loadInFlight = false;
    m_loadQueue.clear();
    m_queuedSet.clear();
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
    m_loopStart = 0;
    m_loopEnd = std::max(0, m_sequence.frameCount() - 1);
    m_loopRangeActive = false;

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
    if (!hasSequence() && m_cache.empty() && !m_playing && m_inFlights.empty() && m_loadQueue.empty()) return;
    ++m_generation;
    for (auto &inf : m_inFlights) {
        if (inf.watcher) {
            disconnect(inf.watcher, nullptr, this, nullptr);
            inf.watcher->cancel();
            inf.watcher->deleteLater();
        }
    }
    m_inFlights.clear();
    m_loadInFlight = false;
    m_loadQueue.clear();
    m_queuedSet.clear();
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
    if (m_loopRangeActive && m_loopStart < m_loopEnd && n > 0) {
        frameIndex = std::clamp(frameIndex, m_loopStart, m_loopEnd);
    } else {
        frameIndex = std::clamp(frameIndex, 0, n - 1);
    }
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
    // Clamp scales with FPS so low FPS (e.g. 1 FPS, interval 1000 ms) is not
    // throttled: allow up to 2 frames of catch-up regardless of FPS.
    // Without this, 1 FPS would be clamped to 0.25 sec → 0.25 frames/tick → 0.25 FPS.
    double maxDt = 0.25;
    double eff = effectiveFps();
    if (eff > 1e-6) maxDt = std::max(0.25, 2.0 / eff);
    dtSec = std::clamp(dtSec, 0.0, maxDt);

    if (m_playing && dtSec > 0.0) {
        const int n = frameCount();
        // ParaView Sequence (blocking) mode: never skip an uncached frame.
        // Candidate advance is computed first; if the next integer frame(s)
        // are not yet cached the playhead stalls (wall-clock stretches) so
        // every timestep is eventually displayed in order. This matches
        // ParaView's default Sequence playback and fixes the large-dataset
        // stutter where the old code advanced unconditionally and held the
        // previous frame while the playhead raced ahead.
        double candPlayhead = m_playhead + dtSec * effectiveFps();
        // Apply loop wrapping to candidate for correct target check
        double wrappedCand = candPlayhead;
        bool willStop = false;
        bool didWrap = false;
        if (n > 0) {
            if (m_loop && m_loopRangeActive && m_loopStart < m_loopEnd) {
                const double range = static_cast<double>(m_loopEnd - m_loopStart) + 1.0;
                if (candPlayhead > m_loopEnd || candPlayhead < m_loopStart) {
                    double off = std::fmod(candPlayhead - m_loopStart, range);
                    if (off < 0) off += range;
                    wrappedCand = m_loopStart + off;
                    if (!std::isfinite(wrappedCand) || wrappedCand < 0) wrappedCand = m_loopStart;
                    didWrap = (wrappedCand != candPlayhead);
                }
            } else if (m_loop && candPlayhead >= n) {
                wrappedCand = std::fmod(candPlayhead, static_cast<double>(n));
                if (!std::isfinite(wrappedCand) || wrappedCand < 0) wrappedCand = 0;
                didWrap = true;
            } else if (!m_loop && candPlayhead >= n) {
                // Non-looping: candidate would clamp to last frame; allow it
                // only if last frame is cached, otherwise stall before the end.
                wrappedCand = n - 1;
                willStop = true;
            }
        }
        const int curTarget = std::clamp(static_cast<int>(std::floor(m_playhead)), 0, n - 1);
        const int candTarget = std::clamp(static_cast<int>(std::floor(wrappedCand)), 0, n - 1);
        bool canAdvance = true;
        if (candTarget != curTarget) {
            // Require every intermediate integer frame to be cached; if any
            // is missing the earliest missing blocks advancement. Handles
            // multi-frame jumps from high effectiveFps and loop wraps.
            bool wrapped = didWrap && candTarget < curTarget;
            // For range wrap, candTarget < curTarget also indicates wrap, but
            // need to handle both full-loop and range-loop.
            // Detect any wrap where candidate wrapped through start.
            bool isRangeWrap = m_loop && m_loopRangeActive && m_loopStart < m_loopEnd && didWrap && candTarget < curTarget;
            if (wrapped || isRangeWrap) {
                if (isRangeWrap) {
                    for (int f = curTarget + 1; f <= m_loopEnd; ++f) {
                        if (m_cache.find(f) == m_cache.end()) { canAdvance = false; break; }
                    }
                    if (canAdvance) {
                        for (int f = m_loopStart; f <= candTarget; ++f) {
                            if (m_cache.find(f) == m_cache.end()) { canAdvance = false; break; }
                        }
                    }
                } else {
                    for (int f = curTarget + 1; f < n; ++f) {
                        if (m_cache.find(f) == m_cache.end()) { canAdvance = false; break; }
                    }
                    if (canAdvance) {
                        for (int f = 0; f <= candTarget; ++f) {
                            if (m_cache.find(f) == m_cache.end()) { canAdvance = false; break; }
                        }
                    }
                }
            } else {
                // Linear forward (no wrap or non-looping tail)
                int stepEnd = willStop ? candTarget : candTarget;
                for (int f = curTarget + 1; f <= stepEnd; ++f) {
                    if (m_cache.find(f) == m_cache.end()) { canAdvance = false; break; }
                }
            }
        }
        if (canAdvance) {
            m_playhead = wrappedCand;
            if (willStop && canAdvance) {
                m_playing = false;
                m_tickTimer.stop();
                emit stateChanged();
            } else if (didWrap) {
                if (!std::isfinite(m_playhead) || m_playhead < 0) m_playhead = m_loop && m_loopRangeActive && m_loopStart < m_loopEnd ? m_loopStart : 0;
            }
        } else {
            // Stall: keep playhead at current position. Wall-clock stretches
            // so every frame is shown. Prefetch is refreshed below so the
            // missing frame is prioritized.
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
    // Identify which watcher finished (concurrent loader)
    QObject* senderObj = sender();
    int slot = -1;
    for (int i = 0; i < static_cast<int>(m_inFlights.size()); ++i) {
        if (static_cast<QObject*>(m_inFlights[i].watcher) == senderObj) { slot = i; break; }
    }
    QFutureWatcher<FrameLoadResult>* finishedWatcher = nullptr;
    int index = -1;
    uint64_t gen = 0;
    FrameLoadResult res;
    if (slot >= 0) {
        finishedWatcher = m_inFlights[slot].watcher;
        res = finishedWatcher->result();
        index = m_inFlights[slot].index;
        gen = m_inFlights[slot].gen;
        m_inFlights.erase(m_inFlights.begin() + slot);
    } else if (senderObj) {
        auto* watcher = static_cast<QFutureWatcher<FrameLoadResult>*>(senderObj);
        finishedWatcher = watcher;
        // Fallback: legacy single watcher path (should not happen after refactor)
        res = watcher->result();
        index = m_inFlightIndex;
        gen = m_inFlightGen;
    } else {
        return;
    }
    MeshPtr mesh = res.mesh;
    // Cleanup watcher
    if (finishedWatcher) {
        finishedWatcher->deleteLater();
    }
    m_loadInFlight = !m_inFlights.empty();
    if (!m_loadInFlight) {
        m_inFlightIndex = -1;
        m_inFlightGen = 0;
    }

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
        // Note: no immediate successor auto-advance while playing — that
        // bypasses FPS timing (playhead += dt*Fps) and makes 1 FPS play as
        // fast as the loader. Stall recovery is handled by the next onTick
        // after the successor is cached, respecting the FPS timer interval.
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
    auto wrapIndex = [&](int raw) -> int {
        if (!m_loop) return raw;
        if (m_loopRangeActive && m_loopStart < m_loopEnd) {
            const int range = m_loopEnd - m_loopStart + 1;
            if (range <= 0) return raw;
            int off = (raw - m_loopStart) % range;
            if (off < 0) off += range;
            return m_loopStart + off;
        } else {
            if (n <= 0) return raw;
            int off = raw % n;
            if (off < 0) off += n;
            return off;
        }
    };
    pushWanted(target);
    for (int k = 1; k <= kPrefetchAhead; ++k) {
        int idx = base + k;
        if (m_loop) idx = wrapIndex(idx);
        pushWanted(idx);
    }
    for (int k = 1; k <= kKeepBehind; ++k) {
        int idx = base - k;
        if (m_loop) idx = wrapIndex(idx);
        pushWanted(idx);
    }
    // Also ensure the loop wrap target ahead of playhead is queued (covers
    // the case where playhead is at the end and target+1 wraps to start).
    if (m_loop) {
        int ahead = wrapIndex(target + 1);
        if (ahead != target) pushWanted(ahead);
    }

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
    auto isNear = [&](int i, int center) -> bool {
        if (!m_loop) {
            return i >= center - kKeepBehind && i <= center + kPrefetchAhead;
        }
        if (m_loopRangeActive && m_loopStart < m_loopEnd) {
            const int range = m_loopEnd - m_loopStart + 1;
            if (i < m_loopStart || i > m_loopEnd) return false;
            if (center < m_loopStart || center > m_loopEnd) return false;
            int fwd = (i - center + range) % range;
            int bwd = (center - i + range) % range;
            return fwd <= kPrefetchAhead || bwd <= kKeepBehind;
        } else {
            if (n <= 0) return false;
            int fwd = (i - center + n) % n;
            int bwd = (center - i + n) % n;
            return fwd <= kPrefetchAhead || bwd <= kKeepBehind;
        }
    };
    auto inWindow = [&](int i) {
        if (isProtected(i)) return true;
        return isNear(i, m_displayFrame) || isNear(i, anchor);
    };
    // Byte accounting helper — null entries contribute 0 and are erased.
    auto dropEntry = [&](auto it) {
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
    // Concurrent loader: fill up to kMaxConcurrent in-flight parses.
    while (static_cast<int>(m_inFlights.size()) < kMaxConcurrent && !m_loadQueue.empty()) {
        const int next = m_loadQueue.front();
        m_loadQueue.pop_front();
        m_queuedSet.erase(next);

        const uint64_t gen = m_generation;
        // Use zero-copy ref to avoid vector copy on hot path
        const auto& filesRef = m_sequence.filesForFrameRef(next);
        std::vector<std::string> files = filesRef;

        auto* watcher = new QFutureWatcher<FrameLoadResult>(this);
        connect(watcher, &QFutureWatcher<FrameLoadResult>::finished,
                this, &AnimationController::onFrameLoaded);
        m_inFlights.push_back({watcher, next, gen});
        m_inFlightIndex = next;
        m_inFlightGen = gen;
        m_loadInFlight = true;

        auto future = QtConcurrent::run([gen, files]() -> FrameLoadResult {
            std::string err;
            MeshPtr mesh = loadFrameFiles(files, &err);
            return FrameLoadResult{mesh, err};
        });
        watcher->setFuture(future);
    }
    m_loadInFlight = !m_inFlights.empty();
}
