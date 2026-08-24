#include "render/settings/AnimationExporter.h"

#include "render/settings/animation_controller.h"
#include "render/settings/render_settings.h"
#include "render/foundation/renderer.h"
#include "render/overlays/AviMjpegWriter.h"

#include <QEventLoop>
#include <QTimer>
#include <QBuffer>
#include <QImageWriter>
#include <QDir>
#include <QMutex>
#include <QCoreApplication>
#include <QtConcurrent/QtConcurrentRun>
#include <QFuture>

AnimationExporter::AnimationExporter(QObject* parent) : QObject(parent) {}

void AnimationExporter::wire(AnimationController* controller, RenderSettings* settings) {
    m_controller = controller;
    m_settings = settings;
}

void AnimationExporter::setCaptureFn(CaptureFn fn) {
    m_capture = std::move(fn);
}

void AnimationExporter::fail(const QString& message) {
    m_error = message;
    m_exporting = false;
    emit finished(false, message);
}

void AnimationExporter::start(const AnimationExportConfig& cfg, ::Renderer* scene) {
    if (m_exporting) return;
    if (!m_controller || !m_controller->hasSequence() || !m_settings || !scene || !m_capture) {
        emit finished(false, QStringLiteral("Animation export is not available"));
        return;
    }

    const int total = m_controller->frameCount();
    const int first = qBound(0, cfg.firstFrame, total - 1);
    const int last = qBound(first, cfg.lastFrame < 0 ? total - 1 : cfg.lastFrame, total - 1);
    const int count = last - first + 1;
    if (count <= 0) {
        emit finished(false, QStringLiteral("Empty frame range"));
        return;
    }
    if (!cfg.writeAvi && !cfg.writePng) {
        emit finished(false, QStringLiteral("No output format selected"));
        return;
    }

    std::unique_ptr<AviMjpegWriter> writer;
    if (cfg.writeAvi) {
        writer = std::make_unique<AviMjpegWriter>();
        if (!writer->open(cfg.aviPath, cfg.width, cfg.height, cfg.fps)) {
            const QString err = writer->errorString();
            emit finished(false, err);
            return;
        }
    }
    if (cfg.writePng && !QDir().mkpath(cfg.pngDir)) {
        emit finished(false, QStringLiteral("Cannot create PNG output folder"));
        return;
    }

    m_cancelRequested = false;
    m_exporting = true;
    m_error.clear();
    m_controller->pause();

    // Encode chain: each task waits on the previous future, so writer access
    // stays serialized on worker threads while the GUI thread keeps capturing.
    QFuture<bool> chain = QtConcurrent::run([] { return true; });
    std::atomic_bool encodeFailed { false };
    QString encodeError;
    QMutex encodeErrorMutex;

    emit progress(0, count);
    QCoreApplication::processEvents();

    int captured = 0;
    for (int i = first; i <= last; ++i) {
        if (m_cancelRequested) break;

        if (!awaitFrame(i)) {
            if (!m_cancelRequested) {
                chain.waitForFinished();
                if (writer) writer->finalize();
                fail(m_error.isEmpty() ? QStringLiteral("Failed to load frame %1").arg(i + 1)
                                       : m_error);
                return;
            }
            break;
        }

        m_settings->publishRenderState(scene);
        QImage img = m_capture(cfg.width, cfg.height, cfg.samples, cfg.transparent);
        if (img.isNull()) {
            chain.waitForFinished();
            if (writer) writer->finalize();
            fail(QStringLiteral("Offscreen render failed at frame %1").arg(i + 1));
            return;
        }
        ++captured;

        const bool wantAvi = cfg.writeAvi && writer;
        const bool wantPng = cfg.writePng;
        const int quality = cfg.jpegQuality;
        const QString pngPath = cfg.pngDir + QStringLiteral("/frame_%1.png")
                                    .arg(i, 4, 10, QChar('0'));
        chain = chain.then([this, &writer, &encodeFailed, &encodeError, &encodeErrorMutex,
                            img, wantAvi, wantPng, quality, pngPath](bool) -> bool {
            if (m_cancelRequested.load()) return true;
            if (wantAvi) {
                QImage frame = img.hasAlphaChannel()
                    ? img.convertToFormat(QImage::Format_RGB888)
                    : img;
                QByteArray jpeg;
                QBuffer buf(&jpeg);
                buf.open(QIODevice::WriteOnly);
                QImageWriter w(&buf, "JPEG");
                w.setQuality(quality);
                if (!w.write(frame)) {
                    QMutexLocker lock(&encodeErrorMutex);
                    encodeError = w.errorString();
                    encodeFailed = true;
                    return false;
                }
                if (!writer->addJpegFrame(jpeg)) {
                    QMutexLocker lock(&encodeErrorMutex);
                    encodeError = writer->errorString();
                    encodeFailed = true;
                    return false;
                }
            }
            if (wantPng) {
                QImageWriter w(pngPath, "PNG");
                w.setCompression(2);
                if (!w.write(img)) {
                    QMutexLocker lock(&encodeErrorMutex);
                    encodeError = w.errorString();
                    encodeFailed = true;
                    return false;
                }
            }
            return true;
        });

        emit progress(i - first + 1, count);
        QCoreApplication::processEvents();
    }

    chain.waitForFinished();
    if (writer) writer->finalize(); // valid header/index even for a cancelled partial run

    const bool cancelled = m_cancelRequested.load();
    m_exporting = false;
    if (encodeFailed.load()) {
        QMutexLocker lock(&encodeErrorMutex);
        emit finished(false, encodeError);
        return;
    }
    if (cancelled) {
        emit finished(true, QStringLiteral("Export cancelled — partial file kept"));
        return;
    }
    emit finished(true, QStringLiteral("Exported %1 frames").arg(captured));
}

bool AnimationExporter::awaitFrame(int index) {
    if (m_controller->currentFrame() == index) return true;

    QEventLoop loop;
    bool got = false;
    bool timedOut = false;
    const QMetaObject::Connection conn = connect(
        m_controller, &AnimationController::frameReady, &loop,
        [this, index, &got, &loop](std::shared_ptr<const RenderMesh> mesh, int frameIndex, double) {
            if (mesh && frameIndex == index) {
                got = true;
                // Queued: exec() clears the exit flag on entry, so a direct
                // quit() emitted synchronously (cached-frame publish inside
                // seek(), below) would be discarded and stall the loop until
                // the watchdog fires.
                QMetaObject::invokeMethod(&loop, &QEventLoop::quit,
                                          Qt::QueuedConnection);
            }
        });
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, [&got, &timedOut, &loop]() {
        timedOut = true;
        loop.quit();
    });

    m_controller->seek(index); // cached frames may publish synchronously
    if (got) {                 // ...so skip the loop entirely for that case
        disconnect(conn);
        return true;
    }
    timer.start(30000);
    loop.exec();
    disconnect(conn);

    if (!got) {
        if (timedOut && !m_cancelRequested.load())
            m_error = QStringLiteral("Timed out waiting for frame %1").arg(index + 1);
        return false;
    }
    return true;
}
