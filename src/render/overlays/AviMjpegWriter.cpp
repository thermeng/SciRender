#include "render/overlays/AviMjpegWriter.h"

#include <QFile>
#include <QDataStream>

namespace {
constexpr quint32 kAvifHasIndex = 0x10;
constexpr quint32 kIdxKeyframe = 0x10;
}

void AviMjpegWriter::writeU32(quint32 v) { *m_out << v; }
void AviMjpegWriter::writeU16(quint16 v) { *m_out << v; }

bool AviMjpegWriter::open(const QString& path, int width, int height, double fps) {
    close();
    m_path = path;
    m_file = std::make_unique<QFile>(path);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_error = QStringLiteral("Cannot open %1 for writing").arg(path);
        m_file.reset();
        return false;
    }
    m_out = std::make_unique<QDataStream>(m_file.get());
    m_out->setByteOrder(QDataStream::LittleEndian);
    m_out->setVersion(QDataStream::Qt_5_0);

    m_width = width;
    m_height = height;
    m_fps = fps > 0 ? fps : 1.0;
    m_frameCount = 0;
    m_maxChunk = 0;
    m_moviDataSize = 0;
    m_failed = false;
    m_error.clear();
    m_frameSizes.clear();
    m_frameOffsets.clear();

    // ---- RIFF 'AVI ' ----
    m_out->writeRawData("RIFF", 4);
    const qint64 riffSizePos = m_file->pos();
    writeU32(0); // patched in finalize()
    m_out->writeRawData("AVI ", 4);

    // ---- LIST 'hdrl' ----
    // Payload = 'hdrl' + avih chunk (64) + strl LIST chunk (124) = 192.
    m_out->writeRawData("LIST", 4);
    writeU32(192);
    m_out->writeRawData("hdrl", 4);

    // 'avih' MainAVIHeader (56 bytes)
    m_out->writeRawData("avih", 4);
    writeU32(56);
    const quint32 usecPerFrame = quint32(1.0e6 / m_fps);
    writeU32(usecPerFrame);      // dwMicroSecPerFrame
    writeU32(0);                 // dwMaxBytesPerSec
    writeU32(0);                 // dwPaddingGranularity
    writeU32(kAvifHasIndex);     // dwFlags
    m_avihFramesPos = m_file->pos();
    writeU32(0);                 // dwTotalFrames (patched)
    writeU32(0);                 // dwInitialFrames
    writeU32(1);                 // dwStreams
    m_avihSuggestedPos = m_file->pos();
    writeU32(0);                 // dwSuggestedBufferSize (patched)
    writeU32(quint32(width));    // dwWidth
    writeU32(quint32(height));   // dwHeight
    writeU32(0); writeU32(0); writeU32(0); writeU32(0); // dwReserved[4]

    // ---- LIST 'strl' ----
    // Payload = 'strl' + strh chunk (64) + strf chunk (48) = 116.
    m_out->writeRawData("LIST", 4);
    writeU32(116);
    m_out->writeRawData("strl", 4);

    // 'strh' AVIStreamHeader (56 bytes)
    m_out->writeRawData("strh", 4);
    writeU32(56);
    m_out->writeRawData("vids", 4);
    m_out->writeRawData("MJPG", 4);
    writeU32(0);                 // dwFlags
    writeU16(0);                 // wPriority
    writeU16(0);                 // wLanguage
    writeU32(0);                 // dwInitialFrames
    writeU32(1);                 // dwScale
    writeU32(quint32(m_fps + 0.5)); // dwRate (fps = rate/scale)
    writeU32(0);                 // dwStart
    m_strhLengthPos = m_file->pos();
    writeU32(0);                 // dwLength (patched)
    m_strhSuggestedPos = m_file->pos();
    writeU32(0);                 // dwSuggestedBufferSize (patched)
    writeU32(0xFFFFFFFF);        // dwQuality
    writeU32(0);                 // dwSampleSize
    writeU16(0); writeU16(0);    // rcFrame top-left
    writeU16(quint16(width)); writeU16(quint16(height)); // rcFrame bottom-right

    // 'strf' BITMAPINFOHEADER (40 bytes)
    m_out->writeRawData("strf", 4);
    writeU32(40);
    writeU32(40);                // biSize — first field of the header, was
                                 // missing and left the chunk 4 bytes short
    const qint32 w = width;
    const qint32 h = height;
    *m_out << w << h;
    writeU16(1);                 // biPlanes
    writeU16(24);                // biBitCount
    m_out->writeRawData("MJPG", 4); // biCompression
    writeU32(quint32(width) * quint32(height) * 3); // biSizeImage
    *m_out << qint32(0) << qint32(0); // biXPelsPerMeter, biYPelsPerMeter
    writeU32(0); writeU32(0);    // biClrUsed, biClrImportant

    // ---- LIST 'movi' ----
    m_out->writeRawData("LIST", 4);
    m_moviSizePos = m_file->pos();
    writeU32(0);                 // patched in finalize()
    m_out->writeRawData("movi", 4);

    return true;
}

bool AviMjpegWriter::addJpegFrame(const QByteArray& jpeg) {
    if (!m_out || m_failed) return false;
    if (jpeg.isEmpty()) {
        m_error = QStringLiteral("Empty frame data");
        m_failed = true;
        return false;
    }
    const quint32 size = quint32(jpeg.size());
    const quint32 padded = (size + 1) & ~quint32(1);

    m_frameOffsets.append(4 + m_moviDataSize); // offset relative to after 'movi'
    m_frameSizes.append(size);
    m_out->writeRawData("00dc", 4);
    writeU32(size);
    m_out->writeRawData(jpeg.constData(), int(jpeg.size()));
    if (padded != size) *m_out << quint8(0); // single pad byte to even boundary

    m_moviDataSize += 8 + padded;
    m_maxChunk = qMax(m_maxChunk, padded);
    ++m_frameCount;

    if (m_file->error() != QFileDevice::NoError) {
        m_error = m_file->errorString();
        m_failed = true;
        return false;
    }
    return true;
}

bool AviMjpegWriter::finalize() {
    if (!m_out || m_failed) return false;

    // ---- 'idx1' index ----
    m_out->writeRawData("idx1", 4);
    writeU32(quint32(m_frameCount) * 16);
    for (int i = 0; i < m_frameCount; ++i) {
        m_out->writeRawData("00dc", 4);
        writeU32(kIdxKeyframe);
        writeU32(m_frameOffsets[i]);
        writeU32(m_frameSizes[i]);
    }

    // ---- Patch headers ----
    const qint64 fileSize = m_file->size();
    m_file->seek(4);
    writeU32(quint32(fileSize - 8)); // RIFF size

    m_file->seek(m_moviSizePos);
    writeU32(4 + m_moviDataSize); // movi list payload: 'movi' fourcc + chunks

    m_file->seek(m_avihFramesPos);
    writeU32(quint32(m_frameCount));
    m_file->seek(m_avihSuggestedPos);
    writeU32(m_maxChunk);

    m_file->seek(m_strhLengthPos);
    writeU32(quint32(m_frameCount));
    m_file->seek(m_strhSuggestedPos);
    writeU32(m_maxChunk);

    m_file->flush();
    if (m_file->error() != QFileDevice::NoError) {
        m_error = m_file->errorString();
        m_failed = true;
        return false;
    }
    close();
    return true;
}

void AviMjpegWriter::close() {
    if (m_file) {
        if (m_file->isOpen()) m_file->close();
        m_file.reset();
    }
    m_out.reset();
}
