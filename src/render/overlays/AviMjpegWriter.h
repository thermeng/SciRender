#pragma once
// Minimal MJPEG-in-AVI muxer (hand-written RIFF; no external dependencies).
// Frames are baseline JPEG images (Qt's built-in encoder); the container plays
// in WMP/VLC/PowerPoint. Headers are written as placeholders and patched in
// finalize(), so an aborted export still yields a playable file.

#include <QString>
#include <QByteArray>
#include <QVector>
#include <memory>

class QFile;
class QDataStream;

class AviMjpegWriter {
public:
    AviMjpegWriter() = default;
    ~AviMjpegWriter() { close(); }

    AviMjpegWriter(const AviMjpegWriter&) = delete;
    AviMjpegWriter& operator=(const AviMjpegWriter&) = delete;

    bool open(const QString& path, int width, int height, double fps);
    bool addJpegFrame(const QByteArray& jpeg);
    // Writes idx1 + patches header sizes/counts. Safe after partial exports.
    bool finalize();
    void close();

    int frameCount() const { return m_frameCount; }
    QString errorString() const { return m_error; }

private:
    void writeU32(quint32 v);
    void writeU16(quint16 v);

    QString m_path;
    QString m_error;
    std::unique_ptr<QFile> m_file;
    std::unique_ptr<QDataStream> m_out;

    int m_width = 0;
    int m_height = 0;
    double m_fps = 0.0;
    int m_frameCount = 0;
    quint32 m_maxChunk = 0;
    quint32 m_moviDataSize = 0;
    qint64 m_moviSizePos = 0;
    qint64 m_avihFramesPos = 0;
    qint64 m_avihSuggestedPos = 0;
    qint64 m_strhLengthPos = 0;
    qint64 m_strhSuggestedPos = 0;
    QVector<quint32> m_frameSizes;
    QVector<quint32> m_frameOffsets;
    bool m_failed = false;
};
