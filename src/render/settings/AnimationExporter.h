#pragma once
// AnimationExporter — deterministic .pvd frame-sequence export.
//
// Per frame: seek the AnimationController, await frameReady(index), publish
// the render state, capture an offscreen QImage via a viewport-provided
// callback (GL work stays on the GUI thread), then queue encode+write on a
// chained QtConcurrent future so encoding overlaps the next frame's capture.
// start() is synchronous but pumps nested event loops, so progress UI and
// cancel stay responsive.

#include <QObject>
#include <QString>
#include <QImage>
#include <atomic>
#include <functional>

class AnimationController;
class RenderSettings;
class Renderer;
class AviMjpegWriter;

struct AnimationExportConfig {
    bool writeAvi = true;
    bool writePng = false;
    QString aviPath;
    QString pngDir;
    double fps = 8.0;
    int jpegQuality = 90;
    int firstFrame = 0;   // inclusive, 0-based
    int lastFrame = -1;   // inclusive; resolved to sequence end when negative
    int width = 1920;
    int height = 1080;
    int samples = 4;
    bool transparent = false;
};

class AnimationExporter : public QObject {
    Q_OBJECT
public:
    // Viewport-provided: renders the CURRENT published state offscreen at the
    // requested size and returns the pixels (GL context work, GUI thread).
    using CaptureFn = std::function<QImage(int w, int h, int samples, bool transparent)>;

    explicit AnimationExporter(QObject* parent = nullptr);

    void wire(AnimationController* controller, RenderSettings* settings);
    void setCaptureFn(CaptureFn fn);

    void start(const AnimationExportConfig& cfg, ::Renderer* scene);
    void cancel() { m_cancelRequested = true; }
    bool isExporting() const { return m_exporting; }

signals:
    void progress(int framesDone, int totalFrames);
    void finished(bool success, const QString& message);

private:
    bool awaitFrame(int index);
    void fail(const QString& message);

    AnimationController* m_controller = nullptr;
    RenderSettings* m_settings = nullptr;
    CaptureFn m_capture;
    std::atomic_bool m_cancelRequested { false };
    bool m_exporting = false;
    QString m_error;
};
