#pragma once

#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QString>
#include <QFutureWatcher>

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>

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
    double currentTime() const { return m_sequence.frameTime(m_displayFrame); }

    // Frames per second playback rate.
    double fps() const { return m_fps; }
    void setFps(double v);
    bool loop() const { return m_loop; }
    void setLoop(bool v);

    // True while loads are queued/in flight for frames ahead of the playhead.
    bool isBuffering() const {
        return m_loadInFlight || !m_loadQueue.empty();
    }

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

    // Continuous playhead in frame units; m_displayFrame is the integer frame
    // actually shown (last successfully published one).
    double m_playhead = 0.0;
    int m_displayFrame = -1;

    QTimer m_tickTimer;
    QElapsedTimer m_clock;
    qint64 m_lastTickMs = 0;

    // Bounded frame cache keyed by unique-timestep index. Entries are never
    // null: null MeshPtr values are a bug and are dropped defensively on sight.
    std::map<int, MeshPtr> m_cache;
    std::unordered_map<int, qint64> m_lastAccess; // LRU timestamp per frame
    std::unordered_map<int, size_t> m_cacheBytesMap; // per-frame bytes for accurate budget
    qint64 m_accessCounter = 0;
    size_t m_cacheBytes = 0;
    static constexpr int kPrefetchAhead = 6;
    static constexpr int kKeepBehind = 2;
    static constexpr int kCacheCap = 10;
    static_assert(kCacheCap > kPrefetchAhead + kKeepBehind + 1,
                  "kCacheCap must exceed prefetch window (kKeepBehind + 1 + kPrefetchAhead) to avoid hard-cap eviction inside the window");
    static constexpr size_t kCacheBudgetBytes = 512 * 1024 * 1024; // 512 MB

    // Serial loader: one in-flight parse + FIFO queue of pending indices.
    // Single QFutureWatcher: pumpQueue is guarded by m_loadInFlight so only one
    // parse runs at a time and setFuture() never overwrites a running future
    // (except via clear()/loadPvd(), where the generation guard drops the result).
    QFutureWatcher<FrameLoadResult> m_watcher;
    std::deque<int> m_loadQueue;
    std::set<int> m_queuedSet;     // dedup mirror of m_loadQueue
    bool m_loadInFlight = false;
    int m_inFlightIndex = -1;      // frame index of the in-flight parse
    uint64_t m_inFlightGen = 0;    // generation of the in-flight parse

    // Generation counter for stale-result guarding across seek()/loadPvd().
    // loadPvd()/clear() bump it and drain the queue; results whose captured
    // generation no longer matches are dropped on arrival.
    uint64_t m_generation = 0;
};
