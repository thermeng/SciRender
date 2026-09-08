#pragma once

#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QString>
#include <QFutureWatcher>

#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/mesh_loader.h"
#include "core/pvd_parser.h"

// AnimationController — streaming playback engine for .pvd time sequences.
//
// Owns a PvdSequence, a bounded per-frame mesh cache, and a serial async
// loader that prefetches frames ahead of the playhead (QtConcurrent + token
// guard, same stale-result pattern as RenderSettings' mesh watcher). The GUI
// thread drives it through play/pause/seek; each parsed frame is handed to
// RenderSettings via frameReady() as an immutable shared_ptr (zero copy).
//
// The controller has NO GL dependencies: workers only run parsers and
// mergeRenderMeshes, so the whole engine is unit-testable headlessly.
class AnimationController : public QObject {
    Q_OBJECT
public:
    explicit AnimationController(QObject* parent = nullptr);

    // Loads (and starts playing) a .pvd collection. An empty sequence or a
    // missing index emits errorOccurred and leaves prior state untouched.
    void loadPvd(const QString& filePath);
    // Halts playback and cancels queued loads but KEEPS the sequence + cache
    // (used when the user pauses).
    void pause();
    void play();
    void togglePlay();
    // Full teardown: stops playback, drops cache and sequence.
    void clear();

    bool hasSequence() const { return m_sequence.frameCount() > 0; }
    bool isPlaying() const { return m_playing; }
    QString sequenceName() const;
    QString sourcePath() const { return m_sourcePath; }

    int frameCount() const { return m_sequence.frameCount(); }
    int currentFrame() const { return m_displayFrame; }
    double frameTime(int i) const { return m_sequence.frameTime(i); }
    double currentTime() const {
        return (m_displayFrame >= 0 && m_displayFrame < frameCount())
            ? m_sequence.frameTime(m_displayFrame) : 0.0;
    }

    // Frames per second playback rate.
    double fps() const { return m_fps; }
    void setFps(double v);
    double speedMultiplier() const { return m_speedMultiplier; }
    void setSpeedMultiplier(double v);
    bool loop() const { return m_loop; }
    void setLoop(bool v);
    int loopRangeStart() const { return m_loopStart; }
    int loopRangeEnd() const { return m_loopEnd; }
    void setLoopRange(int start, int end);
    bool loopRangeActive() const { return m_loopRangeActive; }
    void setLoopRangeActive(bool v);
    double effectiveFps() const { return m_fps * m_speedMultiplier; }

    // Seek to the nearest frame for a given physical time.
    void seekToTime(double t);

    // True while loads are queued/in flight for frames ahead of the playhead.
    bool isBuffering() const {
        return !m_inFlights.empty() || !m_loadQueue.empty();
    }
    // 0..1 progress of the prefetch queue (frames loaded / frames needed).
    float bufferingProgress() const;

public slots:
    void stepForward();
    void stepBackward();
    void seek(int frameIndex);

signals:
    // Emitted on every visible state change (sequence loaded/unloaded,
    // play/pause, displayed frame moved). UI syncs by re-reading getters.
    void stateChanged();
    // A parsed frame is ready to display. mesh is never null here.
    void frameReady(std::shared_ptr<const RenderMesh> mesh, int frameIndex, double time);
    void errorOccurred(const QString& message);

private slots:
    void onTick();
    void onFrameLoaded();

private:
    using MeshPtr = std::shared_ptr<const RenderMesh>;
    struct FrameLoadResult {
        MeshPtr mesh;
        std::string error;
    };

    void publishFrame(MeshPtr mesh, int index);
    void updatePrefetch();
    void pumpQueue();
    // protectedIndex: a frame that must survive this eviction pass (e.g. the
    // frame that just finished parsing and is about to be published). The
    // keep-window is anchored on BOTH the last displayed frame and the current
    // playhead target, so a loop-wrap / seek to a distant frame never evicts
    // the frame it is about to show.
    void evictOutsideWindow(int protectedIndex = -1);

    PvdSequence m_sequence;
    QString m_sourcePath;
    QString m_errorContext;   // sequence name for error messages

    bool m_playing = false;
    bool m_loop = true;
    double m_fps = 8.0;
    double m_speedMultiplier = 1.0;
    int m_loopStart = 0;
    int m_loopEnd = -1;
    bool m_loopRangeActive = false;

    // Continuous playhead in frame units; m_displayFrame is the integer frame
    // actually shown (last successfully published one).
    double m_playhead = 0.0;
    int m_displayFrame = -1;

    QTimer m_tickTimer;
    QElapsedTimer m_clock;
    qint64 m_lastTickMs = 0;

    // Bounded frame cache keyed by unique-timestep index. Entries are never
    // null: null MeshPtr values are a bug and are dropped defensively on sight.
    // unordered_map gives O(1) lookup vs map's O(log N) — hot on every tick.
    std::unordered_map<int, MeshPtr> m_cache;
    std::unordered_map<int, qint64> m_lastAccess; // LRU timestamp per frame
    std::unordered_map<int, size_t> m_cacheBytesMap; // per-frame bytes for accurate budget
    qint64 m_accessCounter = 0;
    size_t m_cacheBytes = 0;
    static constexpr int kPrefetchAhead = 8;
    static constexpr int kKeepBehind = 2;
    static constexpr int kCacheCap = 14;
    static_assert(kCacheCap > kPrefetchAhead + kKeepBehind + 1,
                  "kCacheCap must exceed prefetch window (kKeepBehind + 1 + kPrefetchAhead) to avoid hard-cap eviction inside the window");
    static constexpr size_t kCacheBudgetBytes = 512 * 1024 * 1024; // 512 MB

    // Concurrent loader: up to kMaxConcurrent parses in flight via QtConcurrent.
    // Generation guard drops stale results after loadPvd/clear.
    static constexpr int kMaxConcurrent = 3;
    struct InFlight {
        QFutureWatcher<FrameLoadResult>* watcher = nullptr;
        int index = -1;
        uint64_t gen = 0;
    };
    std::vector<InFlight> m_inFlights;
    std::deque<int> m_loadQueue;
    std::unordered_set<int> m_queuedSet;     // dedup mirror of m_loadQueue — O(1) vs set's O(log N)
    // Legacy single-flight compat helpers
    bool m_loadInFlight = false; // mirrors !m_inFlights.empty() for isBuffering
    int m_inFlightIndex = -1;      // for debugging
    uint64_t m_inFlightGen = 0;

    // Generation counter for stale-result guarding across seek()/loadPvd().
    // loadPvd()/clear() bump it and drain the queue; results whose captured
    // generation no longer matches are dropped on arrival.
    uint64_t m_generation = 0;
};
